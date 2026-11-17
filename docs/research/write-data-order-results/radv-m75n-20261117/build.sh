#!/bin/bash
# Configure and build shadps4 like /mnt/UsbSSD447G/uma-e3-radv-review54-u2jbnp/build-e3.sh
# (Release with -O0 C++ / -O1 C, clang-19, lld, ccache), but in this folder.
#   [SRC=source-dir] ./build.sh [BUILD_DIR] [JOBS]
set -eu
d="$(cd -- "$(dirname -- "$0")" && pwd)"
build=${1:-$d/build}
jobs=${2:-4}
src=${SRC:-$d/source}
mkdir -p "$d/tmp"
export TMPDIR="$d/tmp" CCACHE_DIR="$d/ccache" CCACHE_BASEDIR="$src" CCACHE_MAXSIZE=12G
cd "$src"
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
    -DCMAKE_CXX_FLAGS_RELEASE="-O0 -DNDEBUG" -DCMAKE_C_FLAGS_RELEASE="-O1 -DNDEBUG" \
    -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DUMA_E0_SOURCE_SHA="$(git rev-parse HEAD)" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DENABLE_UPDATER=OFF -DENABLE_DISCORD_RPC=OFF -DCMAKE_JOB_POOLS=link=1 \
    -DCMAKE_JOB_POOL_LINK=link
ccache -z >/dev/null
start=$(date +%s)
cmake --build "$build" --target shadps4 --parallel "$jobs"
echo "build wall time: $(( $(date +%s) - start )) s"
ccache -s
