WRITE_DATA ordering with UMA shared backing: raw RADV results from the M75n
==========================================================================

Date: 2026-11-17 (step 2 baseline 07:2x UTC, stage 3 runs 12:0x UTC)
Machine: M75n, AMD Ryzen 3 3300U, AMD Radeon Vega 6 Graphics (RADV RAVEN)
Driver: RADV, Mesa 25.0.7-2+deb13u1, Vulkan 1.4.305
Kernel: 6.12.117+deb13-amd64 (Debian 13)
Validation layers: vulkan-validationlayers 1.4.309.0-1
Workspace on the machine: /root/Projects/shadps4/e1c-wd-order (paths in the logs)

Sources
-------
shadPS4 baseline: c364436e1e759b781631c7a3e85c801060f8936a (PR #54 head)
shadPS4 fix:      2366205a6d363f304df183ef7bcbc99f92ef8f6e
                  (8971952 -> 5e41afe -> 2366205 on c364436; 2366205 only adds docs,
                  src/ is identical to 5e41afe)
open-test:        8296f5ec6f19ab18a85d4438722cbd3124a68485
                  (branch claude/project-thread-e00v7o-write-data-interleave)
Guest toolchain:  .deps copied from an earlier open-test tree with an identical deps.lock;
                  guest tests built with scripts/build-test.sh and /usr/lib/llvm-19/bin
                  first in PATH (build-tests-*.txt).

Binaries (not included)
-----------------------
baseline  build/shadps4      119539160 bytes  sha256 d22d29489c124d46d5ef89106cd1282bd28a35f4c8237a3497d9709e1304710e
fix       build-fix/shadps4  119593712 bytes  sha256 c87a180a2a9b759b7dd76d0fd643466f88b27115f6d6cabacf54d124b55a11d2
step 2 baseline binary (an existing PR #54 build, source c364436):
          /mnt/UsbSSD447G/uma-e3-radv-review54-u2jbnp/build-intermediate-Release/shadps4
          sha256 88cf07ec44d9d192546ff8b83561ddcb945e184c8eaf053fc821c1e872fa7c29

Build configuration (build.sh): CMAKE_BUILD_TYPE=Release with CMAKE_CXX_FLAGS_RELEASE="-O0 -DNDEBUG"
and CMAKE_C_FLAGS_RELEASE="-O1 -DNDEBUG" (as the earlier PR #54 RADV builds), clang-19/clang++-19,
-fuse-ld=lld, Ninja, ccache, link pool 1, ENABLE_UPDATER=OFF, ENABLE_DISCORD_RPC=OFF, -j4.
Wall time: baseline 2495 s, fix 2753 s (build-baseline.txt, build-fix.txt).

How the tests were run
----------------------
radv.sh starts a private headless weston (VK_DRIVER_FILES=radeon_icd.json, SDL_VIDEO_DRIVER=wayland,
SDL_AUDIO_DRIVER=dummy) and runs
  python3 open-test/scripts/run-test.py TEST --shadps4 BIN --readbacks MODE [--shared-backing] --timeout 120
run-stage3.sh holds the matrix; commands.txt lists every command line. Every log shows
GPU_Model "AMD Radeon Vega 6 Graphics (RADV RAVEN)". All shared-config logs (42 per binary) show
"shared backing: on" and "UMA shared backing enabled: backend=dma-buf"; no mirror log has shared
backing on.

Layout
------
baseline-c364436/TEST.CONFIG.N.txt    stage 3 runs of the baseline (+ .weston.txt)
baseline-c364436/step2/               earlier step 2 runs with the review54 binary of c364436
fix-2366205/TEST.CONFIG.N.txt         stage 3 runs of the fix
validation/{baseline,fix}/            runs under VK_LAYER_KHRONOS_validation with sync validation
validation/canary-probe.txt           validation layer canary (tools/uma_gpuvm_probe)
stage3-results.txt                    one HOST_RESULT line per stage 3 run
Weston logs are included (all of them together are about 0.5 MB). *.log files were renamed to .txt.

Results (stage 3)
-----------------
| test                  | config          | baseline c364436                                   | fix 2366205 |
| write_data_order      | mirror-precise  | PASS 5/5                                           | PASS 5/5    |
| write_data_order      | shared-precise  | FAIL 5/5 a_saw_later_write (large_a, small_a)      | PASS 5/5    |
| write_data_order      | shared-disabled | FAIL 5/5 a_saw_later_write (large_a, small_a)      | PASS 5/5    |
| write_data_interleave | mirror-precise  | PASS 5/5                                           | PASS 5/5    |
| write_data_interleave | shared-precise  | FAIL 5/5 saw_later_write (interleave, gpu_then_cp) | PASS 5/5    |
| write_data_interleave | shared-disabled | FAIL 5/5 saw_later_write (interleave, gpu_then_cp) | PASS 5/5    |

Eleven neighbour tests, one run per configuration; the fix gives the same result as the baseline in
all 33 runs:
- PASS in mirror-precise, shared-precise and shared-disabled: dma_order, map_unmap_reuse,
  buffer_compute_roundtrip, buffer_pingpong, pm4_controls, ordered_async_handoff, uma_churn.
- cond_exec_gpu_predicate: mirror PASS; shared-precise and shared-disabled FAIL branch_skipped
  (gpu_pred_1, gpu_pred_0), the record-time predicate read of issue #11.
- dma_alias_order: mirror FAIL a_dispatch_after_fill (contiguous_fill, contiguous_copy,
  noncontiguous_fill, noncontiguous_copy); shared-precise and shared-disabled FAIL
  a_dispatch_after_fill (noncontiguous_fill, noncontiguous_copy).
- physical_alias: mirror FAIL gpu_stale_alias (cpu_to_gpu_again, gpu_to_cpu, gpu_to_gpu);
  shared-precise and shared-disabled PASS.
- fallback_in_shared: mirror-precise and shared-precise PASS; shared-disabled FAIL
  gpu_write_not_in_guest_memory (chunk_straddle, noncontiguous).

Fix log lines: "CpWriteData: UMA shared backing: first CP write on the GPU timeline" appears in 22 fix
logs (write_data_order and write_data_interleave shared-precise and shared-disabled, 5 each; dma_order
shared-precise and shared-disabled) and in no mirror or baseline log. No log contains
"waits for the GPU".

Step 2 (review54 binary of c364436, 3 runs): write_data_order mirror-precise PASS 3/3,
mirror-disabled FAIL unwritten 3/3, shared-precise and shared-disabled FAIL a_saw_later_write
(large_a, small_a) 3/3; cond_exec_gpu_predicate mirror-precise PASS, other configs FAIL
branch_skipped.

Validation
----------
Environment: VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true
VK_KHRONOS_VALIDATION_REPORT_FLAGS=error,warn,info.
Canary: the layer prints "Current Enables: VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT"
and reports the planted VUID-VkBufferCreateInfo-None-09500.
Runs: write_data_order and write_data_interleave, mirror-precise and shared-precise, 2 runs each, both
binaries (16 logs, all with the sync enable line). Per log: "VUID-" 4, "SYNC-HAZARD" 0,
"Validation Error" 2. The only id in baseline and fix is VUID-VkPipelineShaderStageCreateInfo-pNext-02755
(compute stage not in requiredSubgroupSizeStages). No id differs between baseline and fix. Test results
under validation equal those without it.

Limits
------
1. Synchronization validation tracks hazards per Vulkan object. A write through the shared chunk buffer
   and a read through another buffer that aliases the same imported memory is not reported as a hazard,
   so "SYNC-HAZARD 0" is weak evidence. The canary has no planted sync hazard, so sync-hazard detection
   itself is not demonstrated.
2. Neighbour tests ran once per configuration and the WRITE_DATA tests five times; one build per binary,
   no rebuild.
