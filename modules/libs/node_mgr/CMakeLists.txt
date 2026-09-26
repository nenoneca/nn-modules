# SPDX-License-Identifier: Apache-2.0
#
# node_mgr sources are added directly to the 'app' target so they compile
# into app/libapp.a and are automatically included in the Zephyr link.
# (zephyr_library() from an add_subdirectory() inside an app CMakeLists
# creates a .a that is compiled but not automatically passed to the linker.)

target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/network_manager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/provision_peripheral.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/provision_central.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/init_manager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/provision_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/heartbeat.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/radio_stats.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/channel_scan.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gw_cost.c
    # hub_crypto.c moved to modules/libs/fw_common/src/hub_crypto.c
    # — pulled into apps via the fw_common add_subdirectory().
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ota_client.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dns_cache.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/messenger.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/coap_field.c      # pure ECIES field-op dispatcher
    ${CMAKE_CURRENT_SOURCE_DIR}/src/coap_log.c        # now D2H LOG_LINE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/auto_engine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/time_sync.c       # now D2H TIME_QUERY
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nn_proto_client.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gw_policy.c   # pure multi-gw HELLO policy (host-tested)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/info_handler.c    # H2D INFO_QUERY (replaces /info)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nn_session_boot.c # Phase-2 symmetric session bootstrap
    ${CMAKE_CURRENT_SOURCE_DIR}/src/nn_group.c       # Phase-4 D2D group-key AEAD
    ${CMAKE_CURRENT_SOURCE_DIR}/src/auto_push_handler.c # H2D AUTO_PUSH (replaces WS ConfigResponse)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ota_hint_handler.c  # H2D OTA_HINT (hub-initiated OTA)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/clear_user_data_handler.c  # H2D CLEAR_USER_DATA (unregister)
)

# ── Phase 6 hub<->device field-op H2D dispatcher (opt-in) ───────────────
if(CONFIG_NODE_MGR_FIELD_RELAY)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/field_relay.c
    )
endif()

# ── Binary-delta OTA (detools / heatshrink) ─────────────────────────────
# Vendored detools C apply library + heatshrink decoder.  Compiled
# heatshrink-only (no file I/O, no LZMA/CRLE/NONE) to minimise footprint.
# These DETOOLS_CONFIG_* defines must be visible to every TU that includes
# detools.h (ota_client.c needs the same struct layout), so they go on
# `app` rather than per-source.
if(CONFIG_NODE_MGR_OTA_DELTA)
    set(_detools ${CMAKE_CURRENT_SOURCE_DIR}/../detools)
    target_sources(app PRIVATE
        ${_detools}/src/detools.c
        ${_detools}/src/heatshrink/heatshrink_decoder.c
    )
    target_include_directories(app PRIVATE
        ${_detools}/include/detools
        ${_detools}/src/heatshrink
    )
    target_compile_definitions(app PRIVATE
        DETOOLS_CONFIG_FILE_IO=0
        DETOOLS_CONFIG_COMPRESSION_NONE=0
        DETOOLS_CONFIG_COMPRESSION_LZMA=0
        DETOOLS_CONFIG_COMPRESSION_CRLE=0
        DETOOLS_CONFIG_COMPRESSION_HEATSHRINK=1
        HEATSHRINK_DYNAMIC_ALLOC=1
    )
endif()

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ── On/off button input (mode-selectable) ────────────────────────────────
# Pick exactly one variant via the NODE_MGR_BUTTON_FLIP / _TOGGLE Kconfig
# choice — only the matching .c file is compiled in, so calling the
# wrong init from app code is a link-time error.

if(CONFIG_NODE_MGR_BUTTON)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/button_base.c
    )
    if(CONFIG_NODE_MGR_BUTTON_FLIP)
        target_sources(app PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/button_flip.c
        )
    elseif(CONFIG_NODE_MGR_BUTTON_TOGGLE)
        target_sources(app PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/button_toggle.c
        )
    endif()
endif()

# ── Platform-specific helpers ────────────────────────────────────────────
# Chip-specific code lives under src/platform/<soc>/ and is gated by
# Kconfig + SOC_SERIES_*.  Add new chips by dropping a sibling folder
# and a matching Kconfig stanza.

if(CONFIG_NODE_MGR_DOWNLOAD_MODE)
    if(CONFIG_SOC_SERIES_ESP32C6)
        target_sources(app PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/esp32c6/download_mode.c
        )
    else()
        message(WARNING "NODE_MGR_DOWNLOAD_MODE=y but no platform impl "
                "for this SOC — `download` shell cmd will be missing.")
    endif()
endif()

# Sibling libs pulled in here so any app that uses node_mgr automatically
# gets the wire-format encode/decode + identity primitives + the shared
# fw_common module that owns hub_crypto.c after the gateway-host split.
# Each add_subdirectory is guarded by a global property so an app that
# pulls these in independently doesn't trip a double-add error.
get_property(_nn_proto_added GLOBAL PROPERTY NN_PROTO_ADDED)
if(NOT _nn_proto_added)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../nn_proto nn_proto_build)
    set_property(GLOBAL PROPERTY NN_PROTO_ADDED TRUE)
endif()
get_property(_nn_pid_added GLOBAL PROPERTY NN_PROTO_IDENTITY_ADDED)
if(NOT _nn_pid_added)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../nn_proto_identity
                     nn_proto_identity_build)
    set_property(GLOBAL PROPERTY NN_PROTO_IDENTITY_ADDED TRUE)
endif()
get_property(_fw_common_added GLOBAL PROPERTY FW_COMMON_ADDED)
if(NOT _fw_common_added)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../fw_common fw_common_build)
    set_property(GLOBAL PROPERTY FW_COMMON_ADDED TRUE)
endif()
get_property(_nn_osal_added GLOBAL PROPERTY NN_OSAL_ADDED)
if(NOT _nn_osal_added)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../nn_osal nn_osal_build)
    set_property(GLOBAL PROPERTY NN_OSAL_ADDED TRUE)
endif()
