/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Linux-side stub for the PSA serialization lock that nn_proto_identity
 * exposes on Zephyr.  Same contract, pthread-mutex backed.
 */

#include <pthread.h>

static pthread_mutex_t s_psa_mutex = PTHREAD_MUTEX_INITIALIZER;

void nn_proto_identity_psa_lock(void)   { pthread_mutex_lock(&s_psa_mutex); }
void nn_proto_identity_psa_unlock(void) { pthread_mutex_unlock(&s_psa_mutex); }
