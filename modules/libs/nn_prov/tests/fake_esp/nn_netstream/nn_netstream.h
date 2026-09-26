/* SPDX-License-Identifier: Apache-2.0 — host-test fake */
#pragma once
#include <stdint.h>
void nn_netstream_set_host(const char *host, uint16_t port);
void nn_netstream_set_stream_key(const uint8_t key[32]);
void nn_netstream_set_wifi(const char *ssid, const char *pass);
