/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gw_ot_apply — push a hub-supplied OT operational dataset into the
 * paired NCP via Spinel.  Platform-neutral (uses ncp_link's
 * uint32_t-ms timeouts).
 *
 * Call AFTER ncp_link_init() and BEFORE flipping NET_IF_UP /
 * NET_STACK_UP — order matters.
 */

#ifndef FW_COMMON_GW_OT_APPLY_H_
#define FW_COMMON_GW_OT_APPLY_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int gw_ot_apply_dataset(const uint8_t *tlvs, size_t tlvs_len);

/* Bring the Thread interface + stack up.  Returns 0 on success, a
 * negative errno on the first Spinel error.  After this the NCP joins
 * (or forms) the network the dataset describes. */
int gw_ot_bring_up(void);

/* Take the Thread stack + interface down.  A gateway host that exits
 * without this leaves its NCP running as a router with the children it
 * collected -- they keep a parent whose host is gone and never look for
 * another one (2026-09-23: three sensors dark for an hour after the
 * camera-hosted gateway was disabled).  Call on every orderly exit. */
int gw_ot_bring_down(void);

#ifdef __cplusplus
}
#endif

#endif /* FW_COMMON_GW_OT_APPLY_H_ */
