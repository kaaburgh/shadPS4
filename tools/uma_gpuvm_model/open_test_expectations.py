# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Expected census results for shadps4-open-test workloads, derived from the guest sources.

Written before the first real capture was taken (see the commit that adds this file), from
kaaburgh/shadps4-open-test at eb1f2fe and the placement rules of shadPS4 at c364436. Nothing
here was fitted to census output. check_open_test.py compares a capture with these.

Sizes are bytes. Guest pages are 16 KiB. "direct" means memory the guest mapped itself with
sceKernelMapDirectMemory; other mappings (code, stack, libc heap) are outside the predictions.

Placement facts used:
- st_arena_init: 2 MiB of direct memory, physical and virtual alignment 2 MiB.
- A non-fixed sceKernelMapDirectMemory returns a VA aligned to the requested alignment.
- sceKernelAllocateDirectMemory returns the lowest free physical range of the alignment.
- A shader binding is one BufferCache request of the size the guest put in the descriptor.
"""

KIB, MIB = 1 << 10, 1 << 20

# Holds for every workload.
COMMON = {
    # lavapipe reports a 4 KiB sparse block; BufferCache clamps it to MIN_BLOCK_SIZE.
    'recorded_block_bytes_lavapipe': 16 * KIB,
    # At a block not larger than the guest page every backed page is its own regular,
    # phase-0 block (condition C4 of docs/research/uma-gpuvm-granularity.md).
    'irregular_blocks_at_16k': 0,
    'nonzero_phase_blocks_at_16k': 0,
    # Every sceKernelMapDirectMemory in the emulator log has a Map event with Piece events
    # that give the same physical address, and every sceKernelMunmap has Unmap events.
    'direct_map_matches_emulator_log': True,
    # The open-test runner stops shadPS4 with SIGTERM after the result marker, and shadPS4
    # has no handler for it, so such a capture cannot be clean. A capture stopped through
    # IPC STOP must be clean and lossless.
    'clean_shutdown': {'sigterm': False, 'ipc_stop': True},
}

EXPECT = {
    'buffer_compute_roundtrip': {
        'direct_mappings': [{'name': 'arena', 'bytes': 2 * MIB, 'maps': 1}],
        'aliased_physical_pages_at_peak': 0,
        'direct_blocks_64k': {'irregular': 0, 'nonzero_phase': 0},
        'request_classes_64k': {'contiguous'},
    },
    'physical_alias': {
        # One 256 KiB allocation (alignment 256 KiB) mapped at A and at B, alignment 256 KiB.
        'direct_mappings': [{'name': 'arena', 'bytes': 2 * MIB, 'maps': 1},
                            {'name': 'alias_a', 'bytes': 256 * KIB, 'maps': 1},
                            {'name': 'alias_b', 'bytes': 256 * KIB, 'maps': 1}],
        'aliased_physical_pages_at_peak': 16,
        # Both aliases have VA = PA = 0 (mod 256 KiB): same phase, the benign alias case.
        'direct_blocks_64k': {'irregular': 0, 'nonzero_phase': 0},
        'cross_object_alias_pages_64k': 0,
        'request_classes_64k': {'contiguous'},
        # 7 dispatches of 2 bindings of 64 KiB. The 14 uses name 11 distinct VA ranges:
        # b_r1, b_r2, b_r3, a_r2, four outputs, three sources.
        'requests': {'count': 14, 'bytes': 14 * 64 * KIB,
                     'unique_ranges': 11, 'unique_bytes': 11 * 64 * KIB},
    },
    'map_unmap_reuse': {
        # 256 KiB mapped, unmapped and mapped again at the same VA twice (fixed).
        'direct_mappings': [{'name': 'arena', 'bytes': 2 * MIB, 'maps': 1},
                            {'name': 'mapping', 'bytes': 256 * KIB, 'maps': 3}],
        'aliased_physical_pages_at_peak': 0,
        'direct_blocks_64k': {'irregular': 0, 'nonzero_phase': 0},
        'request_classes_64k': {'contiguous'},
        # 3 lifetimes of 3 dispatches of 2 bindings of 32 KiB, always X, Y, Z or W.
        'requests': {'count': 18, 'bytes': 18 * 32 * KIB,
                     'unique_ranges': 4, 'unique_bytes': 4 * 32 * KIB},
    },
    'uma_churn': {
        # 4 MiB pool (alignment 2 MiB), remapped at the same VA in generations 4, 8, 12.
        'direct_mappings': [{'name': 'arena', 'bytes': 2 * MIB, 'maps': 1},
                            {'name': 'pool', 'bytes': 4 * MIB, 'maps': 4}],
        'aliased_physical_pages_at_peak': 0,
        'direct_blocks_64k': {'irregular': 0, 'nonzero_phase': 0},
        'request_classes_64k': {'contiguous'},
        # 16 generations; writer binds pool, widx (49152), params (12); reader binds pool,
        # ridx (4096), out (4096), params (12). The pool is requested 32 times but is 4 MiB.
        'requests': {'count': 16 * 7,
                     'bytes': 16 * (2 * 4 * MIB + 49152 + 4096 + 4096 + 2 * 12),
                     'unique_ranges': 5,
                     'unique_bytes': 4 * MIB + 49152 + 4096 + 4096 + 12},
    },
    'fallback_in_shared': {
        # straddle: 32 KiB at physical [256 MiB - 16 KiB, 256 MiB + 16 KiB), one mapping.
        # noncontiguous: two 16 KiB pages from separate allocations (physical 16 MiB and
        # 16 MiB + 64 KiB), mapped back to back with two fixed maps.
        'direct_mappings': [{'name': 'arena', 'bytes': 2 * MIB, 'maps': 1},
                            {'name': 'straddle', 'bytes': 32 * KIB, 'maps': 1},
                            {'name': 'noncontiguous_0', 'bytes': 16 * KIB, 'maps': 1},
                            {'name': 'noncontiguous_1', 'bytes': 16 * KIB, 'maps': 1}],
        'aliased_physical_pages_at_peak': 0,
        # 3 dispatches of 2 bindings of 32 KiB: three sources and the control range in the
        # arena, the straddle range, the noncontiguous range.
        'requests': {'count': 6, 'bytes': 6 * 32 * KIB,
                     'unique_ranges': 6, 'unique_bytes': 6 * 32 * KIB},
        'request_class_counts_64k': {
            'contiguous': 4,
            # 256 MiB is a multiple of the default 16 MiB chunk and of 256 MiB chunks.
            'contiguous_cross_chunk': 1,
            # One of the two, decided by the VA of the range (see noncontiguous_class).
            'discontiguous': 1,
        },
        'request_class_counts_16k': {'contiguous': 4, 'contiguous_cross_chunk': 1,
                                     'discontiguous_phase0': 1},
        # With one chunk that covers the whole backing the straddle range is contiguous.
        'request_class_counts_64k_one_chunk': {'contiguous': 5, 'discontiguous': 1},
    },
    'dma_alias_order': {
        # Six cases, each with P mapped at A (16 KiB). contiguous_*: P also at B.
        # noncontiguous_*: B is two pages, P then Q = P + 64 KiB. single_*: B is A.
        'direct_mapping_count': 1 + 6 + 2 + 2 * 2,
        # P of the four alias cases is reachable through two VAs.
        'aliased_physical_pages_at_peak': 4,
        # Single 16 KiB pages from different 256 KiB physical windows sit next to each
        # other in VA, so at 64 KiB some block holds two physical runs.
        'direct_blocks_64k': {'irregular_min': 1},
        'direct_blocks_16k': {'irregular': 0},
    },
}


def noncontiguous_class(va, block_bytes=64 * KIB):
    """Expected request class of a two-page range at va whose pages lie on two physical
    runs, both multiples of 64 KiB (fallback_in_shared, dma_alias_order B)."""
    if block_bytes <= 16 * KIB:
        return {'discontiguous_phase0'}
    if va % block_bytes <= block_bytes - 32 * KIB:
        return {'discontiguous_irregular'}          # both pages in one block
    # One page in each block: every block may still be regular, but its phase is not 0,
    # and a neighbour page from another run can make it irregular.
    return {'discontiguous_regular', 'discontiguous_irregular'}
