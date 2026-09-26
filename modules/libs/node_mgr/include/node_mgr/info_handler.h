/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NODE_MGR_INFO_HANDLER_H_
#define NODE_MGR_INFO_HANDLER_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Register an H2D INFO_QUERY handler that replies with a JSON
 * description ({"firmware":..,"name":..,"eui64":..,"fields":[...]}).
 * Must be called AFTER nn_proto_client_init().  `device_name` is
 * captured by reference — caller keeps it alive. */
/* *image_name* is the build-time app identity (the firmware project
 * name, e.g. "mdns_ot_esp32c6") — reported as "img" in INFO_REPLY so
 * the hub can key the OTA catalog by WHAT the device runs instead of
 * guessing from its registration-era device_type. */
int info_handler_start(const char *device_name, const char *image_name);

/* Send the same JSON description unsolicited as a D2H INFO_REPLY with
 * tid=0.  The hub treats tid=0 INFO_REPLY as a device-initiated config
 * sync (vs the paired QUERY→REPLY flow).  Use on:
 *   - boot, after Thread attach (so the hub's running_version updates
 *     immediately after an OTA reboot)
 *   - any state change the hub should know about (e.g. AUTO_PUSH applied)
 *
 * Safe to call repeatedly; this is a fire-and-forget D2H frame.
 * Returns 0 on success or negative errno from the wire path. */
int info_handler_send_unsolicited(void);

#ifdef __cplusplus
}
#endif

#endif
