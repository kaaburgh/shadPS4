import json
from pathlib import Path
import tempfile
import unittest
from analyze_trace import analyze, REC

ROOT=Path(__file__).resolve().parents[2]
class EvidenceTests(unittest.TestCase):
    def capture(self, directory, early=False, drop=0):
        trace=directory/'trace.jsonl'
        trace.write_text(json.dumps(dict(family=0,queue=0,sequence=0,tick=1,packet=9,parsed_ns=99,submitted_ns=199,observed_ns=305,ready_ns=306,published_ns=321,synchronous=False))+'\n')
        def event(seq,ns,kind,a=0,b=0,c=0):return (seq,ns,7,kind,10,20,1,0,0,a,b,c,0)
        values=[event(0,100,15,c=9),event(1,202,13,b=200),event(2,300,14),event(3,304 if early else 310,16,c=9),event(4,320,17,c=9),event(5,390,26,a=7)]
        (directory/'events.bin').write_bytes(b''.join(REC.pack(*v) for v in values))
        (directory/'metadata.json').write_text(json.dumps(dict(clean_shutdown=True,dropped_events=drop,written_events=len(values))))
        return analyze(trace,directory)
    def test_producer_join_and_existing_finish(self):
        with tempfile.TemporaryDirectory() as tmp:
            result=self.capture(Path(tmp));self.assertTrue(result['E0']['transport_complete'])
            self.assertEqual(result['E0']['joins']['ticket_joined_to_successful_submit'],1)
            self.assertEqual(result['E0']['existing_Finish_callers'],{'7':1})
            self.assertEqual(result['E0']['joins']['store_or_irq_before_E1_completion_observation'],0)
    def test_negative_control_early_store_and_drops(self):
        with tempfile.TemporaryDirectory() as tmp:
            result=self.capture(Path(tmp),early=True,drop=1)
            self.assertFalse(result['E0']['transport_complete'])
            self.assertEqual(result['E0']['joins']['store_or_irq_before_E1_completion_observation'],1)
    def test_write_data_and_other_cp_predicates_stay_outside_completion_lane(self):
        source=(ROOT/'src/video_core/amdgpu/liverpool.cpp').read_text()
        for packet in ('WriteData','DmaData','WaitRegMem','MemSemaphore'):
            blocks=source.split('case PM4ItOpcode::'+packet+':')[1:]
            self.assertTrue(blocks)
            for block in blocks:
                body=block.split('case PM4ItOpcode::',1)[0]
                self.assertNotIn('QueueScalar(',body)
                self.assertNotIn('FlushAndGetSubmittedTick',body)
        for block in source.split('case PM4ItOpcode::WriteData:')[1:]:
            self.assertIn('std::memcpy(',block.split('case PM4ItOpcode::',1)[0])
