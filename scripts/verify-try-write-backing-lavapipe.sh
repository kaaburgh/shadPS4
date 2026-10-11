#!/usr/bin/env bash
# Regression oracle for MemoryManager::TryWriteBacking across multiple VMAs.
#
# Required:
#   SHADPS4=/absolute/path/to/shadps4
#   OPEN_TEST=/absolute/path/to/shadps4-open-test (with fallback_in_shared built)
# Optional:
#   EXPECT=pass|fail       pass: candidate fix; fail: unfixed negative control
#   VERIFY_SHARED=0|1     1: also check shared Precise on an E3 build
#   LOG_DIR=/path         preserve output for review
#
# Invoke with "bash scripts/verify-try-write-backing-lavapipe.sh".
set -euo pipefail

if [[ -z ${SHADPS4:-} || -z ${OPEN_TEST:-} ]]; then
    echo "Set SHADPS4=<built emulator> and OPEN_TEST=<shadps4-open-test checkout>" >&2
    exit 2
fi
EXPECT="${EXPECT:-pass}"
VERIFY_SHARED="${VERIFY_SHARED:-0}"
if [[ "$EXPECT" != pass && "$EXPECT" != fail ]]; then
    echo "EXPECT must be pass or fail" >&2
    exit 2
fi
if [[ "$VERIFY_SHARED" != 0 && "$VERIFY_SHARED" != 1 ]]; then
    echo "VERIFY_SHARED must be 0 or 1" >&2
    exit 2
fi
SHADPS4="$(realpath -- "$SHADPS4")"
OPEN_TEST="$(realpath -- "$OPEN_TEST")"
LOG_DIR="${LOG_DIR:-$PWD/try-write-backing-logs}"
if [[ ! -x "$SHADPS4" || ! -f "$OPEN_TEST/out/fallback_in_shared/fallback_in_shared.elf" ]]; then
    echo "Missing emulator binary or built fallback_in_shared. See the verification guide." >&2
    exit 2
fi
mkdir -p -- "$LOG_DIR"

run_case() {
    local mode="$1"
    local log="$LOG_DIR/$mode-precise.log"
    local rc=0
    local -a args=(fallback_in_shared --readbacks precise)
    [[ "$mode" == shared ]] && args+=(--shared-backing)

    # The test runner intentionally returns 1 for a guest FAIL and 2 for an
    # infrastructure error. Keep both outputs for an old-vs-new comparison.
    (cd "$OPEN_TEST" && bash scripts/run-test-lavapipe.sh "${args[@]}") >"$log" 2>&1 || rc=$?
    echo "$mode/Precise: runner rc=$rc; log=$log"

    local expected_status=PASS expected_rc=0
    if [[ "$EXPECT" == fail ]]; then
        expected_status=FAIL
        expected_rc=1
    fi
    if (( rc != expected_rc )); then
        echo "Unexpected runner code (wanted $expected_rc)" >&2
        tail -n 40 "$log" >&2
        return 1
    fi
    if ! grep -Eq "HOST_RESULT[[:space:]]+SHADTEST[[:space:]]+name=fallback_in_shared[[:space:]]+status=$expected_status([[:space:]]|$)" "$log"; then
        echo "Missing expected guest $expected_status marker" >&2
        tail -n 40 "$log" >&2
        return 1
    fi
    for name in chunk_straddle control; do
        if ! grep -q "case $name: ok" "$log"; then
            echo "Control case $name is not OK" >&2
            tail -n 40 "$log" >&2
            return 1
        fi
    done
    local expected_case=ok
    [[ "$EXPECT" == fail ]] && expected_case=FAIL
    if ! grep -q "case noncontiguous: $expected_case" "$log"; then
        echo "Unexpected noncontiguous case; wanted $expected_case" >&2
        tail -n 40 "$log" >&2
        return 1
    fi
    if [[ "$mode" == shared ]] && ! grep -q 'UMA shared backing enabled' "$log"; then
        echo "Shared mode was not enabled in shadPS4; use the E3 research build" >&2
        return 2
    fi
    grep -E 'case (chunk_straddle|noncontiguous|control):|HOST_RESULT SHADTEST' "$log" | tail -n 5
}

run_case mirror
if [[ "$VERIFY_SHARED" == 1 ]]; then
    run_case shared
fi

echo "Verified EXPECT=$EXPECT (Precise readbacks)."
