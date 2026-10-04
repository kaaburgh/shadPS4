#!/usr/bin/env python3
"""External version of the historical event-gated Bloodborne lifecycle controller."""
import json
from pathlib import Path
import select
import subprocess
import time
from fs_watch import FileObserver

BANK='/app0/dvdroot_ps4/sound/sprj_xm24.fsb'
SAVE0='/savedata0/userdata0000'
SAVE10='/savedata0/userdata0010'
LOADING='/app0/dvdroot_ps4/menu/nowloading2.gfx'
LOGO='/app0/dvdroot_ps4/menu/logo_fromsoft.gfx'

class Bridge:
    def __init__(self,pid,python,env):
        self.proc=subprocess.Popen([str(python),str(Path(__file__).with_name('x11_bridge.py')),str(pid)],
            env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
    def call(self,action,**fields):
        self.proc.stdin.write(json.dumps(dict(action=action,**fields))+'\n');self.proc.stdin.flush()
        if not select.select([self.proc.stdout],[],[],5)[0]:raise RuntimeError('X11 bridge timeout')
        line=self.proc.stdout.readline()
        if not line:raise RuntimeError('X11 bridge exited: '+self.proc.stderr.read()[-1000:])
        result=json.loads(line)
        if not result['ok']:raise RuntimeError(result['error'])
        return result
    def close(self):
        if self.proc.poll() is None:
            try:self.call('close')
            except Exception:pass
            finally:self.proc.stdin.close()
            try:self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:self.proc.kill();self.proc.wait()

class Controller:
    def __init__(self,output,iterations,stage_timeout=90,iteration_timeout=90,plan=None):
        self.output=Path(output);self.requested=iterations
        self.stage_timeout=stage_timeout;self.iteration_timeout=iteration_timeout
        self.plan=plan or json.loads(Path(__file__).with_name('deathloop-plan.json').read_text())
        self.state='boot';self.entered=time.monotonic();self.due=None
        self.flags=set();self.playgo=0;self.candidate=None;self.post_token=None
        self.cycles=[];self.current=None;self.failure=None;self.log_offset=0;self.log_partial=b''
        self.events=(self.output/'lifecycle.jsonl').open('w');self.raw=(self.output/'host-events.jsonl').open('w')
        self.input_events=(self.output/'input-events.jsonl').open('w')
        self.shot_count=0
        self.completed=False;self.bridge=None;self.observer=None
        self.splash=False;self.last_screenshot=0;self.focused=False
    def emit(self,event,**fields):
        row=dict(schema='bb-external-lifecycle/v1',ns=time.monotonic_ns(),state=self.state,
                 iteration=self.current['iteration'] if self.current else (len(self.cycles) if self.state=='complete' or fields.get('next_state')=='complete' else len(self.cycles)+1),event=event,oracle='compound-host-fs+INFO-log (strong proxy)',**fields)
        self.events.write(json.dumps(row)+'\n');self.events.flush();return row
    def transition(self,state):
        self.emit('state_transition',next_state=state);self.state=state;self.entered=time.monotonic();self.due=None
    def action(self,action,**fields):
        self.bridge.call(action,**fields)
        self.input_events.write(json.dumps(dict(ns=time.monotonic_ns(),state=self.state,action=action,**fields))+'\n');self.input_events.flush()
    def screenshot(self,label):
        self.shot_count+=1
        iteration=self.current['iteration'] if self.current else len(self.cycles)+1
        path=self.output/f'iteration-{iteration:03d}-{label}-{self.shot_count:05d}.png'
        self.bridge.call('screenshot',path=str(path))
        return str(path)
    def start_ready(self,source):
        shot=self.screenshot('start-ready')
        self.transition('ready')
        self.current=dict(iteration=len(self.cycles)+1,start_observed_ns=time.monotonic_ns(),
            start_evidence=source,start_screenshot=shot,oracle='compound-host-fs+INFO-log (strong proxy)')
        self.emit('start_observed',source=source,screenshot=shot)
        self.due=time.monotonic()+self.plan['observation_hold_seconds']
    def feed(self,e):
        self.raw.write(json.dumps(e)+'\n');self.raw.flush()
        path,kind=e['path'],e['event']
        if path==LOGO and kind=='read_close':
            self.flags.add('logo_closed')
            if self.state=='intro':self.due=time.monotonic()+self.plan['selector_settle_seconds']
        if self.state=='offline' and path==SAVE10 and kind=='read_close':
            self.flags.add('offline_checked');self.due=time.monotonic()+self.plan['offline_settle_seconds']
        if self.state in ('continue','initial_loading'):
            if path==SAVE0 and kind=='read_close':self.flags.add('initial_save')
            if path==LOADING and kind=='read_close':self.flags.add('loading_closed')
            if path==BANK and kind=='open':self.flags.add('initial_bank')
            if {'initial_save','loading_closed','initial_bank'} <= self.flags and self.playgo>=2:
                self.start_ready('save-read + loading-resource-close + bank-open + >=2 PlayGo calls')
        if self.state=='moving':
            if path=='/savedata0/backup0000' and kind=='unlink':
                self.candidate={'backup_ns':e['ns']}
            if self.candidate and path==SAVE0 and kind=='write_close':
                if 'save_close_ns' not in self.candidate:
                    self.candidate['save_close_ns']=e['ns'];self.emit('death_candidate',source='backup0000 unlink + userdata0000 write close')
                    self.current['death_candidate_screenshot']=self.screenshot('death-candidate')
            if self.candidate and 'save_close_ns' in self.candidate and path==BANK and kind in ('read_close','write_close'):
                self.action('release',key=self.plan['death_axis_key'])
                self.current.update(death_observed_ns=self.candidate['save_close_ns'],reload_start_observed_ns=e['ns'],
                    death_evidence='save sequence followed by sound bank close; strong proxy',
                    death_trigger_reached='confirmed retrospectively by compound death/reload proxy; exact fall position unobserved')
                self.emit('death_observed',source=self.current['death_evidence'])
                self.emit('reload_observed',source='sound bank close after death candidate')
                self.transition('loading');self.post_token=None
        elif self.state=='loading' and path==BANK and kind=='open':
            self.current['reload_end_observed_ns']=e['ns'];self.emit('reload_resource_open')
            self.transition('await_next_start');self.post_token=None
        elif self.state=='await_next_start':
            if path.startswith('/savedata0/backup') and path!='/savedata0/backup0000' and kind in ('unlink','open'):
                self.post_token=path.split('backup')[-1]
            if self.post_token and path=='/savedata0/userdata'+self.post_token and kind=='write_close':
                self.current['next_start_observed_ns']=e['ns']
                self.current['next_start_screenshot']=self.screenshot('next-start')
                self.current['duration_seconds']=(e['ns']-self.current['start_observed_ns'])/1e9
                self.current['completed']=True;self.emit('next_start_observed',source='bank reopen + matching post-load save close')
                self.cycles.append(self.current);self.current=None
                if len(self.cycles)>=self.requested:
                    self.transition('complete');self.completed=True
                else:self.start_ready('previous confirmed next-start; same respawn checkpoint')
    def logs(self,path):
        with path.open('rb') as source:source.seek(self.log_offset);blob=source.read();self.log_offset=source.tell()
        lines=(self.log_partial+blob).split(b'\n');self.log_partial=lines.pop()
        for line in lines:
            if b'sceSystemServiceHideSplashScreen:' in line:
                self.splash=True;self.emit('splash_hidden_log')
            if b'scePlayGoSetInstallSpeed:' in line and self.state in ('continue','initial_loading'):
                self.playgo+=1;self.emit('playgo_speed_log',count=self.playgo)
                if {'initial_save','loading_closed','initial_bank'} <= self.flags and self.playgo>=2:
                    self.start_ready('save-read + loading-resource-close + bank-open + >=2 PlayGo calls')
    def tick(self):
        now=time.monotonic()
        if now-self.entered>self.stage_timeout:raise RuntimeError('lifecycle stalled in '+self.state)
        if self.current and (time.monotonic_ns()-self.current['start_observed_ns'])/1e9>self.iteration_timeout:
            raise RuntimeError('iteration timeout in '+self.state)
        if self.state=='boot' and self.splash:
            if not self.focused:self.action('focus');self.focused=True;self.due=now+self.plan['startup_settle_seconds']
            if now>=self.due:
                self.screenshot('startup');self.action('tap',key=self.plan['cross_key']);self.transition('intro')
                if 'logo_closed' in self.flags:self.due=now+self.plan['selector_settle_seconds']
        elif self.state=='intro' and self.due and now>=self.due:
            self.screenshot('selector');self.action('tap',key=self.plan['cross_key']);self.transition('offline')
        elif self.state=='offline' and self.due and now>=self.due:
            self.screenshot('continue');self.playgo=0;self.action('tap',key=self.plan['cross_key']);self.transition('continue')
            self.due=now+2
        elif self.state=='continue' and self.due and now>=self.due:
            if not ({'initial_save','initial_bank'} & self.flags):
                self.screenshot('continue-recovery');self.action('tap',key=self.plan['cross_key'])
            self.transition('initial_loading')
        elif self.state=='ready' and now>=self.due:
            self.current['input_start_screenshot']=self.screenshot('input-start')
            self.action('press',key=self.plan['death_axis_key']);self.current['input_start_ns']=time.monotonic_ns()
            self.emit('iteration_start',source='explicit LeftY255 press');self.candidate=None;self.transition('moving')
            self.last_screenshot=now
        elif self.state=='moving' and now-self.last_screenshot>=2:
            # Independent evidence only; no pixel score drives or suppresses the lifecycle.
            if len(list(self.output.glob('*moving-*.png'))) < 2048:
                label=f"moving-{int((time.monotonic_ns()-self.current['start_observed_ns'])/1e6):06d}"
                shot=self.screenshot(label);self.emit('movement_observation',screenshot=shot)
            self.last_screenshot=now
    def result(self):
        return dict(schema='bb-external-deathloop/v1',oracle='compound-host-fs+INFO-log (strong proxy)',
            requested_iterations=self.requested,completed_iterations=len(self.cycles),cycles=self.cycles,
            incomplete_iteration=self.current,failure=self.failure,state=self.state,success=self.completed and not self.failure,
            nondeterminism=['host event/log observation latency','loading/shader/OS scheduling','prepared save/facing requirement','input delivered through configured keyboard'],
            precise_visual_death_coverage='screenshots supporting; filesystem sequence is a strong proxy, not OCR')
    def close(self):
        for stream in (self.events,self.raw,self.input_events):stream.close()
