/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

/*
 * nn_features.h — compile-time view of the nn_registry feature matrix.
 *
 * The registry (nn_registry/Kconfig) derives CONFIG_NN_FEATURE_* and
 * CONFIG_NN_LINK_ROLE_* from the chosen app identity.  This header turns
 * those (which are either `1` or undefined) into always-defined 0/1 macros
 * so application and library code can write plain `#if NN_HAS_WIFI` and
 * `if (NN_HAS_WIFI)` without `#ifdef` gymnastics, and adds static guards
 * that fail the build if an app's IDF config contradicts its declared
 * feature set (e.g. OpenThread left enabled when the registry excludes it).
 */

#include "sdkconfig.h"

#if defined(CONFIG_NN_FEATURE_WIFI)
#  define NN_HAS_WIFI 1
#else
#  define NN_HAS_WIFI 0
#endif

#if defined(CONFIG_NN_FEATURE_BLE)
#  define NN_HAS_BLE 1
#else
#  define NN_HAS_BLE 0
#endif

#if defined(CONFIG_NN_FEATURE_OPENTHREAD)
#  define NN_HAS_OPENTHREAD 1
#else
#  define NN_HAS_OPENTHREAD 0
#endif

#if defined(CONFIG_NN_FEATURE_P4CTL)
#  define NN_HAS_P4CTL 1
#else
#  define NN_HAS_P4CTL 0
#endif

#if defined(CONFIG_NN_LINK_ROLE_MASTER)
#  define NN_LINK_IS_MASTER 1
#else
#  define NN_LINK_IS_MASTER 0
#endif

#if defined(CONFIG_NN_LINK_ROLE_SLAVE)
#  define NN_LINK_IS_SLAVE 1
#else
#  define NN_LINK_IS_SLAVE 0
#endif

/* ── App identity string ────────────────────────────────────────────── */
#if defined(CONFIG_NN_APP_MEDIA)
#  define NN_APP_NAME "media"
#elif defined(CONFIG_NN_APP_MEDIA_NETWORK)
#  define NN_APP_NAME "media-network"
#else
#  define NN_APP_NAME "generic"
#endif

/* ── Cross-checks: the registry's exclusions must hold in the real build ─
 *
 * If the registry says a subsystem is excluded but the IDF Kconfig still
 * pulled it in, that's a misconfigured app — fail loudly at compile time
 * rather than silently shipping a fatter/incorrect image.
 */
#if (NN_HAS_OPENTHREAD == 0) && defined(CONFIG_OPENTHREAD_ENABLED)
#  error "nn_registry excludes OpenThread for this app, but CONFIG_OPENTHREAD_ENABLED is set. Remove it from sdkconfig."
#endif

#if (NN_HAS_WIFI == 0) && defined(CONFIG_ESP_WIFI_ENABLED)
#  error "nn_registry excludes Wi-Fi for this app, but CONFIG_ESP_WIFI_ENABLED is set. Remove it from sdkconfig."
#endif

#if (NN_HAS_BLE == 0) && defined(CONFIG_BT_ENABLED)
#  error "nn_registry excludes BLE for this app, but CONFIG_BT_ENABLED is set. Remove it from sdkconfig."
#endif

/* A node cannot be both ends of the SDIO link, and (for the media system)
 * must be exactly one of them. */
#if NN_LINK_IS_MASTER && NN_LINK_IS_SLAVE
#  error "A node cannot be both SDIO master and slave."
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Human-readable one-line summary of the active feature set, e.g.
 * "media-network [slave wifi ble p4ctl]".  Backed by a static buffer. */
const char *nn_registry_summary(void);

#ifdef __cplusplus
}
#endif
