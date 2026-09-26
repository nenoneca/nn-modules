/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/* Internal contract between button_base.c and the flip/toggle
 * variants.  Not part of the public API. */

typedef void (*button_settled_cb_t)(int held, void *user);

int button_base_init(button_settled_cb_t on_settled, void *user);
