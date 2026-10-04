import json
from pathlib import Path
import struct
import tempfile
import unittest
from validate_transport import validate

class TransportTests(unittest.TestCase):
    def make(self,sequences,drops=0,clean=True):
        t=tempfile.TemporaryDirectory();self.addCleanup(t.cleanup);p=Path(t.name)
        (p/'events.bin').write_bytes(b''.join(struct.pack('<13Q',n,0,0,1,*([0]*9)) for n in sequences))
        (p/'metadata.json').write_text(json.dumps(dict(schema='shadps4-uma-e0/v1',record_bytes=104,
            byte_order='little',dropped_events=drops,clean_shutdown=clean,source_sha='synthetic',written_events=len(sequences))))
        return p
    def test_complete_sequence_passes(self):self.assertTrue(validate(self.make([0,1,2]))['success'])
    def test_gap_rejected(self):
        with self.assertRaisesRegex(ValueError,'sequence'):validate(self.make([0,2]))
    def test_visible_drop_fails(self):self.assertFalse(validate(self.make([0,1],drops=1))['success'])
    def test_partial_tail_rejected(self):
        p=self.make([0]);(p/'events.bin').write_bytes((p/'events.bin').read_bytes()+b'x')
        with self.assertRaisesRegex(ValueError,'partial'):validate(p)
    def test_unclean_capture_fails(self):self.assertFalse(validate(self.make([0],clean=False))['success'])

if __name__=='__main__':unittest.main()
