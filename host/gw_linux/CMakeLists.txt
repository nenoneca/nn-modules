# SPDX-License-Identifier: Apache-2.0
#
# Linux gateway-host daemon (gw_linux).  Reuses platform-neutral C from
# device/modules/libs/fw_common/ and adds Linux-only platform glue
# (POSIX termios for the Spinel UART, BlueZ for BLE provisioning, BSD
# sockets for the hub TCP transport).
#
# Build options:
#   Native build on the RPi:
#     cmake -S host/gw_linux -B build && cmake --build build
#   Cross-compile from x86_64 dev host:
#     cmake -DCMAKE_TOOLCHAIN_FILE=<root>/cmake/aarch64-pi.cmake \
#           -S host/gw_linux -B build-aarch64
#     cmake --build build-aarch64
#   (Cross-compile path also needs PSA crypto headers in the toolchain
#    sysroot — easier to do native + scp the binary.)

cmake_minimum_required(VERSION 3.20)
project(gw_linux C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Locate fw_common.  Two layouts:
#   (a) Local working tree:    <root>/device/modules/libs/fw_common
#   (b) Manifest checkout:     <root>/modules/libs/fw_common
get_filename_component(NN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(EXISTS "${NN_ROOT}/device/modules/libs/fw_common/CMakeLists.txt")
    set(NN_LIBS_DIR "${NN_ROOT}/device/modules/libs")
elseif(EXISTS "${NN_ROOT}/modules/libs/fw_common/CMakeLists.txt")
    set(NN_LIBS_DIR "${NN_ROOT}/modules/libs")
else()
    message(FATAL_ERROR "fw_common not found under ${NN_ROOT}")
endif()
set(FW_COMMON_DIR "${NN_LIBS_DIR}/fw_common")
message(STATUS "fw_common: ${FW_COMMON_DIR}")

# mbedTLS 3.6+ ships the PSA crypto API in libmbedcrypto.  We require
# the dev package (apt: libmbedtls-dev).
find_path(MBEDTLS_INCLUDE_DIR psa/crypto.h
    HINTS /usr/include /usr/local/include
    REQUIRED)
find_library(MBEDCRYPTO_LIB mbedcrypto
    HINTS /usr/lib /usr/local/lib
          /usr/lib/aarch64-linux-gnu
          /usr/lib/x86_64-linux-gnu
    REQUIRED)
message(STATUS "mbedcrypto: ${MBEDCRYPTO_LIB}")

# sd-bus comes from libsystemd (apt: libsystemd-dev).  We use it for the
# BlueZ GATT/advertisement backend.
find_library(SYSTEMD_LIB systemd
    HINTS /usr/lib /usr/local/lib
          /usr/lib/aarch64-linux-gnu
          /usr/lib/x86_64-linux-gnu
    REQUIRED)
message(STATUS "libsystemd: ${SYSTEMD_LIB}")

add_executable(gw_linux
    src/main.c
    # Platform-neutral sources shared with the Zephyr ESP32-C6 gateway-host
    # app (apps/ncp_host_esp32c6/) — same files, no copying.
    ${FW_COMMON_DIR}/src/hdlc.c
    ${FW_COMMON_DIR}/src/spinel.c
    ${FW_COMMON_DIR}/src/base64.c
    ${FW_COMMON_DIR}/src/envelope.c
    ${FW_COMMON_DIR}/src/hub_crypto.c
    ${FW_COMMON_DIR}/src/gw_identity.c
    ${FW_COMMON_DIR}/src/gw_provision.c
    ${FW_COMMON_DIR}/src/gw_ble_prov.c
    # Linux backends for the kvstore + BLE abstractions (Zephyr has its
    # own counterparts under src/platform/zephyr/).
    ${FW_COMMON_DIR}/src/platform/linux/kvstore.c
    ${FW_COMMON_DIR}/src/platform/linux/nn_proto_identity_lock.c
    ${FW_COMMON_DIR}/src/platform/linux/gw_ble_prov_backend.c
    ${FW_COMMON_DIR}/src/platform/linux/gw_net_prov_backend.c
    ${FW_COMMON_DIR}/src/platform/linux/proto_tcp.c
    ${FW_COMMON_DIR}/src/platform/linux/ncp_link.c
    ${FW_COMMON_DIR}/src/platform/linux/proto_udp_via_ncp.c
    ${FW_COMMON_DIR}/src/proto_router.c
    ${FW_COMMON_DIR}/src/gw_ot_apply.c
    # nn_proto wire-format library — same source the Zephyr gateway uses,
    # pure C with no Zephyr deps.
    ${NN_LIBS_DIR}/nn_proto/src/nn_proto.c
)

target_include_directories(gw_linux PRIVATE
    src
    ${FW_COMMON_DIR}/include
    ${NN_LIBS_DIR}/nn_proto/include
    ${NN_LIBS_DIR}/nn_proto_identity/include
    ${MBEDTLS_INCLUDE_DIR}
)

target_link_libraries(gw_linux PRIVATE
    ${MBEDCRYPTO_LIB}
    ${SYSTEMD_LIB}
    pthread
)

target_compile_options(gw_linux PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -O2 -g
)

# Installed as a plain binary; the systemd unit and supervisor script under
# lxc/ are installed by whoever packages this (Debian LXC rootfs, Buildroot
# nn-gw package) because their paths and env differ per host.
install(TARGETS gw_linux RUNTIME DESTINATION bin)
