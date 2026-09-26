# gw_linux in an LXC container (RPi hub deployment)

A tiny LXC wrapper for the Linux gateway daemon so the hub-running Pi can
host one (or more) gateway daemons in isolation, started at boot and
restarted automatically on the daemon's post-commit `_exit(0)`.

## Layout

```
host/gw_linux/lxc/
  build-rootfs.sh        Create a fresh Debian Trixie LXC rootfs with
                         only the runtime deps gw_linux needs.
  install-gw_linux.sh    Copy the host-built `gw_linux` binary into
                         the rootfs and install the systemd unit.
  nn-gw.conf             LXC container config: host networking, bind-
                         mount /var/lib/nn-gw → /root/.local/state/nn-gw,
                         auto-start at host boot.
  nn-gw.service          Systemd unit run inside the container; supervises
                         `gw_linux provision-net` with Restart=always so
                         the daemon's `_exit(0)` after COMMIT trips a
                         restart and the persisted creds are picked up.
```

## Why host networking

mDNS multicast (`_nn-gw._tcp.local`) and the TCP listener on 8770 land
directly on the Pi's LAN interfaces with zero bridge fiddling.  The Pi
*is* the hub, so the container sharing the Pi's network namespace is
fine for this deployment.  Macvlan is the next step up if/when we want
multiple gateways per Pi or stricter isolation.

## Why bind-mount state

`gw_identity` + `hub_crypto` + `gw_provision` persist under
`$XDG_STATE_HOME/nn-gw/kv/` (file-per-key from
`fw_common/platform/linux/kvstore.c`).  Bind-mounting a host directory
into the container keeps that state outside the container rootfs, so
re-creating the container preserves the gateway identity and provisioning
blob.

## Usage

On the Pi, with the daemon binary already built at
`~/nn_project/build-gw_linux/gw_linux`:

```bash
sudo apt install -y lxc debootstrap
sudo ./build-rootfs.sh                  # one-time, ~3 min
sudo ./install-gw_linux.sh              # idempotent; re-run after rebuilds
sudo lxc-start -n nn-gw
sudo lxc-autostart -L                   # confirm it'll come up on boot
```

Inside the container the daemon publishes `_nn-gw._tcp.local` and
listens on `:8770` exactly like the bare-metal run.  From the hub:

```bash
nn-hub gateway new --transport net --ssid X --psk Y --hub-host Z
```

## Caveats

- Requires LXC ≥ 5.0 for `lxc.net.0.type = none` (host networking).
- The container does NOT include avahi-daemon; we use the host's
  avahi-daemon (visible because of host networking) by exec'ing
  `avahi-publish-service` from inside the container.  That requires the
  D-Bus socket `/var/run/dbus/system_bus_socket` to be bind-mounted in
  (the included `nn-gw.conf` does this).
- `apt install lxc` on Debian Trixie pulls in `lxc-templates`; the
  `debian` template needs `debootstrap`.
