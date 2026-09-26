# nn-sysupd — whole-system A/B updater for the BeagleY Buildroot image

Design: *byai System OTA* (2026-09-15). Layout `ab-v1`: MBR, disk signature
`6e6e6279`, partitions `-01` boot FAT (boot chain only), `-02` rootfs_a,
`-03` rootfs_b, `-04` nn-data. Each root slot carries its own kernel, device
trees and modules under `/boot`; one artifact `byai_system-<ver>.rootfs.ext4.xz`
is one coherent system. U-Boot picks the slot from `nn_slot` / `nn_slot_try`
and falls back by boot count (`bootlimit=3`, `altbootcmd` sets `nn_rollback=1`).

## What one run does

| state on entry | action |
|---|---|
| `nn_rollback=1` in the U-Boot env | U-Boot already fell back: blacklist `pending.version`, clear the flag, report |
| `pending` file on nn-data | health gate on the trial slot → confirm (`nn_slot=<try>`), or blacklist + clear `nn_slot_try` + reboot back |
| neither | poll `GET /api/v1/firmware` for `byai_system`; gate on `requires.layout`, `boot_chain_min`, format, size; download (resumable) and check `sha256`; stream `xz -dc | dd` into the idle slot; read the slot back and check `raw_sha256`; `e2fsck -fn`; mount once: version file, `/boot/Image`, rewrite `root=PARTUUID=` to the slot's own id, copy `/etc/machine-id`; write `pending`; `fw_setenv nn_slot_try=<idle> upgrade_available=1 bootcount=0`; reboot |

The trial slot is armed only after its content has been verified **on the
card**, and `pending` lives on nn-data, so a power cut at any point leaves a
board that boots the good slot and an agent that resumes on the next tick.
The single-instance lock lives in `/run/nn-sysupd` (tmpfs, gone on every
boot) and records the holder's pid, so a crash mid-run can never silence the
next boot's ticks — the first version kept it on nn-data and did exactly that.

## Health gate (trial slot)

Uptime ≥ 60 s (else "not yet decidable", retried next tick); provisioning KV
present; no remoteproc `crashed`, and every name listed in
`/etc/nn-sysupd/expect-remoteprocs` (shipped by the image that carries the
firmware — the bare image ships none; all its cores are `offline`) is `running`
or `attached` (a core the boot chain started, e.g. the DM R5); at least the
number of CPUs in `/etc/nn-sysupd/expect-cpus` online (a trial U-Boot once left
the board single-core and the slot still confirmed); hub reachable; when
`nn-camera.service` is enabled it must be active for ≥ 60 s; every executable
in `/etc/nn-sysupd/health.d/` must exit 0.

## Reporting

`PUT /api/v1/cameras/by-addr/<BLE addr>/bundle` with `system`, `layout`,
`slot`, `sysupd` (idle / armed / confirmed / rolled-back). The board does not
know its slot id; the hub maps the address it bound at provisioning. The
address comes from, in order: the provisioning KV field `ble_addr` (when the
setup daemon writes it), `/sys/class/bluetooth/hci*/address` (absent on the
6.12 + cc33xx kernel), `hciconfig hci0` (deprecated BlueZ tool, kept in the
image; exits at once without a tty), then a cache on nn-data written by the
first success — so a late hci0 or a powered-off adapter costs at most the
very first report. `btmgmt` is deliberately not used: without a tty it either
prints nothing or never exits, and the image has no `timeout(1)`.

## Packaging (image side)

* install `nn-sysupd` → `/usr/sbin/nn-sysupd` (0755), the two units →
  `/usr/lib/systemd/system/`, enable `nn-sysupd.timer`;
* `/etc/fw_env.config` for the redundant env at `0x100000` / `0x140000`
  (0x40000 each) on the SD; the agent refuses to run `fw_setenv` unless that
  device is the disk holding the booted root;
* mount the boot FAT read-only at `/boot-fat` (fstab) and have the image build
  write the boot-chain version into `boot.vfat:/nn-boot-chain` — that is what
  `requires.boot_chain_min` is compared against;
* `/etc/nn-layout` = `ab-v1`, `/etc/nn-system-version` = image version,
  `/etc/machine-id` shipped empty;
* tools: curl, xz, e2fsprogs (e2fsck), util-linux (blockdev, mount, ionice),
  uboot-tools (fw_printenv, fw_setenv), sha256sum/dd/sed/od from busybox or
  coreutils. `nice`/`ionice` are optional.

## Tests

`tests/run.sh` runs the whole state machine on a dev host against fakes
(fake hub via `curl`, fake `fw_setenv`/`fw_printenv`, file-backed "partitions",
a tar-backed fake `mount`). Wired into `modules/tests/run_host_tests.sh`.
Every path and device the script touches is an `NN_*` override.
