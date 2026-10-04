#!/usr/bin/env python3
"""Bounded E1 timing summary; optional join to unchanged E0 transport records."""
import argparse
from array import array
from collections import Counter
import json
from pathlib import Path
import struct

REC = struct.Struct('<13Q')
K = {'Submit':13, 'Complete':14, 'Packet':15, 'CpuWrite':16, 'Irq':17, 'Finish':26}

def distribution(values):
    ordered = sorted(values)
    if not ordered: return {'n':0}
    return {'n':len(ordered), 'min':ordered[0], 'p50':ordered[len(ordered)//2],
            'p95':ordered[min(len(ordered)-1, int(.95*len(ordered)))],
            'p99':ordered[min(len(ordered)-1, int(.99*len(ordered)))], 'max':ordered[-1]}

def analyze(trace, capture=None):
    phases = {k:array('d') for k in ('parse_to_submit_ms','submit_to_observed_ms','ready_to_publication_ms')}
    rows, families, ordering = {}, Counter(), Counter()
    last_sequence, first, last, count, summary = {}, None, None, 0, {}
    with trace.open() as source:
        for line in source:
            row=json.loads(line)
            if row.get('summary'): summary=row;continue
            count+=1;families[str(row['family'])]+=1
            p,s,o,r,x=(row[key] for key in ('parsed_ns','submitted_ns','observed_ns','ready_ns','published_ns'))
            first=min(first or p,p);last=max(last or x,x)
            if not p<=s<=o<=r<=x:ordering['timestamp_violation']+=1
            q=row['queue'];seq=row['sequence']
            if seq!=last_sequence.get(q,-1)+1:ordering['per_queue_sequence_violation']+=1
            last_sequence[q]=seq
            for key,value in zip(phases, ((s-p)/1e6,(o-s)/1e6,(x-r)/1e6)):phases[key].append(value)
            if capture and row['packet']:
                rows[row['packet']]=(row['tick'],o,x,row['synchronous'])
    seconds=(last-first)/1e9 if first is not None else 0
    result={'schema':'shadps4-e1b-timing/v1','trace':str(trace),'published_rows':count,
            'families':families,'ordering':ordering,'trace_summary':summary,'observed_seconds':seconds,
            'actions_per_second':count/seconds if seconds else None,
            'latency':{key:distribution(value) for key,value in phases.items()},
            'limitations':['completion observations are upper bounds, not physical GPU timestamps',
                          'empty_proxy means no recorded command-buffer access before sealing; pending bind dependencies are not classified as GPU commands',
                          'optional trace file I/O affects these measurements; these are first-order smoke evidence',
                          'row publication timestamp follows the guest store and IRQ; E0 writes validate the earlier store boundary']}
    if summary.get('prefix_submits'):
        result['empty_prefix_proxy_fraction']=summary['known_empty_prefixes']/summary['prefix_submits']
    if capture:
        meta=json.loads((capture/'metadata.json').read_text());counts=Counter();joins=Counter();contexts={};submits={};completions={};stores={}
        first_e0,last_e0,n,previous,gaps=None,None,0,-1,0
        finish_callers=Counter()
        with (capture/'events.bin').open('rb') as source:
            while block:=source.read(REC.size*8192):
                if len(block)%REC.size:raise ValueError('truncated E0 stream')
                for e in REC.iter_unpack(block):
                    seq,ns,thread,kf,context,session,tick,address,size,a,b,c,cmd=e
                    if seq<=previous:raise ValueError('non-increasing E0 sequence')
                    gaps+=seq-previous-1;previous=seq;n+=1
                    first_e0=min(first_e0 or ns,ns);last_e0=max(last_e0 or ns,ns)
                    kind=kf & 0xffffffff;counts[str(kind)]+=1
                    if kind==K['Submit'] and a==0:submits[context,tick]=b
                    elif kind==K['Complete'] and a==0:completions[context,tick]=ns
                    elif kind==K['Finish'] and b==0:finish_callers[str(a)]+=1
                    elif kind==K['Packet'] and c in rows:contexts[c]=context
                    elif kind in (K['CpuWrite'],K['Irq']) and c in rows:
                        proof=rows[c]
                        joins['scalar_stores' if kind==K['CpuWrite'] else 'scalar_irqs']+=1
                        if ns<proof[1]:joins['store_or_irq_before_E1_completion_observation']+=1
                        key=contexts.get(c),proof[0]
                        if key not in submits:joins['publication_without_prior_successful_submit']+=1
                        if kind==K['CpuWrite']:stores[c]=ns
                        elif c in stores and ns<stores[c]:joins['irq_before_store']+=1
        for packet,proof in rows.items():
            key=contexts.get(packet),proof[0]
            if key in submits:joins['ticket_joined_to_successful_submit']+=1
            else:joins['unjoined_ticket']+=1
            if key in completions:
                joins['independent_E0_completion_joined']+=1
                if proof[2]<completions[key]:joins['publication_before_independent_observer_upper_bound']+=1
            if proof[3]:joins['explicit_existing_Finish_proof']+=1
        duration=(last_e0-first_e0)/1e9
        result['E0']={'metadata':meta,'events':n,'sequence_gaps':gaps,
                      'transport_complete':bool(meta.get('clean_shutdown') and not meta.get('dropped_events') and not gaps and meta.get('written_events')==n),
                      'duration_seconds':duration,'successful_submits_all_schedulers':len(submits),
                      'submit_rate_all_schedulers':len(submits)/duration,
                      'joins':joins,'existing_Finish_callers':finish_callers,
                      'independent_completion_note':'E0 observer can timestamp completion after CP publication even though the E1 waiter already proved completion; this is not an early-publication failure'}
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('trace',type=Path);p.add_argument('--capture',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    result=analyze(a.trace,a.capture);a.output.write_text(json.dumps(result,indent=2)+'\n');print(a.output)
