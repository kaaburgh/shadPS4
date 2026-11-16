<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# P0 GPUVM: guest 16 KiB mappings against the Vulkan sparse block

Question: the [architecture review of PR #54][pr54] proposed, as P0, to bind the imported guest
backing into the sparse guest-VA arenas when the guest maps memory ("GPUVM model"). The guest
maps memory in 16 KiB pages. Vulkan binds sparse buffers in blocks of the buffer alignment,
which is reported as 64 KiB on the target drivers ([issue #12][i12]). Can a sparse buffer indexed
by guest VA represent the guest memory map exactly, with no copies and no address translation in
shaders?

Code read at `c364436` (`research/uma-e3-radv-dmabuf`). Vulkan text read from Vulkan-Docs `main`
at `VK_HEADER_VERSION 368`. Everything below was checked with a CPU-only model
(`tools/uma_gpuvm_model/`) and, where marked, with the lavapipe runs of the [#56 probe][pr56].
**Nothing in this note was run on RADV or NVIDIA**, and no statement here claims that either
driver supports a path. Section 9 lists the hardware checks each recommendation depends on.

## Summary

1. **No, not in general.** With 64 KiB blocks and 16 KiB guest pages a sparse VA buffer bound to
   contiguous imports represents a block if and only if all its mapped pages lie on one
   contiguous physical run (section 4). With the imports PR #54 already has, which start at
   block multiples, it additionally needs `VA ≡ PA (mod 64 KiB)`. Neither is guaranteed by the
   guest contract or by the emulator.
2. **Counterexamples exist and are small.** Six of them are in the test
   (`tools/uma_gpuvm_model/test_gpuvm_model.py`, `Counterexamples`). The simplest: map 128 KiB
   at VA `0x40000` to PA `0x24000`. It is fully contiguous, PR #54 serves it today from a
   per-chunk buffer, and a sparse VA arena over block-aligned imports cannot bind a single block
   of it.
3. **The mismatch disappears when the block is not larger than the guest page.** With a
   16 KiB or 4 KiB block every backed map is represented exactly. This is why lavapipe (4 KiB)
   would hide the problem even if it aliased imported memory.
4. **A VA-indexed sparse arena is therefore not strictly better than the PA-indexed chunk
   buffers of PR #54.** The chunk buffers are insensitive to phase and to anything inside a
   block, because buffer offsets and device addresses have no alignment rule. The arena only
   adds something for ranges that are contiguous in VA and not in PA.
5. **Recommended P0** (section 8): keep the PA-indexed chunk buffers as the base layer, make the
   shader translation table 16 KiB granular (exact for every backed map), and use the sparse VA
   arena only as an optional accelerator for blocks that satisfy the condition. Both layers
   alias the same pages, so a block can leave the arena without moving data. Whether the arena
   is needed at all is a measurable question: `gpuvm_model.py census` answers it from existing
   UMA E0 captures.

This corrects one statement of the review. Comment A1 said `ObtainBuffer` could always return
`{arena, VA offset}` for importable memory, with "no contiguity fallback beyond the sparse block
alignment". That holds only for blocks that meet the condition of section 4.

## 1. Guest side: what a mapping can look like

Facts from the emulator code. "Guest page" below means 16 KiB.

| Property | Source | Guarantee |
| --- | --- | --- |
| Backing page table granularity | `AddressSpace::Traits::PAGE_BITS = 14`, `address_space.cpp:870` | 16 KiB |
| Direct memory allocation | `kernel/memory.cpp:51-55`, `MemoryManager::Allocate` (`memory.cpp:365`) | length multiple of 16 KiB; alignment 0 or a multiple of 16 KiB; default 16 KiB |
| Direct mapping (`sceKernelMapNamedDirectMemory`, `MapDirectMemory2`) | `kernel/memory.cpp:205-216`, `272-283` | length and physical address multiples of 16 KiB |
| Non-fixed VA choice | `memory.cpp:694-696` | `SearchFree` with the caller's alignment, default 16 KiB |
| Flexible memory | `memory.cpp:721-751` | VA and length 16 KiB; PA taken first-fit from `fmem_map`, possibly in several pieces |
| Pool memory | `PoolExpand` (`memory.cpp:323`), `PoolCommit` (`memory.cpp:500`) | VA, PA and length multiples of 64 KiB; a commit may take several physical pieces |
| Unmap | `memory.cpp:1043-1044` | address rounded down and length rounded up to 16 KiB |
| Fixed map over a live mapping | `memory.cpp:704-705` | old range is unmapped first (remap) |
| Physical aliasing | `memory.cpp:669` (`validate_dmem`), `sceKernelEnableDmemAliasing` | the same direct memory may be mapped at several VAs unless validation is on |
| Importable backing | `HasPhysicalBacking` (`memory.h:329`) | Direct, Flexible, Pooled only; Code, Stack, File, System are private anonymous memory |
| What reaches the GPU cache | `IsValidGpuMapping` (`memory.h:210`) | every mapping below 2^40, whatever its type or protection |

What follows for a 64 KiB block:

- **Nothing ties VA to PA modulo 64 KiB** for direct memory. The guest picks the physical
  address at allocation and the VA at mapping, each with its own alignment. The difference
  `(PA − VA) mod 64 KiB`, called the *phase* below, can be 0, 16, 32 or 48 KiB.
- **One block can hold pages of several mappings.** Two 16 KiB-aligned mappings can meet inside
  a block, a fixed map can replace one page of it, an unmap can remove one page of it.
- **Whether the pages of one block lie on one physical run is not guaranteed.** It holds when
  allocation and mapping advance in step (the same anchor continues across the boundary,
  scenario `adjacent_same_anchor`). It fails as soon as the physical allocations are not
  adjacent (`adjacent_other_anchor`).
- **Only map calls change the page table.** Sub-allocation inside a mapping never does. So the
  incidence of irregular blocks is a property of the map/unmap call sequence, and the Map, Piece
  and Unmap events of the E0 census are sufficient to measure it.
- **Pool memory is always block-regular with phase 0** as the emulator implements it.
- **Flexible physical addresses are the emulator's free choice.** The guest cannot observe
  them. Today they are first-fit and may split inside a block.
- **A block that contains any non-backing page can never be shared** (code, stack, file
  mappings sit below 2^40 as well).

Two gaps in the emulator, relevant because the model assumes 16 KiB alignment throughout:

- The direct-map alignment check is `!has_single_bit(alignment) && !Is16KBAligned(alignment)`
  (`kernel/memory.cpp:216`, `283`). It accepts any power of two, including values below 16 KiB.
  `sceKernelReserveVirtualRange` (`kernel/memory.cpp:172`) uses `||` for the same check.
- A fixed `in_addr` is not checked for 16 KiB alignment in the direct-map calls, while
  `AddressSpace::Map` indexes by `virtual_addr >> 14`.

PS4 contracts. The 16 KiB page, the 16 KiB length and alignment rules and the 40-bit GPU address
range are taken from how shadPS4 encodes the kernel API. I did not verify them against hardware
or SDK documentation, and I do not know what the console does for an unaligned fixed address.
The question of what a title may assume about a remap before the GPU work that used the old
mapping has finished is also open (it is the same question as in E1B).

## 2. Vulkan side

### 2.1 What the specification requires

- **Block size.** For a sparse buffer, `VkMemoryRequirements::alignment` is "both the memory
  alignment requirement and the binding granularity". It is a power of two and is the same for
  buffers created with the same usage and flags.
- **Bind alignment.** `VUID-VkSparseMemoryBind-resourceOffset-09491`: for buffers,
  `resourceOffset`, `memoryOffset` and `size` must be integer multiples of that alignment. This
  one rule creates the whole problem: a block of VA maps to a block-aligned range *of one memory
  object*, linearly.
- **Bounds.** `-size-01098`, `-memoryOffset-01101`, `-size-01102`: the bound range lies inside
  the memory object.
- **Rebinding.** Allowed at any time through `vkQueueBindSparse` on a queue with
  `SPARSE_BINDING`. The application "must use synchronization primitives to guarantee that other
  queues do not access ranges of memory concurrently with a binding change". Other ranges may be
  accessed meanwhile. Binds "are not automatically ordered against command buffer execution,
  even within a single queue".
- **Unbound ranges.** Reads are undefined unless `residencyNonResidentStrict` is set (then they
  return zero and writes are discarded). Neither case is unsafe.
- **Aliasing.** Binding one memory range at several places is allowed. The data-consistency
  guarantee for sparse resources comes with `VK_BUFFER_CREATE_SPARSE_ALIASED_BIT`
  (`sparseResidencyAliased`). In every case "memory dependencies must be used to separate writes
  to one alias from reads or writes to another alias". Aliasing is defined per range of one
  `VkDeviceMemory`. Two different memory objects created over the same physical pages are
  outside what Vulkan describes.
- **External memory.** `VUID-VkBufferCreateInfo-pNext-00920`: the handle type must be reported
  as compatible for the buffer's flags, sparse flags included. `-memory-02730/-02731`: memory
  with an external handle type can only be bound to a resource created with that type.
  `VUID-VkMemoryDedicatedAllocateInfo-buffer-01436`: a dedicated allocation cannot name a sparse
  buffer, so a handle type reported as `DEDICATED_ONLY` for the sparse buffer rules the path
  out.
- **Import limits.** A host pointer and its size must be multiples of
  `minImportedHostPointerAlignment`, which may be as large as 64 KiB. An implementation "may
  fail to import the same payload multiple times" for host pointers. fd imports of the same
  payload are allowed and give distinct memory objects. `maxMemoryAllocationCount` is only
  guaranteed to be 4096.

### 2.2 What is driver specific

| Fact | Status |
| --- | --- |
| lavapipe: sparse block 4 KiB, emulator clamps to 16 KiB | measured (#56 probe, PR #54 notes) |
| lavapipe: accepts host-pointer memory in sparse binds and does not alias it | measured (#56 probe); lavapipe cannot answer any question below |
| RADV and NVIDIA: sparse block 64 KiB | reported in [issue #12][i12], not measured here |
| RADV, NVIDIA: imported memory in `vkQueueBindSparse` aliases the pages | **unknown**, queued as M75n run and NV-5 |
| Same pages imported twice at different offsets, both bound | **unknown** on every driver |
| `UDMABUF_CREATE_LIST` object bound sparsely | **unknown** |
| `minImportedHostPointerAlignment`, `sparseResidencyAliased`, `residencyNonResidentStrict` values | not recorded for the target drivers |
| Accounting of overlapping imports against the dma-buf pin budget and GTT | **unknown** |

## 3. Model

`tools/uma_gpuvm_model/gpuvm_model.py`, no Vulkan and no emulator dependency.

- Unit 4 KiB. Guest page `g`, host block `B`, backing size `N`, all powers of two (so `g`
  divides `B` or `B` divides `g`).
- `GuestMap`: VA page to PA page, or to `UNBACKED` for mapped memory that has no importable
  backing.
- Memory objects: `Linear(base, length)`, a contiguous view of the backing (one udmabuf range or
  one host-pointer import), and `Stitched(units)`, an arbitrary page list
  (`UDMABUF_CREATE_LIST`, or a host import of a guest VA range).
- `HostMap.bind` accepts exactly what section 2.1 allows and raises otherwise.
- *Anchor* of a block: `PA(unit) − offset of the unit in the block`. A block is *regular* when
  all its mapped units have one anchor, *irregular* otherwise.
- Import policies: `phase0` (imports start at block multiples: the chunk layout of PR #54),
  `multiphase` (imports may start at any guest page), `stitched` (additionally one list-built
  object per block that needs it).
- Two notions of "represents". *Strict*: mapped VA resolves to its PA, unmapped VA resolves to
  nothing. *Superset*: mapped VA resolves to its PA, unmapped VA inside a bound block may
  resolve to guest physical memory.

Superset exposure is what a real GPU never has. It is harmless for a correct title, which does
not issue GPU accesses to unmapped addresses. A title that does would read or write an unrelated
physical page instead of faulting.

## 4. Necessary and sufficient conditions

**Locality.** Binds are per block, so a map is representable iff each of its blocks is. Blocks
couple only through the number and layout of memory objects.

**C1, contiguous memory objects.** A block is representable (superset) iff it is empty, or

1. no page of it is non-backing memory,
2. it is regular, with anchor `a`,
3. `0 ≤ a ≤ N − B`.

*Why.* A bind sends VA unit `kB + i` to memory offset `o + i`, and a contiguous object with base
`m` sends that to PA `m + o + i`. So the block gets the single anchor `m + o`, which gives 2.
The object must contain `[o, o + B)` and lie in the backing, which gives 3. Conversely an import
that contains `[a, a + B)` with base `≡ a (mod B)` satisfies every bind rule. The anchor is
always a multiple of the guest page, so imports at finer offsets add nothing (the test's oracle
steps import bases by 4 KiB and agrees).

**C2, strict.** C1 and the block is fully mapped. A bound block exposes all of `[a, a + B)`.

**C3, block-aligned imports (PR #54 chunks).** C1 and `a ≡ 0 (mod B)`, that is
`VA ≡ PA (mod B)` for the pages of the block.

**C4, small block.** If `B ≤ g`, every map without non-backing pages is representable strictly
with block-aligned imports. Each block lies inside one guest page, so 2, 3 and the phase
condition hold by construction.

**C5, stitched objects.** With list-built objects every block without non-backing pages is
representable, strictly if unmapped positions get a placeholder page. The price is one extra
memory object per distinct irregular block and aliases across memory objects.

**Whole map.** A pure sparse GPUVM over the existing imports is exact iff every non-empty block
below 2^40 satisfies C3. With `B = 64 KiB` that is a property of pool memory only.

How this is checked: `ExhaustiveConditions` enumerates every map of one 64 KiB block over an
eight-page backing (6561 maps) and of two 32 KiB blocks (2401 maps), and compares the closed
form with a brute-force search over every import base, length and bind offset, for both import
policies and both modes. It also checks C4 and C5 by enumeration. Four mutants of the closed
form (phase check removed, irregular blocks accepted, backing edge ignored, strict ignored) each
make the tests fail.

## 5. Counterexamples at 64 KiB

Guest pages are 16 KiB. Each line is a test in `Counterexamples`.

| # | Guest map | Result |
| --- | --- | --- |
| CE1 | VA `0x40000` to PA `0x24000`, 128 KiB | phase 16 KiB. `phase0`: 0 of 2 blocks. `multiphase`: both, through a second import of the same pages. The PR #54 chunk buffer serves the whole range as is. |
| CE2 | VA `0x40000` to PA `0x20000` (32 KiB), VA `0x48000` to PA `0xA0000` (32 KiB) | irregular. No contiguous import represents the block. A consumer of the whole 64 KiB has neither a sparse source nor a chunk source. Needs a stitched object or a gather. |
| CE3 | PA `0x20000` mapped at VA `0x40000` and at VA `0x84000` | the second alias has phase 48 KiB. `phase0` shares one alias and falls back for the other, which makes the pages a mixed shared/mirror alias (issues #5, #8). `multiphase` binds both, through two memory objects over the same pages, and only as a superset. |
| CE4 | 64 KiB mapped, then one page unmapped, then that page mapped to another PA | after the unmap the bind can stay (superset), strict is impossible. After the remap the block is irregular. Changing its binding makes the three unchanged neighbour pages unavailable to every queue for the duration. |
| CE5 | one page at VA `0x44000` to PA `0` | the run would start at PA `−0x4000`. Regular, and still not representable by any import of the backing. |
| CE6 | three backed pages and one stack or code page in a block | no policy shares it. |

Not counterexamples (`test_whole_blocks_and_chunk_boundaries_are_free`): whole blocks on
unrelated physical runs, and ranges that cross an import chunk boundary. These are exactly the
cases the arena is good at, and exactly the ones a chunk buffer cannot serve.

`python3 tools/uma_gpuvm_model/gpuvm_model.py scenarios` prints the full table per block size.
At 4 KiB and 16 KiB every scenario is shared except CE6.

## 6. What changes cost

| Guest action | Sparse VA arena | 16 KiB table over chunk buffers |
| --- | --- | --- |
| Map into an empty block | one bind per block | one 8-byte entry per page |
| Map next to a mapping, same anchor | nothing if the block is already bound | entries |
| Map next to a mapping, other anchor | block becomes irregular: leaves the arena | entries |
| Unmap of part of a block | keep the bind (superset) or lose the rest of the block | entries set to 0, exact |
| Remap of one page | block-wide: finish all GPU work on the block, bind, wait | one entry |
| Alias at another phase | second import of the same pages, or fallback | entries |

The bind itself is asynchronous queue work and is not ordered against command buffers, so every
binding change of a block needs: flush recorded work that may touch the block, wait for the last
submitted tick that may touch it, bind, wait for the bind. `rebind_cost` in the model counts the
blocks and the collateral pages (CE4: one remapped page, three collateral pages). On the console
a remap of one page does not stall work on its neighbours.

The table has its own ordering requirement (commit `d661928` orders table reads against table
writes), but it is per entry and has no collateral.

## 7. Options compared

| | A. Pure sparse VA arena | B. Sparse arena + mirror | C. 16 KiB table over PA-indexed buffers |
| --- | --- | --- | --- |
| Exact for | blocks meeting C1 (C3 with today's imports) | everything | every backed page |
| When a block fails | no representation at all | block is mirrored: copies return | cannot fail for backed memory |
| Per-access cost | none | none for shared blocks | one dependent load, already paid by the DMA path today |
| Cost at map/unmap | sparse binds, block-wide quiesce on remap | same, plus demotion with `Finish` | table writes |
| Memory cost | none per page; up to `B/g` imports of the same pages (pin budget) | mirror arenas for failing blocks | table 4 times larger than at 64 KiB: 512 MiB flat for 2^40 |
| Fixed-function consumers (vertex, index, indirect, copies) | covered when every touched block is shared | covered, by copy where mirrored | **not covered**: need `{buffer, offset}` |
| Aliases | cross-object aliases with shifted imports | mixed shared/mirror aliases (#5, #8) | none: one buffer per physical chunk |
| Unverified dependencies | sparse + import, shifted imports, stitched imports | sparse + import | none beyond PR #54 |

Where the extra cost appears:

- **A** pays at map time and at every remap, and pays in capabilities nobody has verified. It is
  "pure" only for titles whose maps happen to satisfy C3. It cannot be the only representation.
- **B** is always correct, and for every failing block it keeps the whole machinery this work is
  trying to remove: write tracking, upload, readback, and the mixed-alias problem when the same
  physical page is also reachable through a shared block. Its cost is data dependent and
  concentrated in the titles with the least regular maps.
- **C** pays one table load per shader access (no change from today) and table memory. It gives
  nothing to consumers that need a contiguous buffer range.

C does not need a sparse buffer at all, so the 64 KiB block never enters it. The current code
ties the table granularity to the sparse block (`buffer_cache.cpp:91`,
`profile.sparse_page_shift`) only because mirror arenas and the table share `block_size`. A
block-granular table has the same irregular-block failure as the arena (`TranslationTable` test)
and none of the phase failure.

## 8. Recommendations for P0

P0 restated: *one copy of the data, reachable through layers that alias the same pages*, instead
of *one VA-indexed arena*.

1. **Keep the PA-indexed chunk buffers as the base layer.** They exist, they are verified on the
   M75n, and they are immune to phase and to anything inside a block.
2. **Make the translation table 16 KiB granular and independent of the sparse block.** This is
   the only option that is exact for every backed map, and it removes block-level fallback for
   shader access (the Relaxed/Disabled readback loss noted in #12 for fallback blocks goes with
   it). The 4x table needs a two-level or sparse-resident layout; that is a bounded, local
   change in `DefineGetBdaPointer` and the table writers.
3. **Treat the sparse VA arena as an accelerator, per block.** Bind a block only if it meets C3
   (later C1, if shifted imports pass on hardware). Never demote by copying: an irregular block
   is unbound and its consumers go back to layers 1 and 2, which see the same bytes.
4. **Decide whether to build 3 at all from data.** `gpuvm_model.py census DIR` replays an E0
   capture and classifies every buffer request: `contiguous` (layer 1 serves it),
   `contiguous_cross_chunk` (larger or overlapping chunks serve it), `discontiguous_phase0`
   (arena over existing imports), `discontiguous_regular` (arena needs shifted imports),
   `discontiguous_irregular` (stitched object or GPU-side gather). If the three `discontiguous`
   classes are negligible in bytes, P0 needs no sparse binding of imported memory.
5. **Use the emulator's freedom to make maps regular.** Choose flexible physical ranges
   contiguous and phase-matched to the VA; for non-fixed direct maps prefer a VA with
   `VA ≡ PA (mod 64 KiB)`. Both are invisible to the guest except through the returned address,
   which stays within the requested alignment. Behind the UMA opt-in.
6. **Fix the two alignment gaps of section 1** (separately from UMA), after checking what the
   console returns for an unaligned fixed address.
7. **Mirror only for non-backing memory.** For backed memory that no layer can serve as one
   range, use a gather on the GPU timeline (P1 of the review), not a tracked mirror.
8. **If the arena is built, fix its rules up front:** superset exposure on partial unmap, unbind
   when a block empties, and the four-step binding change of section 6.

## 9. Hardware checks these recommendations depend on

None of these has been run. The first two exist in the [#56 probe][pr56]; the rest need a probe
extension.

| Check | Decides | Where |
| --- | --- | --- |
| Imported memory bound sparsely aliases the pages (S1 to S3) | whether layer 3 exists at all | M75n (dma-buf), NVIDIA NV-5 (host pointer) |
| `Q` line: block size with external handle types, `DEDICATED_ONLY` for sparse + external, `sparseResidencyAliased`, `residencyNonResidentStrict`, `minImportedHostPointerAlignment`, `maxMemoryAllocationCount` | C3 inputs, strict semantics, import granularity | both |
| Second import of the same pages at a 16 KiB offset, bound in the same arena; write through one, read through the other | `multiphase` (CE1, CE3) | both, NV-7 |
| `UDMABUF_CREATE_LIST` over non-adjacent pages, bound sparsely; kernel list and size limits | `stitched` (CE2, CE4) | M75n |
| Host-pointer import of a guest VA range that spans several mappings, and its behaviour after a remap | `stitched` without udmabuf | NVIDIA NV-8 |
| Pin budget and GTT accounting with overlapping imports | cost of `multiphase` | M75n |
| Bind time per block under load | cost of section 6 | both |

Independent of hardware: `gpuvm_model.py census` on existing E0 captures (item 4).

## 10. How this was checked, and what was not

- Model and tests: `python3 -m unittest tools/uma_gpuvm_model/test_gpuvm_model.py`, 32 tests,
  about 10 s, CPU only, deterministic (no randomness, no time, no I/O outside a temp directory).
- Conditions: exhaustive enumeration against an independent brute-force search (section 4).
- Census replay: checked on captures of real runs of the shadps4-open-test workloads on
  lavapipe, against predictions made before the runs and against the emulator log. Method,
  results and the defects this found: [uma-e0-census-validation.md](uma-e0-census-validation.md).
  No capture of a real title was replayed; those are on the M75n. The replay needs captures
  that contain Piece events.
- Guest facts: read from the code at `c364436`, not from running titles.
- Vulkan facts: specification text. Section 2.2 separates what was measured from what was not.
- Not verified: every RADV and NVIDIA behaviour, every PS4 hardware contract, real-title
  incidence of phases and irregular blocks.

[pr54]: https://github.com/kaaburgh/shadPS4/pull/54
[pr56]: https://github.com/kaaburgh/shadPS4/pull/56
[i12]: https://github.com/kaaburgh/shadPS4/issues/12
