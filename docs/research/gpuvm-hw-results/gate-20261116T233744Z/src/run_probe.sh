#!/bin/sh
# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds the UMA GPUVM probe, runs a fixed list of tests and keeps everything needed to
# compare two machines or two runs: sources, hashes, versions, raw logs, exit statuses.
#
#   tools/uma_gpuvm_probe/run_probe.sh OUT_DIR [SET]
#
# OUT_DIR must not exist. SET is "gate" (default: S1-S3 control and dma-buf) or "all".
# Environment: PROBE_DEVICE (Vulkan device index, default 0), PROBE_MIB (default 16),
# CC (default cc). Needs a C compiler, libvulkan headers and, for dma-buf, read/write
# access to /dev/udmabuf.
#
# Every test runs twice: plain, and under VK_LAYER_KHRONOS_validation when that layer is
# installed. Exit status: 0 when every run exited 0 and validation printed no error,
# 1 otherwise, 2 on a setup error. OUT_DIR/summary.txt lists each run.
set -u
out=${1:?usage: run_probe.sh OUT_DIR [gate|all]}
set_name=${2:-gate}
here=$(cd "$(dirname "$0")" && pwd)
device=${PROBE_DEVICE:-0}
mib=${PROBE_MIB:-16}
cc=${CC:-cc}

[ -e "$out" ] && { echo "$out exists" >&2; exit 2; }
mkdir -p "$out/src" || exit 2
cp "$here"/*.c "$here"/run_probe.sh "$out/src/"

# Tests of a set: "name|arguments". S1-S3 control first: it separates the driver's own sparse
# support from support for imported memory.
gate="control|--backend control --bw-mib 0
dmabuf|--backend dmabuf --bw-mib 0"
all="$gate"
case $set_name in
gate) tests=$gate ;;
all) tests=$all ;;
*) echo "unknown set $set_name" >&2; exit 2 ;;
esac

{
    echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "set: $set_name"
    echo "device_index: $device"
    echo "mib: $mib"
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
    if ! $cc -O1 -g -Wall -Wextra -o "$bin" "$src" -lvulkan >"$bin.build.log" 2>&1; then
        echo "build of $src failed, see $bin.build.log" >&2
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

failed=0
: >"$out/summary.txt"
run() { # name, mode (plain|validation), arguments
    name=$1 mode=$2
    shift 2
    log=$out/$name.$mode.log
    before=$(gtt_used)
    if [ "$mode" = validation ]; then
        VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_ENABLES= \
            "$out/uma_gpuvm_probe" --device "$device" --mib "$mib" "$@" >"$log" 2>&1
    else
        "$out/uma_gpuvm_probe" --device "$device" --mib "$mib" "$@" >"$log" 2>&1
    fi
    status=$?
    errors=$(grep -c -E 'Validation Error|VUID-|UNASSIGNED-' "$log")
    [ "$status" -ne 0 ] || [ "$errors" -ne 0 ] && failed=1
    {
        echo "== $name ($mode): exit $status, validation messages $errors, arguments: $*"
        echo "   gtt_used_bytes before: ${before:-n/a} after: $(gtt_used)"
        grep -E '^(Q|S[0-9]|T[0-9]|U[0-9]|E[0-9]|B[0-9]|summary)' "$log" | sed 's/^/   /'
    } >>"$out/summary.txt"
}

echo "$tests" | while IFS='|' read -r name arguments; do
    # shellcheck disable=SC2086
    run "$name" plain $arguments
    # shellcheck disable=SC2086
    [ "$layer" = yes ] && run "$name" validation $arguments
    echo $failed >"$out/.failed"
done
failed=$(cat "$out/.failed" 2>/dev/null || echo 1)
rm -f "$out/.failed"

(cd "$out" && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum >SHA256SUMS)
echo "result: $([ "$failed" = 0 ] && echo every run passed || echo at least one run failed)" \
    >>"$out/summary.txt"
cat "$out/summary.txt"
exit "$failed"
