/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NODE_MGR_CLEAR_USER_DATA_HANDLER_H
#define NODE_MGR_CLEAR_USER_DATA_HANDLER_H

/* H2D CLEAR_USER_DATA responder (device unregister, step 1 of 2).
 *
 * The hub asks the device to factory-reset its USER data.  The command
 * only arms a flag ("clear on next boot") — the actual erase runs at
 * early boot, before Thread auto-attach, after the hub's follow-up
 * REBOOT.  Arming is idempotent, so unlike REBOOT the hub may retry
 * this command freely.
 *
 * Frames are session-sealed BOTH ways: the device acts only on a body
 * it can open under k_h2d (only the hub holds it), and the hub gets a
 * sealed ack proving the flag landed on the right device.
 */

int clear_user_data_handler_start(void);

/* Early-boot hook: if the clear flag is armed, erase user data (OT
 * dataset, hub key, names, identity keys, OTA state), enter SETUP
 * state and delete the flag LAST (a power cut mid-erase re-runs the
 * whole erase on the next boot — every step is idempotent).
 * Returns true when a clear ran (caller should treat the device as
 * unprovisioned and skip auto-attach). */
bool clear_user_data_boot_check(void);

#endif
