/* SPDX-License-Identifier: Apache-2.0 */
/* Fake NimBLE for host-side nn_pal contract tests.
 *
 * This is not a mock of NimBLE's code — it is a written-down statement of
 * the INTERFACE PATTERNS the ESP backend assumes, so a test can fail when
 * the PAL violates one of them.  The patterns encoded here (each learned
 * the hard way on hardware):
 *
 *   P1  The first connection gets conn handle 0.  0 is VALID.
 *   P2  ble_gatts_add_svcs() only queues — val_handles stay 0 until the
 *       host registers services on start, just before sync_cb fires.
 *   P3  A notify on attr handle 0 is accepted and silently dropped.
 *   P4  Every conn-handle argument NimBLE receives must be the RAW NimBLE
 *       handle — a biased/wrong handle gets BLE_HS_ENOTCONN, not a crash.
 *   P5  ble_uuid128_t.value is LSB-first; the PAL's spec-order bytes must
 *       arrive reversed, and NimBLE keeps only the pointers it was given
 *       until start, so the PAL must own that storage.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct ble_gatt_svc_def;

/* — test control — */
void fake_nimble_reset(void);
/* Assign val_handles to every queued service (P2) and fire sync_cb. */
void fake_nimble_go_sync(void);
/* Deliver a GAP connect/disconnect to the callback ble_gap_adv_start saved.
 * The handle handed out is always 0: P1. */
void fake_nimble_connect(void);
void fake_nimble_disconnect(int reason);
void fake_nimble_mtu(uint16_t mtu);

/* — recorded state the tests assert on — */
typedef struct {
    uint16_t conn;
    uint16_t attr;
    uint8_t  data[600];
    size_t   len;
} fake_notify_rec_t;

extern fake_notify_rec_t fake_notifies[8];
extern unsigned          fake_notify_count;
extern unsigned          fake_notify_dropped;    /* attr==0 arrivals (P3) */
extern int               fake_last_terminate;    /* handle or -1 */
extern const struct ble_gatt_svc_def *fake_svcs; /* what add_svcs queued */
extern bool              fake_adv_running;

/* access dispatch, captured at registration so tests can drive reads/writes */
uint16_t fake_attr_handle_of(unsigned chr_index);
int fake_drive_read(unsigned chr_index, uint16_t conn,
                    uint8_t *out, size_t cap, size_t *outlen);
int fake_drive_write(unsigned chr_index, uint16_t conn,
                     const void *data, size_t len);
