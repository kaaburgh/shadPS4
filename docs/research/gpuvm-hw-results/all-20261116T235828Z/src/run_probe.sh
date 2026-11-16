#!/bin/sh
# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds the UMA GPUVM probe, runs a fixed list of tests and keeps everything needed to
# compare two machines or two runs: sources, hashes, versions, raw logs, exit statuses.
#
#   tools/uma_gpuvm_probe/run_probe.sh OUT_DIR [SET]
#
# OUT_DIR must not exist. SET is "gate" (default: S1-S3 on driver memory and on dma-buf) or
# "all" (the gate, then the shifted and stitched import tests T and U, each on driver memory
# and on dma-buf).
# Environment: PROBE_DEVICE (Vulkan device index, default 0), PROBE_MIB (default 16),
# PROBE_BLOCK_KIB (block of the T and U tests, default 64), CC (default cc).
# Needs a C compiler, libvulkan headers and, for dma-buf, read/write access to /dev/udmabuf.
#
# Every test runs in two driver configurations:
#   default   the driver as installed
#   force64k  radv_force_64k_sparse_alignment=true in the environment. RADV of Mesa 25.0
#             then reports a sparse bind granularity of 64 KiB, which is what RADV reports
#             unconditionally from Mesa 26.2. Drivers without that option ignore the variable
#             and the run repeats the default one; the granularity each run saw is recorded.
# and in two modes: plain, and under VK_LAYER_KHRONOS_validation when that layer is installed.
#
# Before the tests, a canary run breaks Vulkan rules on purpose under the layer and passes
# only if the layer reports them, so that silence of the layer in the tests means something.
#
# A run is "failed" when it exited non-zero or validation printed a message, "skipped" when
# it exited 0 without one PASS line, "passed" otherwise. Exit status: 0 when no run failed,
# 1 otherwise, 2 on a setup error. OUT_DIR/summary.txt lists each run and the totals.
set -u
out=${1:?usage: run_probe.sh OUT_DIR [gate|all]}
set_name=${2:-gate}
here=$(cd "$(dirname "$0")" && pwd)
device=${PROBE_DEVICE:-0}
mib=${PROBE_MIB:-16}
block_kib=${PROBE_BLOCK_KIB:-64}
cc=${CC:-cc}

[ -e "$out" ] && { echo "$out exists" >&2; exit 2; }
mkdir -p "$out/src" || exit 2
cp "$here"/*.c "$here"/run_probe.sh "$out/src/"

# Tests of a set: "name|arguments". The control on driver memory comes first each time: it
# separates what the driver's sparse binding does from what imported memory adds.
gate="control|--backend control --bw-mib 0
dmabuf|--backend dmabuf --bw-mib 0"
all="$gate
shifted-control|--backend shifted-control --block-kib $block_kib
shifted-dmabuf|--backend shifted-dmabuf --block-kib $block_kib
stitched-control|--backend stitched-control --block-kib $block_kib
stitched-dmabuf|--backend stitched-dmabuf --block-kib $block_kib"
case $set_name in
gate) tests=$gate ;;
all) tests=$all ;;
*) echo "unknown set $set_name" >&2; exit 2 ;;
esac
configs="default force64k"

{
    echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "set: $set_name"
    echo "device_index: $device"
    echo "mib: $mib"
    echo "block_kib: $block_kib"
    echo "uname: $(uname -srvm)"
    [ -r /etc/os-release ] && . /etc/os-release && echo "os: ${PRETTY_NAME:-unknown}"
    echo "cc: $($cc --version 2>&1 | head -1)"
    echo "git_head: $(git -C "$here" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "git_dirty_files: $(git -C "$here" status --porcelain -- . 2>/dev/null | wc -l)"
    echo "udmabuf_device: $(ls -l /dev/udmabuf 2>&1)"
    echo "udmabuf_access: $([ -r /dev/udmabuf ] && [ -w /dev/udmabuf ] && echo yes || echo no)"
    for p in list_limit size_limit_mb; do
        f=/sys/module/udmabuf/parameters/$p
        [ -r $f ] && echo "udmabuf_$p: $(cat $f)"
    done
    echo "user: $(id -un) groups: $(id -Gn)"
    echo "memlock_kib: $(ulimit -l)"
    for f in /sys/class/drm/card*/device/mem_info_gtt_total; do
        [ -r "$f" ] && echo "gtt_total_bytes $(dirname "$f"): $(cat "$f")"
    done
    for pkg in mesa-vulkan-drivers vulkan-radeon mesa libvulkan1 vulkan-icd-loader \
        vulkan-validationlayers vulkan-validation-layers; do
        if command -v dpkg-query >/dev/null 2>&1; then
            v=$(dpkg-query -W -f '${Version}' $pkg 2>/dev/null)
        elif command -v pacman >/dev/null 2>&1; then
            v=$(pacman -Q $pkg 2>/dev/null | cut -d' ' -f2)
        elif command -v rpm >/dev/null 2>&1; then
            v=$(rpm -q --qf '%{VERSION}-%{RELEASE}' $pkg 2>/dev/null) || v=
        else
            v=
        fi
        [ -n "$v" ] && echo "package $pkg: $v"
    done
} >"$out/environment.txt" 2>&1
command -v vulkaninfo >/dev/null 2>&1 && vulkaninfo --summary >"$out/vulkaninfo-summary.txt" 2>&1
command -v lspci >/dev/null 2>&1 && lspci -nn 2>/dev/null | grep -Ei 'vga|display|3d' \
    >"$out/gpu-pci.txt"

for src in "$out"/src/*.c; do
    bin=$out/$(basename "$src" .c)
    if ! $cc -O1 -g -Wall -Wextra -o "$bin" "$src" -lvulkan >"$bin.build.txt" 2>&1; then
        echo "build of $src failed, see $bin.build.txt" >&2
        exit 2
    fi
done

layer=no
if command -v vulkaninfo >/dev/null 2>&1 &&
    vulkaninfo --summary 2>/dev/null | grep -q VK_LAYER_KHRONOS_validation; then
    layer=yes
fi
echo "validation_layer: $layer" >>"$out/environment.txt"

gtt_used() {
    for f in /sys/class/drm/card*/device/mem_info_gtt_used; do
        [ -r "$f" ] && printf '%s ' "$(cat "$f")"
    done
}

: >"$out/summary.txt"
: >"$out/.verdicts"
run() { # name, driver configuration, mode (plain|validation), arguments
    name=$1 config=$2 mode=$3
    shift 3
    log=$out/$name.$config.$mode.txt
    before=$(gtt_used)
    (
        [ "$config" = force64k ] && export radv_force_64k_sparse_alignment=true
        if [ "$mode" = validation ]; then
            export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_ENABLES=
        fi
        exec "$out/uma_gpuvm_probe" --device "$device" --mib "$mib" "$@"
    ) >"$log" 2>&1
    status=$?
    errors=$(grep -c -E 'Validation Error|VUID-|UNASSIGNED-' "$log")
    pass=$(grep -c -E '^[A-Z][0-9]+ +PASS' "$log")
    fail=$(grep -c -E '^[A-Z][0-9]+ +FAIL' "$log")
    skip=$(grep -c -E '^[A-Z][0-9]+ +SKIP' "$log")
    if [ "$status" -ne 0 ] || [ "$errors" -ne 0 ]; then
        verdict=failed
    elif [ "$pass" -eq 0 ]; then
        verdict=skipped
    else
        verdict=passed
    fi
    echo "$verdict" >>"$out/.verdicts"
    {
        echo "== $name ($config, $mode): $verdict; exit $status, PASS $pass, FAIL $fail," \
            "SKIP $skip, validation messages $errors"
        echo "   arguments: $*"
        echo "   gtt_used_bytes before: ${before:-n/a} after: $(gtt_used)"
        grep -E '^(Q|S[0-9]|T[0-9]|U[0-9]|E[0-9]|B[0-9]|summary|    )' "$log" | sed 's/^/   /'
    } >>"$out/summary.txt"
}

# Shows that the validation runs can fail: the probe breaks a rule on purpose (V1: any driver;
# V2: a sparse bind below the reported granularity, only where RADV was told to report 64 KiB)
# and the layer has to report it. Without this a silent layer and an absent layer look alike.
canary() { # driver configuration
    config=$1
    log=$out/canary.$config.validation.txt
    (
        [ "$config" = force64k ] && export radv_force_64k_sparse_alignment=true
        export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_ENABLES=
        exec "$out/uma_gpuvm_probe" --device "$device" --backend validation-canary
    ) >"$log" 2>&1
    status=$?
    ids=$(grep -o -E 'VUID-[A-Za-z0-9-]+' "$log" | sort -u | tr '\n' ' ')
    v1=no v2=no
    echo "$ids" | grep -q 'VUID-VkBufferCreateInfo' && v1=yes
    echo "$ids" | grep -q -E 'VUID-VkSparse|VUID-vkQueueBindSparse' && v2=yes
    grep -q '^V2 *SKIP' "$log" && v2=skipped
    if [ "$status" -eq 0 ] && [ "$v1" = yes ] && [ "$v2" != no ]; then
        verdict=passed
    else
        verdict=failed
    fi
    echo "$verdict" >>"$out/.verdicts"
    {
        echo "== canary ($config, validation): $verdict; exit $status, layer reported the" \
            "planted errors: V1 $v1, V2 $v2"
        echo "   reported: ${ids:-nothing}"
        grep -E '^V[0-9]' "$log" | sed 's/^/   /'
    } >>"$out/summary.txt"
}
if [ "$layer" = yes ]; then
    for config in $configs; do
        canary "$config"
    done
fi

echo "$tests" | while IFS='|' read -r name arguments; do
    for config in $configs; do
        # shellcheck disable=SC2086
        run "$name" "$config" plain $arguments
        # shellcheck disable=SC2086
        [ "$layer" = yes ] && run "$name" "$config" validation $arguments
    done
done

count() { grep -c "^$1\$" "$out/.verdicts"; }
passed=$(count passed) skipped=$(count skipped) failed=$(count failed)
rm -f "$out/.verdicts"
echo "result: $passed runs passed, $skipped skipped, $failed failed" >>"$out/summary.txt"
(cd "$out" && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum >SHA256SUMS)
cat "$out/summary.txt"
[ "$failed" -eq 0 ]
