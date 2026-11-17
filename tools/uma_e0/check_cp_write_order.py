#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Checks from an E0 capture how CP writes (WRITE_DATA) were ordered with UMA shared backing.

A guest test such as write_data_order or write_data_interleave passes both when a WRITE_DATA
is ordered on the GPU timeline and when the emulator waits for the GPU before writing guest
memory. This check tells the two apart from the host events:

  timeline write   a GPU-timeline CP write (Command 12, vkCmdUpdateBuffer) recorded in the
                   same submission (tick) as the last dispatch before it, with no Finish in
                   between: that dispatch had not executed when the write was recorded, so the
                   order comes from the command stream and not from a wait.
  waited write     a parse-time CP write (CpuWrite of WRITE_DATA) right after a Finish of the
                   same thread: the emulator waited for the GPU and then wrote guest memory.

  check_cp_write_order.py CAPTURE_DIR [--min-timeline N] [--max-waited N]

Prints a JSON summary. Exit status 1 when fewer than N timeline writes were found or more
than N waited writes, 2 when the capture cannot be read.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze  # noqa: E402

K = analyze.K
WRITE_DATA = 0x37
DISPATCH_COMMAND = 3
UPDATE_BUFFER_COMMAND = 12


def check(directory):
    path = directory / 'events.bin'
    last_dispatch = {}  # thread -> dispatch Command event
    finish_since_dispatch = Counter()  # thread -> Finish events since that dispatch
    last_finish_seq = {}  # thread -> seq of the last Finish end
    last_event_seq = {}  # thread -> seq of its previous event
    summary = Counter()
    finish_callers = Counter()
    for e in analyze.records(path):
        kind = e.kf & 0xffffffff
        prev = last_event_seq.get(e.thread)
        last_event_seq[e.thread] = e.seq
        if kind == K['Finish']:
            if e.b == 0:
                finish_callers[str(e.a)] += 1
                finish_since_dispatch[e.thread] += 1
            else:
                last_finish_seq[e.thread] = e.seq
        elif kind == K['Command'] and e.a == DISPATCH_COMMAND:
            last_dispatch[e.thread] = e
            finish_since_dispatch[e.thread] = 0
        elif kind == K['Command'] and e.a == UPDATE_BUFFER_COMMAND:
            summary['gpu_timeline_writes'] += 1
            dispatch = last_dispatch.get(e.thread)
            if dispatch is None:
                summary['gpu_timeline_writes_without_prior_dispatch'] += 1
            elif dispatch.tick == e.tick and finish_since_dispatch[e.thread] == 0:
                summary['timeline_ordered_after_unexecuted_dispatch'] += 1
            else:
                summary['gpu_timeline_writes_after_submitted_dispatch'] += 1
        elif kind == K['CpuWrite'] and e.a == WRITE_DATA:
            summary['parse_time_write_data'] += 1
            if prev is not None and last_finish_seq.get(e.thread) == prev:
                summary['parse_time_write_data_right_after_finish'] += 1
    result = dict(sorted(summary.items()))
    result['finish_starts_by_caller'] = dict(sorted(finish_callers.items()))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('capture', type=Path)
    parser.add_argument('--min-timeline', type=int, default=1)
    parser.add_argument('--max-waited', type=int, default=0)
    args = parser.parse_args()
    try:
        result = check(args.capture)
    except (OSError, ValueError) as error:
        print(f'cannot read capture: {error}', file=sys.stderr)
        return 2
    timeline = result.get('timeline_ordered_after_unexecuted_dispatch', 0)
    waited = result.get('parse_time_write_data_right_after_finish', 0)
    ok = timeline >= args.min_timeline and waited <= args.max_waited
    result['verdict'] = 'PASS' if ok else 'FAIL'
    print(json.dumps(result, indent=2))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
