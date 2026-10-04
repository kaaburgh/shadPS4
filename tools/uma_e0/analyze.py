#!/usr/bin/env python3
"""Offline E0 census. Raw events are never rewritten; uncertainty is part of output."""
import argparse
import bisect
from collections import Counter, defaultdict, namedtuple
import json
from pathlib import Path
import struct

RECORD = struct.Struct('<13Q')
Event = namedtuple('Event', 'seq ns thread kf context session tick address size a b c cmd_seq')
NAMES = ('Map Piece Unmap Protect Vma BlockSize Buffer Snapshot DmaSet DmaRange '
         'Session SubmitSession Submit Complete Packet CpuWrite Irq Fault Watch Tracker '
         'Readback End CommandBegin Command CommandEnd Finish ArenaBind BackingWrite Resident '         'Name MemoryType UnmapCall GpuMap GpuUnmap').split()
K = {name: i + 1 for i, name in enumerate(NAMES)}


def records(path):
    with path.open('rb') as stream:
        while block := stream.read(RECORD.size * 4096):
            if len(block) % RECORD.size:
                raise ValueError('truncated event record')
            for values in RECORD.iter_unpack(block):
                yield Event(*values)


def dist(values):
    values = sorted(values)
    if not values:
        return {'n': 0}
    return {'n': len(values), 'min': values[0], 'p50': values[len(values)//2],
            'p95': values[min(len(values)-1, int(len(values)*.95))], 'max': values[-1]}


def overlap(a, b, x, y):
    return a < y and x < b


def analyze(directory):
    meta = json.loads((directory / 'metadata.json').read_text())
    if meta.get('schema') != 'shadps4-uma-e0/v1' or meta.get('record_bytes') != RECORD.size:
        raise ValueError('unsupported schema/record size')
    path = directory / 'events.bin'
    counts, submits, completed, membership, consumers = Counter(), {}, {}, {}, {}
    finish_by_thread, packets, labels = defaultdict(list), {}, defaultdict(list)
    first, last, n, gaps = None, None, 0, 0
    prior = -1
    for e in records(path):
        kind = e.kf & 0xffffffff
        if not 1 <= kind <= len(NAMES):
            raise ValueError(f'unknown kind {kind}')
        if e.seq <= prior:
            raise ValueError('non-increasing event sequence')
        gaps += e.seq - prior - 1
        prior = e.seq
        first = min(first if first is not None else e.ns, e.ns)
        last = max(last if last is not None else e.ns, e.ns)
        n += 1
        counts[NAMES[kind-1]] += 1
        key = e.context, e.tick
        if kind == K['Submit']:
            submits[key] = e
        elif kind == K['Complete'] and e.a == 0:
            completed[key] = e.ns
        elif kind == K['SubmitSession']:
            membership[e.context, e.session] = key
        elif kind == K['Command'] and e.cmd_seq:
            consumers[e.context, e.cmd_seq] = e
        elif kind == K['Finish']:
            finish_by_thread[e.thread].append(e)
        elif kind == K['Packet']:
            packets[e.c] = e
        elif kind in (K['CpuWrite'], K['Irq']) and e.c:
            labels[e.c].append(e)
    successful = {key: e for key, e in submits.items() if e.a == 0}
    duration = max((last - first) / 1e9, 1e-9) if first is not None else 0
    complete_capture = (meta.get('clean_shutdown') and not meta.get('dropped_events') and
                        not gaps and meta.get('written_events') == n)
    confidence = 'upper bound'  # Actual shader accesses and GPU completion instants are not observed.
    joined, unjoined, candidate_changed = Counter(), Counter(), 0
    confidence_levels = Counter()

    def join(e, consumer=False):
        source = consumers.get((e.context, e.cmd_seq)) if consumer else e
        if source is None:
            return None
        key = membership.get((source.context, source.session))
        return key if key in successful else None

    maps, starts, epochs, pieces = [], [], {}, defaultdict(list)
    raw_sizes, vma_sizes, merged_sizes, lifetimes, touched_lifetimes = [], [], [], [], []
    imports_lifetimes = {'raw': [], 'merged': []}
    use_history = defaultdict(dict)  # epoch -> last submitted use per (scheduler,tick), with intent
    holds = []
    touch = set()
    ever_touch = set()
    raw_imports, merged_imports = {}, {}
    lifecycle = {'raw': Counter(), 'merged': Counter()}
    peaks = Counter()
    unmaps, remaps, gpu_unmaps, gpu_remaps, boundary_cross, raw_cross = 0, 0, 0, 0, 0, 0
    aliases, segmentation, alias_topology = [], [], []
    boundary = Counter()
    block_size = None
    watcher = {}  # page -> (all_write_watchers, buffer_write_watchers, timestamp)
    faults = {}  # thread -> raw fault available for tracker pairing
    hazard = Counter()
    fault_provenance = Counter()
    snapshot_bytes = Counter()
    staleness = Counter()
    dma = Counter()
    resident_sizes = []

    def reindex():
        nonlocal starts
        maps.sort(key=lambda m: m['start'])
        starts = [m['start'] for m in maps]

    def hits(addr, size):
        if not maps or not size:
            return []
        i = max(0, bisect.bisect_right(starts, addr) - 1)
        result = []
        while i < len(maps) and maps[i]['start'] < addr + size:
            m = maps[i]
            if overlap(m['start'], m['end'], addr, addr+size):
                result.append(m)
            i += 1
        return result

    def regions():
        result = []
        for m in maps:
            if m['end'] >= 1 << 40:
                continue
            merge = False
            if result:
                old = result[-1]
                prev = old['members'][-1]
                merge = (old['end'] == m['start'] and prev['prot'] == m['prot'] and
                         prev['type'] == m['type'] and prev['name'] == m['name'] and
                         not prev['no_merge'] and not m['no_merge'])
                if merge and m['type'] == 2:
                    pp, cp = physical(prev), physical(m)
                    merge = bool(pp and cp and pp[-1][1] == cp[0][0] and
                                 pp[-1][2] == cp[0][2])
            if merge:
                result[-1]['end'] = m['end']
                result[-1]['members'].append(m)
            else:
                result.append({'start': m['start'], 'end': m['end'], 'members': [m]})
        return result

    def physical(m):
        out = []
        for p in pieces[m['epoch']]:
            lo, hi = max(p.address, m['start']), min(p.address+p.size, m['end'])
            if lo < hi:
                out.append((p.a + lo-p.address, p.a + hi-p.address, p.b))
        return out

    def region_key(r):
        # Underlying epoch identity matters even when identical VA bounds are remapped.
        return (r['start'], r['end'], tuple((m['epoch'], m['start'], m['end'], m['prot'])
                                            for m in r['members']))

    def sample_aliases():
        sweep = []
        for m in maps:
            for lo,hi,_ in physical(m):
                sweep.extend([(lo,1,m['epoch']),(hi,-1,m['epoch'])])
        active = Counter()
        previous = None
        for pa,delta,epoch in sorted(sweep):
            if previous is not None and pa > previous and active:
                alias_topology.append(len(active))
            active[epoch] += delta
            if not active[epoch]: del active[epoch]
            previous = pa

    def refresh_imports(ns):
        sample_aliases()
        live_epochs = {m['epoch'] for m in maps}
        touch.intersection_update(live_epochs)
        for epoch in list(raw_imports):
            if epoch not in live_epochs:
                lifecycle['raw']['destroy'] += 1
                imports_lifetimes['raw'].append(ns-raw_imports.pop(epoch))
        keys = {region_key(r) for r in regions()}
        for key in list(merged_imports):
            if key not in keys:
                lifecycle['merged']['destroy'] += 1
                imports_lifetimes['merged'].append(ns-merged_imports.pop(key))
        peaks['live_raw_mapping_epochs'] = max(peaks['live_raw_mapping_epochs'], len(live_epochs))
        peaks['live_gpu_eligible_mapping_epochs'] = max(peaks['live_gpu_eligible_mapping_epochs'],
                len({m['epoch'] for m in maps if m['end'] < 1 << 40}))
        peaks['live_import_region_candidates'] = max(peaks['live_import_region_candidates'],len(regions()))
        peaks['gpu_touched_mapping_epochs'] = max(peaks['gpu_touched_mapping_epochs'], len(touch))

    def activate(selected, ns):
        for m in selected:
            epoch = m['epoch']
            touch.add(epoch)
            ever_touch.add(epoch)
            if epoch not in raw_imports:
                raw_imports[epoch] = ns
                lifecycle['raw']['create'] += 1
        selected_epochs = {m['epoch'] for m in selected}
        for r in regions():
            if selected_epochs & {m['epoch'] for m in r['members']}:
                key = region_key(r)
                if key not in merged_imports:
                    merged_imports[key] = ns
                    lifecycle['merged']['create'] += 1
                    merged_sizes.append(r['end']-r['start'])
        peaks['gpu_touched_mapping_epochs'] = max(peaks['gpu_touched_mapping_epochs'], len(touch))
        peaks['simultaneous_raw_imports'] = max(peaks['simultaneous_raw_imports'], len(raw_imports))
        peaks['simultaneous_merged_imports'] = max(peaks['simultaneous_merged_imports'], len(merged_imports))

    def pending(selected, ns, mask, addr=None, size=None):
        result = set()
        for m in selected:
            history = use_history[m['epoch']]
            for key in list(history):
                if completed.get(key, 2**64) <= ns:
                    history.pop(key)
                    continue
                s = successful[key]
                if s.b <= ns and any(intent & mask and (addr is None or overlap(
                        lo, hi, addr, addr+size)) for lo,hi,intent in history[key]):
                    result.add(key)
        return result

    def remove(addr, size, ns):
        selected = hits(addr, size)
        if pending(selected, ns, 3):
            hazard['mapping_overlap_upper_bound'] += 1
        selected_ids = {m['epoch'] for m in selected}
        old_live = {m['epoch'] for m in maps}
        new = []
        for m in maps:
            if not overlap(m['start'], m['end'], addr, addr+size):
                new.append(m)
                continue
            if m['start'] < addr:
                new.append(dict(m, end=addr))
            if m['end'] > addr+size:
                new.append(dict(m, start=addr+size))
            # Any partial unmap invalidates a whole raw-import candidate: recreate on next touch.
            if m['epoch'] in raw_imports:
                lifecycle['raw']['destroy'] += 1
                imports_lifetimes['raw'].append(ns-raw_imports.pop(m['epoch']))
        maps[:] = new
        reindex()
        ended = old_live - {m['epoch'] for m in maps}
        for epoch in ended:
            lifetimes.append(ns-epochs[epoch].ns)
            if epoch in ever_touch:
                touched_lifetimes.append(ns-epochs[epoch].ns)
        refresh_imports(ns)
        return bool(selected), bool(selected_ids & ever_touch)

    for e in records(path):
        kind, flags = e.kf & 0xffffffff, e.kf >> 32
        if kind == K['BlockSize']:
            block_size = e.size
        elif kind == K['Map']:
            replaced, gpu_replaced = remove(e.address, e.size, e.ns)
            remaps += replaced
            gpu_remaps += gpu_replaced
            if flags not in (0, 1, 5):  # mapped types; GPU eligibility determined separately
                epochs[e.context] = e
                maps.append(dict(start=e.address, end=e.address+e.size, epoch=e.context,
                                 prot=e.a, type=flags, name=e.c, no_merge=e.b >> 32))
                raw_sizes.append(e.size)
                reindex()
                refresh_imports(e.ns)
        elif kind == K['Piece']:
            pieces[e.context].append(e)
            refresh_imports(e.ns)
        elif kind == K['Unmap']:
            _, used = remove(e.address, e.size, e.ns)
            unmaps += 1
            gpu_unmaps += used
            if e.c == 100:
                remaps += 1
                gpu_remaps += used
        elif kind == K['Protect']:
            selected = hits(e.address, e.size)
            if pending(selected, e.ns, 3):
                hazard['protection_overlap_upper_bound'] += 1
            new = []
            for m in maps:
                if not overlap(m['start'], m['end'], e.address, e.address+e.size):
                    new.append(m)
                    continue
                lo, hi = max(m['start'], e.address), min(m['end'], e.address+e.size)
                if m['start'] < lo: new.append(dict(m, end=lo))
                new.append(dict(m, start=lo, end=hi, prot=e.a))
                if hi < m['end']: new.append(dict(m, start=hi))
            maps[:] = new
            reindex()
            refresh_imports(e.ns)
        elif kind in (K['Name'], K['MemoryType']):
            new = []
            for m in maps:
                if not overlap(m['start'], m['end'], e.address, e.address+e.size):
                    new.append(m)
                    continue
                lo, hi = max(m['start'],e.address), min(m['end'],e.address+e.size)
                if m['start'] < lo: new.append(dict(m,end=lo))
                changed = dict(m,start=lo,end=hi)
                if kind == K['Name']: changed['name'] = e.a
                else:
                    pieces[m['epoch']] = [p._replace(b=e.a) if overlap(p.address,p.address+p.size,lo,hi) else p
                                          for p in pieces[m['epoch']]]
                new.append(changed)
                if hi < m['end']: new.append(dict(m,start=hi))
            maps[:] = new
            reindex()
            refresh_imports(e.ns)
        elif kind == K['Vma'] and e.address+e.size < 1 << 40:
            vma_sizes.append(e.size)
            segmentation.append(e.b)
            if block_size:
                boundary['vma_samples'] += 1
                boundary['vma_start_unaligned'] += bool(e.address % block_size)
                boundary['vma_end_unaligned'] += bool((e.address+e.size) % block_size)
        elif kind in (K['Buffer'], K['DmaRange']):
            selected = hits(e.address, e.size)
            key = join(e, consumer=kind == K['Buffer'])
            if key:
                joined[NAMES[kind-1]] += 1
                confidence_levels['recorded intent joined; actual accessed bytes unknown'] += 1
                source = consumers.get((e.context, e.cmd_seq))
                if source and e.tick != source.tick:
                    candidate_changed += 1
                for m in selected if kind == K['DmaRange'] or e.c == 0 else []:
                    intervals = use_history[m['epoch']].setdefault(key, set())
                    intervals.add((max(m['start'], e.address), min(m['end'], e.address+e.size), e.a))
            else:
                unjoined[NAMES[kind-1]] += 1
                confidence_levels['unobservable exact submission association'] += 1
            activate(selected, e.ns)
            if kind == K['Buffer']:
                raw_cross += len(selected) > 1
                boundary_cross += sum(overlap(r['start'], r['end'], e.address, e.address+e.size)
                                      for r in regions()) > 1
            else:
                dma['candidate_range_events'] += 1
                dma['candidate_range_bytes'] += e.size
        elif kind == K['Snapshot']:
            snapshot_bytes[str(e.a)] += e.size
            (joined if join(e, consumer=e.a in (1,2)) else unjoined)['Snapshot'] += 1
        elif kind == K['DmaSet']:
            dma['shader_uses' if e.c == 1 else 'resident_set_observations'] += 1
        elif kind == K['Resident']:
            holds.append((e.address, e.address+e.size))
            resident_sizes.append(e.size)
        elif kind == K['Watch']:
            previous = watcher.get(e.address, (0, 0, 0))
            delta = 0
            if flags == 2 and e.c & 1:
                delta = (e.c >> 8) & 255
                if delta > 127: delta -= 256
            watcher[e.address] = e.a, max(0, previous[1] + delta), e.ns
        elif kind == K['Fault']:
            faults[e.thread] = e
            page = e.address & ~4095
            watch = watcher.get(page)
            provenance = ('BufferCache watcher active; first-after-upload unproven'
                          if complete_capture and watch and watch[1] > 0 else
                          'observed CPU access to watched page; watcher provenance uncertain')
            fault_provenance[provenance] += 1
            if pending(hits(e.address, e.size), e.ns, 1 if e.a else 2, e.address, e.size):
                hazard['WAR_fault_upper_bound' if e.a else 'RAW_fault_upper_bound'] += 1
        elif kind == K['Tracker']:
            fault = faults.get(e.thread)
            if fault and fault.a and 0 <= e.ns-fault.ns < 1000000 and overlap(
                    e.address, e.address+e.size, fault.address, fault.address+fault.size):
                fault_provenance['paired accepted BufferCache invalidation (strong proxy)'] += 1
                faults.pop(e.thread, None)
        elif kind == K['CpuWrite']:
            if pending(hits(e.address, e.size), e.ns, 1, e.address, e.size):
                hazard['WAR_CP_write_upper_bound'] += 1
        elif kind == K['Readback']:
            if pending(hits(e.address, e.size), e.ns, 2, e.address, e.size):
                hazard['RAW_existing_readback_upper_bound'] += 1
        elif kind == K['BackingWrite']:
            alias_mappings = []
            alias_holds = []
            alias_pending = set()
            for m in maps:
                for p in pieces[m['epoch']]:
                    lo, hi = max(m['start'], p.address), min(m['end'], p.address+p.size)
                    plo = p.a + lo-p.address
                    phi = plo + max(0, hi-lo)
                    if overlap(plo, phi, e.a, e.a+e.size):
                        alias_mappings.append(m)
                        alo, ahi = max(plo, e.a), min(phi, e.a+e.size)
                        va_lo = lo + alo-plo
                        alias_pending.update(pending([m], e.ns, 1, va_lo, ahi-alo))
                        if any(overlap(va_lo, va_lo+ahi-alo, a, b) for a, b in holds):
                            alias_holds.append(m['epoch'])
            aliases.append(len({m['epoch'] for m in alias_mappings}))
            if alias_holds:
                staleness['backing_writes_overlapping_mirror_holds'] += 1
                staleness['alias_mapping_overlap_instances'] += len(set(alias_holds))
                staleness[f'origin_{e.b}'] += 1
            if alias_pending:
                hazard['WAR_backing_write_alias_upper_bound'] += 1

    fences, parse_submit_ms, submit_complete_ms = Counter(), [], []
    fence_delays = defaultdict(list)
    for packet_id, p in packets.items():
        key = membership.get((p.context, p.session))
        submit = successful.get(key)
        if submit:
            parse_submit_ms.append((submit.b-p.ns)/1e6)
        for label in labels.get(packet_id, []):
            prior_finish = [f for f in finish_by_thread[p.thread]
                            if f.b == 1 and p.ns <= f.ns <= label.ns and f.context == p.context]
            if prior_finish:
                classification = 'preceded_by_existing_Finish'
            elif submit and label.ns < submit.b:
                classification = 'signalled_before_host_submit_confirmed'
            elif key in completed and label.ns < completed[key]:
                classification = 'signalled_before_observed_completion_upper_bound'
            elif key in completed:
                classification = 'completion_observed_before_signal'
            else:
                classification = 'completion_unassociated_or_unobserved'
            fences[classification] += 1
            if key in completed:
                fence_delays[classification].append((completed[key]-p.ns)/1e6)
    for key, s in successful.items():
        if key in completed:
            submit_complete_ms.append((completed[key]-s.b)/1e6)

    for epoch, p in epochs.items():
        if p.address+p.size < 1 << 40 and block_size:
            boundary['raw_gpu_maps'] += 1
            boundary['raw_start_unaligned'] += bool(p.address % block_size)
            boundary['raw_end_unaligned'] += bool((p.address+p.size) % block_size)
    noncontiguous = 0
    for ps in pieces.values():
        noncontiguous += any(a.a+a.size != b.a for a, b in zip(ps, ps[1:]))
    arena_joins = Counter()
    for e in records(path):
        if (e.kf & 0xffffffff) == K['ArenaBind']:
            arena_joins['bind_submits_joined_to_graphics_submit' if (e.context,e.c) in successful
                        else 'unproven_bind_submits'] += 1
    cost = {}
    for policy in ('raw', 'merged'):
        creates, destroys = lifecycle[policy]['create'], lifecycle[policy]['destroy']
        cost[policy] = {
            'create': creates, 'destroy': destroys,
            'lifecycle_pairs_per_second': min(creates, destroys)/duration if duration else 0,
            'create_per_second': creates/duration if duration else 0,
            'destroy_per_second': destroys/duration if duration else 0,
            'completed_lifetime_ns': dist(imports_lifetimes[policy]),
            'right_censored_live': len(raw_imports if policy == 'raw' else merged_imports),
            'reference_64KiB_object_lifecycle_us': [80, 120],
            'estimate_only_reference_equivalent_ms_per_second': [
                min(creates, destroys)/duration * us/1000 if duration else 0 for us in (80,120)],
            'limitation': 'Not a production estimate; object sizes differ; no universal per-event cost. Partial unmap invalidates the whole raw import; next use recreates remaining fragments.'}
    return {
        'schema': meta['schema'], 'metadata': meta, 'events': n, 'counts': counts,
        'duration_seconds': duration, 'sequence_gaps': gaps, 'complete_transport': bool(complete_capture),
        'drop_effect': 'any drops invalidate exact counts, joins and negative coverage',
        'raw_map_call_size_bytes': dist(raw_sizes),
        'raw_gpu_eligible_map_call_size_bytes': dist([p.size for p in epochs.values() if p.address+p.size < 1 << 40]), 'observed_merged_vma_size_bytes': dist(vma_sizes),
        'gpu_touched_import_candidate_size_bytes': dist(merged_sizes),
        'mapping_completed_lifetime_ns': dist(lifetimes),
        'gpu_touched_completed_mapping_lifetime_ns': dist(touched_lifetimes),
        'right_censored_live_mapping_epochs': len({m['epoch'] for m in maps}),
        'churn': {'raw_maps_per_second': counts['Map']/duration if duration else 0,
                  'raw_unmap_calls_per_second': counts['UnmapCall']/duration if duration else 0,
                  'unmap_affected_subranges_per_second': unmaps/duration if duration else 0,
                  'replacement_subranges_per_second': remaps/duration if duration else 0,
                  'gpu_touched_unmap_subranges': gpu_unmaps, 'gpu_touched_replacement_subranges': gpu_remaps,
                  'note': 'UnmapBytesFromEntry records affected subranges; not necessarily one API call.'},
        'gpu_touched_raw_mapping_epochs': len(ever_touch), 'peaks': peaks,
        'physical_segment_count_per_vma_sample': dist(segmentation),
        'noncontiguous_raw_mapping_epochs': noncontiguous,
        'aliases_per_live_physical_extent_sample': dist(alias_topology),
        'alias_mappings_per_observed_backing_write': dist(aliases),
        'fragmented_live_epochs': sum(sum(m['epoch'] == ep for m in maps) > 1
                                     for ep in {m['epoch'] for m in maps}),
        'boundary_alignment': {'runtime_bda_block_size': block_size, **boundary},
        'ObtainBuffer_crosses_raw_boundary': raw_cross,
        'ObtainBuffer_crosses_import_candidate_boundary': boundary_cross,
        'submitted_joins': joined, 'unproven_or_unsubmitted_joins': unjoined,
        'candidate_tick_changed_before_consumer_recording': candidate_changed,
        'timeline': {'successful_submits': len(successful), 'completion_observations': len(completed),
                     'submitted_ticks_unobserved_at_end': len(set(successful)-set(completed)),
                     'submit_to_observed_completion_ms': dist(submit_complete_ms),
                     'completion_timestamps': 'upper bounds on actual completion'},
        'guest_fence_signals': fences, 'packet_parse_to_submit_ms': dist(parse_submit_ms),
        'packet_parse_to_completion_ms_by_signal_class': {k: dist(v) for k,v in fence_delays.items()},
        'existing_Finish_callers': Counter(str(e.a) for es in finish_by_thread.values() for e in es if e.b == 0),
        'arena_bind_submit_events': counts['ArenaBind'], 'arena_bind_joins': arena_joins,
        'snapshot_bytes_by_path': snapshot_bytes,
        'fault_watcher_provenance': fault_provenance, 'hazard_evidence': hazard,
        'hazard_confidence': confidence, 'buffer_use_evidence_levels': confidence_levels, 'possible_existing_mirror_staleness': staleness,
        'dma_bda': {**dma, 'resident_allocation_bytes': sum(resident_sizes),
                    'resident_allocation_size_bytes': dist(resident_sizes),
                    'exact_shader_accessed_ranges': 'unknown; resident set is a candidate upper bound'},
        'import_lifecycle_models': cost,
        'blind_spots': [
            'No exact shader access set, buffer intent is conservative, failed/HLE paths remain unjoined.',
            'Completion observer times are upper bounds; no completion yet is not proof of pending GPU work.',
            'Snapshot bytes cannot be modified by later guest writes; current mirror WAR is future-shared-backend exposure.',
            'Disabled readbacks cannot establish absence of RAW hazards; UFFD has no direct read-fault observer.',
            'CPU writes via alias VAs have upper-bound coverage only; TryWriteBacking physical aliases are enumerated. Post-remap retired-epoch CPU overlap can be missed.',
            'Mirror hold overlap means possible existing staleness, not proof of stale bytes or a gameplay failure.',
            'Anonymous/file backing identity, old SDK coalescing and host import alignment can limit inferred importability.',
            'Lifetimes still live at capture end are right-censored; buffer-use candidates include unsent work.',
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = analyze(args.capture)
    out = args.output or args.capture / 'summary.json'
    out.write_text(json.dumps(result, indent=2) + '\n')
    lines = ['E0 observation-only census', f"Events: {result['events']}; duration: {result['duration_seconds']:.3f}s",
             f"Dropped: {result['metadata'].get('dropped_events', 'unknown')}; complete transport: {result['complete_transport']}",
             f"GPU-touched raw epochs: {result['gpu_touched_raw_mapping_epochs']}; peaks: {dict(result['peaks'])}",
             f"Boundary alignment: {result['boundary_alignment']}",
             f"Joins: {dict(result['submitted_joins'])}; unproven: {dict(result['unproven_or_unsubmitted_joins'])}",
             f"Guest signals: {dict(result['guest_fence_signals'])}",
             f"Hazard evidence (upper bounds): {dict(result['hazard_evidence'])}",
             f"Possible existing mirror staleness: {dict(result['possible_existing_mirror_staleness'])}",
             'Disabled-mode lack of RAW observations does not establish absence of RAW hazards.',
             'Full distributions and limits: ' + str(out), *result['blind_spots']]
    text = '\n'.join(lines) + '\n'
    out.with_suffix('.txt').write_text(text)
    print(text, end='')

if __name__ == '__main__':
    main()
