<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# UMA GPUVM probe

Standalone Vulkan probe for the zero-copy direction proposed in the
[PR #54 architecture review](https://github.com/kaaburgh/shadPS4/pull/54).
It does not build or link the emulator.

The question is whether the host GPU page tables can mirror the guest mapping.
In that model, imported guest physical memory is bound with `vkQueueBindSparse` into a
sparse buffer laid out by guest VA, the way the PS4 GPUVM maps the same physical pages
the CPU sees. If the driver supports it, aliases, physically noncontiguous mappings,
chunk boundaries and remaps become bind operations. SharedBacking would no longer need
per-request lookups, mirror demotion, or mixed shared/mirror aliases.

```sh
cc -O1 -g -o uma_gpuvm_probe tools/uma_gpuvm_probe/uma_gpuvm_probe.c -lvulkan
./uma_gpuvm_probe                     # auto: control, dma-buf if /dev/udmabuf is usable, host pointer
./uma_gpuvm_probe --backend dmabuf --mib 16 --bw-mib 64
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./uma_gpuvm_probe
```

`--device N` selects the physical device. Exit status is 0 when every test that ran passed,
1 on any FAIL, and 2 on a setup error.

| Test | What it shows |
| --- | --- |
| `Q` | `vkGetPhysicalDeviceExternalBufferProperties` for host-pointer and dma-buf, plain vs sparse buffers. |
| `S1` | A sparse "guest VA" buffer bound to imported memfd backing, with every VA block mapped to a noncontiguous physical block and one alias. The GPU copy returns the right bytes. |
| `S2` | A GPU fill through a VA block lands at its physical offset in the memfd mapping. |
| `S3` | Remap: rebinding one VA block to another physical block changes what the GPU reads. |
| `S1–S3 control` | The same tests on driver-allocated memory. A control PASS with an import FAIL means the driver can sparse-bind but not with imported memory. |
| `B1` | Copy bandwidth for imported↔device-local and device-local↔device-local (information only). This feeds the placement policy. On an APU every path uses the same DRAM; on a discrete GPU imported memory goes over PCIe. |
| `E1` | Inverse ownership. The driver allocates host-visible memory exported as a dma-buf, and the CPU maps it in 16 KiB pieces at permuted offsets into one VA range, as AddressSpace maps guest pages. CPU writes are visible to the GPU. |

`bind:` lines report the time of each `vkQueueBindSparse` plus queue idle. That is the
cost of a guest map or unmap under the GPUVM model.

## Results so far

lavapipe (Mesa 25.2.8, LLVM 20.1.2), 8 MiB, 4 KiB sparse blocks, validation clean:

- Control S1–S3 PASS: the probe logic is sound.
- Host-pointer S1–S3 FAIL. lavapipe reports host-pointer memory as importable for sparse
  buffers, but binding it does not alias the imported pages. lavapipe is therefore not an
  oracle for this path. RADV and NVIDIA must answer it.
- E1 SKIP: lavapipe does not export dma-buf.

## Checks to run on M75n (RADV)

1. `./uma_gpuvm_probe --backend dmabuf --mib 16` with validation. Needs `/dev/udmabuf`
   access, which is the `kvm` group on the M75n. Record `Q`, S1–S3, and the `bind:` timings.
   - S1–S3 PASS means the GPUVM model works on RADV.
   - If `Q` reports the sparse case as not importable and S1 fails while the control passes,
     only RADV's sparse + external path is missing. The kernel can map any BO into the VM,
     so this is a Mesa change, not a hardware limit.
2. Same with `--backend host`. This is expected to fail at import, because RADV registers
   userptr BOs for anonymous memory only and the backing is a shared memfd. Record the
   reason.
3. `E1`. The driver-owned backing alternative needs dma-buf export of a cached coherent GTT
   type plus `mmap` of that fd at offsets.
4. `B1 --bw-mib 64`. Compare the imported (snooped GTT) bandwidth with device-local
   (the carve-out). This decides whether Garlic-type guest memory should prefer
   non-snooped memory.
5. GTT accounting. Run S1 with `--mib 256`, raising `udmabuf.size_limit_mb` if needed,
   and read `mem_info_gtt_used` before and after. The PR #54 numbers already show
   imports counting against GTT (+64.5 MiB for 4 × 16 MiB).

NVIDIA items are in [`docs/research/nvidia-check-queue.md`](../../docs/research/nvidia-check-queue.md)
(NV-5, NV-6).
