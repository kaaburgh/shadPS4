#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""CPU-only tests: python3 -m unittest tools/uma_gpuvm_model/test_gpuvm_model.py"""
from itertools import product
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'uma_e0'))
import analyze
import check_open_test
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


def write_capture(root, rows, **metadata):
    """A capture directory from (kind, fields) rows, numbered the way the recorder does."""
    data = []
    for i, (kind, fields) in enumerate(rows):
        e = dict(seq=i, ns=i, thread=1, kf=analyze.K[kind], context=1, session=0,
                 tick=0, address=0, size=0, a=0, b=0, c=0, cmd_seq=0)
        e.update(fields)
        data.append(analyze.RECORD.pack(*(e[k] for k in analyze.Event._fields)))
    root = Path(root)
    (root / 'events.bin').write_bytes(b''.join(data))
    (root / 'metadata.json').write_text(json.dumps(dict(
        dict(schema='shadps4-uma-e0/v1', record_bytes=104, clean_shutdown=True,
             written_events=len(rows), reserved_events=len(rows), dropped_events=0),
        **metadata)))
    return root


def kf(kind, vma_type):
    return analyze.K[kind] | (vma_type << 32)


def mapping(address, size, physical, vma_type=2):
    return [('Map', dict(address=address, size=size, kf=kf('Map', vma_type))),
            ('Piece', dict(address=address, size=size, a=physical, kf=kf('Piece', vma_type)))]


END = [('End', dict(context=0))]


class Census(unittest.TestCase):
    """The replay of an event stream. The streams here are written by hand to pin down one
    property each; RealCapture below runs the same code on a capture of a real run."""

    def census(self, rows, block=65536, **metadata):
        with tempfile.TemporaryDirectory() as temp:
            return gm.replay_census(write_capture(temp, rows, **metadata), block)

    def test_census_replay(self):
        rows = [('BlockSize', dict(size=65536)),
                *mapping(0x40000, 0x20000, 0x24000),
                *mapping(0x80000, 0x8000, 0x100000),
                ('Map', dict(address=0x88000, size=0x8000, kf=kf('Map', 7))),
                ('Buffer', dict(address=0x44000, size=0x1000, a=1)),     # inside one run
                ('Buffer', dict(address=0x80000, size=0x9000, a=1)),     # into code memory
                ('Map', dict(address=0xc0000, size=0x8000, kf=kf('Map', 2))),
                ('Piece', dict(address=0xc0000, size=0x4000, a=0x200000, kf=kf('Piece', 2))),
                ('Piece', dict(address=0xc4000, size=0x4000, a=0x300000, kf=kf('Piece', 2))),
                ('Buffer', dict(address=0xc3000, size=0x2000, a=3)),     # across two runs
                ('Buffer', dict(address=0xc3000, size=0x2000, a=3)),
                ('Unmap', dict(address=0x40000, size=0x10000)),
                ('Buffer', dict(address=0x44000, size=0x1000, a=1))]     # now unmapped
        report = self.census(rows + END, block=None)
        self.assertEqual(report['capture'], {'complete': True, 'problems': [], 'events': 16,
                                             'valid_events': 16})
        self.assertEqual((report['block_bytes'], report['block_bytes_source']),
                         (65536, 'recorded'))
        self.assertEqual(report['map_events'], 9)
        self.assertEqual({name: stats['requests']
                          for name, stats in report['buffer_requests'].items()},
                         {'contiguous': 1, 'discontiguous_irregular': 2, 'unbacked': 1,
                          'unmapped': 1})
        irregular = report['buffer_requests']['discontiguous_irregular']
        self.assertEqual((irregular['bytes'], irregular['unique_bytes']), (0x4000, 0x2000))
        peak, final = report['at_peak_backed'], report['final']
        self.assertTrue(report['final_is_end_of_run'])
        self.assertEqual(peak['backed_guest_pages'], 12)
        self.assertEqual(peak['unbacked_guest_pages'], 2)
        self.assertEqual(peak['guest_pages_by_type'], {'code': 2, 'direct': 12})
        self.assertEqual(peak['blocks'], {'irregular': 1, 'mixed_backing': 1,
                                          'regular_full': 2})
        self.assertEqual(peak['regular_phase_bytes'], {16384: 2})
        self.assertEqual(peak['policies'][PHASE0]['shared_blocks'], 0)
        self.assertEqual(peak['policies'][MULTIPHASE]['shared_blocks'], 2)
        self.assertEqual(peak['policies'][STITCH]['shared_blocks'], 3)
        self.assertEqual(final['blocks'], {'irregular': 1, 'mixed_backing': 1,
                                           'regular_full': 1})

    def test_repeated_requests_are_not_unique_memory(self):
        """A range used 500 times is 500 requests and 500 times its bytes, but one range and
        its size once. Overlapping ranges count their union."""
        rows = mapping(0x100000, 0x100000, 0x100000)
        rows += [('Buffer', dict(address=0x100000, size=0x80000, a=1, c=3))] * 500
        rows += [('Buffer', dict(address=0x140000, size=0x80000, a=1, c=0))] * 2
        total = self.census(rows + END)['buffer_requests_total']
        self.assertEqual(total, {'requests': 502, 'bytes': 502 * 0x80000, 'unique_ranges': 2,
                                 'unique_bytes': 0xc0000,
                                 'paths': {'mirror': 2, 'shared': 500}})

    def test_sizes_that_are_not_page_multiples(self):
        """A mapping or request covers every page it touches, also the last, partial one."""
        rows = mapping(0x100000, 0x4000 + 12, 0x200000)       # 16 KiB and 12 bytes: 2 pages
        rows += [('Buffer', dict(address=0x103ff8, size=12, a=1)),     # last 8 + first 4
                 ('Buffer', dict(address=0x107ff8, size=12, a=1))]     # runs off the mapping
        report = self.census(rows + END)
        self.assertEqual(report['final']['backed_guest_pages'], 2)
        self.assertEqual({name: stats['requests']
                          for name, stats in report['buffer_requests'].items()},
                         {'contiguous': 1, 'unmapped': 1})

    def test_aliases_same_and_other_phase(self):
        rows = (mapping(0x100000, 0x10000, 0x40000) +       # P at A
                mapping(0x200000, 0x10000, 0x40000) +       # P at B, same offset in a block
                mapping(0x304000, 0x4000, 0x80000) +        # Q at C ...
                mapping(0x400000, 0x4000, 0x80000))         # ... and at D, other offset
        report = self.census(rows + END)['final']
        self.assertEqual(report['aliased_physical_pages'], 5)
        self.assertEqual(report['aliased_physical_pages_other_phase'], 1)

    def test_unbacked_blocks_are_not_fallback(self):
        """Code, stack and file mappings have no Piece events. Their blocks are reported as
        unbacked and must not appear as blocks a UMA policy fails to share."""
        rows = [('Map', dict(address=0x800000, size=0x100000, kf=kf('Map', 7)))]
        rows += mapping(0x100000, 0x10000, 0x40000)
        report = self.census(rows + END)['final']
        self.assertEqual(report['blocks'], {'regular_full': 1, 'unbacked': 16})
        self.assertEqual(report['unbacked_guest_pages'], 64)
        for policy in (PHASE0, MULTIPHASE, STITCH):
            self.assertEqual(report['policies'][policy]['shared_blocks'], 1)
            self.assertEqual(report['policies'][policy]['fallback_blocks'], 0)

    def test_peak_is_the_moment_of_most_backed_pages(self):
        rows = [('Map', dict(address=0x800000, size=0x400000, kf=kf('Map', 7)))]   # unbacked
        rows += mapping(0x100000, 0x20000, 0x40000)                                # 8 pages
        rows += [('Unmap', dict(address=0x800000, size=0x400000)),
                 ('Unmap', dict(address=0x100000, size=0x20000))]
        rows += mapping(0x100000, 0x10000, 0x40000)                                # 4 pages
        report = self.census(rows + END)
        self.assertEqual(report['at_peak_backed']['backed_guest_pages'], 8)
        self.assertEqual(report['final']['backed_guest_pages'], 4)

    def test_incomplete_capture_is_replayed_and_marked(self):
        rows = mapping(0x100000, 0x10000, 0x40000) + [
            ('Buffer', dict(address=0x100000, size=0x1000, a=1))]
        with tempfile.TemporaryDirectory() as temp:
            root = write_capture(temp, rows, clean_shutdown=False, written_events=1)
            with (root / 'events.bin').open('ab') as stream:
                stream.write(b'\0' * 40)                     # the file ends inside a record
            report = gm.replay_census(root, 65536)
        self.assertFalse(report['capture']['complete'])
        self.assertFalse(report['final_is_end_of_run'])
        self.assertEqual(report['capture']['valid_events'], 3)
        self.assertEqual(report['buffer_requests_total']['requests'], 1)
        problems = ' '.join(report['capture']['problems'])
        for text in ('ends inside a record (40 trailing bytes)', 'did not shut down cleanly',
                     'metadata counts 1 written events, events.bin holds 3', 'no End event'):
            self.assertIn(text, problems)

    def test_dropped_events_make_the_capture_incomplete(self):
        rows = mapping(0x100000, 0x10000, 0x40000) + END
        report = self.census(rows, dropped_events=7, reserved_events=10)
        self.assertFalse(report['final_is_end_of_run'])
        self.assertEqual(report['capture']['problems'], ['7 events dropped by the recorder'])


FIXTURE = Path(__file__).resolve().parent / 'testdata' / 'fallback_in_shared-shared'


class RealCapture(unittest.TestCase):
    """A capture of fallback_in_shared on lavapipe with shared backing, as recorded by
    tools/uma_e0/run_synthetic.py (see docs/research/uma-e0-census-validation.md). The
    damaged variants are that capture with bytes removed, as an interrupted or failed write
    leaves it. They are not a substitute for recording: check_open_test.py is run on fresh
    captures of every workload."""

    def copy(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        return Path(shutil.copytree(FIXTURE, Path(temp.name) / FIXTURE.name))

    def failed(self, run):
        return [name for name, ok, _ in check_open_test.check_run(run).rows if not ok]

    def test_every_check_passes(self):
        result = check_open_test.check_run(FIXTURE)
        self.assertGreater(len(result.rows), 12)
        self.assertEqual([row for row in result.rows if not row[1]], [])

    def test_request_classes_and_paths(self):
        report = gm.replay_census(FIXTURE / 'capture', None, 256 << 20)
        self.assertEqual(report['block_bytes_source'], 'recorded')
        self.assertEqual({name: (stats['requests'], stats['paths'])
                          for name, stats in report['buffer_requests'].items()},
                         {'contiguous': (4, {'shared': 4}),
                          'contiguous_cross_chunk': (1, {'mirror': 1}),
                          'discontiguous_phase0': (1, {'mirror': 1})})
        self.assertEqual(report['buffer_requests_total']['unique_bytes'], 6 * 32768)

    def test_capture_cut_inside_a_record(self):
        run = self.copy()
        events = run / 'capture' / 'events.bin'
        events.write_bytes(events.read_bytes()[:-64])
        state = analyze.integrity(run / 'capture')
        self.assertFalse(state['complete'])
        self.assertEqual(state['trailing_bytes'], 40)
        with self.assertRaises(ValueError):
            list(analyze.records(events))
        report = gm.replay_census(run / 'capture')            # must not raise
        self.assertFalse(report['final_is_end_of_run'])
        self.assertEqual(report['buffer_requests_total']['requests'], 6)
        self.assertEqual(self.failed(run), ['capture complete (exited by itself)'])

    def test_lost_block_shifts_records(self):
        """The old stdio writer lost a 4096 byte buffer on a full disk and went on. Every
        record after the hole is read at the wrong offset and can look like a Map or Unmap."""
        run = self.copy()
        events = run / 'capture' / 'events.bin'
        data = events.read_bytes()
        events.write_bytes(data[:8192] + data[12288:])
        state = analyze.integrity(run / 'capture')
        self.assertEqual(state['valid_events'], 8192 // 104)
        self.assertIn('does not continue the sequence', ' '.join(state['problems']))
        self.assertEqual(sum(state['counts'].values()), state['valid_events'])
        report = gm.replay_census(run / 'capture')
        self.assertEqual(report['capture']['valid_events'], 78)
        self.assertFalse(report['final_is_end_of_run'])

    def test_empty_metadata(self):
        run = self.copy()
        (run / 'capture' / 'metadata.json').write_text('')
        state = analyze.integrity(run / 'capture')
        self.assertIn('metadata.json unreadable', state['problems'][0])
        self.assertFalse(gm.replay_census(run / 'capture')['final_is_end_of_run'])

    def patch(self, run, kind, field, change, vma_type=None):
        """Changes one field of the first event of a kind, keeping the file well formed."""
        events = run / 'capture' / 'events.bin'
        out, done = [], False
        for e in analyze.records(events):
            hit = (not done and e.kf & 0xffffffff == analyze.K[kind] and
                   vma_type in (None, e.kf >> 32))
            if hit:
                e, done = e._replace(**{field: change(getattr(e, field))}), True
            out.append(analyze.RECORD.pack(*e))
        self.assertTrue(done)
        events.write_bytes(b''.join(out))

    def test_checker_notices_a_wrong_physical_address(self):
        run = self.copy()
        self.patch(run, 'Piece', 'a', lambda physical: physical + 0x4000, vma_type=2)
        failed = self.failed(run)
        self.assertIn('direct Map events = logged map calls', failed)
        self.assertIn('direct page table after every map and munmap call = log', failed)

    def test_checker_notices_a_wrong_request(self):
        run = self.copy()
        self.patch(run, 'Buffer', 'size', lambda size: size * 2)
        self.assertIn('requests: count, bytes, unique ranges, unique bytes', self.failed(run))

    def test_checker_notices_a_wrong_path(self):
        run = self.copy()
        self.patch(run, 'Buffer', 'c', lambda path: 0 if path == 3 else 3)
        self.assertIn('class contiguous <=> path shared (recorded block, real chunk)',
                      self.failed(run))


if __name__ == '__main__':
    unittest.main()
