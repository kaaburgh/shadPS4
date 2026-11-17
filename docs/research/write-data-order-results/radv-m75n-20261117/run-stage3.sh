#!/bin/bash
# Stage 3 runs: baseline vs fix binaries on RADV, raw logs under logs/after-5e41afe.
#   ./run-stage3.sh runs|validation
set -u
d="$(cd -- "$(dirname -- "$0")" && pwd)"
cd "$d"
declare -A BIN=([baseline]="$d/build/shadps4" [fix]="$d/build-fix/shadps4")
L=$d/logs/after-5e41afe
mkdir -p "$L"
cmds=$L/commands.txt

run() { # bin_name test config n [env...]
    local b=$1 t=$2 c=$3 n=$4 dir=$5
    shift 5
    local args=(--readbacks "${c#*-}")
    [[ $c == shared-* ]] && args+=(--shared-backing)
    mkdir -p "$L/$dir"
    local log=$L/$dir/$t.$c.$n.log
    echo "$* ./radv.sh python3 open-test/scripts/run-test.py $t --shadps4 ${BIN[$b]} ${args[*]} > $log" >>"$cmds"
    env "$@" WESTON_LOG="${log%.log}.weston.txt" ./radv.sh python3 open-test/scripts/run-test.py \
        "$t" --shadps4 "${BIN[$b]}" "${args[@]}" --timeout 120 >"$log" 2>&1
    echo "runner_exit=$?" >>"$log"
}

others="cond_exec_gpu_predicate dma_order dma_alias_order physical_alias fallback_in_shared
map_unmap_reuse buffer_compute_roundtrip buffer_pingpong pm4_controls ordered_async_handoff uma_churn"
case ${1:-runs} in
runs)
    for b in baseline fix; do
        for t in write_data_order write_data_interleave; do
            for c in mirror-precise shared-precise shared-disabled; do
                for n in 1 2 3 4 5; do run $b $t $c $n $b; done
            done
        done
        for t in $others; do
            for c in mirror-precise shared-precise shared-disabled; do run $b $t $c 1 $b; done
        done
    done
    ;;
validation)
    for b in baseline fix; do
        for t in write_data_order write_data_interleave; do
            for c in mirror-precise shared-precise; do
                for n in 1 2; do
                    run $b $t $c $n validation/$b VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
                        VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true \
                        VK_KHRONOS_VALIDATION_REPORT_FLAGS=error,warn,info
                done
            done
        done
    done
    ;;
esac
