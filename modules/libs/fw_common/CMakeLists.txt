# SPDX-License-Identifier: Apache-2.0
#
# fw_common — platform-neutral firmware logic shared between the Zephyr
# end-device + gateway-host apps and the upcoming pure-Linux gateway-host
# daemon (host/gw_linux/).
#
# Apps that want fw_common add this directory via `add_subdirectory()`
# from their own CMakeLists.txt — same pattern as nn_proto.  Under
# Zephyr the CONFIG_FW_COMMON gate enables the file set; under plain
# Linux the host CMake project (host/gw_linux/CMakeLists.txt) references
# these source files directly without going through this CMakeLists.

# Transitive deps that fw_common's Zephyr-side sources rely on:
#   - hub_crypto.c #include <nn_proto_identity/nn_proto_identity.h>
#   - kvstore.c + gw_ble_prov_backend.c go through nn_osal/osal.h
# Pulled in here idempotently so an app can just add_subdirectory(fw_common)
# and get the full transitive chain.
if(CONFIG_FW_COMMON)
    # Idempotent — apps that already add these dirs themselves get a
    # cheap no-op; otherwise fw_common pulls them in.  Guarded via
    # global property because Zephyr's add_subdirectory uses
    # target_sources(app …) and doesn't define a library target.
    get_property(_nn_pid_added GLOBAL PROPERTY NN_PROTO_IDENTITY_ADDED)
    if(NOT _nn_pid_added)
        add_subdirectory(
            ${CMAKE_CURRENT_SOURCE_DIR}/../nn_proto_identity
            nn_proto_identity_build)
        set_property(GLOBAL PROPERTY NN_PROTO_IDENTITY_ADDED TRUE)
    endif()
    get_property(_nn_osal_added GLOBAL PROPERTY NN_OSAL_ADDED)
    if(NOT _nn_osal_added)
        add_subdirectory(
            ${CMAKE_CURRENT_SOURCE_DIR}/../nn_osal
            nn_osal_build)
        set_property(GLOBAL PROPERTY NN_OSAL_ADDED TRUE)
    endif()
    get_property(_nn_pal_added GLOBAL PROPERTY NN_PAL_ADDED)
    if(NOT _nn_pal_added)
        add_subdirectory(
            ${CMAKE_CURRENT_SOURCE_DIR}/../nn_pal
            nn_pal_build)
        set_property(GLOBAL PROPERTY NN_PAL_ADDED TRUE)
    endif()

    target_sources(app PRIVATE
        # Platform-neutral (compile under both Zephyr libc and host glibc):
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hdlc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/spinel.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/base64.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/envelope.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/hub_crypto.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/nn_session.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gw_identity.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gw_provision.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gw_ble_prov.c
        # Zephyr-side backends for the kvstore + GATT abstractions.
        # Linux has its own implementations under src/platform/linux/
        # used by host/gw_linux/CMakeLists.txt.
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/zephyr/kvstore.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/zephyr/gw_ble_prov_backend.c
    )
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
endif()
