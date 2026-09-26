/* SPDX-License-Identifier: Apache-2.0 — host-test fake */
#pragma once
void esp_restart(void);
#ifndef strlcpy
#include <stddef.h>
size_t strlcpy(char *dst, const char *src, size_t cap);
#endif
