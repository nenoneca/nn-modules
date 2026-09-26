# syntax=docker/dockerfile:1.7
# nn-build — the fleet's build/test container.
# Rule: TOOLCHAINS live in the image, SOURCE lives in volumes.
# Stage 1 ships the host-ci target; espidf/zephyr targets follow
# (see the docker-build design doc).
#
#   docker build -t nn-build:host-ci --target host-ci docker/
#
# The image bakes NO credentials and NO project code — safe to publish.

FROM debian:bookworm-slim AS host-ci
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential make git ca-certificates curl \
        python3 python3-pip python3-venv python3-dev \
        nodejs \
        libsystemd-dev \
    && rm -rf /var/lib/apt/lists/*

# One python for everything (bookworm = 3.11): kills the host's
# venv312-vs-3.10 split at the root.
RUN python3 -m venv /opt/venv
ENV PATH=/opt/venv/bin:$PATH
# Test deps for every repo: hub (pyproject deps), media logic (scipy:
# the nnvideo motion tests use scipy.ndimage), and detools for the
# binary-delta OTA tests (the dep CI once missed).  libsystemd-dev above:
# nn_pal's posix BLE backend includes <systemd/sd-bus.h>.
RUN pip install --no-cache-dir wheel && \
    pip install --no-cache-dir \
        pytest pytest-asyncio numpy scipy detools \
        aiohttp aiocoap click websockets cryptography pyyaml zeroconf bleak

# Mounted repos belong to the caller's UID, not the container's.
RUN git config --system safe.directory '*'

WORKDIR /work
CMD ["bash"]

# ── espidf: ESP-IDF TOOLS baked; IDF SOURCE stays the repo-vendored tree ──
# The vendored IDF is bind-mounted at image-BUILD time at its real host
# path (buildx --build-context idf=<host idf dir>), so install.sh records
# exactly the layout runtime will see.  Only the toolchains land in the
# image; the IDF tree itself is mounted at runtime, like all source.
#
#   docker buildx build --load -t nn-build:espidf --target espidf \
#     --build-context idf=/media/chalos/mx500/nn_project_nowest/media/esp-idf-v6.0.1 \
#     docker/
FROM host-ci AS espidf
ENV IDF_TOOLS_PATH=/opt/idf-tools
RUN apt-get update && apt-get install -y --no-install-recommends \
        wget flex bison gperf cmake ninja-build ccache \
        libffi-dev libssl-dev dfu-util libusb-1.0-0 \
    && rm -rf /var/lib/apt/lists/*
RUN --mount=from=idf,target=/media/chalos/mx500/nn_project_nowest/media/esp-idf-v6.0.1 \
    /media/chalos/mx500/nn_project_nowest/media/esp-idf-v6.0.1/install.sh esp32p4,esp32c6 \
    && rm -rf ${IDF_TOOLS_PATH}/dist

# ── zephyr: SDK + python3.12 baked; the 37GB west workspace stays a mount ──
# SDK pinned to the same official release the host uses (1.0.1, riscv
# only — the sensor fleet is ESP32-C6).  Python deps install from the
# MOUNTED workspace's own requirements so container and host resolve
# the same zephyr version's constraints.
#
#   docker buildx build --load -t nn-build:zephyr --target zephyr \
#     --build-context zephyrws=/media/chalos/mx500/oss/zephyr \
#     docker/
FROM host-ci AS zephyr
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build gperf ccache dfu-util device-tree-compiler \
        xz-utils file wget \
    && rm -rf /var/lib/apt/lists/*
# python 3.12 (the sensor build's requirement; bookworm ships 3.11)
COPY --from=python:3.12-slim-bookworm /usr/local /usr/local
RUN ldconfig && python3.12 --version
# BOTH SDKs, mirroring the dev host exactly: the tree's version check
# is satisfied by 1.0.1's presence, while the TOOLCHAIN resolves to
# 0.17.0 (gcc 12.2) through CMake's user package registry — that is
# the fleet's actual binary lineage.  nnbuild seeds the registry and
# HOME symlinks per run (HOME is ephemeral in the container).
RUN for v in 0.17.0 1.0.1; do \
      wget -qO /tmp/sdk.tar.xz \
        https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${v}/zephyr-sdk-${v}_linux-x86_64_minimal.tar.xz \
      && tar -xf /tmp/sdk.tar.xz -C /opt && rm /tmp/sdk.tar.xz \
      && /opt/zephyr-sdk-${v}/setup.sh -t riscv64-zephyr-elf -h -c ; \
    done
RUN --mount=from=zephyrws,target=/zws \
    python3.12 -m venv /opt/venv312 \
    && /opt/venv312/bin/pip install --no-cache-dir west 'esptool>=5.0.2' \
    && /opt/venv312/bin/pip install --no-cache-dir -r /zws/zephyr/scripts/requirements.txt

# ── all: every toolchain (dev box default) ─────────────────────────────
FROM zephyr AS all
COPY --from=espidf /opt/idf-tools /opt/idf-tools

# ── gateway: NATIVE arm64 build env for gw_linux (runs under qemu) ─────
# gw_linux is built natively on arm64 (as the beagle/OPi LXC does), not
# cross: Debian's arm64 libsystemd-dev + a static mbedTLS 3.6 satisfy the
# systemd + PSA deps that the cross sysroot lacks.  Needs host qemu-aarch64
# binfmt.  Build command below (needs the mbedtls build context).
FROM --platform=linux/arm64 debian:bookworm-slim AS gateway
# The binary must run on the Debian BOOKWORM hosts (the OrangePi hub):
# built on bookworm (glibc 2.36), and mbedTLS 3.6 (bookworm ships 2.28,
# without the PSA API gw_linux needs) linked STATICALLY from ESP-IDF's
# vendored copy, passed in as a build context:
#   docker buildx build --load --platform linux/arm64 -t nn-build:gateway \
#     --target gateway --build-context \
#     mbedtls=/media/chalos/mx500/nn_project_nowest/media/esp-idf-v5.4.2/components/mbedtls/mbedtls \
#     nn-modules/docker
# (was trixie + libmbedtls-dev: glibc 2.38 / libmbedcrypto.so.16 -- the
# published gw_linux did not start on the OPi, 2026-09-25.)
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake gcc libc6-dev make pkg-config git ca-certificates python3 \
        libsystemd-dev \
    && rm -rf /var/lib/apt/lists/*
COPY --from=mbedtls . /src/mbedtls
RUN cmake -S /src/mbedtls -B /tmp/mbedtls-build -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
        -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX=/opt/mbedtls \
    && cmake --build /tmp/mbedtls-build -j"$(nproc)" && cmake --install /tmp/mbedtls-build \
    && rm -rf /tmp/mbedtls-build /src/mbedtls
RUN git config --system safe.directory '*'
WORKDIR /work
CMD ["bash"]

# ── byai-image: Buildroot host env for the BeagleY-AI factory card image ──
# Runs the app repo's packaging/build-image.sh (Buildroot 2025.08 +
# BR2_EXTERNAL tree) from a clean checkout.  Only HOST tools live here;
# Buildroot fetches and builds its own cross toolchain, so a download
# cache mounted at /work/dl (nnbuild byai-image does this) is what keeps
# a rebuild from refetching gigabytes.  Vendor blobs never enter the
# image: the pipeline stage fetches them into NN_FW_DIR at run time.
# gcc-13 pinned by the defconfig (gcc 14 breaks pre-6.8 kernels), so no
# host compiler version matters beyond "works to build Buildroot itself".
FROM debian:bookworm-slim AS byai-image
ENV DEBIAN_FRONTEND=noninteractive
# Buildroot's mandatory host set (perl and patch listed explicitly: they
# exist on most base images, which is exactly how an incomplete list
# survives until someone builds on a slim one).  No dtc, no u-boot-tools,
# no genimage/mtools/dosfstools: Buildroot builds all of those as host
# packages itself, so listing them here would be one more thing to keep
# true for no benefit.
# cmake is the exception that pays: Buildroot uses the host's when it is at
# least BR2_HOST_CMAKE_AT_LEAST (3.18 for this config; bookworm has 3.25)
# and otherwise compiles host-cmake -- 5.5 min of every clean image build.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential git wget curl ca-certificates cpio unzip rsync bc file \
        libssl-dev libncurses-dev flex bison gawk xz-utils bzip2 python3 perl patch \
        locales cmake \
    && rm -rf /var/lib/apt/lists/* \
    && sed -i 's/^# *\(en_US.UTF-8\)/\1/' /etc/locale.gen && locale-gen
ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
RUN git config --system safe.directory '*'
WORKDIR /work
CMD ["bash"]
