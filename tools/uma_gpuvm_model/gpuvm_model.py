#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""CPU-only model: guest 16 KiB VA->PA mappings represented by Vulkan sparse buffer binds.

The model has no Vulkan and no emulator dependency. It encodes only the rules the
specification gives for VkSparseMemoryBind on a buffer (offsets and size are multiples of
the sparse block, the range lies inside the memory object) and asks which guest memory maps
a "guest VA" sparse buffer can represent, per host block size and per kind of memory object.

All quantities are in model units of 4 KiB. See docs/research/uma-gpuvm-granularity.md.

    python3 tools/uma_gpuvm_model/gpuvm_model.py scenarios
    python3 tools/uma_gpuvm_model/gpuvm_model.py census CAPTURE_DIR [--block 65536] [--chunk N]
"""
import argparse
from collections import Counter, namedtuple
from itertools import product
import json
from pathlib import Path
import sys

UNIT = 4096
GUEST_PAGE = 16384
UNBACKED = -1  # mapped guest page with no importable backing (code, stack, file, system)

Geometry = namedtuple('Geometry', 'g b backing chunk')
Geometry.__doc__ = """g: guest page, b: host sparse block, backing: guest physical backing,
chunk: import chunk, all in units. Vulkan alignments and the guest page are powers of two,
so either g divides b or b divides g."""


def geometry(block_bytes=65536, backing_bytes=1 << 20, chunk_bytes=None, guest_bytes=GUEST_PAGE):
    g, b = guest_bytes // UNIT, block_bytes // UNIT
    backing = backing_bytes // UNIT
    chunk = (chunk_bytes // UNIT) if chunk_bytes else backing
    assert g and b and (b % g == 0 or g % b == 0), 'block and guest page must nest'
    assert backing % g == 0 and chunk % b == 0 and chunk % g == 0
    return Geometry(g, b, backing, chunk)


class GuestMap:
    """The guest page table: VA page -> PA page, both in 16 KiB guest pages."""

    def __init__(self, pages=None):
        self.pages = dict(pages or {})

    def copy(self):
        return GuestMap(self.pages)

    def map(self, va_page, pa_page, count):
        for i in range(count):
            self.pages[va_page + i] = pa_page + i if pa_page != UNBACKED else UNBACKED

    def unmap(self, va_page, count):
        for i in range(count):
            self.pages.pop(va_page + i, None)

    def unit(self, geom, va_unit):
        """Physical unit behind a VA unit, None if unmapped, UNBACKED if not importable."""
        page = self.pages.get(va_unit // geom.g)
        if page is None or page == UNBACKED:
            return page
        return page * geom.g + va_unit % geom.g

    def blocks(self, geom):
        return sorted({unit // geom.b for page in self.pages
                       for unit in range(page * geom.g, (page + 1) * geom.g)})


class Linear(namedtuple('Linear', 'base length')):
    """A contiguous view of the backing: one udmabuf range or one host-pointer import."""

    def phys(self, offset):
        return self.base + offset


class Stitched(namedtuple('Stitched', 'units')):
    """An arbitrary list of backing units (UDMABUF_CREATE_LIST, or a host import of the
    guest VA range). None is a placeholder page that belongs to no guest mapping."""

    @property
    def length(self):
        return len(self.units)

    def phys(self, offset):
        return self.units[offset]


class BindError(ValueError):
    pass


class HostMap:
    """A sparse buffer indexed by guest VA. bind() enforces the VkSparseMemoryBind rules."""

    def __init__(self, geom):
        self.geom = geom
        self.binds = {}

    def bind(self, resource_offset, size, memory, memory_offset):
        b = self.geom.b
        if resource_offset % b or memory_offset % b or size % b:
            # VUID-VkSparseMemoryBind-resourceOffset-09491
            raise BindError('resourceOffset, memoryOffset and size must be block multiples')
        if size <= 0 or memory_offset >= memory.length or size > memory.length - memory_offset:
            # VUID-VkSparseMemoryBind-size-01098, -memoryOffset-01101, -size-01102
            raise BindError('bind range outside the memory object')
        if isinstance(memory, Linear) and (memory.base < 0 or
                                           memory.base + memory.length > self.geom.backing):
            raise BindError('import outside the backing')
        for i in range(size // b):
            self.binds[resource_offset // b + i] = (memory, memory_offset + i * b)

    def unbind(self, block):
        self.binds.pop(block, None)

    def unit(self, va_unit):
        bound = self.binds.get(va_unit // self.geom.b)
        if bound is None:
            return None
        memory, offset = bound
        return memory.phys(offset + va_unit % self.geom.b)


def violations(guest, host, strict, blocks=None):
    """VA units where the host translation differs from the guest one. In strict mode an
    unmapped guest unit must not reach guest physical memory; otherwise it may (superset)."""
    geom = host.geom
    blocks = set(guest.blocks(geom)) | set(host.binds) if blocks is None else blocks
    out = []
    for block in sorted(blocks):
        for va in range(block * geom.b, (block + 1) * geom.b):
            want, have = guest.unit(geom, va), host.unit(va)
            if want is None:
                if strict and have is not None:
                    out.append((va, want, have))
            elif want != have:
                out.append((va, want, have))
    return out


EMPTY, REGULAR, IRREGULAR, NOT_BACKED = 'empty', 'regular', 'irregular', 'unbacked'
BlockClass = namedtuple('BlockClass', 'kind anchor full')


def classify(guest, geom, block):
    """regular: every mapped unit of the block lies on one contiguous physical run, so
    PA(unit) - (unit - block start) is one constant, the anchor."""
    anchors, mapped = set(), 0
    for va in range(block * geom.b, (block + 1) * geom.b):
        phys = guest.unit(geom, va)
        if phys == UNBACKED:
            return BlockClass(NOT_BACKED, None, False)
        if phys is not None:
            mapped += 1
            anchors.add(phys - (va - block * geom.b))
    if not mapped:
        return BlockClass(EMPTY, None, False)
    if len(anchors) > 1:
        return BlockClass(IRREGULAR, None, mapped == geom.b)
    return BlockClass(REGULAR, anchors.pop(), mapped == geom.b)


PHASE0, MULTIPHASE, STITCH = 'phase0', 'multiphase', 'stitched'
POLICIES = (PHASE0, MULTIPHASE, STITCH)
SHARED, FALLBACK, UNBOUND = 'shared', 'fallback', 'unbound'
Plan = namedtuple('Plan', 'host classes decisions')


def linear_chunk(geom, anchor):
    """The import chunk of the anchor's phase that holds [anchor, anchor + b), or None when
    that range leaves the backing. Chunks of phase p start at p + n * chunk."""
    if anchor < 0 or anchor + geom.b > geom.backing:
        return None
    phase = anchor % geom.b
    base = phase + (anchor - phase) // geom.chunk * geom.chunk
    length = min(geom.chunk, (geom.backing - base) // geom.b * geom.b)
    return Linear(base, length)


def plan(guest, geom, policy, strict=False):
    """Decides every block. phase0: imports start at block multiples (the PR #54 chunk
    layout). multiphase: imports may start at any guest page. stitched: additionally one
    list-built object per block that no contiguous import can serve."""
    host, classes, decisions = HostMap(geom), {}, {}
    for block in guest.blocks(geom):
        cls = classes[block] = classify(guest, geom, block)
        memory = offset = None
        if cls.kind == REGULAR and (cls.full or not strict):
            if policy != PHASE0 or cls.anchor % geom.b == 0:
                memory = linear_chunk(geom, cls.anchor)
                offset = memory and cls.anchor - memory.base
        if memory is None and policy == STITCH and cls.kind in (REGULAR, IRREGULAR):
            start = block * geom.b
            memory = Stitched(tuple(guest.unit(geom, va) for va in range(start, start + geom.b)))
            offset = 0
        if memory is None:
            decisions[block] = FALLBACK if cls.kind != EMPTY else UNBOUND
            continue
        host.bind(block * geom.b, geom.b, memory, offset)
        decisions[block] = SHARED
    return Plan(host, classes, decisions)


def search_block(guest, geom, block, policy, strict):
    """Oracle, independent of classify(): tries every contiguous import and every legal
    memoryOffset for one block. Import bases step by one unit (4 KiB) under multiphase to
    show that finer import alignment than the guest page adds nothing."""
    step = geom.b if policy == PHASE0 else 1
    for base in range(0, geom.backing, step):
        for length in range(geom.b, geom.backing - base + 1, geom.b):
            for offset in range(0, length, geom.b):
                host = HostMap(geom)
                host.bind(block * geom.b, geom.b, Linear(base, length), offset)
                if not violations(guest, host, strict, {block}):
                    return True
    return False


def mixed_alias_pages(guest, geom, decisions):
    """Guest physical pages reachable both through a shared block and through a fallback
    block: the class of issues #5 and #8, which a block-level hybrid keeps."""
    seen = {SHARED: set(), FALLBACK: set()}
    for va_page, pa_page in guest.pages.items():
        if pa_page == UNBACKED:
            continue
        kinds = {decisions.get(unit // geom.b)
                 for unit in range(va_page * geom.g, (va_page + 1) * geom.g)}
        for kind in kinds & set(seen):
            seen[kind].add(pa_page)
    return seen[SHARED] & seen[FALLBACK]


def cross_object_alias_pages(geom, host):
    """Guest physical pages bound through more than one memory object. Vulkan defines
    aliasing per VkDeviceMemory range, so consistency here rests on the hardware."""
    owners = {}
    for memory, offset in host.binds.values():
        for i in range(geom.b):
            phys = memory.phys(offset + i)
            if phys is not None:
                owners.setdefault(phys // geom.g, set()).add(memory)
    return {page for page, memories in owners.items() if len(memories) > 1}


def rebind_cost(geom, guest_before, guest_after, plan_before, plan_after):
    """Blocks whose binding changes between two plans, and the collateral: guest pages
    whose own mapping did not change but whose block is rebound. Vulkan requires that no
    queue accesses a range while its binding changes, so those pages must be idle too."""
    def resolved(a_plan, block):
        return tuple(a_plan.host.unit(va) for va in range(block * geom.b, (block + 1) * geom.b))

    blocks = set(plan_before.decisions) | set(plan_after.decisions)
    rebound = {blk for blk in blocks if resolved(plan_before, blk) != resolved(plan_after, blk)}
    collateral = set()
    for block in rebound:
        for va in range(block * geom.b, (block + 1) * geom.b, geom.g):
            page = va // geom.g
            before = guest_before.pages.get(page)
            if before is not None and before == guest_after.pages.get(page):
                collateral.add(page)
    return {'rebound_blocks': sorted(rebound), 'collateral_pages': sorted(collateral)}


def serve(guest, geom, a_plan, va_page, count):
    """Where a fixed-function consumer (vertex, index, indirect, copy) can take a range
    from: 'arena' when every touched block is shared in the sparse buffer; 'chunk' when the
    range is physically contiguous inside one phase-0 import (the PR #54 per-chunk buffer,
    which has no alignment rule); otherwise 'copy'."""
    units = range(va_page * geom.g, (va_page + count) * geom.g)
    if all(a_plan.decisions.get(unit // geom.b) == SHARED for unit in units):
        return 'arena'
    phys = [guest.unit(geom, unit) for unit in units]
    if None in phys or UNBACKED in phys:
        return 'copy'
    contiguous = all(b - a == 1 for a, b in zip(phys, phys[1:]))
    if contiguous and phys[0] // geom.chunk == phys[-1] // geom.chunk:
        return 'chunk'
    return 'copy'


def request_class(guest, geom, address, size, chunk_bytes=None):
    """What one consumer range [address, address + size) in bytes needs. 'contiguous' is
    served by a physically indexed buffer with no alignment rule. The 'discontiguous_*'
    classes need the sparse VA buffer: with block-aligned imports, with shifted imports, or
    not at all ('irregular': a stitched object, a gather copy or a mirror)."""
    first, last = address // UNIT, (address + size - 1) // UNIT
    phys = [guest.unit(geom, unit) for unit in range(first, last + 1)]
    if None in phys:
        return 'unmapped'
    if UNBACKED in phys:
        return 'unbacked'
    if all(b - a == 1 for a, b in zip(phys, phys[1:])):
        chunk = (chunk_bytes or geom.chunk * UNIT) // UNIT
        return 'contiguous' if phys[0] // chunk == phys[-1] // chunk else 'contiguous_cross_chunk'
    classes = [classify(guest, geom, block) for block in range(first // geom.b, last // geom.b + 1)]
    if any(cls.kind != REGULAR or cls.anchor < 0 for cls in classes):
        return 'discontiguous_irregular'
    if all(cls.anchor % geom.b == 0 for cls in classes):
        return 'discontiguous_phase0'
    return 'discontiguous_regular'


def table_irregular_blocks(guest, geom, entry_units):
    """Shader-side translation with one base address per entry_units of VA: the entries
    that cannot hold one base because their pages lie on different physical runs. Device
    addresses have no alignment rule, so the phase does not matter here."""
    table = geom._replace(b=entry_units)
    return [blk for blk in guest.blocks(table) if classify(guest, table, blk).kind == IRREGULAR]


def summarize(guest, geom, strict=False):
    report = {'block_bytes': geom.b * UNIT, 'mapped_guest_pages': len(guest.pages)}
    classes = Counter()
    phases = Counter()
    for block in guest.blocks(geom):
        cls = classify(guest, geom, block)
        name = cls.kind if cls.kind != REGULAR else ('regular_full' if cls.full
                                                     else 'regular_partial')
        classes[name] += 1
        if cls.kind == REGULAR:
            phases[cls.anchor % geom.b * UNIT] += 1
    report['blocks'] = dict(sorted(classes.items()))
    report['regular_phase_bytes'] = dict(sorted(phases.items()))
    report['policies'] = {}
    for policy in POLICIES:
        a_plan = plan(guest, geom, policy, strict)
        decisions = Counter(a_plan.decisions.values())
        memories = {memory for memory, _ in a_plan.host.binds.values()}
        report['policies'][policy] = {
            'shared_blocks': decisions[SHARED],
            'fallback_blocks': decisions[FALLBACK],
            'linear_imports': sum(isinstance(m, Linear) for m in memories),
            'stitched_objects': sum(isinstance(m, Stitched) for m in memories),
            'mixed_alias_pages': len(mixed_alias_pages(guest, geom, a_plan.decisions)),
            'cross_object_alias_pages': len(cross_object_alias_pages(geom, a_plan.host)),
            'violations': len(violations(guest, a_plan.host, strict,
                                         {blk for blk, d in a_plan.decisions.items()
                                          if d == SHARED})),
        }
    report['table_16k_irregular_entries'] = len(table_irregular_blocks(guest, geom, geom.g))
    report['table_block_irregular_entries'] = len(table_irregular_blocks(guest, geom, geom.b))
    return report


def scenarios():
    """Named guest maps, in guest pages, on a 1 MiB backing (64 pages)."""
    out = {}

    m = GuestMap()
    m.map(16, 8, 8)
    out['aligned'] = (m, 'VA 256K -> PA 128K, 128K long: VA = PA (mod 64K)')

    m = GuestMap()
    m.map(16, 9, 8)
    out['phase'] = (m, 'VA 256K -> PA 144K: contiguous, but VA != PA (mod 64K)')

    m = GuestMap()
    m.map(16, 8, 2)
    m.map(18, 10, 2)
    out['adjacent_same_anchor'] = (m, 'two mappings meet inside a block; allocated and '
                                      'mapped in step, so the run continues')

    m = GuestMap()
    m.map(16, 8, 2)
    m.map(18, 40, 2)
    out['adjacent_other_anchor'] = (m, 'two mappings meet inside a block on different '
                                       'physical runs')

    m = GuestMap()
    m.map(16, 8, 4)
    m.map(32, 8, 4)
    out['alias_same_phase'] = (m, 'two VAs of one physical run, same phase')

    m = GuestMap()
    m.map(16, 8, 4)
    m.map(33, 8, 4)
    out['alias_other_phase'] = (m, 'two VAs of one physical run, phases 0 and 48K')

    m = GuestMap()
    m.map(16, 8, 4)
    m.unmap(18, 1)
    out['partial_unmap'] = (m, 'one 16K page of a bound block is unmapped')

    m = GuestMap()
    m.map(16, 8, 4)
    m.map(18, 40, 1)
    out['remap_one_page'] = (m, 'one 16K page of a bound block is remapped elsewhere')

    m = GuestMap()
    m.map(17, 0, 1)
    out['backing_edge'] = (m, 'the run of a partial block would start before PA 0')

    m = GuestMap()
    m.map(16, 60, 4)
    m.map(20, 0, 4)
    out['noncontiguous_blocks'] = (m, 'whole blocks on unrelated physical runs')

    m = GuestMap()
    m.map(16, 8, 3)
    m.map(19, UNBACKED, 1)
    out['unbacked_neighbour'] = (m, 'a block shares its last page with non-backing memory')
    return out


def replay_census(directory, block_bytes=None, chunk_bytes=16 << 20):
    """Replays a UMA E0 capture. Reports the block classification of the memory map at the
    moment most pages were mapped and at the end, and classifies every BufferCache request
    against the map in effect when it was made. Pages of GPU-range mappings without Piece
    events count as unbacked."""
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'uma_e0'))
    import analyze  # noqa: E402

    kinds = analyze.K
    shift = GUEST_PAGE.bit_length() - 1
    map_kinds = (kinds['Map'], kinds['Piece'], kinds['Unmap'])

    def replay(stop=None):
        guest, seen, peak, peak_at, block = GuestMap(), 0, 0, 0, block_bytes
        requests, request_bytes, memo = Counter(), Counter(), {}
        for e in analyze.records(Path(directory) / 'events.bin'):
            kind, vma_type = e.kf & 0xffffffff, e.kf >> 32
            if kind == kinds['BlockSize'] and block_bytes is None:
                block = e.size
            if e.address + e.size > 1 << 40:
                continue
            if kind == kinds['Buffer'] and e.size and stop is None:
                key = (e.address, e.size)
                if key not in memo:
                    geom = Geometry(GUEST_PAGE // UNIT, (block or 65536) // UNIT, 1 << 40, 1 << 40)
                    memo[key] = request_class(guest, geom, e.address, e.size, chunk_bytes)
                requests[memo[key]] += 1
                request_bytes[memo[key]] += e.size
            if kind not in map_kinds:
                continue
            memo.clear()
            first, count = e.address >> shift, e.size >> shift
            if kind == kinds['Map']:
                guest.unmap(first, count)
                if vma_type not in (0, 1, 5):  # free, reserved, pool-reserved
                    guest.map(first, UNBACKED, count)
            elif kind == kinds['Piece']:
                guest.map(first, e.a >> shift, count)
            else:
                guest.unmap(first, count)
            seen += 1
            # A Piece completes the Map before it, so it may move the peak forward.
            if len(guest.pages) > peak or (kind == kinds['Piece'] and len(guest.pages) == peak):
                peak, peak_at = len(guest.pages), seen
            if seen == stop:
                break
        return guest, seen, peak_at, block, requests, request_bytes

    final, events, peak_at, block, requests, request_bytes = replay()
    at_peak = replay(peak_at)[0] if peak_at else GuestMap()
    block = block or 65536

    def report(guest):
        top = max(list(guest.pages.values()) + [0]) + 1
        backing = -(-(top * GUEST_PAGE) // block) * block
        return summarize(guest, geometry(block, max(backing, block, GUEST_PAGE)))

    return {'events': events, 'chunk_bytes': chunk_bytes,
            'buffer_requests': dict(sorted(requests.items())),
            'buffer_request_bytes': dict(sorted(request_bytes.items())),
            'at_peak_mapped': report(at_peak), 'final': report(final)}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    scen = sub.add_parser('scenarios', help='print the built-in scenarios per block size')
    scen.add_argument('--block', type=int, nargs='*', default=[4096, 16384, 65536])
    scen.add_argument('--strict', action='store_true')
    census = sub.add_parser('census', help='classify the memory map of a UMA E0 capture')
    census.add_argument('directory')
    census.add_argument('--block', type=int, help='sparse block in bytes; default: recorded')
    census.add_argument('--chunk', type=int, default=16 << 20, help='import chunk in bytes')
    args = parser.parse_args()
    if args.command == 'census':
        print(json.dumps(replay_census(args.directory, args.block, args.chunk), indent=1))
        return
    for block_bytes, (name, (guest, text)) in product(args.block, scenarios().items()):
        report = summarize(guest, geometry(block_bytes), args.strict)
        cells = ' '.join('{}={}/{}'.format(policy, v['shared_blocks'],
                                           v['shared_blocks'] + v['fallback_blocks'])
                         for policy, v in report['policies'].items())
        print(f'{block_bytes >> 10:>3}K {name:<22} {cells}  # {text}')


if __name__ == '__main__':
    main()
