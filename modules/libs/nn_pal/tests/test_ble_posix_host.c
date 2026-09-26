/* SPDX-License-Identifier: Apache-2.0 */
/*
 * nn_pal POSIX/BlueZ backend — host contract test.
 *
 * Scope is deliberately the part that can be wrong SILENTLY.  The D-Bus
 * plumbing fails loudly (bluetoothd rejects a malformed application), but
 * a UUID byte-order slip produces a service that registers happily and is
 * simply never found by the hub's wizard — the same shape of bug that
 * once cost this project a whole provisioning outage.  So pin the exact
 * text BlueZ will see for the real provisioning UUIDs.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <nn_osal/log.h>
#include <nn_pal/ble.h>

/* The backend logs; the host harness does not need the output.  Silent
 * stub so the test links without dragging in an osal backend. */
void nn_osal_log_emit(nn_osal_log_level_t level, const char *module,
                      const char *fmt, ...)
{
    (void)level; (void)module; (void)fmt;
}

/* Mirror of the backend's internal formatter — kept byte-identical to
 * uuid_to_str() in src/posix/ble.c.  If that changes, this test must be
 * updated deliberately, which is the point. */
static void fmt(const nn_pal_ble_uuid_t *u, char *out, size_t cap)
{
    const uint8_t *b = u->bytes;
    snprintf(out, cap,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void check_roundtrip(const char *s)
{
    nn_pal_ble_uuid_t u;
    char back[64];
    assert(nn_pal_ble_uuid_from_str(&u, s) == 0);
    fmt(&u, back, sizeof back);
    if (strcmp(back, s) != 0) {
        fprintf(stderr, "  UUID round-trip broke: %s -> %s\n", s, back);
        assert(0);
    }
}

int main(void)
{
    /* The provisioning service + characteristics the hub wizard looks for.
     * These strings are the contract; the ESP cameras advertise exactly
     * these, and a Linux camera must be indistinguishable. */
    check_roundtrip("e7f00001-6b3e-4f6b-9232-3e26d0d5a2f0"); /* service */
    check_roundtrip("e7f00003-6b3e-4f6b-9232-3e26d0d5a2f0"); /* status  */
    check_roundtrip("e7f00004-6b3e-4f6b-9232-3e26d0d5a2f0"); /* config  */
    check_roundtrip("e7f00005-6b3e-4f6b-9232-3e26d0d5a2f0"); /* devpub  */
    check_roundtrip("e7f00006-6b3e-4f6b-9232-3e26d0d5a2f0"); /* wifi    */
    check_roundtrip("e7f00007-6b3e-4f6b-9232-3e26d0d5a2f0"); /* fw name */

    /* First byte must be the most-significant one: the PAL header states
     * bytes[] is big-endian, and BlueZ takes the canonical text form.  A
     * backend that stored little-endian (as the Zephyr/raw-array style
     * does) would pass a round-trip but present a REVERSED uuid to the
     * hub, so assert the actual byte. */
    nn_pal_ble_uuid_t u;
    assert(nn_pal_ble_uuid_from_str(&u, "e7f00001-6b3e-4f6b-9232-3e26d0d5a2f0") == 0);
    assert(u.bytes[0] == 0xe7);
    assert(u.bytes[1] == 0xf0);
    assert(u.bytes[15] == 0xf0);

    /* Malformed input must be rejected, not silently half-parsed. */
    assert(nn_pal_ble_uuid_from_str(&u, "not-a-uuid") != 0);
    assert(nn_pal_ble_uuid_from_str(&u, "") != 0);
    assert(nn_pal_ble_uuid_from_str(NULL, "e7f00001-6b3e-4f6b-9232-3e26d0d5a2f0") != 0);

    printf("nn_pal posix BLE contract: OK\n");
    return 0;
}
