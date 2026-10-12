# Ten upstream-only, apparently unclaimed agent tasks for lavapipe

**Prepared:** 2026-10-13 (Europe/Berlin)  
**Upstream source snapshot:** [shadps4-emu/shadPS4 `23c41333822fe8eca4c609b43119180af63ae4ac`](https://github.com/shadps4-emu/shadPS4/tree/23c41333822fe8eca4c609b43119180af63ae4ac) (2026-10-12 21:07 UTC)  
**Planning PR target:** kaaburgh/shadPS4 `main` (**documentation only**, not a port of any upstream code)

## Selection and freshness rules

- **Candidate discovery used upstream only:** the upstream commit/file snapshot; GitHub upstream open PRs, updated PRs and issues; Supabase **`shadPS4 upstream watch`** records (particularly daily records 120 and 138 and hourly records 119–140, spanning 2026-10-11/12). The fork and other emulator projects **were not used to select candidates**.
- Searched upstream PRs for the targeted components/operations, checked the open PR inventory (78 results at review time), and searched updated PRs since **2026-10-06**. There is **no visible, directly matching open/recent PR** for the named HLE modules or fixes below. This is a **negative evidence screen, not proof nobody is developing a private branch**, nor a guarantee that nearby generic refactoring will not touch these files.
- **Intentionally excluded from selection**: shader execution and compilation, compute, PM4/CE, Vulkan memory/image coherency, GPU GC, network/NP, save-data, USB, gamepad/touchpad, and input-wide refactors, where current/nearby upstream activity exists (#5355, #5190, #5357, #5168, #5280, #3396, #5362, #5350, #5226, #5338, #5238 and others). Also excluded console-language configuration (#5326 recently addressed neighboring font policy), screenshot implementation (recent screenshot pipeline PR #4907), and libSceKeyboard (recent input-side changes).
- **Scope here is a worklist, not implemented fixes.** A line-level code finding is marked **observed**; assumptions about exact PS4 return codes, game compatibility or untested behavior are marked **hypotheses**.

## Universal execution / acceptance contract

Each item can be delegated to one agent as an **independent** small PR after a freshness check. Before writing the fix:

1. Freeze upstream SHA and query upstream **open PRs and last 7 days of merged/updated PRs**, including changed files, for overlap. **Stop or re-scope** on evidence of overlapping work. Do not open an upstream PR with an agent.
2. Build a **red-before/green-after** host test where practical and a **freely distributable minimal OpenOrbis/OpenGNM guest executable** targeting the affected API. Tests must not embed Sony SDK, proprietary PRX/assets, commercial games or copied vendor binaries. If building a guest reproducer is impossible within the limited task, mark the integration gate **NOT VERIFIED**; do not claim successful lavapipe validation from unit tests.
3. Boot the guest via shadPS4 under **Mesa lavapipe** (prefer Xvfb/llvmpipe Vulkan), record exact shadPS4/guest SHA, Mesa/ICD version, command, timeout, exit code and JSON or marker-based guest oracle. Use canary guards and nonzero failure exits; capture sanitizers in separate host test runs when possible.
4. The agent should run a control with a valid request, one invalid/edge case, repeat the case, and check that unrelated functionality does not regress. **PASS** means an observed oracle, **FAIL** means mismatched/crash/hang, **UNVERIFIED** means no executable or no usable oracle. A clean build or boot alone is not a pass.
5. **PS4 ABI uncertainty gate:** do not guess an undocumented signature or error code. First inspect upstream headers and publicly licensed evidence; distinguish emulator-internal safety fixes from claims about real-hardware semantics. Where necessary defer precise API behavior until a lawful hardware reference is available.
6. Keep changes strictly confined to the nominated subsystem; no speculative framework, generalized refactor or default-on emulator behavior outside the test's requirements. Re-evaluate upstream ownership immediately before implementing.

## Ranked candidates

### 1. [P0 | observed] Bound the PNG decoder's input callback and recover cleanly from truncated data

- **Evidence:** [`src/core/libraries/libpng/pngdec.cpp`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/core/libraries/libpng/pngdec.cpp), `scePngDecDecode` and `scePngDecParseHeader`: both `png_set_read_fn` callbacks `memcpy(..., len)` and increase `offset` without checking `size - offset`. ParseHeader reads eight signature bytes before checking input length. `PngDecError` logs errors without a documented local recovery path.
- **Agent task:** add overflow-safe source-span checks and a libpng-compatible error/recovery path, including malformed/truncated signature, IHDR and IDAT and NULL output/handle cases. Preserve valid-image behavior; do not claim a particular Sony error code without ABI evidence.
- **Oracle on lavapipe:** a guest decodes a tiny known valid PNG and several truncated variants. Valid pixels match the fixture; invalid input returns an error without a crash, overread, hang or canary change. Host ASan/UBSan confirms memory safety.
- **Estimated scope:** small–medium; high reproducibility, high confidence in unsafe read.

### 2. [P0 | observed] Validate PNG decode output dimensions, pitch and destination capacity

- **Evidence:** same `pngdec.cpp`: `scePngDecDecode` computes `horizontal_bytes = num_channels * width`, selects a stride from `image_pitch`, and calls `png_read_row` across image height, but does not visibly validate `image_mem_size` or `stride >= transformed row bytes`. Multiplication uses signed 32-bit intermediates.
- **Agent task:** use the **post-transform** libpng row-byte count, checked arithmetic, and supplied image capacity to reject small pitch/output buffers before writing; handle interlace passes and avoid partial overwrite on rejected descriptors where feasible.
- **Oracle:** 1×1, odd-width, padded-pitch and interlaced fixtures; undersized output by one byte, zero pitch, huge dimensions. On invalid sizes, error and unchanged canaries; on valid sizes, exact expected pixels.
- **Estimated scope:** small–medium; independent of task 1 although touching same file.

### 3. [P0 | observed] Make PNG encode validate pointers and source layout before reading rows

- **Evidence:** [`pngenc.cpp`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/core/libraries/libpng/pngenc.cpp): `scePngEncEncode` logs `param->...` **before** checking `param == nullptr`; the RGBA loop uses `width * 4` contiguous rows while the RGB loop uses `image_pitch`; neither visibly validates `image_mem_size` against all row reads. Output-capacity checking already exists and should be **preserved**, not rewritten.
- **Agent task:** reorder null checks, calculate layout/capacity with checked arithmetic, handle supported padded rows consistently, and reject short source buffers without reading beyond them. Avoid changing compression behavior.
- **Oracle:** host ASan plus guest round-trip against a PNG fixture for padded and contiguous RGBA/RGB and invalid pointers/short source. Decode result equals expected pixel array, both canaries intact.
- **Estimated scope:** small; high confidence for null dereference, medium for pitch semantics (must verify ABI).

### 4. [P0 | observed] Make RTC string parsing total for malformed and short inputs

- **Evidence:** [`rtc.cpp`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/core/libraries/rtc/rtc.cpp), `sceRtcParseDateTime` reads `dateTimeString[19]` without minimum-length check, and `sceRtcParseRFC3339` uses fixed-index access and `std::stoi` without local validation/exception conversion.
- **Agent task:** validate length, field boundaries and numeric input before parsing; reject invalid calendar values via existing RTC validation. Preserve accepted RFC3339/RFC2822/asctime inputs.
- **Oracle:** guest tests a fixed table of valid timestamps plus empty, 1–18-character, truncated offset, non-numeric and impossible-date strings. No exception, crash or output mutation on invalid input; repeatability under lavapipe.
- **Estimated scope:** small–medium; high confidence.

### 5. [P0 | observed] Reject NULL network-tick inputs before RTC dereferences

- **Evidence:** same `rtc.cpp`: `sceRtcSetCurrentAdNetworkTick`, `sceRtcSetCurrentDebugNetworkTick`, `sceRtcSetCurrentNetworkTick` dereference `pTick->tick` before branching on `pTick != nullptr`. The code also has an apparent intended NULL-forwarding path; its meaning must be resolved before changing externally visible errors.
- **Agent task:** make all three entry points safe for NULL and invalid epoch ticks, decide NULL behavior from actual API evidence, and avoid modifying other clock behavior.
- **Oracle:** direct host tests and a synthetic guest invoking each with NULL, valid tick, and boundary tick; no segmentation fault, stable documented result, and unchanged guest clock on rejected calls.
- **Estimated scope:** very small; high confidence in crash path.

### 6. [P0 | observed] Enforce PlayGo initialization before reading chunk state

- **Evidence:** [`playgo.cpp`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/core/libraries/playgo/playgo.cpp), `scePlayGoGetChunkId`: checks handle/output fields but accesses `playgo->GetPlaygoHeader()` and `playgo->chunks` without an initialization guard; other PlayGo entry points explicitly return `ORBIS_PLAYGO_ERROR_NOT_INITIALIZED`.
- **Agent task:** audit the narrow PlayGo query surface for unchecked `playgo` accesses; add consistent initialization guards without changing chunk scheduling or installation logic.
- **Oracle:** guest calls before initialization, after init/close where applicable, and with bad handles; correct deterministic errors, no crash, same valid chunk enumeration. Fake/minimal metadata is permitted if freely generated.
- **Estimated scope:** small; high confidence for the uninitialized dereference.

### 7. [P0 | observed] Validate SharePlay connection-info pointers on the dormant path

- **Evidence:** [`shareplay.cpp`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/core/libraries/share_play/shareplay.cpp), `sceSharePlayGetCurrentConnectionInfo` unconditionally performs `memset(pInfo, 0, sizeof(*pInfo))`. This is currently the already implemented, deterministic `DORMANT` status path.
- **Agent task:** check pointer before touching it and return an ABI-supported invalid-parameter error; do **not** attempt network streaming or reimplement the many unrelated SharePlay stubs.
- **Oracle:** guest queries valid structure (entire structure defined and status dormant) and NULL (error, no crash); no network traffic required. Host sentinel tests check no out-of-bounds writes.
- **Estimated scope:** very small; error code needs evidence.

### 8. [P0 | observed] Validate Remote Play status output pointers

- **Evidence:** [`remoteplay.cpp`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/core/libraries/remote_play/remoteplay.cpp), `sceRemoteplayGetConnectionStatus(userId, pStatus)`: writes `*pStatus = ...DISCONNECT` without a NULL check. This is a discrete HLE API from SharePlay.
- **Agent task:** add the minimal pointer/user-ID validation consistent with the API, retaining the unsupported/always-disconnected implementation. No Remote Play transport.
- **Oracle:** guest calls valid and NULL output, bad and valid user IDs. Expected disconnected result for a valid query; no crash for invalid arguments. Host canaries unchanged.
- **Estimated scope:** very small; user-ID/return-code semantics require confirmation.

### 9. [P1 | observed implementation gap] Give VideoRecording metadata validation a defined contract

- **Evidence:** [`video_recording.cpp`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/core/libraries/video_recording/video_recording.cpp), `sceVideoRecordingSetInfo` currently reports STUBBED, checks enum ranges and non-null `info`, but **never reads `info_len`** and returns `ORBIS_OK` for accepted types regardless of supplied length. [`video_recording.h`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/core/libraries/video_recording/video_recording.h) defines certain metadata structures.
- **Agent task:** first establish documented minimum lengths for a **small supported subset** (e.g. `UserMeta` if confirmed), reject undersized buffers, and avoid out-of-bounds access or claiming that metadata is persisted when it is not. Stop if the ABI cannot be established with public sources.
- **Oracle:** guest calls one known type with valid/short/zero size and invalid enum; return code and observable state conform to documented contract, no crash and stable repeated behavior.
- **Estimated scope:** small if ABI evidence available; otherwise research-only/UNVERIFIED, never invent error semantics.

### 10. [P1 | hypothesis from open issue] Unicode IME guest-to-UI-to-guest round-trip

- **Evidence:** [upstream issue #4403](https://github.com/shadps4-emu/shadPS4/issues/4403) requests additional alphabet support; [`ime.cpp`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/core/libraries/ime/ime.cpp) and [`ime_ui.cpp`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/core/libraries/ime/ime_ui.cpp) already convert UTF-16/UTF-8 and manage caret indices. **A Unicode failure has not been established**: this must begin as a targeted reproduction rather than an assumed missing feature.
- **Agent task:** create a tiny homebrew using existing `sceImeOpen/SetText/SetCaret/Update/Close` APIs, round-trip Cyrillic, CJK and surrogate pairs, including max-length and caret events. If all cases already pass, stop with a regression-test-only PR; if not, fix only the demonstrated conversion/caret/callback defect. Do not build new keyboard layout or controller gesture features.
- **Oracle:** machine-readable echo of input Unicode scalar sequence and UTF-16 caret index, with strict byte comparison, across two open/close cycles. Under lavapipe/Xvfb, automate event delivery where supported; do not claim full PS4 keyboard fidelity.
- **Estimated scope:** medium; lower probability of an actual bug, potentially high compatibility benefit.

## Priority and work sequencing

| Order | Candidate | Confidence of code defect | Agent size | Lavapipe oracle |
| --- | --- | --- | --- | --- |
| 1 | PNG decode input span | High | S/M | Pixels, error, canaries |
| 2 | PNG decode destination capacity | High | S/M | Pixels, pitch, canaries |
| 3 | PNG encode input validation | High (null), medium (pitch) | S | Encode/decode round-trip |
| 4 | RTC malformed date strings | High | S/M | Fixed timestamp table |
| 5 | RTC network-tick null | High | XS | Return status / no crash |
| 6 | PlayGo state guards | High | S | Pre-init/initialized paths |
| 7 | SharePlay pointer validation | High | XS | Dormant/status/error |
| 8 | Remote Play pointer validation | High | XS | Disconnected/status/error |
| 9 | VideoRecording metadata size | Medium gap; ABI unknown | S or stop | Length contract |
| 10 | IME Unicode round-trip | Hypothesis | M | Guest string + caret |

**Size** estimates are relative, not elapsed-time promises. The first three PNG cases share files but test different failure mechanisms; RTC string parsing and network tick are independent entry points.

## What this worklist does *not* claim

- No item has been implemented, built or tested in this planning PR.
- The list makes **no claim** that a particular commercial game will improve.
- Lavapipe proves a specific host execution path for the synthetic guest; it **cannot** establish exact PlayStation 4 hardware behavior or vendor-specific Vulkan correctness.
- Search and Supabase historical monitoring can miss unannounced private work. Check again immediately before starting.
- A hypothetical API behavior must not be presented as a reproduced bug. Return codes, struct sizes, and unsupported-device behavior require public ABI evidence or a separately authorized hardware comparator.
