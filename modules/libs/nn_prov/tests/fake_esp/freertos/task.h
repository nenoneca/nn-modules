/* SPDX-License-Identifier: Apache-2.0 — host-test fake */
#pragma once
typedef void (*TaskFunction_t)(void *);
int xTaskCreate(TaskFunction_t fn, const char *name, int stack,
                void *arg, int prio, void *handle);
