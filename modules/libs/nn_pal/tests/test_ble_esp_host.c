/* SPDX-License-Identifier: Apache-2.0 */
/* Host-side contract test for the ESP-IDF (NimBLE) nn_pal BLE backend.
 *
 * Compile + run on Linux (or use modules/tests/run_host_tests.sh):
 *
 *     gcc -std=c11 -Wall -Wextra -O0 -g \
 *         -DCONFIG_NN_PAL_BACKEND_ESP_IDF \
 *         -I tests/fake_nimble -I include \
 *         tests/test_ble_esp_host.c tests/fake_nimble/fake_nimble.c \
 *         src/esp_idf/ble.c \
 *         -o /tmp/nn_pal_ble_test && /tmp/nn_pal_ble_test
 *
 * The fake (tests/fake_nimble/) is a written-down statement of the NimBLE
 * interface patterns the backend assumes; each test pins one PAL obligation
 * against them.  Two of these obligations were real field defects:
 * handle-0-is-valid (every provisioning job UNCONFIRMED, 2026-08-21) and
 * handles-published-only-at-sync (notify to attr 0, silently dropped).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <nn_pal/ble.h>
#include "fake_nimble.h"
#include "host/ble_hs.h"

/* — captured PAL callbacks — */
static nn_pal_ble_conn_t last_conn;
static int conn_events, disc_events, mtu_events;
static uint16_t last_mtu;

static void conn_cb(nn_pal_ble_conn_t conn, nn_pal_ble_conn_event_t ev,
                    const nn_pal_ble_conn_info_t *info, void *user)
{
    (void)user;
    last_conn = conn;
    if (ev == NN_PAL_BLE_CONN_EV_CONNECTED)    conn_events++;
    if (ev == NN_PAL_BLE_CONN_EV_DISCONNECTED) disc_events++;
    if (ev == NN_PAL_BLE_CONN_EV_MTU_UPDATED) { mtu_events++; last_mtu = info->mtu; }
}

static const char READ_VAL[] = "status:idle";
static uint8_t written[600]; static size_t written_len;
static nn_pal_ble_conn_t rw_seen_conn;

static int on_read(const nn_pal_ble_read_ctx_t *ctx, void *user,
                   uint8_t *buf, size_t cap, size_t *outlen)
{
    (void)user;
    rw_seen_conn = ctx->conn;
    if (sizeof READ_VAL - 1 > cap) return -1;
    memcpy(buf, READ_VAL, sizeof READ_VAL - 1);
    *outlen = sizeof READ_VAL - 1;
    return 0;
}
static int on_write(const nn_pal_ble_write_ctx_t *ctx, void *user,
                    const uint8_t *data, size_t len)
{
    (void)user;
    rw_seen_conn = ctx->conn;
    memcpy(written, data, len);
    written_len = len;
    return 0;
}

int main(void)
{
    fake_nimble_reset();

    /* — UUID parsing: spec-order string → 16 big-endian bytes — */
    nn_pal_ble_uuid_t u;
    assert(nn_pal_ble_uuid_from_str(&u,
        "e7f00001-0203-0405-0607-08090a0b0c0d") == 0);
    assert(u.bytes[0] == 0xe7 && u.bytes[15] == 0x0d);
    assert(nn_pal_ble_uuid_from_str(&u, "not-a-uuid") == -EINVAL);

    assert(nn_pal_ble_init() == 0);
    assert(ble_hs_cfg.sync_cb != NULL);       /* backend must hook sync */
    assert(nn_pal_ble_conn_cb_register(conn_cb, NULL) == 0);

    /* — register a service from CALLER-OWNED, SHORT-LIVED storage —
     * The Zephyr outage of 2026-08-11 was exactly this pattern violated
     * (compound-literal UUIDs died with the stack frame).  The chrc array
     * must stay alive (the PAL keeps `handle` in it), but the UUID bytes
     * are clobbered after registration to prove the backend copied them. */
    static nn_pal_ble_chrc_t chrcs[2];
    nn_pal_ble_uuid_t tmp_uuid;
    nn_pal_ble_uuid_from_str(&tmp_uuid, "e7f00001-0000-0000-0000-000000000001");
    chrcs[0] = (nn_pal_ble_chrc_t){ .uuid = tmp_uuid,
        .flags = NN_PAL_BLE_CHRC_READ | NN_PAL_BLE_CHRC_WRITE,
        .on_read = on_read, .on_write = on_write };
    nn_pal_ble_uuid_from_str(&chrcs[1].uuid,
        "e7f00002-0000-0000-0000-000000000002");
    chrcs[1].flags = NN_PAL_BLE_CHRC_NOTIFY;

    static nn_pal_ble_service_t svc;
    nn_pal_ble_uuid_from_str(&svc.uuid, "e7f00000-0000-0000-0000-00000000000a");
    svc.chrcs = chrcs;
    svc.chrc_count = 2;
    assert(nn_pal_ble_gatt_register_service(&svc) == 0);
    memset(&tmp_uuid, 0xAA, sizeof tmp_uuid);   /* clobber caller storage */

    /* P2: before sync the handles must still be unpublished... */
    assert(chrcs[0].handle == 0 && chrcs[1].handle == 0);
    /* ...and a notify at this stage must be REFUSED loudly by the PAL
     * (-EINVAL), never silently swallowed by the stack (the UNCONFIRMED
     * class of bug).  conn 3 is arbitrary-but-nonzero. */
    assert(nn_pal_ble_gatt_notify(3, chrcs[1].handle, "x", 1) == -EINVAL);
    assert(fake_notify_dropped == 0);           /* it never reached NimBLE */

    assert(nn_pal_ble_advertise_start(&(nn_pal_ble_adv_params_t){
        .service_uuid = &svc.uuid }) == 0);

    fake_nimble_go_sync();

    /* P2: handles published at sync; P5: UUID bytes arrived LSB-first and
     * survived the caller clobbering its copy. */
    assert(chrcs[0].handle != 0 && chrcs[1].handle != 0);
    assert(chrcs[0].handle == fake_attr_handle_of(0));
    assert(fake_svcs != NULL);
    const ble_uuid128_t *reg0 =
        (const ble_uuid128_t *)fake_svcs[0].characteristics[0].uuid;
    assert(reg0->value[15] == 0xe7 && reg0->value[0] == 0x01);
    assert(fake_adv_running);                   /* sync re-armed advertising */

    /* P1: first connection is NimBLE handle 0 → PAL conn must be VALID
     * (nonzero), because the PAL contract says 0 = "no connection". */
    fake_nimble_connect();
    assert(conn_events == 1);
    assert(last_conn != 0);
    nn_pal_ble_conn_t c = last_conn;

    /* notify round-trip: PAL conn → raw NimBLE handle 0 */
    assert(nn_pal_ble_gatt_notify(c, chrcs[1].handle, "APPLYING", 8) == 0);
    assert(fake_notify_count == 1);
    assert(fake_notifies[0].conn == 0);         /* unbiased before NimBLE */
    assert(fake_notifies[0].attr == chrcs[1].handle);
    assert(memcmp(fake_notifies[0].data, "APPLYING", 8) == 0);

    /* PAL-side guards */
    assert(nn_pal_ble_gatt_notify(0, chrcs[1].handle, "x", 1) == -ENOTCONN);
    assert(nn_pal_ble_gatt_notify(c, 0, "x", 1) == -EINVAL);
    assert(fake_notify_count == 1 && fake_notify_dropped == 0);

    /* read/write contexts carry the biased conn too */
    uint8_t rbuf[64]; size_t rlen = 0;
    assert(fake_drive_read(0, 0, rbuf, sizeof rbuf, &rlen) == 0);
    assert(rlen == sizeof READ_VAL - 1 && memcmp(rbuf, READ_VAL, rlen) == 0);
    assert(rw_seen_conn == c);
    assert(fake_drive_write(0, 0, "cfg=1", 5) == 0);
    assert(written_len == 5 && memcmp(written, "cfg=1", 5) == 0);
    assert(rw_seen_conn == c);

    /* MTU event: NimBLE handle 0 in → biased conn out */
    fake_nimble_mtu(247);
    assert(mtu_events == 1 && last_mtu == 247 && last_conn == c);

    /* P4: disconnect must hand NimBLE the RAW handle (0), not the biased
     * one.  This is the exact mistake the bias macros exist to prevent —
     * and the one call site that skipped them until this test. */
    assert(nn_pal_ble_conn_disconnect(c) == 0);
    assert(fake_last_terminate == 0);
    assert(nn_pal_ble_conn_disconnect(0) == -ENOTCONN);

    fake_nimble_disconnect(0x13);
    assert(disc_events == 1);
    assert(fake_adv_running);                   /* peripheral re-advertises */

    printf("nn_pal esp_idf/ble: all contract tests passed\n");
    return 0;
}
