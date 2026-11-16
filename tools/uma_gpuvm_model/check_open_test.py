#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Checks real UMA E0 captures of shadps4-open-test workloads against predictions.

    python3 tools/uma_gpuvm_model/check_open_test.py RUN_DIR [RUN_DIR ...]

RUN_DIR is what tools/uma_e0/run_synthetic.py wrote: capture/, run.log, launch.json.
The census is compared with three things, and none of them comes from the census itself:

  predictions   open_test_expectations.py, written from the guest sources before any capture
  emulator log  the sceKernelMapNamedDirectMemory and sceKernelMunmap lines of run.log,
                replayed into a second page table that never saw an E0 event
  path taken    the path field of every Buffer event: with shared backing, shadPS4 itself
                decided whether the range could be served from one imported chunk

A run that the harness cut short on purpose (--seconds, --stop sigkill, --max-bytes) is
checked for the capture state it must have, and its workload predictions are skipped.

Exit status 0 when every check of every run passed, 1 otherwise.
"""
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'uma_e0'))
import analyze  # noqa: E402
import gpuvm_model as gm  # noqa: E402
from open_test_expectations import COMMON, EXPECT, noncontiguous_class  # noqa: E402

PAGE = gm.GUEST_PAGE
DIRECT = 2
MAP_IN = re.compile(r'sceKernelMapNamedDirectMemory: in_addr = (0x[0-9a-f]+), len = (0x[0-9a-f]+)'
                    r', prot = 0x[0-9a-f]+, flags = (0x[0-9a-f]+), phys_addr = (0x[0-9a-f]+), '
                    r"alignment = 0x[0-9a-f]+, name = '([^']*)'")
MAP_OUT = re.compile(r'sceKernelMapNamedDirectMemory: out_addr = (0x[0-9a-f]+)')
MUNMAP = re.compile(r'sceKernelMunmap: addr = (0x[0-9a-f]+), len = (0x[0-9a-f]+)')
CHUNK = re.compile(r'UMA shared backing enabled: backend=\S+ chunk_bytes=(\d+)')
WRITE_ERROR = re.compile(r'UMA E0 capture stopped after (\d+) events')
GUEST_RANGES = re.compile(r'noncontiguous (0x[0-9a-f]+) \(physical')


def log_oracle(text):
    """Direct mappings as the kernel HLE logged them: the list of map calls, the page table
    left after replaying maps and unmaps in log order, and that table after every call."""
    calls, table, pending, steps = [], {}, None, []
    for line in text.splitlines():
        if match := MAP_IN.search(line):
            pending = [int(g, 16) for g in match.group(1, 2, 3, 4)] + [match.group(5)]
        elif (match := MAP_OUT.search(line)) and pending:
            _, size, _, phys, name = pending
            va, pending = int(match.group(1), 16), None
            calls.append((va, size, phys, name))
            for i in range(size // PAGE):
                table[va // PAGE + i] = phys // PAGE + i
            steps.append(('map', va, size, dict(table)))
        elif match := MUNMAP.search(line):
            va, size = (int(g, 16) for g in match.groups())
            for page in range(va // PAGE, (va + size - 1) // PAGE + 1):
                table.pop(page, None)
            steps.append(('unmap', va, size, dict(table)))
    return calls, table, steps


def trusted_records(capture):
    return analyze.records(capture / 'events.bin', allow_truncated=True,
                           limit=analyze.integrity(capture)['valid_events'])


def event_maps(capture):
    """Direct Map events with their Piece events, straight from events.bin."""
    kinds, maps, by_epoch = analyze.K, [], {}
    for e in trusted_records(capture):
        kind, vma_type = e.kf & 0xffffffff, e.kf >> 32
        if kind == kinds['Map'] and vma_type == DIRECT:
            by_epoch[e.context] = entry = {'va': e.address, 'size': e.size, 'pieces': []}
            maps.append(entry)
        elif kind == kinds['Piece'] and e.context in by_epoch:
            by_epoch[e.context]['pieces'].append((e.address, e.size, e.a))
    return maps


class Checks:
    def __init__(self, run):
        self.run, self.rows = run, []

    def add(self, name, ok, detail=''):
        self.rows.append((name, bool(ok), detail))

    def equal(self, name, got, want):
        self.add(name, got == want, f'{got}' if got == want else f'got {got}, expected {want}')


def census_at(capture, block, chunk=16 << 20):
    return gm.replay_census(capture, block, chunk)


def classes(report):
    return {name: stats['requests'] for name, stats in report['buffer_requests'].items()}


def check_run(run):
    run = Path(run)
    launch = json.loads((run / 'launch.json').read_text())
    log = (run / 'run.log').read_text(errors='replace')
    capture = run / 'capture'
    if not capture.is_dir():            # recorded elsewhere with --capture
        capture = Path(launch.get('capture_dir', capture))
    test = Path(launch['parameters']['elf']).stem
    shared = launch['parameters']['shared_backing']
    out = Checks(f"{run.name} ({test}, {'shared' if shared else 'mirror'})")
    expect = EXPECT.get(test)

    # 1. Transport. What the capture must look like follows from how the run was ended.
    state = analyze.integrity(capture)
    problems = '; '.join(state['problems'])
    ended, budget = launch.get('ended'), 'SHADPS4_UMA_E0_MAX_BYTES' in launch['environment']
    stopped = WRITE_ERROR.search(log)
    whole_run = launch.get('result') in ('PASS', 'FAIL') and not (budget or stopped)
    if stopped:
        # The recorder met a write error (full disk). The file must end at the last whole
        # record, with nothing shifted after it, and the capture must not look complete.
        out.add('write error: capture flagged incomplete', not state['complete'], problems)
        out.equal('write error: events.bin holds the records the recorder reported',
                  (state['events'], state['valid_events'], state['trailing_bytes']),
                  (int(stopped.group(1)),) * 2 + (0,))
    elif budget:
        out.add('budget: drops counted, capture flagged incomplete',
                not state['complete'] and bool(state['metadata']['dropped_events']) and
                state['file_bytes'] <= int(launch['environment']['SHADPS4_UMA_E0_MAX_BYTES']),
                problems)
    elif ended == 'SIGKILL':
        out.equal('capture complete after SIGKILL', state['complete'],
                  COMMON['clean_shutdown']['sigkill'])
        out.add('SIGKILL capture is still readable', state['events'] > 0, problems)
    else:
        want = COMMON['clean_shutdown'].get({'SIGTERM': 'sigterm', 'IPC STOP': 'ipc_stop'}
                                            .get(ended), True)
        out.add(f'capture complete ({ended})', state['complete'] == want,
                problems or f"{state['events']} events")
    if not state['complete'] and not (budget or stopped or ended == 'SIGKILL'):
        return out
    if not whole_run:
        report = census_at(capture, None)
        out.add('census marks the capture', report['capture']['complete'] == state['complete']
                and report['final_is_end_of_run'] == state['complete'],
                f"complete={report['capture']['complete']}, "
                f"final_is_end_of_run={report['final_is_end_of_run']}")
        out.add('workload predictions', True, 'skipped, the run was cut short on purpose')
        return out

    # 2. Map, Piece and Unmap against the emulator log.
    calls, table, steps = log_oracle(log)
    guest_calls = [c for c in calls if c[3] != 'SceGnmDriver']
    maps = event_maps(capture)
    logged = sorted((va, size, phys) for va, size, phys, _ in calls)
    recorded = sorted((m['va'], m['size'], m['pieces'][0][2] if m['pieces'] else None)
                      for m in maps)
    out.add('direct Map events = logged map calls', logged == recorded,
            f'{len(recorded)} maps' if logged == recorded else
            f'log {len(logged)}, events {len(recorded)}')
    out.add('each direct Map has one Piece over its range',
            all(len(m['pieces']) == 1 and m['pieces'][0][:2] == (m['va'], m['size'])
                for m in maps))
    native = census_at(capture, None)
    replayed, event_steps = replay_direct(capture)
    out.add('final direct page table = log page table', replayed == table,
            f'{len(table)} pages' if replayed == table else
            f'log {len(table)} pages, events {len(replayed)}')
    same = [a[:3] for a in steps] == [b[:3] for b in event_steps]
    tables = same and all(a[3] == b[3] for a, b in zip(steps, event_steps))
    out.add('direct page table after every map and munmap call = log', tables,
            f"{len(steps)} calls, largest table {max(len(s[3]) for s in steps)} pages"
            if tables else 'call sequences differ' if not same else 'page tables differ')

    # 3. Common predictions.
    if 'lavapipe' in launch['environment'].get('VK_DRIVER_FILES', '') or \
            'lvp' in launch['environment'].get('VK_DRIVER_FILES', ''):
        out.equal('recorded block (lavapipe)', native['block_bytes'],
                  COMMON['recorded_block_bytes_lavapipe'])
    if native['block_bytes'] <= PAGE:
        peak = native['at_peak_backed']
        out.equal('irregular blocks at the recorded block', peak['blocks'].get('irregular', 0),
                  COMMON['irregular_blocks_at_16k'])
        out.equal('nonzero-phase blocks at the recorded block',
                  sum(n for phase, n in peak['regular_phase_bytes'].items() if phase),
                  COMMON['nonzero_phase_blocks_at_16k'])

    # 4. The emulator's own decision, shared backing only.
    chunk = CHUNK.search(log)
    if shared and chunk:
        actual = census_at(capture, None, int(chunk.group(1)))
        wrong = []
        for name, stats in actual['buffer_requests'].items():
            for path, count in stats['paths'].items():
                if (name == 'contiguous') != (path == 'shared'):
                    wrong.append(f'{count} {name} requests took the {path} path')
        out.add('class contiguous <=> path shared (recorded block, real chunk)', not wrong,
                '; '.join(wrong) or f"{actual['buffer_requests_total']['requests']} requests")

    if not expect:
        out.add('predictions', True, 'none for this workload')
        return out

    # 5. Predictions for this workload.
    at64 = census_at(capture, 65536)
    peak = at64['at_peak_backed']
    if 'direct_mappings' in expect:
        want = sorted(m['bytes'] for m in expect['direct_mappings'] for _ in range(m['maps']))
        out.equal('direct map calls of the guest (bytes)',
                  sorted(size for _, size, _, _ in guest_calls), want)
    if 'direct_mapping_count' in expect:
        out.equal('direct map calls of the guest', len(guest_calls),
                  expect['direct_mapping_count'])
    out.equal('aliased physical pages at peak', peak['aliased_physical_pages'],
              expect['aliased_physical_pages_at_peak'])
    direct = peak.get('blocks_holding_type', {}).get('direct', {})
    blocks = expect.get('direct_blocks_64k', {})
    if 'irregular' in blocks:
        out.equal('irregular direct blocks at 64 KiB', direct.get('irregular', 0),
                  blocks['irregular'])
    if 'nonzero_phase' in blocks:
        out.equal('nonzero-phase direct blocks at 64 KiB',
                  direct.get('regular_nonzero_phase', 0), blocks['nonzero_phase'])
    if 'irregular_min' in blocks:
        out.add('irregular direct blocks at 64 KiB', direct.get('irregular', 0) >=
                blocks['irregular_min'], f"{direct.get('irregular', 0)}")
    if 'direct_blocks_16k' in expect:
        at16 = census_at(capture, 16384)['at_peak_backed']
        out.equal('irregular direct blocks at 16 KiB',
                  at16['blocks_holding_type']['direct'].get('irregular', 0),
                  expect['direct_blocks_16k']['irregular'])
    if 'cross_object_alias_pages_64k' in expect:
        out.equal('cross-object alias pages, phase0 at 64 KiB',
                  peak['policies']['phase0']['cross_object_alias_pages'],
                  expect['cross_object_alias_pages_64k'])
    if 'request_classes_64k' in expect:
        out.equal('request classes at 64 KiB', set(at64['buffer_requests']),
                  expect['request_classes_64k'])
    if 'requests' in expect:
        total = at64['buffer_requests_total']
        out.equal('requests: count, bytes, unique ranges, unique bytes',
                  {k: total[k] for k in ('requests', 'bytes', 'unique_ranges', 'unique_bytes')},
                  {'requests': expect['requests']['count'], 'bytes': expect['requests']['bytes'],
                   'unique_ranges': expect['requests']['unique_ranges'],
                   'unique_bytes': expect['requests']['unique_bytes']})
    if 'request_class_counts_64k' in expect:
        va = int(GUEST_RANGES.search(log).group(1), 16)
        for label, report, key in (
                ('64 KiB', at64, 'request_class_counts_64k'),
                ('16 KiB', census_at(capture, 16384), 'request_class_counts_16k'),
                ('64 KiB, one chunk', census_at(capture, 65536, 1 << 40),
                 'request_class_counts_64k_one_chunk')):
            got, want = classes(report), dict(expect[key])
            if 'discontiguous' in want:
                count = want.pop('discontiguous')
                allowed = noncontiguous_class(va, 65536)
                seen = {name: n for name, n in got.items() if name.startswith('discontiguous')}
                out.add(f'noncontiguous range class at {label}',
                        set(seen) <= allowed and sum(seen.values()) == count,
                        f'{seen}, allowed {sorted(allowed)} for VA {va:#x}')
                got = {name: n for name, n in got.items()
                       if not name.startswith('discontiguous')}
            out.equal(f'request classes at {label}', got, want)
    return out


def replay_direct(capture):
    """Direct pages after replaying Map, Piece and Unmap events, without gpuvm_model: the
    final table, and the table after every direct map and every sceKernelMunmap call."""
    kinds, table, types, steps = analyze.K, {}, {}, []

    def direct():
        return {page: phys for page, phys in table.items() if types[page] == DIRECT}

    for e in trusted_records(capture):
        kind, vma_type = e.kf & 0xffffffff, e.kf >> 32
        if kind == kinds['UnmapCall'] and e.a == 1:     # 1: sceKernelMunmap, 2: pool decommit
            steps.append(('unmap', e.address, e.size, direct()))
        if kind not in (kinds['Map'], kinds['Piece'], kinds['Unmap']) or not e.size:
            continue
        for index, page in enumerate(range(e.address // PAGE,
                                           (e.address + e.size - 1) // PAGE + 1)):
            if kind == kinds['Piece']:
                table[page], types[page] = e.a // PAGE + index, vma_type
            else:
                table.pop(page, None)
                types.pop(page, None)
        if kind == kinds['Piece'] and vma_type == DIRECT:
            steps.append(('map', e.address, e.size, direct()))
    return direct(), steps


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    failed = 0
    for run in sys.argv[1:]:
        if not (Path(run) / 'launch.json').is_file():
            print(f'{run}: not a run directory, skipped')
            continue
        result = check_run(run)
        print(result.run)
        for name, ok, detail in result.rows:
            failed += not ok
            print(f"  {'ok  ' if ok else 'FAIL'} {name}{': ' + detail if detail else ''}")
    print(f'{failed} failed checks')
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
