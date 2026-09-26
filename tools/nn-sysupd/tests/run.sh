#!/usr/bin/env bash
# nn-sysupd state-machine tests on a dev host.  Everything the agent touches
# is faked: the hub (curl), the U-Boot environment (fw_printenv/fw_setenv),
# the card (file-backed "partitions", a tar-backed mount), systemd, sysfs.
# The raw "ext4" stream is a tar archive so the fake mount can unpack it.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
AGENT="$HERE/../nn-sysupd"
T="$(mktemp -d "${TMPDIR:-/tmp}/nn-sysupd-test.XXXXXX")"
[ "${KEEP:-0}" = 1 ] && echo "keeping $T" || trap 'rm -rf "$T"' EXIT
FAKEBIN="$T/bin"; mkdir -p "$FAKEBIN"
PASS=0; FAIL=0
ok()   { PASS=$((PASS+1)); echo "  ok   $*"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL $*"; }
assert_eq() { [ "$2" = "$3" ] && ok "$1" || fail "$1: expected '$3', got '$2'"; }
assert_has() { grep -q -- "$2" "$3" 2>/dev/null && ok "$1" || fail "$1: '$2' not in $(basename "$3")"; }
assert_not_has() { grep -q -- "$2" "$3" 2>/dev/null && fail "$1: '$2' present in $(basename "$3")" || ok "$1"; }
SIG=6e6e6279

# ── fakes ────────────────────────────────────────────────────────────────────
cat > "$FAKEBIN/fw_printenv" <<'EOF'
#!/bin/sh
[ "$1" = -n ] && shift
sed -n "s/^$1=//p" "$T_UENV" 2>/dev/null | head -1
EOF
cat > "$FAKEBIN/fw_setenv" <<'EOF'
#!/bin/sh
echo "fw_setenv $*" >> "$T_CALLS"
[ "$1" = -s ] || { echo "fake fw_setenv: only -s supported" >&2; exit 2; }
touch "$T_UENV"
while read -r name value; do
    [ -n "$name" ] || continue
    grep -v "^$name=" "$T_UENV" > "$T_UENV.new" || true
    [ -n "$value" ] && echo "$name=$value" >> "$T_UENV.new"
    mv "$T_UENV.new" "$T_UENV"
    echo "env $name=$value" >> "$T_CALLS"
done < "$2"
EOF
cat > "$FAKEBIN/curl" <<'EOF'
#!/bin/sh
# minimal router: URL is the last http argument; -o file, -X method, -d data
url=""; out=""; method=GET; data=""
while [ $# -gt 0 ]; do
    case "$1" in
        -o) out="$2"; shift 2;; -X) method="$2"; shift 2;; -d) data="$2"; shift 2;;
        -H|-m) shift 2;; -C) shift 2;;
        http*) url="$1"; shift;; *) shift;;
    esac
done
echo "curl $method $url" >> "$T_CALLS"
case "$url" in
    */api/v1/firmware) cat "$T_HUB/firmware.json";;
    */api/v1/camera-slots) [ "$(cat "$T_HUB/reachable")" = 1 ] && echo '[]' || exit 7;;
    */api/v1/cameras/by-addr/*/bundle) echo "$method $url $data" >> "$T_REPORTS"; echo '{"ok":true}';;
    */gw_firmware/byai_system/*/meta) v=${url%/meta}; v=${v##*/}; [ -f "$T_HUB/meta-$v.json" ] && cat "$T_HUB/meta-$v.json" || exit 22;;
    */gw_firmware/byai_system/*) v=${url##*/}; [ -f "$T_HUB/img-$v.xz" ] || exit 22; cp "$T_HUB/img-$v.xz" "$out";;
    *) exit 22;;
esac
EOF
cat > "$FAKEBIN/blockdev" <<'EOF'
#!/bin/sh
stat -L -c %s "$2"
EOF
cat > "$FAKEBIN/e2fsck" <<'EOF'
#!/bin/sh
echo "e2fsck $*" >> "$T_CALLS"; exit "${FAKE_E2FSCK_RC:-0}"
EOF
cat > "$FAKEBIN/mount" <<'EOF'
#!/bin/sh
echo "mount $1 $2" >> "$T_CALLS"; echo "$1" > "$2.dev"
tar -xf "$1" -C "$2" 2>/dev/null || exit 32
EOF
cat > "$FAKEBIN/umount" <<'EOF'
#!/bin/sh
dev=$(cat "$1.dev"); echo "umount $1" >> "$T_CALLS"
tar -cf "$dev" -C "$1" . && rm -rf "$1"/* "$1"/.[!.]* "$1.dev" 2>/dev/null; true
EOF
cat > "$FAKEBIN/systemctl" <<'EOF'
#!/bin/sh
echo "systemctl $*" >> "$T_CALLS"
case "$1" in
    is-enabled) [ "$(cat "$T_SVC/enabled" 2>/dev/null)" = 1 ];;
    is-active)  [ "$(cat "$T_SVC/active" 2>/dev/null)" = 1 ];;
    show)       case "$*" in
                    *ConditionResult*) cat "$T_SVC/condition" 2>/dev/null || echo yes;;
                    *)                 cat "$T_SVC/active_since";;
                esac;;
    reboot)     echo REBOOT >> "$T_CALLS";;
esac
EOF
cat > "$FAKEBIN/btmgmt" <<'EOF'
#!/bin/sh
echo "BTMGMT CALLED" >> "$T_CALLS"; sleep 30; exit 1
EOF
cat > "$FAKEBIN/hciconfig" <<'EOF'
#!/bin/sh
[ -s "$T_HCICONFIG" ] && cat "$T_HCICONFIG" || exit 1
EOF
cat > "$FAKEBIN/ionice" <<'EOF'
#!/bin/sh
shift; exec "$@"
EOF
cat > "$FAKEBIN/nice" <<'EOF'
#!/bin/sh
shift 2; exec "$@"
EOF
chmod +x "$FAKEBIN"/*

# ── board fixture ────────────────────────────────────────────────────────────
export T_UENV="$T/uenv" T_CALLS="$T/calls.log" T_REPORTS="$T/reports.log" T_HUB="$T/hub" T_SVC="$T/svc" \
       T_BTMGMT="$T/btmgmt.out" T_HCICONFIG="$T/hciconfig.out"
mkdir -p "$T/dev/by-partuuid" "$T/sys/class/block" "$T/sys/devices/mmcblk1" "$T/kv" "$T/state" "$T/hub" \
         "$T/svc" "$T/bt/hci0" "$T/rproc" "$T/mnt" "$T/etc"
for n in 2 3; do
    truncate -s 8M "$T/dev/mmcblk1p$n"
    ln -sf "$T/dev/mmcblk1p$n" "$T/dev/by-partuuid/$SIG-0$n"
    mkdir -p "$T/sys/devices/mmcblk1/mmcblk1p$n"
    ln -sfn "$T/sys/devices/mmcblk1/mmcblk1p$n" "$T/sys/class/block/mmcblk1p$n"
done
printf 'hub.test' > "$T/kv/hub_host"; printf '\001' > "$T/kv/done"; printf 'BeagleY' > "$T/kv/name"
echo "console=ttyS2,115200n8 root=PARTUUID=$SIG-02 rootwait rw panic=10" > "$T/cmdline"
echo "120.00 300.00" > "$T/uptime"
echo "ab-v1" > "$T/layout"; echo "20260915-1" > "$T/sysver"; echo "2026.01" > "$T/bootchain"
echo "abcdef0123456789" > "$T/etc/machine-id"
echo "/dev/mmcblk1 0x100000 0x40000" > "$T/fw_env.config"; echo "/dev/mmcblk1 0x140000 0x40000" >> "$T/fw_env.config"
echo "10:CA:BF:DA:35:B7" > "$T/bt/hci0/address"
# the bare image runs NO remoteprocs and ships no expectations file; the
# camera image lists the ones that must run (T11 below)
echo 1 > "$T/svc/enabled"; echo 1 > "$T/svc/active"; echo 10000000 > "$T/svc/active_since"   # active since 10 s
echo 1 > "$T/hub/reachable"
echo '[]' > "$T/hub/firmware.json"

# a "system image": tar of a minimal rootfs, xz-compressed, with its meta
make_image() {  # make_image <version> [requires-layout] [boot_chain_min]
    local v="$1" lay="${2:-ab-v1}" bc="${3:-2026.01}"
    local d="$T/img-$v"
    rm -rf "$d"; mkdir -p "$d/etc" "$d/boot/extlinux"
    echo "$v" > "$d/etc/nn-system-version"; : > "$d/etc/machine-id"; echo kernel > "$d/boot/Image"
    printf 'default nn\nlabel nn\n   kernel /boot/Image\n   devicetreedir /boot/ti\nappend root=PARTUUID=%s-02 rootwait rw console=ttyS2,115200n8 panic=10\n' "$SIG" > "$d/boot/extlinux/extlinux.conf"
    tar -cf "$T/raw-$v.tar" -C "$d" .
    xz -c "$T/raw-$v.tar" > "$T/hub/img-$v.xz"
    local sha raw_sha size raw_size
    sha=$(sha256sum "$T/hub/img-$v.xz" | cut -d' ' -f1); size=$(stat -c %s "$T/hub/img-$v.xz")
    raw_sha=$(sha256sum "$T/raw-$v.tar" | cut -d' ' -f1); raw_size=$(stat -c %s "$T/raw-$v.tar")
    printf '{"type":"byai_system","version":"%s","size":%s,"sha256":"%s","format":"rootfs.ext4.xz","raw_size_bytes":%s,"raw_sha256":"%s","requires":{"layout":"%s","boot_chain_min":"%s"},"kernel":"6.12.57","git":"abc"}\n' \
        "$v" "$size" "$sha" "$raw_size" "$raw_sha" "$lay" "$bc" > "$T/hub/meta-$v.json"
}
set_target() { printf '[{"device_type":"byai_camera","target_version":"0.1.9"},{"device_type":"byai_system","target_version":"%s"}]\n' "$1" > "$T/hub/firmware.json"; }
slot_file() { case "$1" in a) echo "$T/dev/mmcblk1p2";; b) echo "$T/dev/mmcblk1p3";; esac; }
slot_read() {  # slot_read <slot> <path-in-slot>
    local x="$T/peek"; rm -rf "$x"; mkdir -p "$x"; tar -xf "$(slot_file "$1")" -C "$x" 2>/dev/null; cat "$x/$2" 2>/dev/null
}

run_agent() {
    : > "$T_CALLS"
    PATH="$FAKEBIN:$PATH" \
    NN_BYUUID="$T/dev/by-partuuid" NN_KV="$T/kv" NN_SYSUPD_STATE="$T/state" NN_LAYOUT_FILE="$T/layout" \
    NN_SYSVER_FILE="$T/sysver" NN_BOOTCHAIN_FILE="$T/bootchain" NN_MACHINE_ID="$T/etc/machine-id" \
    NN_FW_ENV_CONFIG="$T/fw_env.config" NN_CMDLINE="$T/cmdline" NN_UPTIME_FILE="$T/uptime" \
    NN_SYSBLOCK="$T/sys/class/block" NN_RPROC_DIR="$T/rproc" NN_BT_ADDR_GLOB="$T/bt/hci*/address" \
    NN_MNT="$T/mnt" NN_HEALTH_HOOKS="$T/health.d" NN_EXPECT_RPROC="$T/expect-rproc" NN_LOCK="$T/run/lock" \
    NN_EXPECT_CPUS="$T/expect-cpus" NN_SYSCPU="$T/syscpu" \
    sh "$AGENT" > "$T/out.log" 2>&1 && echo 0 > "$T/rc" || echo $? > "$T/rc"
    cat "$T/out.log" >> "$T/all.log"
}
rc() { cat "$T/rc"; }

echo "── T1 not provisioned: no hub contact, no writes"
printf '\000' > "$T/kv/done"; run_agent
assert_eq "T1 exit 0" "$(rc)" 0
assert_not_has "T1 no curl" "curl" "$T_CALLS"
printf '\001' > "$T/kv/done"

echo "── T2 no newer target: idle report only"
set_target 20260915-1; run_agent
assert_eq "T2 exit 0" "$(rc)" 0
assert_has "T2 idle report by BLE address" "by-addr/10:CA:BF:DA:35:B7/bundle" "$T_REPORTS"
assert_has "T2 report carries system+layout+slot" '"system":"20260915-1","layout":"ab-v1","slot":"a"' "$T_REPORTS"
assert_not_has "T2 nothing written" "fw_setenv" "$T_CALLS"

echo "── T3 newer target: stage into the idle slot and arm"
make_image 20260916-1; set_target 20260916-1; run_agent
assert_eq "T3 exit 0" "$(rc)" 0
assert_has "T3 downloaded" "curl GET http://hub.test:8770/gw_firmware/byai_system/20260916-1" "$T_CALLS"
assert_eq "T3 slot b holds the new version" "$(slot_read b etc/nn-system-version)" "20260916-1"
assert_has "T3 root= rewritten to slot b" "root=PARTUUID=$SIG-03" <(slot_read b boot/extlinux/extlinux.conf)
assert_eq "T3 machine-id copied" "$(slot_read b etc/machine-id)" "abcdef0123456789"
assert_has "T3 pending written" "to_slot=b" "$T/state/pending"
assert_has "T3 env: trial slot b" "env nn_slot_try=b" "$T_CALLS"
assert_has "T3 env: upgrade_available" "env upgrade_available=1" "$T_CALLS"
assert_has "T3 env: bootcount reset" "env bootcount=0" "$T_CALLS"
assert_has "T3 e2fsck ran on slot b" "e2fsck -fn $T/dev/by-partuuid/$SIG-03" "$T_CALLS"
assert_has "T3 reboot" "REBOOT" "$T_CALLS"
assert_has "T3 armed report" '"sysupd":"armed"' "$T_REPORTS"
[ -e "$T/state/dl/byai_system-20260916-1.xz" ] && fail "T3 download not removed" || ok "T3 download removed"
assert_eq "T3 slot a untouched" "$(stat -c %s "$(slot_file a)")" "$((8*1024*1024))"

echo "── T4 booted the trial slot, healthy: confirm"
echo "console=ttyS2 root=PARTUUID=$SIG-03 rootwait panic=10" > "$T/cmdline"; echo "20260916-1" > "$T/sysver"
echo "200.00 400.00" > "$T/uptime"; echo 100000000 > "$T/svc/active_since"   # camera active for 100 s
run_agent
assert_eq "T4 exit 0" "$(rc)" 0
assert_has "T4 env: nn_slot=b" "env nn_slot=b" "$T_CALLS"
assert_has "T4 env: try cleared" "env nn_slot_try=$" "$T_CALLS"
[ -f "$T/state/pending" ] && fail "T4 pending still present" || ok "T4 pending removed"
assert_eq "T4 confirmed marker" "$(cat "$T/state/confirmed")" "20260916-1"
assert_has "T4 confirmed report on slot b" '"system":"20260916-1","layout":"ab-v1","slot":"b","sysupd":"confirmed"' "$T_REPORTS"
assert_not_has "T4 no reboot" "REBOOT" "$T_CALLS"

echo "── T4b health not yet decidable (camera active 5 s): retry, no decision"
make_image 20260917-1; set_target 20260917-1; run_agent          # stages 20260917-1 into slot a
assert_has "T4b staged into slot a" "env nn_slot_try=a" "$T_CALLS"
echo "console=ttyS2 root=PARTUUID=$SIG-02 rootwait panic=10" > "$T/cmdline"; echo "20260917-1" > "$T/sysver"
echo "70.00 100.00" > "$T/uptime"; echo 65000000 > "$T/svc/active_since"
run_agent
assert_eq "T4b exit 0" "$(rc)" 0
assert_has "T4b waits" "not yet decidable" "$T/out.log"
[ -f "$T/state/pending" ] && ok "T4b pending kept" || fail "T4b pending lost"
assert_not_has "T4b no env change" "fw_setenv" "$T_CALLS"

echo "── T5 trial slot unhealthy (camera dead): blacklist, clear try, reboot back"
echo "200.00 400.00" > "$T/uptime"; echo 0 > "$T/svc/active"; run_agent
assert_eq "T5 exit 0" "$(rc)" 0
assert_has "T5 blacklisted" "20260917-1" "$T/state/blocked"
assert_has "T5 env: try cleared" "env nn_slot_try=$" "$T_CALLS"
assert_has "T5 env: upgrade_available=0" "env upgrade_available=0" "$T_CALLS"
assert_not_has "T5 nn_slot NOT moved" "env nn_slot=a" "$T_CALLS"
assert_has "T5 reboot back" "REBOOT" "$T_CALLS"
assert_has "T5 rolled-back report" '"sysupd":"rolled-back"' "$T_REPORTS"
echo 1 > "$T/svc/active"

echo "── T5b blacklisted target is not retried"
echo "console=ttyS2 root=PARTUUID=$SIG-03 rootwait panic=10" > "$T/cmdline"; echo "20260916-1" > "$T/sysver"; run_agent
assert_has "T5b skipped" "previously failed health" "$T/out.log"
assert_not_has "T5b no download" "gw_firmware/byai_system/20260917-1\$" "$T_CALLS"

echo "── T6 U-Boot fell back by boot count (nn_rollback=1): blacklist the pending version"
make_image 20260918-1; set_target 20260918-1; run_agent
assert_has "T6 staged" "env nn_slot_try=a" "$T_CALLS"
echo "nn_rollback=1" >> "$T/uenv"          # altbootcmd ran; we are back on slot b
run_agent
assert_eq "T6 exit 0" "$(rc)" 0
assert_has "T6 blacklisted 20260918-1" "20260918-1" "$T/state/blocked"
assert_has "T6 flag cleared" "env nn_rollback=$" "$T_CALLS"
[ -f "$T/state/pending" ] && fail "T6 pending left" || ok "T6 pending removed"
assert_has "T6 rolled-back report" '"sysupd":"rolled-back"' "$T_REPORTS"

echo "── T7 gates: layout mismatch, boot chain too old, wrong format"
make_image 20260919-1 ab-v2; set_target 20260919-1; run_agent
assert_has "T7a layout refused" "requires layout 'ab-v2'" "$T/out.log"
assert_not_has "T7a no download" "gw_firmware/byai_system/20260919-1\$" "$T_CALLS"
make_image 20260919-2 ab-v1 2027.01; set_target 20260919-2; run_agent
assert_has "T7b boot chain refused" "requires boot chain >= 2027.01, board has 2026.01" "$T/out.log"
make_image 20260919-3; sed -i 's/rootfs.ext4.xz/tar.gz/' "$T/hub/meta-20260919-3.json"; set_target 20260919-3; run_agent
assert_has "T7c format refused" "format 'tar.gz'" "$T/out.log"
assert_not_has "T7 no env change" "fw_setenv" "$T_CALLS"

echo "── T8 fw_env.config names another disk: refuse before any env write"
echo "/dev/mmcblk0 0x100000 0x40000" > "$T/fw_env.config"; make_image 20260920-1; set_target 20260920-1; run_agent
assert_eq "T8 exit 1" "$(rc)" 1
assert_has "T8 reason" "names /dev/mmcblk0 but the booted root is on /dev/mmcblk1" "$T/out.log"
assert_not_has "T8 no env write" "fw_setenv" "$T_CALLS"
assert_not_has "T8 no download" "gw_firmware" "$T_CALLS"
printf '/dev/mmcblk1 0x100000 0x40000\n/dev/mmcblk1 0x140000 0x40000\n' > "$T/fw_env.config"

echo "── T9 corrupt download: nothing written, download discarded"
make_image 20260921-1; set_target 20260921-1
printf 'garbage' >> "$T/hub/img-20260921-1.xz"      # digest no longer matches meta
before=$(sha256sum "$(slot_file a)" | cut -d' ' -f1); run_agent
assert_has "T9 mismatch detected" "download digest mismatch" "$T/out.log"
assert_eq "T9 idle slot a untouched" "$(sha256sum "$(slot_file a)" | cut -d' ' -f1)" "$before"
assert_not_has "T9 no env write" "fw_setenv" "$T_CALLS"
[ -e "$T/state/dl/byai_system-20260921-1.xz" ] && fail "T9 download kept" || ok "T9 download discarded"

echo "── T10 slot read-back mismatch (image raw digest wrong): slot left unarmed"
make_image 20260922-1; sed -i 's/"raw_sha256":"[0-9a-f]*"/"raw_sha256":"'"$(printf 'f%.0s' $(seq 64))"'"/' "$T/hub/meta-20260922-1.json"; set_target 20260922-1; run_agent
assert_has "T10 read-back mismatch" "read-back digest mismatch" "$T/out.log"
assert_not_has "T10 no env write" "fw_setenv" "$T_CALLS"
[ -f "$T/state/pending" ] && fail "T10 pending written" || ok "T10 no pending"

echo "── T11 remoteproc expectations come from the image"
# camera image: three named cores must run; one is absent -> unhealthy
printf 'c7x-dsp0\nc7x-dsp1\nmain-r5f0\n' > "$T/expect-rproc"
for i in 0 1; do mkdir -p "$T/rproc/remoteproc$i"; echo running > "$T/rproc/remoteproc$i/state"; done
echo c7x-dsp0 > "$T/rproc/remoteproc0/name"; echo c7x-dsp1 > "$T/rproc/remoteproc1/name"
echo 'version=20260930-1
from_slot=a
to_slot=b' > "$T/state/pending"; echo 1 > "$T/svc/active"; echo 100000000 > "$T/svc/active_since"
echo "console=ttyS2 root=PARTUUID=$SIG-03 rootwait panic=10" > "$T/cmdline"; echo "20260930-1" > "$T/sysver"
run_agent
assert_has "T11a absent expected core fails health" "remoteproc main-r5f0 is 'absent'" "$T/out.log"
assert_has "T11a rolled back" "20260930-1" "$T/state/blocked"
# all three present; one is "attached" (started by the boot chain, Linux only attached) -> confirm
mkdir -p "$T/rproc/remoteproc2"; echo main-r5f0 > "$T/rproc/remoteproc2/name"; echo attached > "$T/rproc/remoteproc2/state"
mkdir -p "$T/rproc/remoteproc3"; echo other-core > "$T/rproc/remoteproc3/name"; echo offline > "$T/rproc/remoteproc3/state"   # unlisted offline is fine
echo 'version=20260930-2
from_slot=a
to_slot=b' > "$T/state/pending"; echo "20260930-2" > "$T/sysver"; run_agent
assert_has "T11b listed cores running/attached, unlisted offline: confirmed" "CONFIRMED 20260930-2" "$T/out.log"
# a crashed core fails even when it is not listed
echo 'version=20260930-3
from_slot=a
to_slot=b' > "$T/state/pending"; echo "20260930-3" > "$T/sysver"
mkdir -p "$T/rproc/remoteproc7"; echo extra-core > "$T/rproc/remoteproc7/name"; echo crashed > "$T/rproc/remoteproc7/state"; run_agent
assert_has "T11c crashed core fails health" "extra-core crashed" "$T/out.log"
rm -rf "$T/rproc/remoteproc7"
# an expectation may name the FIRMWARE instead of the node address.  name/ is
# the DT node address (7e000000.dsp) and says nothing about which core it is;
# firmware/ is the DT firmware-name (j722s-c71_0-fw), which is what an image
# actually ships -- so an image must be able to express the expectation that
# way without it silently matching nothing.
printf 'j722s-c71_0-fw\n' > "$T/expect-rproc"
echo j722s-c71_0-fw > "$T/rproc/remoteproc0/firmware"
echo 'version=20260930-3b
from_slot=a
to_slot=b' > "$T/state/pending"; echo "20260930-3b" > "$T/sysver"; run_agent
assert_has "T11c2 expectation by firmware name matches" "CONFIRMED 20260930-3b" "$T/out.log"
# ...and a firmware name nothing provides is still absent, not a free pass
printf 'j722s-nosuch-fw\n' > "$T/expect-rproc"
echo 'version=20260930-3c
from_slot=a
to_slot=b' > "$T/state/pending"; echo "20260930-3c" > "$T/sysver"; run_agent
assert_has "T11c3 unknown firmware name is absent" "remoteproc j722s-nosuch-fw is 'absent'" "$T/out.log"
printf 'c7x-dsp0\nc7x-dsp1\nmain-r5f0\n' > "$T/expect-rproc"
# bare image: no expectations file, no remoteprocs at all -> health passes
rm -f "$T/expect-rproc"; rm -rf "$T/rproc"/*
echo 'version=20260930-4
from_slot=a
to_slot=b' > "$T/state/pending"; echo "20260930-4" > "$T/sysver"; run_agent
assert_has "T11d bare image with zero remoteprocs confirms" "CONFIRMED 20260930-4" "$T/out.log"

echo "── T12 BLE address sources: sysfs missing (6.12 + cc33xx) -> hciconfig -> cache; btmgmt never called"
echo "console=ttyS2 root=PARTUUID=$SIG-03 rootwait panic=10" > "$T/cmdline"; echo "20260930-4" > "$T/sysver"
rm -f "$T/state/pending" "$T/state/ble_addr"; rm -rf "$T/bt/hci0"; : > "$T_REPORTS"; set_target 20260930-4
: > "$T_HCICONFIG"; run_agent
assert_has "T12a no source at all: skipped with retry note" "no BLE address yet" "$T/out.log"
assert_not_has "T12a btmgmt never invoked" "BTMGMT CALLED" "$T_CALLS"
printf 'hci0:\tType: Primary  Bus: UART\n\tBD Address: 10:ca:bf:da:35:b7  ACL MTU: 27:7  SCO MTU: 0:0\n\tDOWN\n' > "$T_HCICONFIG"; run_agent
assert_has "T12b hciconfig address used, upper-cased" "by-addr/10:CA:BF:DA:35:B7/bundle" "$T_REPORTS"
assert_eq "T12b cached on nn-data" "$(cat "$T/state/ble_addr")" "10:CA:BF:DA:35:B7"
assert_not_has "T12b btmgmt never invoked" "BTMGMT CALLED" "$T_CALLS"
: > "$T_HCICONFIG"; : > "$T_REPORTS"; run_agent
assert_has "T12c cache serves the address when the adapter is gone" "by-addr/10:CA:BF:DA:35:B7/bundle" "$T_REPORTS"
rm -f "$T/state/ble_addr"; : > "$T_REPORTS"; printf 'aa:bb:cc:dd:ee:01' > "$T/kv/ble_addr"; run_agent
assert_has "T12d KV field from the provisioning daemon wins" "by-addr/AA:BB:CC:DD:EE:01/bundle" "$T_REPORTS"
rm -f "$T/kv/ble_addr"

echo "── T13 single-instance lock: stale lock taken over, live lock respected, legacy nn-data lock removed"
mkdir -p "$T/run/lock"; echo 999999999 > "$T/run/lock/pid"          # dead pid
mkdir -p "$T/state/lock"                                             # what a pre-fix agent left on nn-data
: > "$T_REPORTS"; run_agent
assert_has "T13a stale lock taken over" "stale lock (pid 999999999 gone) — taking over" "$T/out.log"
assert_has "T13a run proceeded (report sent)" "by-addr/" "$T_REPORTS"
[ -d "$T/state/lock" ] && fail "T13a legacy lock on nn-data not removed" || ok "T13a legacy lock on nn-data removed"
[ -d "$T/run/lock" ] && fail "T13a lock not released on exit" || ok "T13a lock released on exit"
mkdir -p "$T/run/lock"; echo $$ > "$T/run/lock/pid"                 # a live pid (this test shell)
: > "$T_REPORTS"; run_agent
assert_has "T13b live lock respected" "another run (pid $$) holds the lock" "$T/out.log"
assert_not_has "T13b no report while locked" "by-addr/" "$T_REPORTS"
rm -rf "$T/run/lock"

echo "── T14 CPU count from the image: three cores lost fails health; all online confirms; no file = no check"
echo "console=ttyS2 root=PARTUUID=$SIG-03 rootwait panic=10" > "$T/cmdline"; echo "20260930-5" > "$T/sysver"
rm -f "$T/expect-rproc" "$T/run/lock"; rm -rf "$T/rproc"/* "$T/syscpu"; mkdir -p "$T/syscpu/cpu0" "$T/syscpu/cpu1" "$T/syscpu/cpu2" "$T/syscpu/cpu3"
for i in 1 2 3; do echo 0 > "$T/syscpu/cpu$i/online"; done          # "CPUx: failed to come online"
echo 4 > "$T/expect-cpus"
printf 'version=20260930-5\nfrom_slot=a\nto_slot=b\n' > "$T/state/pending"; run_agent
assert_has "T14a single-core board fails health" "health: 1 CPUs online, image expects 4" "$T/out.log"
assert_has "T14a rolled back" "20260930-5" "$T/state/blocked"
for i in 1 2 3; do echo 1 > "$T/syscpu/cpu$i/online"; done
printf 'version=20260930-6\nfrom_slot=a\nto_slot=b\n' > "$T/state/pending"; echo "20260930-6" > "$T/sysver"; run_agent
assert_has "T14b four cores online confirms" "CONFIRMED 20260930-6" "$T/out.log"
rm -f "$T/expect-cpus"; for i in 1 2 3; do echo 0 > "$T/syscpu/cpu$i/online"; done
printf 'version=20260930-7\nfrom_slot=a\nto_slot=b\n' > "$T/state/pending"; echo "20260930-7" > "$T/sysver"; run_agent
assert_has "T14c no expectations file: no CPU check" "CONFIRMED 20260930-7" "$T/out.log"

echo "── T15 a hub that is briefly unreachable does NOT condemn the trial"
# A camera roams between APs; a roam outlasts one 8 s probe.  Treating that as
# "the image is bad" threw away a good update on hardware (2026-09-16): the
# trial was associated and downloading happily, but the one probe landed in a
# gap, so the slot rolled back AND the version was blacklisted.  The real
# failure -- an image that can never reach the hub -- is still caught, by
# nn-trial-guard's deadline rather than by the first probe.
rm -f "$T/expect-rproc" "$T/expect-cpus" "$T/run/lock"; rm -rf "$T/rproc"/*
for i in 1 2 3; do echo 1 > "$T/syscpu/cpu$i/online"; done
echo "console=ttyS2 root=PARTUUID=$SIG-03 rootwait panic=10" > "$T/cmdline"
echo "20260930-8" > "$T/sysver"
printf 'version=20260930-8\nfrom_slot=a\nto_slot=b\n' > "$T/state/pending"
echo 0 > "$T/hub/reachable"; run_agent
assert_has "T15a unreachable hub is not yet decidable" "health: hub unreachable" "$T/out.log"
assert_has "T15a retried, not condemned" "not yet decidable, will retry" "$T/out.log"
grep -q '20260930-8' "$T/state/blocked" 2>/dev/null \
    && { echo "  FAIL T15a blacklisted a trial over one missed poll"; FAIL=$((FAIL+1)); } \
    || { echo "  ok   T15a not blacklisted"; PASS=$((PASS+1)); }
[ -f "$T/state/pending" ] \
    && { echo "  ok   T15a trial still pending"; PASS=$((PASS+1)); } \
    || { echo "  FAIL T15a trial was dropped"; FAIL=$((FAIL+1)); }
# ...and once the hub answers again, the very same trial confirms.
echo 1 > "$T/hub/reachable"; rm -f "$T/run/lock"; run_agent
assert_has "T15b same trial confirms when the hub returns" "CONFIRMED 20260930-8" "$T/out.log"

echo "── T16 a camera unit skipped by its own condition is not judged"
# The image ships nn-camera.service; the app it runs is installed separately
# on nn-data.  Enabled + inactive + ConditionResult=no means "no app yet", and
# must not roll back an otherwise healthy image.
echo 1 > "$T/hub/reachable"; rm -f "$T/run/lock"
echo 1 > "$T/svc/enabled"; echo 0 > "$T/svc/active"; echo no > "$T/svc/condition"
echo "20260930-9" > "$T/sysver"
printf 'version=20260930-9\nfrom_slot=a\nto_slot=b\n' > "$T/state/pending"; run_agent
assert_has "T16a condition-skipped unit is not judged" "skipped by its own condition" "$T/out.log"
assert_has "T16a and the image confirms" "CONFIRMED 20260930-9" "$T/out.log"
# ...but the same unit with its condition MET and not running is still a fault.
echo yes > "$T/svc/condition"; rm -f "$T/run/lock"
echo "20260930-10" > "$T/sysver"
printf 'version=20260930-10\nfrom_slot=a\nto_slot=b\n' > "$T/state/pending"; run_agent
assert_has "T16b condition met + inactive still fails health" "not active" "$T/out.log"
echo 1 > "$T/svc/active"; rm -f "$T/svc/condition"

echo
echo "nn-sysupd tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
