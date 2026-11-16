#!/bin/sh
# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Records the UMA E0 captures that tools/uma_gpuvm_model/check_open_test.py checks.
#
#   tools/uma_e0/run_open_test_matrix.sh SHADPS4 OPEN_TEST_OUT RUNS_DIR
#
# SHADPS4        the shadPS4 binary
# OPEN_TEST_OUT  the out/ directory of a built kaaburgh/shadps4-open-test
# RUNS_DIR       receives one run directory per capture; must not hold earlier runs
#
# Every workload runs on lavapipe under a private Xvfb, once with the mirror path and once with
# shared backing. Four more runs of uma_churn end the capture in other ways: IPC STOP, SIGTERM
# and SIGKILL in the middle of the run, and a byte budget. As root a fifth one records onto a
# 128 KiB tmpfs, so that the recorder meets real write errors.
set -u
shadps4=$1 out=$2 runs=$3
here=$(dirname "$0")
tests="buffer_compute_roundtrip physical_alias map_unmap_reuse uma_churn fallback_in_shared
dma_alias_order buffer_granularity_matrix buffer_page_false_sharing"

run() { # name, elf, extra arguments
    name=$1 elf=$2
    shift 2
    python3 "$here/run_synthetic.py" --shadps4 "$shadps4" --elf "$out/$elf/$elf.elf" \
        --output "$runs/$name" --xvfb --lavapipe --label "$name" "$@" >"$runs/$name.out" 2>&1
    echo "$name: harness exit $?"
}

mkdir -p "$runs"
for test in $tests; do
    [ -f "$out/$test/$test.elf" ] || { echo "$test: not built, skipped"; continue; }
    run "$test-mirror" "$test"
    run "$test-shared" "$test" --shared-backing
done
run int-ipc uma_churn --seconds 0.6 --stop ipc
run int-sigterm uma_churn --seconds 0.6 --stop sigterm
run int-sigkill uma_churn --seconds 0.6 --stop sigkill
run int-budget uma_churn --max-bytes 208000
if [ "$(id -u)" = 0 ]; then
    mkdir -p "$runs/tiny" && mount -t tmpfs -o size=128k tmpfs "$runs/tiny" &&
        run int-diskfull uma_churn --capture "$runs/tiny/capture"
fi
