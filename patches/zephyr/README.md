# Patches to the vendored Zephyr tree

Apply to `device/third_party/zephyr` (base commit noted in each patch
header's context) with `git -C third_party/zephyr apply <patch>`.

* `0001-ieee802154-esp32-report-tx-failures-to-the-MAC.patch` -- the ESP32
  802.15.4 driver discarded the HAL's transmit error and returned 0, so
  OpenThread saw every failed frame (no ACK, CCA busy, abort) as delivered
  and never retried.  Maps NO_ACK/INVALID_ACK -> -ENOMSG, CCA_BUSY/COEXIST
  -> -EBUSY, the rest -> -EIO, which `modules/openthread/platform/radio.c`
  turns into NO_ACK / CHANNEL_ACCESS_FAILURE / ABORT, all retried.  Also
  keeps per-error counters (`nn_esp32_tx_outcome[]`, read by the sensor's
  `mesh status`).  Needed by BOTH the sensor app (mdns_ot_esp32c6) and the
  gateway radio firmware (ncp_esp32c6).  Measured 2026-09-24: with this
  fix delivery through a sensor's parent went 17-21/30 -> 25-27/30 on a
  busy channel.  Worth upstreaming.
