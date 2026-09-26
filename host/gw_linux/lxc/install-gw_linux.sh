#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# install-gw_linux.sh — copy a host-built gw_linux binary + systemd
# unit into the nn-gw LXC rootfs, install the LXC config, and create
# the persistent-state bind-mount target.  Idempotent.

set -euo pipefail

NAME=${NAME:-nn-gw}
LXC_ROOT=${LXC_ROOT:-/var/lib/lxc}
STATE_DIR=${STATE_DIR:-/var/lib/nn-gw}
BIN=${BIN:-/home/chalos/nn_project/build-gw_linux/gw_linux}

if [[ $EUID -ne 0 ]]; then
    echo "install-gw_linux.sh: must run as root" >&2
    exit 1
fi

if [[ ! -x "${BIN}" ]]; then
    echo "binary not found at ${BIN}" >&2
    echo "build first: cd ~/nn_project && cmake --build build-gw_linux" >&2
    exit 1
fi

if [[ ! -d "${LXC_ROOT}/${NAME}" ]]; then
    echo "container '${NAME}' doesn't exist — run build-rootfs.sh first" >&2
    exit 1
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOTFS="${LXC_ROOT}/${NAME}/rootfs"

echo "==> copying gw_linux into ${ROOTFS}/usr/local/bin/"
install -D -m 0755 "${BIN}" "${ROOTFS}/usr/local/bin/gw_linux"

echo "==> installing systemd unit + supervisor"
install -D -m 0644 "${SCRIPT_DIR}/nn-gw.service" \
    "${ROOTFS}/etc/systemd/system/nn-gw.service"
install -D -m 0755 "${SCRIPT_DIR}/gw-supervise" \
    "${ROOTFS}/usr/local/sbin/gw-supervise"
mkdir -p "${ROOTFS}/etc/systemd/system/multi-user.target.wants"
ln -sf /etc/systemd/system/nn-gw.service \
    "${ROOTFS}/etc/systemd/system/multi-user.target.wants/nn-gw.service"

echo "==> creating persistent-state dir ${STATE_DIR}"
mkdir -p "${STATE_DIR}"
chmod 700 "${STATE_DIR}"

echo "==> installing LXC config"
# Render nn-gw.conf with the live rootfs path.
sed -e "s|@ROOTFS@|${ROOTFS}|g" \
    -e "s|@STATE_DIR@|${STATE_DIR}|g" \
    "${SCRIPT_DIR}/nn-gw.conf" > "${LXC_ROOT}/${NAME}/config"

echo "==> masking container avahi-daemon (host avahi-daemon owns mDNS)"
# Two avahi-daemons in the same network namespace fight over hostname
# claims.  We turn the container's off and rely on the host's via the
# nn-gw-mdns.service we install below.
chroot "${ROOTFS}" /bin/bash -eu -c '
    systemctl mask avahi-daemon.service avahi-daemon.socket 2>/dev/null || true
    rm -f /etc/systemd/system/multi-user.target.wants/avahi-daemon.service \
          /etc/systemd/system/sockets.target.wants/avahi-daemon.socket
' 2>/dev/null || true

echo "==> installing host-side mDNS publisher"
install -D -m 0755 "${SCRIPT_DIR}/nn-gw-mdns-publish" \
    /usr/local/sbin/nn-gw-mdns-publish
install -D -m 0644 "${SCRIPT_DIR}/nn-gw-mdns.service" \
    /etc/systemd/system/nn-gw-mdns.service
systemctl daemon-reload
systemctl enable nn-gw-mdns.service 2>/dev/null || true

echo "==> enabling lxc auto-start on host boot"
# lxc.start.auto = 1 is already in nn-gw.conf; just ensure the
# host-side service is enabled.
systemctl enable lxc.service 2>/dev/null || true

cat <<MSG

installed.

To start the container now:
    sudo lxc-start -n ${NAME}
    sudo lxc-ls -f
    sudo lxc-attach -n ${NAME} -- systemctl status nn-gw

To watch the daemon log:
    sudo lxc-attach -n ${NAME} -- journalctl -u nn-gw -f
MSG
