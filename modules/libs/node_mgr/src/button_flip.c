/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>

#include <nn_osal/osal.h>
#include <node_mgr/auto_engine.h>
#include <node_mgr/button.h>

#include "button_base.h"

static char g_field[AUTO_MAX_FIELD];

static void on_settled(int held, void *user)
{
	NN_OSAL_UNUSED(user);
	auto_engine_set_field(g_field, (float)held);
}

int button_flip_init(const char *field_name)
{
	if (!field_name || !field_name[0]) {
		return -EINVAL;
	}
	strncpy(g_field, field_name, sizeof(g_field) - 1);
	g_field[sizeof(g_field) - 1] = '\0';

	auto_engine_register_field(g_field, AUTO_FIELD_TYPE_SENSOR, 0, 1);
	auto_engine_set_field(g_field, 0);

	return button_base_init(on_settled, NULL);
}
