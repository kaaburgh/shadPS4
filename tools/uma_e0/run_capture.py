#!/usr/bin/env python3
"""Bounded upstream launch; reuse independent historical config/input/screenshot helpers."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import shutil
import time

AREA = Path('/media/ubuntu/UsbSSD447G/shadps4')
REPO = Path(__file__).resolve().parents[2]
BENCHMARK = Path('/home/ubuntu/bb-shadPS4-correctness-instrumentation/tools/run_bb_death_reload_benchmark.py')
X11_PYTHON = AREA / 'work/bb-1.09-exploration/diagnostics/x11-venv/bin/python'
X11_CONTROL = AREA / 'work/bb-1.09-exploration/diagnostics/x11_control.py'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--mode', required=True, choices=['Disabled', 'Precise'])
    p.add_argument('--seconds', type=float, default=None)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--binary', type=Path, default=AREA/'shadPS4-build-uma-e0/shadps4')
    p.add_argument('--target', type=Path, default=AREA/'games/CUSA03173-1.09-DISPOSABLE')
    p.add_argument('--display', default=os.environ.get('DISPLAY', ':1'))
    p.add_argument('--xauthority', default=os.environ.get('XAUTHORITY'))
    p.add_argument('--off', action='store_true')
    p.add_argument('--death-loop', action='store_true', help='automatic event-gated death/reload campaign from prepared save')
    p.add_argument('--iterations', type=int, default=2)
    p.add_argument('--stage-timeout', type=float, default=90)
    p.add_argument('--iteration-timeout', type=float, default=90)
    p.add_argument('--profile-template', type=Path, default=Path('/home/ubuntu/.local/share/shadPS4'))
    p.add_argument('--x11-hold', action='store_true', help='manual diagnostic hold; not a lifecycle campaign')
    p.add_argument('--death-hold', type=float, default=180)
    p.add_argument('--screenshots', action='store_true')
    p.add_argument('--analyze', action='store_true', help='run full unchanged E0 semantic analyzer after campaign; may be expensive')
    args = p.parse_args()
    args.seconds = args.seconds if args.seconds is not None else (300 if args.death_loop else 30)
    if args.death_loop and args.x11_hold: p.error('choose campaign or diagnostic hold')
    if not 1 <= args.iterations <= 100 or min(args.stage_timeout,args.iteration_timeout) <= 0: p.error('positive iteration count and timeouts required')
    if not 0 < args.seconds <= 3600 or (args.x11_hold and not 0 < args.death_hold <= args.seconds):
        p.error('positive bounds required; seconds <= 3600, diagnostic death-hold <= seconds')
    receipt = args.target/'.bb-env1-disposable-copy.json'
    if not receipt.is_file() or not (args.target/'app/eboot.bin').is_file() or not (args.target/'app-UPDATE').is_dir():
        p.error('verified disposable app + app-UPDATE with copy receipt required')
    # Use the independently reusable configuration helper verbatim; do not invoke its source-dependent benchmark.
    import sys
    sys.path.insert(0, str(BENCHMARK.parent))
    spec = importlib.util.spec_from_file_location('historical_bb_runner', BENCHMARK)
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    args.output.mkdir(parents=True, exist_ok=False)
    profile = None
    if args.death_loop:
        profile = args.output.resolve()/'profile-xdg/shadPS4'
        profile.parent.mkdir(parents=True)
        shutil.copytree(args.profile_template,profile,ignore=shutil.ignore_patterns('log','captures','screenshots'))
        profile.chmod(0o700)
        bundles=list((profile/'home/1000/savedata').glob('*/SPRJ0005'))
        if not bundles:p.error('prepared modern title/SPRJ0005 save bundle missing; preserve original and supply profile template')
        config=profile/'config.json'
        settings=json.loads(config.read_text());settings.setdefault('General',{})['home_dir']=''
        settings.setdefault('Log',{})['skip_duplicate']=False
        config.write_text(json.dumps(settings,indent=2)+'\n')
        runner.CONFIG_PATH=config
    mode = {'Disabled':0,'Precise':2}[args.mode]
    override = runner.TemporaryReadbacksOverride(mode)
    provenance = {'schema':'uma-e0-launch/v1', 'evidence':'exploratory-unverified',
                  'parameters':vars(args).copy(), 'runner_sha256':digest(Path(__file__)),
                  'historical_runner_sha256':digest(BENCHMARK),
                  'input_helper_sha256':digest(X11_CONTROL), 'binary_sha256':digest(args.binary),
                  'source_sha':subprocess.check_output(['git','-C',str(REPO),'rev-parse','HEAD'],text=True).strip(),
                  'copy_receipt_sha256':digest(receipt), 'start_unix_ns':time.time_ns(),
                  'checkpoint':'unverified unless operator confirms', 'raw_log_private':True,
                  'automatic_xml_patches':'disabled by upstream IPC; no PATCH_MEMORY commands sent'}
    provenance['parameters'] = {k:str(v) if isinstance(v,Path) else v for k,v in provenance['parameters'].items()}
    env = os.environ.copy()
    # Existing source instrumentation envs must never silently affect the new upstream launch.
    for key in list(env):
        if key.startswith('SHADPS4_BB_') or key.startswith('SHADPS4_MEMORY_TELEMETRY'):
            env.pop(key)
    env.update(DISPLAY=args.display, SHADPS4_ENABLE_IPC='true')
    if profile is not None:env['XDG_DATA_HOME']=str(profile.parent)
    if args.xauthority: env['XAUTHORITY'] = args.xauthority
    else: env.pop('XAUTHORITY', None)
    env.pop('SHADPS4_UMA_E0_CAPTURE', None)
    if not args.off:
        env.update(SHADPS4_UMA_E0_CAPTURE=str(args.output.resolve()/'capture'),
                   SHADPS4_UMA_E0_HARNESS=f'run_capture.py:{digest(Path(__file__))}',
                   SHADPS4_UMA_E0_PARAMETERS=json.dumps(provenance['parameters'],sort_keys=True),
                   SHADPS4_UMA_E0_SOURCE_SHA=provenance['source_sha'],
                   SHADPS4_UMA_E0_PATCH='automatic XML patches disabled by upstream IPC; no PATCH_MEMORY commands; game update recorded separately')
    controller = None
    bridge = None
    observer = None
    proc = None
    input_proc = None
    try:
        override.apply()
        selected = {}
        for config in [runner.CONFIG_PATH, runner.game_config_path()]:
            if config.is_file():
                data = json.loads(config.read_text())
                selected[str(config)] = {'sha256':digest(config), 'GPU':data.get('GPU',{})}
        provenance['selected_config'] = selected
        # Current patch selection is configuration, not copied payload. Preserve its digest/list.
        provenance['patch_configuration'] = {str(p):digest(p) for p in
            (runner.CONFIG_PATH.parent/'patches').rglob('*.xml') if p.is_file()}
        for config in [runner.CONFIG_PATH.parent/'input_config/default.ini',
                       runner.CONFIG_PATH.parent/'input_config/global.ini',
                       runner.CONFIG_PATH.parent/'input_config/CUSA03173.ini']:
            if config.is_file(): provenance.setdefault('input_config_sha256',{})[str(config)] = digest(config)
        if args.death_loop:
            from deathloop import Controller, Bridge
            from fs_watch import FileObserver
            plan=Path(__file__).with_name('deathloop-plan.json')
            provenance['replay_kind']='historical event-gated input plan; not .bbpad'
            provenance['replay_file']=str(plan);provenance['replay_file_sha256']=digest(plan)
            provenance['harness_components']={f:digest(Path(__file__).with_name(f)) for f in ['deathloop.py','fs_watch.py','x11_bridge.py']}
            provenance['seed_save_bundles']={str(b.relative_to(profile)):{str(f.relative_to(b)):digest(f) for f in b.rglob('*') if f.is_file()} for b in bundles}
            provenance['oracle']='compound host filesystem events + existing splash/PlayGo INFO logs (strong proxy)'
            provenance['checkpoint']='prepared-save identity; Central Yharnam/facing require supporting screenshot verification'
            observer=FileObserver(args.target,profile)
            controller=Controller(args.output,args.iterations,args.stage_timeout,args.iteration_timeout)
        (args.output/'launch.json').write_text(json.dumps(provenance,indent=2)+'\n')
        with (args.output/'emulator-private.log').open('wb') as log:
            proc = subprocess.Popen([str(args.binary),str(args.target/'app/eboot.bin')],
                cwd=AREA/'work/bb-1.09-exploration', env=env, stdin=subprocess.PIPE,
                stdout=log,stderr=subprocess.STDOUT)
            proc.stdin.write(b'RUN\nSTART\n');proc.stdin.flush()
            start = time.monotonic()
            if args.death_loop:
                bridge=Bridge(proc.pid,X11_PYTHON,env)
                controller.bridge=bridge;controller.observer=observer
            if args.x11_hold:
                # This bounded operator confirmation replaces unavailable old source callbacks.
                import select
                print('Reach Central Yharnam using existing controls. Press Enter after confirming the checkpoint.',flush=True)
                while proc.poll() is None and time.monotonic()-start < args.seconds:
                    if select.select([sys.stdin], [], [], .2)[0]:
                        if not sys.stdin.readline(): raise RuntimeError('operator confirmation stdin closed')
                        provenance['checkpoint'] = 'reported by operator: Central Yharnam'
                        hold = min(args.death_hold, args.seconds-(time.monotonic()-start))
                        input_proc = subprocess.Popen([str(X11_PYTHON),str(X11_CONTROL),
                            'key','CUSA03173','s','--hold',str(hold)],env=env,
                            stdout=log,stderr=subprocess.STDOUT)
                        provenance['death_start_elapsed_seconds'] = time.monotonic()-start
                        break
            next_shot = 0
            while proc.poll() is None and time.monotonic()-start < args.seconds:
                elapsed = time.monotonic()-start
                if controller:
                    try:
                        for event in observer.poll():controller.feed(event)
                        controller.logs(args.output/'emulator-private.log')
                        controller.tick()
                        if controller.completed:break
                    except Exception as exc:
                        controller.failure=str(exc);controller.emit('failure',reason=str(exc));break
                if args.screenshots and elapsed >= next_shot:
                    shot = args.output/f'screen-{int(elapsed):04d}.png'
                    result = subprocess.run([str(X11_PYTHON),str(X11_CONTROL),'screenshot',str(shot),
                        '--name','CUSA03173','--pid',str(proc.pid)],env=env,capture_output=True,timeout=5)
                    provenance.setdefault('screenshots',[]).append({'elapsed':elapsed,'returncode':result.returncode})
                    next_shot = elapsed+15
                time.sleep(.05 if controller else .2)
            if controller and not controller.completed and not controller.failure:
                controller.failure='wall timeout or emulator exited before requested iterations'
                controller.emit('failure',reason=controller.failure)
            if controller:
                provenance['lifecycle']=controller.result()
                if bridge:
                    try:bridge.call('release_all')
                    except Exception:pass
                    try:controller.screenshot('final')
                    except Exception:pass
            if proc.poll() is None:
                proc.stdin.write(b'STOP\n');proc.stdin.flush()
                try: proc.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    provenance['forced_termination'] = True
                    proc.terminate()
                    try: proc.wait(timeout=5)
                    except subprocess.TimeoutExpired: proc.kill();proc.wait()
            provenance['exit_code'] = proc.returncode
    finally:
        if input_proc and input_proc.poll() is None:
            input_proc.terminate(); input_proc.wait(timeout=5)
            # Ensure externally pressed key is released by the existing helper's press/release.
            subprocess.run([str(X11_PYTHON),str(X11_CONTROL),'key','CUSA03173','s','--hold','0.01'],
                           env=env,capture_output=True,timeout=5)
        if proc and proc.poll() is None: proc.kill();proc.wait()
        if observer:
            if observer.active_save_bundle:
                relative=str(observer.active_save_bundle.relative_to(profile))
                provenance['active_save_bundle']=relative
                provenance['save_identity']=provenance['seed_save_bundles'].get(relative)
            observer.close()
        if bridge:bridge.close()
        if controller:
            provenance['lifecycle']=controller.result()
            provenance['requested_iterations']=controller.requested
            provenance['completed_iterations']=len(controller.cycles)
            provenance['failure_or_stall']=controller.failure
            (args.output/'lifecycle-summary.json').write_text(json.dumps(controller.result(),indent=2)+'\n')
            controller.close()
        override.restore()
        provenance['end_unix_ns'] = time.time_ns()
        (args.output/'launch.json').write_text(json.dumps(provenance,indent=2)+'\n')
    if (args.output/'emulator-private.log').exists():
        with (args.output/'emulator-private.log').open('rb') as source:
            provenance['device_loss_observed']=any(b'device lost' in line.lower() or b'vk_error_device_lost' in line.lower() for line in source)
    if not args.off and (args.output/'capture/metadata.json').exists():
        from validate_transport import validate
        try:provenance['capture_validation']=validate(args.output/'capture')
        except Exception as exc:provenance['capture_validation']={'success':False,'error':str(exc)}
        if args.analyze or not args.death_loop:
            analysis=subprocess.run(['python3',str(Path(__file__).with_name('analyze.py')),str(args.output/'capture')])
            provenance['semantic_analyzer_exit_code']=analysis.returncode
        else:provenance['semantic_analysis']='deferred; use unchanged analyze.py offline'
    elif not args.off:provenance['capture_validation']={'success':False,'error':'E0 metadata missing'}
    (args.output/'launch.json').write_text(json.dumps(provenance,indent=2)+'\n')
    print(args.output)
    success=provenance.get('exit_code') == 0 and not provenance.get('forced_termination') and not provenance.get('device_loss_observed')
    if not args.off:success=success and provenance.get('capture_validation',{}).get('success',False)
    if provenance.get('semantic_analyzer_exit_code',0):success=False
    if args.death_loop:success=success and provenance.get('lifecycle',{}).get('success',False)
    return 0 if success else 1

if __name__ == '__main__':
    raise SystemExit(main())
