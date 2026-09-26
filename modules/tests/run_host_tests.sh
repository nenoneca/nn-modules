#!/usr/bin/env bash
# Build + run every host-side C test in nn-modules.  Needs only gcc.
# These are contract/logic tests that compile firmware sources for Linux
# against stub or fake dependencies — no hardware, no Zephyr, no IDF.
set -euo pipefail
cd "$(dirname "$0")/.."      # modules/
OUT="${TMPDIR:-/tmp}/nn-host-tests"
mkdir -p "$OUT"
CFLAGS="-std=c11 -Wall -Wextra -Wno-unused-parameter -O0 -g"

echo "── CTS T0: seam symbol census (posix)"
# First, because it is the cheapest check in the repo and the only one that
# catches a seam function that exists in a header and nowhere else.  Green
# means COMPLETE, not working -- see cts/README.md.
../cts/runner/t0_census.sh posix

echo "── nn-sysupd: A/B system-slot agent state machine (fakes: hub, fw_env, card)"
# POSIX sh on the board (busybox ash); the harness needs bash + xz + tar.
sh -n ../tools/nn-sysupd/nn-sysupd
../tools/nn-sysupd/tests/run.sh
echo "── nn_proto: framing + cross-language golden vectors"
gcc $CFLAGS \
    -I libs/nn_proto/include \
    libs/nn_proto/tests/test_proto_host.c libs/nn_proto/src/nn_proto.c \
    -o "$OUT/nn_proto_test"
"$OUT/nn_proto_test"

echo "── fw_common: proto_router"
gcc $CFLAGS -pthread \
    -I libs/fw_common/include -I libs/nn_proto/include \
    libs/fw_common/tests/test_proto_router_host.c \
    libs/fw_common/src/proto_router.c libs/nn_proto/src/nn_proto.c \
    -o "$OUT/proto_router_test"
"$OUT/proto_router_test"

echo "── nn_pal: ESP (NimBLE) BLE backend contract"
gcc $CFLAGS \
    -DCONFIG_NN_PAL_BACKEND_ESP_IDF \
    -I libs/nn_pal/tests/fake_nimble -I libs/nn_pal/include \
    libs/nn_pal/tests/test_ble_esp_host.c \
    libs/nn_pal/tests/fake_nimble/fake_nimble.c \
    libs/nn_pal/src/esp_idf/ble.c \
    -o "$OUT/nn_pal_ble_test"
"$OUT/nn_pal_ble_test"

echo "── nn_pal: POSIX (BlueZ) BLE backend contract"
if pkg-config --exists libsystemd; then
    gcc $CFLAGS \
        -I libs/nn_pal/include -I libs/nn_osal/include \
        $(pkg-config --cflags libsystemd) \
        libs/nn_pal/tests/test_ble_posix_host.c \
        libs/nn_pal/src/posix/ble.c \
        $(pkg-config --libs libsystemd) -lpthread \
        -o "$OUT/nn_pal_ble_posix_test"
    "$OUT/nn_pal_ble_posix_test"
else
    echo "   SKIP: libsystemd-dev not installed"
fi

echo "── node_mgr: multi-gateway HELLO policy"
gcc $CFLAGS \
    -I libs/node_mgr/include \
    libs/node_mgr/tests/test_gw_policy_host.c libs/node_mgr/src/gw_policy.c \
    -o "$OUT/gw_policy_test"
"$OUT/gw_policy_test"

echo "── nn_prov: CONFIG/WIFI handlers (real POSIX kv; fake netstream/crypto)"
# Storage is the REAL posix nn_osal_kv backend, not a fake: nn_prov's
# persistence was the part being ported off NVS, so faking it would have
# tested the assumption instead of the code.  It writes under a temp dir.
gcc $CFLAGS \
    -I libs/nn_prov/tests/fake_esp -I libs/nn_prov/include -I libs/nn_prov/src \
    -I libs/nn_osal/include \
    libs/nn_prov/tests/test_prov_config_host.c \
    libs/nn_prov/tests/fake_esp/fake_esp.c \
    libs/nn_prov/src/nn_prov.c \
    libs/nn_osal/src/posix/storage.c \
    -o "$OUT/prov_test"
"$OUT/prov_test"

echo "── nn_osal: POSIX backend (smoke + kv dispatch)"
make -s -C libs/nn_osal/tests/posix clean run

echo
echo "all host tests passed"
