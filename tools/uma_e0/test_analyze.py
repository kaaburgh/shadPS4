#!/usr/bin/env python3
import json
from pathlib import Path
import tempfile
import unittest
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze

class CensusTests(unittest.TestCase):
    def capture(self, rows, drops=0):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        data = []
        for i, row in enumerate(rows):
            kind, ns, fields = row
            e = dict(seq=i, ns=ns, thread=1, kf=analyze.K[kind], context=10,
                     session=1, tick=1, address=0, size=0, a=0, b=0, c=0, cmd_seq=0)
            e.update(fields)
            data.append(analyze.RECORD.pack(*(e[k] for k in analyze.Event._fields)))
        (root/'events.bin').write_bytes(b''.join(data))
        (root/'metadata.json').write_text(json.dumps(dict(schema='shadps4-uma-e0/v1',
            record_bytes=104, clean_shutdown=True, written_events=len(rows), dropped_events=drops)))
        return root

    def test_changed_tick_command_join_and_snapshot_exclusion(self):
        rows = [('BlockSize',1,dict(size=65536)),
            ('Map',2,dict(context=101,address=65536,size=131072,kf=analyze.K['Map']|(2<<32),a=3)),
            ('Piece',3,dict(context=101,address=65536,size=131072,a=0)),
            ('Buffer',4,dict(address=65536,size=4096,cmd_seq=200,a=1,c=1)),
            ('Buffer',5,dict(address=73728,size=4096,cmd_seq=200,a=1)),
            ('Command',6,dict(session=2,tick=2,cmd_seq=200,a=1)),
            ('SubmitSession',7,dict(session=2,tick=2)),
            ('Submit',8,dict(session=0,tick=2,b=7)),
            ('CpuWrite',9,dict(address=65536,size=4096)),
            ('CpuWrite',10,dict(address=73728,size=4096)),
            ('Complete',20,dict(session=0,tick=2))]
        r = analyze.analyze(self.capture(rows))
        self.assertEqual(r['submitted_joins']['Buffer'],2)
        self.assertEqual(r['candidate_tick_changed_before_consumer_recording'],2)
        self.assertEqual(r['hazard_evidence']['WAR_CP_write_upper_bound'],1)
        self.assertEqual(r['gpu_touched_raw_mapping_epochs'],1)
        self.assertEqual(r['boundary_alignment']['runtime_bda_block_size'],65536)

    def test_existing_finish_and_early_label_are_distinct(self):
        rows = [('Packet',10,dict(c=100,address=65536,size=4)),
                ('Finish',11,dict(a=3)),
                ('SubmitSession',12,dict()), ('Submit',13,dict(b=12)),
                ('Complete',15,dict()), ('Finish',16,dict(a=3,b=1,c=1,session=2,tick=2)),
                ('CpuWrite',17,dict(c=100,address=65536,size=4,session=2,tick=2)),
                ('Packet',18,dict(c=101,session=2,tick=2)),
                ('CpuWrite',19,dict(c=101,session=2,tick=2)),
                ('SubmitSession',20,dict(session=2,tick=2)),
                ('Submit',21,dict(session=0,tick=2,b=20)),
                ('Complete',30,dict(tick=2))]
        r = analyze.analyze(self.capture(rows))
        self.assertEqual(r['guest_fence_signals']['preceded_by_existing_Finish'],1)
        self.assertEqual(r['guest_fence_signals']['signalled_before_host_submit_confirmed'],1)

    def test_backing_write_affects_alias_mirror_hold(self):
        rows = []
        for epoch, va in [(101,65536),(102,262144)]:
            rows.extend([('Map',len(rows)+1,dict(context=epoch,address=va,size=65536,kf=analyze.K['Map']|(2<<32),a=3)),
                         ('Piece',len(rows)+2,dict(context=epoch,address=va,size=65536,a=0))])
        rows.extend([('Resident',6,dict(address=262144,size=65536)),
                     ('BackingWrite',7,dict(address=65536,size=4,a=0,b=71))])
        r = analyze.analyze(self.capture(rows))
        self.assertEqual(r['possible_existing_mirror_staleness']['backing_writes_overlapping_mirror_holds'],1)
        self.assertEqual(r['alias_mappings_per_observed_backing_write']['max'],2)

    def test_drops_invalidate_exact_and_negative_claims(self):
        r = analyze.analyze(self.capture([('Fault',1,dict(address=65536,size=8,a=1))],drops=3))
        self.assertFalse(r['complete_transport'])
        self.assertIn('watcher provenance uncertain',next(iter(r['fault_watcher_provenance'])))

    def test_failed_submit_and_other_scheduler_never_join(self):
        rows = [('Buffer',1,dict(cmd_seq=1,a=1,address=65536,size=4096)),
                ('Command',2,dict(cmd_seq=1,a=1)),
                ('SubmitSession',3,dict(context=20)),('Submit',4,dict(context=20,b=3)),
                ('SubmitSession',5,dict()),('Submit',6,dict(a=2**64-4,b=5))]
        r=analyze.analyze(self.capture(rows))
        self.assertEqual(r['unproven_or_unsubmitted_joins']['Buffer'],1)

    def test_copy_fill_runtime_command_is_a_consumer(self):
        rows = [('Buffer',1,dict(cmd_seq=100,a=1,address=65536,size=4096)),
                ('Command',2,dict(cmd_seq=100,a=10,session=2,tick=2)),
                ('SubmitSession',3,dict(session=2,tick=2)),
                ('Submit',4,dict(tick=2,b=3))]
        r=analyze.analyze(self.capture(rows))
        self.assertEqual(r['submitted_joins']['Buffer'],1)
        self.assertEqual(r['candidate_tick_changed_before_consumer_recording'],1)

    def test_truncated_stream_rejected(self):
        root=self.capture([('End',1,{})])
        with (root/'events.bin').open('ab') as stream:stream.write(b'x')
        with self.assertRaisesRegex(ValueError,'truncated'):analyze.analyze(root)

class IntegrityTests(unittest.TestCase):
    """analyze.integrity: what makes a capture incomplete, and which records are trusted."""

    def capture(self, seqs, end=True, kinds=None, **metadata):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        data = []
        for i, seq in enumerate(seqs):
            last = end and i == len(seqs) - 1
            kind = (kinds or {}).get(i, analyze.K['End' if last else 'Vma'])
            data.append(analyze.RECORD.pack(seq, i, 1, kind, 0 if last else 10,
                                            0, 0, 0, 0, 0, 0, 0, 0))
        (root / 'events.bin').write_bytes(b''.join(data))
        meta = dict(schema='shadps4-uma-e0/v1', record_bytes=104, clean_shutdown=True,
                    written_events=len(seqs), reserved_events=len(seqs), dropped_events=0)
        meta.update(metadata)
        (root / 'metadata.json').write_text(json.dumps(meta))
        return root

    def problems(self, root):
        return analyze.integrity(root)['problems']

    def test_complete_capture(self):
        state = analyze.integrity(self.capture(range(5)))
        self.assertTrue(state['complete'])
        self.assertEqual((state['events'], state['valid_events'], state['counts']),
                         (5, 5, {'End': 1, 'Vma': 4}))

    def test_end_of_an_observer_is_not_the_end_of_the_capture(self):
        root = self.capture(range(3), end=False, kinds={2: analyze.K['End']})
        self.assertEqual(self.problems(root), ['no End event of the recorder'])

    def test_events_after_the_end(self):
        root = self.capture(range(4))
        with (root / 'events.bin').open('ab') as stream:
            stream.write(analyze.RECORD.pack(4, 4, 1, analyze.K['Vma'], 10, *[0] * 8))
        self.assertEqual(self.problems(root), [
            'metadata counts 4 written events, events.bin holds 5',
            'events after the End event of the recorder'])

    def test_file_that_ends_inside_a_record(self):
        root = self.capture(range(4), end=False, clean_shutdown=False)
        with (root / 'events.bin').open('ab') as stream:
            stream.write(b'x' * 17)
        with self.assertRaisesRegex(ValueError, 'truncated'):
            list(analyze.records(root / 'events.bin'))
        self.assertEqual(len(list(analyze.records(root / 'events.bin', allow_truncated=True))), 4)
        self.assertEqual(len(list(analyze.records(root / 'events.bin', allow_truncated=True,
                                                  limit=2))), 2)
        state = analyze.integrity(root)
        self.assertEqual((state['events'], state['valid_events'], state['trailing_bytes']),
                         (4, 4, 17))
        self.assertEqual(state['problems'], [
            'events.bin ends inside a record (17 trailing bytes)',
            'recorder did not shut down cleanly', 'no End event of the recorder'])

    def test_budget_drops_are_reported_once(self):
        root = self.capture(range(3), end=False, reserved_events=10, dropped_events=7)
        self.assertEqual(self.problems(root), ['7 events dropped by the recorder',
                                               'no End event of the recorder'])

    def test_events_lost_without_being_counted(self):
        root = self.capture(range(3), end=False, reserved_events=10, dropped_events=2,
                            clean_shutdown=False)
        self.assertIn('5 reserved events neither written nor counted as dropped',
                      self.problems(root))

    def test_counted_drops_may_skip_sequence_numbers(self):
        state = analyze.integrity(self.capture([0, 1, 5, 6], dropped_events=3,
                                               reserved_events=7))
        self.assertEqual((state['valid_events'], state['sequence_gaps']), (4, 3))
        self.assertEqual(state['problems'], ['3 events dropped by the recorder'])

    def test_records_after_a_sequence_break_are_not_trusted(self):
        for seqs in ([0, 1, 2, 585439531024, 585439534868, 5], [0, 1, 2, 2, 4, 5],
                     [0, 1, 2, 4, 5, 6]):
            state = analyze.integrity(self.capture(seqs))
            self.assertEqual((state['events'], state['valid_events']), (6, 2), seqs)
            self.assertEqual(state['counts'], {'Vma': 2})
            self.assertIn('record 3 does not continue the sequence', state['problems'][0])
            self.assertIn('no End event of the recorder', state['problems'])

    def test_unknown_kind_ends_the_trusted_part(self):
        state = analyze.integrity(self.capture(range(5), kinds={2: 99}))
        self.assertEqual(state['valid_events'], 1)

    def test_missing_empty_or_foreign_metadata(self):
        root = self.capture(range(3))
        (root / 'metadata.json').write_text('')
        self.assertEqual(self.problems(root), ['metadata.json unreadable: JSONDecodeError'])
        (root / 'metadata.json').unlink()
        self.assertEqual(self.problems(root), ['metadata.json unreadable: FileNotFoundError'])
        (root / 'metadata.json').write_text(json.dumps(dict(schema='other', record_bytes=104)))
        self.assertIn('unsupported schema or record size', self.problems(root))

    def test_write_error_is_reported(self):
        root = self.capture(range(3), end=False, clean_shutdown=False,
                            write_error='No space left on device')
        self.assertIn('recorder stopped writing: No space left on device', self.problems(root))


if __name__=='__main__':unittest.main()
