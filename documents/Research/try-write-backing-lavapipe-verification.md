# Reproduce and validate `MemoryManager::TryWriteBacking` across VMAs

Related: [kaaburgh/shadPS4#14](https://github.com/kaaburgh/shadPS4/issues/14), [UMA roadmap #15](https://github.com/kaaburgh/shadPS4/issues/15), [open-test B4](https://github.com/kaaburgh/shadps4-open-test/blob/dfc749fa8837f81922d194a965f46738a3c42146/tests/fallback_in_shared/src/main.c).

## What changes

`TryWriteBacking` writes one incoming buffer into potentially multiple VMA / physical backing areas. The old loop reduces `size` but reuses the same `data` pointer after every `memcpy`. This PR advances the source by `copy_size`, with no other memory-management changes.

The existing `fallback_in_shared` guest scenario B4 contains a two-page noncontiguous virtual range backed by two independent direct-memory allocations. On shadPS4 E3 `8d07f08`, the test previously reproduced the corruption **in mirror Precise and shared Precise**; a temporary build with this exact one-line fix passed. Neither the test nor the host importer needs modification.

## Requirements

Ubuntu/Linux with a compiler and shadPS4 build dependencies (see [Linux build guide](../building-linux.md)), OpenOrbis/OpenGNM test deps and Mesa lavapipe/Xvfb. These exact instructions have **not yet been executed on GPU/lavapipe in this PR**.

Prepare once (skip steps already done):

```bash
sudo apt install -y mesa-vulkan-drivers xvfb
git clone https://github.com/kaaburgh/shadps4-open-test.git
cd shadps4-open-test
bash scripts/bootstrap-deps.sh
bash scripts/build-test.sh fallback_in_shared
```

If you are using your existing `shadps4-open-test` checkout, make sure it includes PR #23 (merged in `main` at `dfc749f`) and rebuild B4. The scripts use `LVP_ICD` (default `/usr/share/vulkan/icd.d/lvp_icd.json`), choose a private Xvfb display and configure an isolated shadPS4 profile with `readbacks_mode=Precise` and linear-image readback enabled.

## Fast path: validate the actual PR branch against fork main

In a **separate shadPS4 checkout**:

```bash
git clone --recursive https://github.com/kaaburgh/shadPS4.git shadPS4
cd shadPS4
git switch --track origin/fix/try-write-backing-source-offset
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build --parallel "$(nproc)"

# Set OPEN_TEST to the absolute path of the shadps4-open-test checkout.
SHADPS4="$PWD/build/shadps4" OPEN_TEST=/absolute/path/to/shadps4-open-test \
    bash scripts/verify-try-write-backing-lavapipe.sh
```

Expected: `mirror/Precise`, all three cases (`chunk_straddle`, `noncontiguous`, `control`) `ok`, `SHADTEST ... status=PASS`, command exit 0. If this **older fork main** cannot run B4, classify it as an infrastructure/branch compatibility limitation (exit 2 or no marker), **not** as a verified pass or fix failure. The already observed reproducer is the E3 branch below.

## Definitive A/B: reproduce the observed E3 failure and test the identical fix

Run from the same shadPS4 checkout (with the PR branch fetched). Worktrees keep the baseline untouched:

```bash
git fetch origin main research/uma-e3-shared-backing fix/try-write-backing-source-offset
git worktree add --detach ../shadPS4-e3-before origin/research/uma-e3-shared-backing
git worktree add --detach ../shadPS4-e3-after  origin/research/uma-e3-shared-backing

# Apply ONLY the one-line implementation commit from this PR to the E3 baseline.
git -C ../shadPS4-e3-after cherry-pick 05b3ad4786c1d1f91f9c84d4e8f7cfbf7401630b

for tree in ../shadPS4-e3-before ../shadPS4-e3-after; do
    git -C "$tree" submodule update --init --recursive
    cmake -S "$tree" -B "$tree/build" \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    cmake --build "$tree/build" --parallel "$(nproc)"
done

# Run from the original shadPS4 PR branch checkout:
OPEN_TEST=/absolute/path/to/shadps4-open-test \
SHADPS4="$(realpath ../shadPS4-e3-before/build/shadps4)" \
EXPECT=fail VERIFY_SHARED=1 \
    bash scripts/verify-try-write-backing-lavapipe.sh

OPEN_TEST=/absolute/path/to/shadps4-open-test \
SHADPS4="$(realpath ../shadPS4-e3-after/build/shadps4)" \
EXPECT=pass VERIFY_SHARED=1 \
    bash scripts/verify-try-write-backing-lavapipe.sh
```

Before (`EXPECT=fail`): both mirror/Precise and shared/Precise have `chunk_straddle: ok`, `control: ok`, `noncontiguous: FAIL`, and a guest FAIL marker; the negative-control runner returns 0 **only if that expected failure is actually observed**. After (`EXPECT=pass`): all three cases `ok` and the guest PASS marker in both modes.

The verifier preserves complete per-mode logs as `try-write-backing-logs/mirror-precise.log` and `shared-precise.log` in the caller's current directory; use different `LOG_DIR` values for before/after if keeping both runs. An E3 shared run must also log `UMA shared backing enabled`. **This log line alone does not demonstrate actual imported chunks**; use the UMA census for backend coverage questions (which are out of scope for #14).

## Why not run the entire six-mode matrix as the sole acceptance gate?

The remaining four modes (mirror/shared × Relaxed/Disabled) are deliberately **R / nondiagnostic** for the GPU-written fallback ranges: those modes do not return GPU writes to guest memory. In E3 shared Relaxed/Disabled the shareable control remains visible, while the fallback ranges do not. The `run-matrix.sh` runner also intentionally returns zero when a guest reports FAIL (only infra errors force exit 2). Thus merely checking the shell exit code of the six-mode script **would produce a false green**.

The verifier above explicitly checks per-case output and the semantic guest marker. A full six-mode run remains useful as supplementary evidence:

```bash
cd /absolute/path/to/shadps4-open-test
SHADPS4=/absolute/path/to/shadps4 bash scripts/run-matrix.sh fallback_in_shared
# Examine the six saved logs in out/fallback_in_shared/matrix-*.log.
```

## Review checklist

- [ ] Source pointer increases exactly by `copy_size` after each copied physical area and is carried across VMA boundaries.
- [ ] E3 before/fixed reproduce expected per-case results in mirror Precise; ideally shared Precise too.
- [ ] Verify B4 reports no emulator `<Critical>` and no infrastructure timeout.
- [ ] Record emulator + open-test git SHAs, Mesa/lavapipe version and runner logs in PR discussion.
- [ ] Optional: `image_buffer_alias`, `buffer_compute_roundtrip` or another readback test still passes its existing diagnostic cases; don't expand scope into #4 or UMA ownership redesign.
