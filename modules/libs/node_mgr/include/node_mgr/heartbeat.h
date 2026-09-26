/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/*
 * node_mgr/heartbeat — explicit periodic D2H liveness frame.
 *
 * The hub side relies on a steady trickle of D2H traffic to keep its
 * `last_seen` field fresh.  Historically that trickle came from
 * incidental LOG_LINE / AUTO_EVENT telemetry — fine until we capped
 * the LOG_LINE forwarder at WRN.  Then a healthy idle sensor could
 * go minutes without emitting anything and the hub would think it
 * was offline.
 *
 * heartbeat_start() schedules a tiny D2H frame
 *   cmd:  NN_PROTO_CMD_DEVICE_HEARTBEAT (0x0005)
 *   body: [u32 LE uptime_ms]
 * every CONFIG_NODE_MGR_HEARTBEAT_PERIOD_MS (default 30000).  Fire and
 * forget — no hub reply expected.  Hub's existing _on_d2h handler
 * already calls touch_device for any D2H, so the heartbeat refreshes
 * last_seen without any new hub-side code.
 *
 * Returns 0 on success, negative errno on failure.  Idempotent; calling
 * heartbeat_start() multiple times is safe.
 */

int heartbeat_start(void);

/** Cancel the periodic heartbeat (used by test code; not normally needed). */
void heartbeat_stop(void);
