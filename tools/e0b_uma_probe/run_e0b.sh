#!/bin/bash
# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds e0b_uma_probe, collects host facts and runs the probe. Produces
# e0b-results-<host>-<time>/ and a .tar.gz of it to send back.
#
# Usage: ./run_e0b.sh [extra probe options, e.g. --device 1 or --t6-max 1024]
# Environment: E0B_VALIDATE=1 also runs a short pass with VK_LAYER_KHRONOS_validation.

set -u
here=$(cd "$(dirname "$0")" && pwd)
host=$(hostname -s 2>/dev/null || hostname)
stamp=$(date +%Y%m%d-%H%M%S)
out="$PWD/e0b-results-$host-$stamp"
mkdir -p "$out"

echo "== building"
cmake -S "$here" -B "$here/build" -DCMAKE_BUILD_TYPE=RelWithDebInfo >"$out/build.log" 2>&1 &&
    cmake --build "$here/build" -j >>"$out/build.log" 2>&1
if [ ! -x "$here/build/e0b_uma_probe" ]; then
    echo "build failed, see $out/build.log"
    exit 1
fi
probe="$here/build/e0b_uma_probe"

echo "== collecting host facts"
{
    echo "### uname"; uname -a
    echo "### os-release"; cat /etc/os-release 2>/dev/null
    echo "### groups"; id
    echo "### lspci (display)"; lspci -nnk 2>/dev/null | grep -A3 -E "VGA|3D|Display"
    echo "### /dev/udmabuf"; ls -l /dev/udmabuf 2>&1
    echo "### udmabuf params"; grep -H . /sys/module/udmabuf/parameters/* 2>&1
    echo "### kernel config (udmabuf)"
    (zcat /proc/config.gz 2>/dev/null || cat "/boot/config-$(uname -r)" 2>/dev/null) | grep -E "CONFIG_UDMABUF|CONFIG_DMABUF" || echo "<config not readable>"
    echo "### swiotlb / iommu (dmesg, may need sudo)"
    (dmesg 2>/dev/null || sudo -n dmesg 2>/dev/null) | grep -i -E "swiotlb|software io tlb|iommu|amd-vi|dmar" | head -40 || echo "<dmesg not readable>"
    echo "### swiotlb debugfs (needs root)"
    sudo -n cat /sys/kernel/debug/swiotlb/io_tlb_used /sys/kernel/debug/swiotlb/io_tlb_nslabs 2>&1 || echo "<not readable>"
    echo "### vulkaninfo --summary"; vulkaninfo --summary 2>&1 | head -80
    echo "### probe devices"; "$probe" --list-devices 2>&1
} >"$out/host.txt" 2>&1

echo "== running probe (log: $out/probe.log)"
"$probe" --json "$out/probe.json" "$@" 2>&1 | tee "$out/probe.log"
status=${PIPESTATUS[0]}

if [ "${E0B_VALIDATE:-0}" = "1" ]; then
    echo "== validation pass"
    "$probe" --validate --tests caps,t1,t2,t3,t5 --json "$out/probe-validate.json" "$@" \
        >"$out/probe-validate.log" 2>&1
    grep -c "validation error" "$out/probe-validate.log" | sed 's/^/validation errors: /'
fi

tar -czf "$out.tar.gz" -C "$(dirname "$out")" "$(basename "$out")"
echo
echo "results: $out.tar.gz (probe exit code $status)"
exit 0
