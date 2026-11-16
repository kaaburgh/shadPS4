#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""CPU-only tests: python3 -m unittest tools/uma_gpuvm_model/test_gpuvm_model.py"""
from itertools import product
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gpuvm_model as gm
from gpuvm_model import (FALLBACK, IRREGULAR, MULTIPHASE, PHASE0, REGULAR, SHARED, STITCH,
                         UNBACKED, GuestMap, HostMap, Linear, geometry)

G64 = geometry(65536)        # 16 KiB guest pages, 64 KiB sparse blocks, 1 MiB backing
BLOCK4 = 4                   # first VA page of block 4 is guest page 16


def shared(guest, geom, policy, strict=False):
    plan = gm.plan(guest, geom, policy, strict)
    shared_blocks = {blk for blk, d in plan.decisions.items() if d == SHARED}
    # A shared block must translate exactly; this is checked on every plan in this file.
    assert not gm.violations(guest, plan.host, strict, shared_blocks)
    return plan, shared_blocks


class BindRules(unittest.TestCase):
    def test_offsets_and_size_must_be_block_multiples(self):
        host, memory = HostMap(G64), Linear(0, 64)
        host.bind(16, 16, memory, 16)
        for args in ((4, 16, memory, 0), (16, 16, memory, 4), (16, 4, memory, 0)):
            with self.assertRaises(gm.BindError):
                host.bind(*args)

    def test_range_must_lie_inside_the_memory_object(self):
        host = HostMap(G64)
        with self.assertRaises(gm.BindError):
            host.bind(0, 32, Linear(0, 16), 0)
        with self.assertRaises(gm.BindError):
            host.bind(0, 16, Linear(0, 16), 16)
        with self.assertRaises(gm.BindError):
            host.bind(0, 16, Linear(248, 16), 0)  # import past the 1 MiB backing


class ExhaustiveConditions(unittest.TestCase):
    """The closed-form block rule equals a search over every legal bind."""

    def check_universe(self, geom, va_pages, pa_pages):
        blocks = sorted({page * geom.g // geom.b for page in va_pages})
        choices = [None] + list(range(pa_pages))
        counts = {(policy, strict): 0 for policy in (PHASE0, MULTIPHASE) for strict in (0, 1)}
        total = 0
        for targets in product(choices, repeat=len(va_pages)):
            guest = GuestMap({v: p for v, p in zip(va_pages, targets) if p is not None})
            total += 1
            for (policy, strict) in counts:
                plan, _ = shared(guest, geom, policy, bool(strict))
                for block in blocks:
                    cls = gm.classify(guest, geom, block)
                    if cls.kind == gm.EMPTY:
                        continue
                    found = gm.search_block(guest, geom, block, policy, bool(strict))
                    self.assertEqual(found, plan.decisions[block] == SHARED,
                                     (policy, strict, guest.pages, block))
                    counts[(policy, strict)] += found
        return total, counts

    def test_one_64k_block_over_eight_physical_pages(self):
        geom = geometry(65536, 8 * 16384)
        total, counts = self.check_universe(geom, [16, 17, 18, 19], 8)
        self.assertEqual(total, 9 ** 4)
        # Superset: more maps are representable once imports may start at any guest page.
        self.assertLess(counts[(PHASE0, 0)], counts[(MULTIPHASE, 0)])
        # Strict: only full contiguous blocks. Phase 0 has 2 aligned windows, any phase 5.
        self.assertEqual(counts[(PHASE0, 1)], 2)
        self.assertEqual(counts[(MULTIPHASE, 1)], 5)

    def test_two_32k_blocks_are_independent(self):
        geom = geometry(32768, 6 * 16384)
        total, _ = self.check_universe(geom, [8, 9, 10, 11], 6)
        self.assertEqual(total, 7 ** 4)

    def test_block_not_larger_than_guest_page_represents_every_map(self):
        for block_bytes in (4096, 16384):
            geom = geometry(block_bytes, 4 * 16384)
            for targets in product([None, 0, 1, 2, 3], repeat=3):
                guest = GuestMap({v: p for v, p in zip((5, 6, 7), targets) if p is not None})
                plan, shared_blocks = shared(guest, geom, PHASE0, strict=True)
                self.assertNotIn(FALLBACK, plan.decisions.values())
                self.assertEqual(shared_blocks, set(guest.blocks(geom)))
                self.assertFalse(gm.violations(guest, plan.host, True))

    def test_stitched_objects_represent_every_backed_map(self):
        geom = geometry(65536, 8 * 16384)
        for targets in product([None, 0, 1, 2, 3, 4, 5], repeat=4):
            guest = GuestMap({v: p for v, p in zip((16, 17, 18, 19), targets) if p is not None})
            for strict in (False, True):
                plan, _ = shared(guest, geom, STITCH, strict)
                self.assertNotIn(FALLBACK, plan.decisions.values())
                self.assertFalse(gm.violations(guest, plan.host, strict))


class Counterexamples(unittest.TestCase):
    """Guest maps that are valid on a PS4 and that a 64 KiB sparse block cannot bind."""

    def scenario(self, name):
        return gm.scenarios()[name][0]

    def test_ce1_phase_mismatch_defeats_block_aligned_imports(self):
        guest = self.scenario('phase')          # VA 0x40000 -> PA 0x24000, contiguous
        self.assertEqual(gm.classify(guest, G64, BLOCK4), (REGULAR, 36, True))
        self.assertEqual(shared(guest, G64, PHASE0)[1], set())
        plan, blocks = shared(guest, G64, MULTIPHASE)
        self.assertEqual(blocks, {4, 5})
        memory, offset = plan.host.binds[4]
        self.assertEqual((memory.base % G64.b, offset % G64.b), (4, 0))  # 16 KiB-shifted import
        # The PR #54 per-chunk buffer serves the same range: buffer offsets have no rule.
        self.assertEqual(gm.serve(guest, G64, gm.plan(guest, G64, PHASE0), 16, 8), 'chunk')

    def test_ce2_two_runs_in_one_block_need_a_stitched_object(self):
        guest = self.scenario('adjacent_other_anchor')
        self.assertEqual(gm.classify(guest, G64, BLOCK4).kind, IRREGULAR)
        for policy in (PHASE0, MULTIPHASE):
            self.assertEqual(shared(guest, G64, policy)[1], set())
            self.assertFalse(gm.search_block(guest, G64, BLOCK4, policy, False))
        self.assertEqual(shared(guest, G64, STITCH)[1], {4})
        plan = gm.plan(guest, G64, MULTIPHASE)
        # A consumer of the whole 64 KiB has neither a sparse nor a per-chunk source.
        self.assertEqual(gm.serve(guest, G64, plan, 16, 4), 'copy')
        self.assertEqual(gm.serve(guest, G64, plan, 16, 2), 'chunk')
        # The same two mappings on one physical run are an ordinary block.
        self.assertEqual(shared(self.scenario('adjacent_same_anchor'), G64, PHASE0)[1], {4})

    def test_ce3_alias_with_another_phase(self):
        guest = self.scenario('alias_other_phase')   # PA pages 8..11 at VA 16.. and VA 33..
        self.assertEqual(shared(guest, G64, PHASE0)[1], {4})
        plan, blocks = shared(guest, G64, MULTIPHASE)
        self.assertEqual(blocks, {4, 8, 9})
        # Same physical pages through two different memory objects.
        self.assertEqual(gm.cross_object_alias_pages(G64, plan.host), {8, 9, 10, 11})
        # Superset only: the second alias exposes neighbours at unmapped VAs.
        self.assertEqual(shared(guest, G64, MULTIPHASE, strict=True)[1], {4})
        # With block-aligned imports the alias is half shared and half mirrored (#5, #8).
        hybrid = gm.plan(guest, G64, PHASE0)
        self.assertEqual(gm.mixed_alias_pages(guest, G64, hybrid.decisions), {8, 9, 10, 11})
        self.assertFalse(gm.cross_object_alias_pages(
            G64, gm.plan(self.scenario('alias_same_phase'), G64, PHASE0).host))

    def test_ce4_partial_unmap_then_remap(self):
        before = self.scenario('aligned')
        before.unmap(20, 4)                          # keep one full block: VA 16..19 -> PA 8..11
        plan_before, _ = shared(before, G64, PHASE0)

        unmapped = before.copy()
        unmapped.unmap(18, 1)
        plan_unmapped, blocks = shared(unmapped, G64, PHASE0)
        self.assertEqual(blocks, {4})                # superset: the bind may stay
        cost = gm.rebind_cost(G64, before, unmapped, plan_before, plan_unmapped)
        self.assertEqual(cost['rebound_blocks'], [])
        self.assertEqual(plan_unmapped.host.unit(18 * 4), 10 * 4)  # stale page still reachable
        self.assertEqual(shared(unmapped, G64, PHASE0, strict=True)[1], set())

        remapped = unmapped.copy()
        remapped.map(18, 40, 1)
        for policy in (PHASE0, MULTIPHASE):
            self.assertEqual(shared(remapped, G64, policy)[1], set())
        plan_remapped, blocks = shared(remapped, G64, STITCH)
        self.assertEqual(blocks, {4})
        cost = gm.rebind_cost(G64, unmapped, remapped, plan_unmapped, plan_remapped)
        self.assertEqual(cost['rebound_blocks'], [4])
        # The guest changed one page; three unchanged neighbours must be idle for the rebind.
        self.assertEqual(cost['collateral_pages'], [16, 17, 19])

    def test_ce5_run_that_leaves_the_backing(self):
        guest = self.scenario('backing_edge')        # VA page 17 -> PA page 0, anchor -16 KiB
        self.assertEqual(gm.classify(guest, G64, BLOCK4), (REGULAR, -4, False))
        for policy in (PHASE0, MULTIPHASE):
            self.assertEqual(shared(guest, G64, policy)[1], set())
        self.assertEqual(shared(guest, G64, STITCH)[1], {4})

    def test_ce6_non_backing_neighbour_blocks_every_policy(self):
        guest = self.scenario('unbacked_neighbour')
        for policy in gm.POLICIES:
            self.assertEqual(shared(guest, G64, policy)[1], set())

    def test_whole_blocks_and_chunk_boundaries_are_free(self):
        self.assertEqual(shared(self.scenario('noncontiguous_blocks'), G64, PHASE0)[1], {4, 5})
        geom = geometry(65536, 1 << 20, chunk_bytes=262144)
        guest = GuestMap()
        guest.map(16, 12, 12)                        # PA 192K..384K crosses the 256K chunk
        plan, blocks = shared(guest, geom, PHASE0)
        self.assertEqual(blocks, {4, 5, 6})
        self.assertEqual(len({memory for memory, _ in plan.host.binds.values()}), 2)
        self.assertEqual(gm.serve(guest, geom, plan, 16, 12), 'arena')
        # The per-chunk buffer alone cannot serve this range (issue #12, item 1).
        none = gm.Plan(HostMap(geom), {}, {})
        self.assertEqual(gm.serve(guest, geom, none, 16, 12), 'copy')


class TranslationTable(unittest.TestCase):
    def test_16k_entries_are_exact_and_block_entries_are_not(self):
        for name, (guest, _) in gm.scenarios().items():
            self.assertEqual(gm.table_irregular_blocks(guest, G64, G64.g), [], name)
        irregular = {name for name, (guest, _) in gm.scenarios().items()
                     if gm.table_irregular_blocks(guest, G64, G64.b)}
        self.assertEqual(irregular, {'adjacent_other_anchor', 'remap_one_page'})
        # A phase mismatch costs a table nothing.
        self.assertEqual(gm.table_irregular_blocks(gm.scenarios()['phase'][0], G64, G64.b), [])


class Report(unittest.TestCase):
    def test_summary_counts(self):
        report = gm.summarize(gm.scenarios()['alias_other_phase'][0], G64)
        self.assertEqual(report['blocks'], {'regular_full': 1, 'regular_partial': 2})
        self.assertEqual(report['regular_phase_bytes'], {0: 1, 49152: 2})
        self.assertEqual(report['policies'][PHASE0]['fallback_blocks'], 2)
        self.assertEqual(report['policies'][MULTIPHASE]['linear_imports'], 2)
        self.assertEqual(report['policies'][MULTIPHASE]['violations'], 0)

    def test_census_replay(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'uma_e0'))
        import analyze
        rows = [('BlockSize', dict(size=65536)),
                ('Map', dict(address=0x40000, size=0x20000, kf=analyze.K['Map'] | (2 << 32))),
                ('Piece', dict(address=0x40000, size=0x20000, a=0x24000)),
                ('Map', dict(address=0x80000, size=0x8000, kf=analyze.K['Map'] | (2 << 32))),
                ('Piece', dict(address=0x80000, size=0x8000, a=0x100000)),
                ('Map', dict(address=0x88000, size=0x8000, kf=analyze.K['Map'] | (7 << 32))),
                ('Buffer', dict(address=0x44000, size=0x1000, a=1)),     # inside one run
                ('Buffer', dict(address=0x80000, size=0x9000, a=1)),     # into code memory
                ('Map', dict(address=0xc0000, size=0x8000, kf=analyze.K['Map'] | (2 << 32))),
                ('Piece', dict(address=0xc0000, size=0x4000, a=0x200000)),
                ('Piece', dict(address=0xc4000, size=0x4000, a=0x300000)),
                ('Buffer', dict(address=0xc3000, size=0x2000, a=3)),     # across two runs
                ('Buffer', dict(address=0xc3000, size=0x2000, a=3)),
                ('Unmap', dict(address=0x40000, size=0x10000)),
                ('Buffer', dict(address=0x44000, size=0x1000, a=1))]     # now unmapped
        with tempfile.TemporaryDirectory() as temp:
            data = []
            for i, (kind, fields) in enumerate(rows):
                e = dict(seq=i, ns=i, thread=1, kf=analyze.K[kind], context=1, session=0,
                         tick=0, address=0, size=0, a=0, b=0, c=0, cmd_seq=0)
                e.update(fields)
                data.append(analyze.RECORD.pack(*(e[k] for k in analyze.Event._fields)))
            (Path(temp) / 'events.bin').write_bytes(b''.join(data))
            report = gm.replay_census(temp)
        self.assertEqual(report['events'], 9)
        self.assertEqual(report['buffer_requests'], {
            'contiguous': 1, 'discontiguous_irregular': 2, 'unbacked': 1, 'unmapped': 1})
        self.assertEqual(report['buffer_request_bytes']['discontiguous_irregular'], 0x4000)
        peak, final = report['at_peak_mapped'], report['final']
        self.assertEqual(peak['mapped_guest_pages'], 14)
        self.assertEqual(peak['blocks'], {'irregular': 1, 'regular_full': 2, 'unbacked': 1})
        self.assertEqual(peak['regular_phase_bytes'], {16384: 2})
        self.assertEqual(peak['policies'][PHASE0]['shared_blocks'], 0)
        self.assertEqual(peak['policies'][MULTIPHASE]['shared_blocks'], 2)
        self.assertEqual(peak['policies'][STITCH]['shared_blocks'], 3)
        self.assertEqual(final['blocks'], {'irregular': 1, 'regular_full': 1, 'unbacked': 1})

    def test_request_classes(self):
        geom = geometry(65536, 1 << 20, chunk_bytes=262144)
        guest = GuestMap()
        guest.map(16, 12, 12)                         # PA 192K..384K, crosses the 256K chunk
        guest.map(32, 60, 4)                          # whole blocks on unrelated runs
        guest.map(36, 0, 4)
        guest.map(48, 9, 4)                           # shifted run next to ...
        guest.map(52, 21, 4)                          # ... another shifted run
        cases = {(16 * 16384, 4096): 'contiguous',
                 (16 * 16384, 12 * 16384): 'contiguous_cross_chunk',
                 (35 * 16384, 2 * 16384): 'discontiguous_phase0',
                 (51 * 16384, 2 * 16384): 'discontiguous_regular',
                 (27 * 16384, 2 * 16384): 'unmapped'}
        for (address, size), expected in cases.items():
            self.assertEqual(gm.request_class(guest, geom, address, size), expected)


if __name__ == '__main__':
    unittest.main()
