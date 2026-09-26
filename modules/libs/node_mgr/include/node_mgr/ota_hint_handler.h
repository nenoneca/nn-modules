/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NODE_MGR_OTA_HINT_HANDLER_H_
#define NODE_MGR_OTA_HINT_HANDLER_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Register the H2D OTA_HINT handler.
 *
 * On a hint the sensor ACKs immediately, then runs check → download → apply
 * on a dedicated workqueue.  Call after ota_client_init(). */
int ota_hint_handler_start(void);

#ifdef __cplusplus
}
#endif

#endif /* NODE_MGR_OTA_HINT_HANDLER_H_ */
