/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * nn_ota_p4 — P4-side OTA receiver.  The P4 is the lifecycle SLAVE: the C6
 * streams the P4's new image over nn_link and drives staging + swap.  The P4
 * writes chunks into its inactive OTA slot, hashes it on request, self-applies
 * (set_boot + restart) on command, and reports its running version.
 *
 * Integration: the app routes nn_link packets whose first byte is
 * NN_OTA_MAGIC to nn_ota_p4_on_msg(); call nn_ota_p4_init() once after the
 * link rx callback is set so the P4 announces its version on link-up.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Start the announce task (sends VERSION once the link is connected). */
void nn_ota_p4_init(void);

/* Handle one NN_OTA_MAGIC packet received from the C6. */
void nn_ota_p4_on_msg(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
