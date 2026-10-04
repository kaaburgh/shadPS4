#!/usr/bin/env python3
"""Bounded-memory structural E0 transport check, independent of semantic analysis."""
from array import array
import argparse
import json
from pathlib import Path
import sys

def validate(root):
    root=Path(root);meta=json.loads((root/'metadata.json').read_text())
    if meta['schema']!='shadps4-uma-e0/v1' or meta['record_bytes']!=104 or meta['byte_order']!='little':
        raise ValueError('unsupported E0 event format')
    size=(root/'events.bin').stat().st_size
    if size%104:raise ValueError('partial binary record')
    count=size//104
    result=dict(schema='uma-e0-transport-check/v1',source_schema=meta['schema'],
                record_count=count,bytes=size,dropped_events=meta['dropped_events'],
                clean_shutdown=meta['clean_shutdown'],source_sha=meta['source_sha'],
                semantic_analysis='not performed by this structural checker')
    # Exact contiguous sequence is admissible only for captures with no drops.
    expected=0
    if not meta['dropped_events']:
        with (root/'events.bin').open('rb') as stream:
            while block:=stream.read(104*65536):
                words=array('Q');words.frombytes(block)
                if sys.byteorder!='little':words.byteswap()
                for seq in words[::13]:
                    if seq!=expected:raise ValueError(f'event sequence mismatch at record {expected}')
                    expected+=1
    result['contiguous_sequence_verified']=not meta['dropped_events'] and expected==count
    result['success']=(not meta['dropped_events'] and meta['clean_shutdown'] and
        meta['written_events']==count and result['contiguous_sequence_verified'])
    (root/'transport-check.json').write_text(json.dumps(result,indent=2)+'\n')
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('capture',type=Path)
    a=p.parse_args();r=validate(a.capture);print(json.dumps(r,indent=2));raise SystemExit(0 if r['success'] else 1)
