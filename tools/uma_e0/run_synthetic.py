#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Record a UMA E0 capture of any guest ELF. No game, no operator, no existing profile.

    python3 tools/uma_e0/run_synthetic.py --shadps4 build/shadps4 --elf TEST.elf \\
        --output RUN_DIR [--xvfb] [--lavapipe] [--shared-backing]

RUN_DIR must not exist. It receives:
  capture/      events.bin and metadata.json, written by the E0 recorder
                (--capture DIR puts them elsewhere, for example on a small file system)
  run.log       combined shadPS4 output
  launch.json   what was run, how it ended, and the capture integrity check
  xdg-data/     the isolated shadPS4 user directory of this run

The run ends when a line matches --marker (default: the SHADTEST result line of
shadps4-open-test), when --seconds have passed, or at --timeout. The harness then waits for
shadPS4 to exit by itself, asks it to stop through IPC, and only then signals it.

Exit status: 0 marker PASS (or --seconds reached) with a capture, 1 marker FAIL,
2 no result or no capture. The recorder is Linux only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import selectors
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze  # noqa: E402

READBACKS = {'disabled': 0, 'relaxed': 1, 'precise': 2}
DEFAULT_MARKER = r'SHADTEST\s+name=\S+\s+status=(?P<status>PASS|FAIL)\b'
LAVAPIPE_ICD = '/usr/share/vulkan/icd.d/lvp_icd.json'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_config(xdg_root, readbacks):
    """A fresh user directory that raises none of shadPS4's first-run dialogs."""
    user = xdg_root / 'shadPS4'
    for sub in ('savedata', 'trophy', 'inputs'):
        (user / 'home' / '1000' / sub).mkdir(parents=True)
    settings = {'GPU': {'readbacks_mode': READBACKS[readbacks],
                        'readback_linear_images_enabled': True}}
    (user / 'config.json').write_text(json.dumps(settings, indent=2) + '\n')
    return settings


def start_xvfb(screen):
    """Starts a private Xvfb on a free display and returns (process, display)."""
    if not shutil.which('Xvfb'):
        raise SystemExit('Xvfb not found (install xvfb)')
    with tempfile.NamedTemporaryFile() as display_file:
        # Xvfb writes its display number to this descriptor once it accepts connections.
        fd = display_file.fileno()
        proc = subprocess.Popen(
            ['Xvfb', '-displayfd', str(fd), '-screen', '0', screen, '-nolisten', 'tcp',
             '-noreset'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, pass_fds=(fd,))
        for _ in range(100):
            number = Path(display_file.name).read_text().strip()
            if number or proc.poll() is not None:
                break
            time.sleep(0.1)
    if not number or proc.poll() is not None:
        proc.kill()
        raise SystemExit('Xvfb did not start')
    return proc, f':{number}'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--shadps4', required=True, type=Path)
    parser.add_argument('--elf', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--marker', default=DEFAULT_MARKER,
                        help='regular expression that ends the run; group "status" is the result')
    parser.add_argument('--seconds', type=float,
                        help='run for this long instead of waiting for --marker')
    parser.add_argument('--timeout', type=float, default=120.0)
    parser.add_argument('--exit-grace', type=float, default=5.0,
                        help='how long to wait for shadPS4 to exit by itself after the result')
    parser.add_argument('--stop', choices=('exit', 'ipc', 'sigterm', 'sigkill'), default='exit',
                        help='first action after the result: wait for the exit, IPC STOP, or '
                             'a signal (the last two give deliberately incomplete captures)')
    parser.add_argument('--readbacks', choices=sorted(READBACKS), default='precise')
    parser.add_argument('--shared-backing', action='store_true',
                        help='set SHADPS4_UMA_SHARED_BACKING=1')
    parser.add_argument('--max-bytes', type=int, help='SHADPS4_UMA_E0_MAX_BYTES')
    parser.add_argument('--capture', type=Path,
                        help='capture directory, must not exist (default: RUN_DIR/capture)')
    parser.add_argument('--lavapipe', action='store_true', help='select the lavapipe ICD')
    parser.add_argument('--xvfb', action='store_true', help='run under a private Xvfb')
    parser.add_argument('--xvfb-screen', default='1280x720x24')
    parser.add_argument('--env', action='append', default=[], metavar='KEY=VALUE')
    parser.add_argument('--label', default='', help='free text stored in launch.json')
    args = parser.parse_args()

    if platform.system() != 'Linux':
        raise SystemExit('the UMA E0 recorder is Linux only')
    binary, elf, output = args.shadps4.resolve(), args.elf.resolve(), args.output.resolve()
    for path in (binary, elf):
        if not path.is_file():
            raise SystemExit(f'not a file: {path}')
    output.mkdir(parents=True)          # must be new: a capture is never overwritten
    capture = args.capture.resolve() if args.capture else output / 'capture'
    if capture.exists():
        raise SystemExit(f'capture directory exists: {capture}')
    marker = re.compile(args.marker)

    env = os.environ.copy()
    for key in [k for k in env if k.startswith('SHADPS4_UMA_')]:
        env.pop(key)
    extra = dict(item.split('=', 1) for item in args.env)
    env.update(extra)
    xdg_root = output / 'xdg-data'
    settings = write_config(xdg_root, args.readbacks)
    parameters = {'elf': elf.name, 'elf_sha256': digest(elf), 'readbacks': args.readbacks,
                  'shared_backing': args.shared_backing, 'stop': args.stop,
                  'label': args.label}
    env.update(XDG_DATA_HOME=str(xdg_root), SHADPS4_ENABLE_IPC='true',
               SHADPS4_UMA_E0_CAPTURE=str(capture),
               SHADPS4_UMA_E0_HARNESS=f'run_synthetic.py:{digest(__file__)}',
               SHADPS4_UMA_E0_PARAMETERS=json.dumps(parameters, sort_keys=True),
               SDL_AUDIO_DRIVER=env.get('SDL_AUDIO_DRIVER', 'dummy'))
    if args.shared_backing:
        env['SHADPS4_UMA_SHARED_BACKING'] = '1'
    if args.max_bytes:
        env['SHADPS4_UMA_E0_MAX_BYTES'] = str(args.max_bytes)
    if args.lavapipe:
        if not Path(LAVAPIPE_ICD).is_file():
            raise SystemExit(f'lavapipe ICD not found: {LAVAPIPE_ICD}')
        env.update(VK_ICD_FILENAMES=LAVAPIPE_ICD, VK_DRIVER_FILES=LAVAPIPE_ICD)

    launch = {'harness': 'run_synthetic.py', 'harness_sha256': digest(__file__),
              'shadps4': str(binary), 'shadps4_sha256': digest(binary),
              'elf': str(elf), 'capture_dir': str(capture), 'parameters': parameters,
              'settings': settings,
              'environment': {k: env[k] for k in sorted(env)
                              if k.startswith(('SHADPS4_', 'VK_', 'SDL_')) or k in extra},
              'host': ' '.join(platform.uname()), 'start_unix_ns': time.time_ns()}
    xvfb = None
    proc = None
    result, lines = None, []
    try:
        if args.xvfb:
            xvfb, env['DISPLAY'] = start_xvfb(args.xvfb_screen)
        if not (env.get('DISPLAY') or env.get('WAYLAND_DISPLAY')):
            raise SystemExit('no DISPLAY or WAYLAND_DISPLAY; pass --xvfb')
        proc = subprocess.Popen([str(binary), str(elf)], cwd=elf.parent, env=env,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, bufsize=0)
        proc.stdin.write(b'RUN\nSTART\n')
        proc.stdin.flush()
        selector = selectors.DefaultSelector()
        selector.register(proc.stdout, selectors.EVENT_READ)
        start = time.monotonic()
        pending = b''

        def feed(data):
            nonlocal pending, result
            pending += data
            *complete, pending = pending.split(b'\n')
            for raw in complete:
                line = raw.decode('utf-8', 'replace')
                lines.append(line)
                match = marker.search(re.sub(r'\x1b\[[0-9;]*m', '', line))
                if match and result is None:
                    result = match.groupdict().get('status') or 'MATCH'
                    launch['marker_line'] = match.group(0)
                    launch['marker_elapsed_seconds'] = time.monotonic() - start

        def pump(until, stop_on_result):
            while time.monotonic() < until and proc.poll() is None:
                if selector.select(timeout=0.05):
                    feed(os.read(proc.stdout.fileno(), 65536))
                if stop_on_result and result is not None:
                    return

        limit = start + (args.seconds if args.seconds else args.timeout)
        pump(limit, stop_on_result=not args.seconds)
        if args.seconds and result is None and proc.poll() is None:
            result = 'SECONDS'
        ended = 'exited by itself'
        if proc.poll() is None and args.stop == 'exit':
            pump(time.monotonic() + args.exit_grace, False)
        if proc.poll() is None and args.stop in ('exit', 'ipc'):
            ended = 'IPC STOP'
            try:
                proc.stdin.write(b'STOP\n')
                proc.stdin.flush()
            except OSError:
                pass
            pump(time.monotonic() + 15, False)
        for name, signum in (('SIGTERM', signal.SIGTERM), ('SIGKILL', signal.SIGKILL)):
            if proc.poll() is None and not (name == 'SIGTERM' and args.stop == 'sigkill'):
                ended = name
                proc.send_signal(signum)
                pump(time.monotonic() + 5, False)
        feed(proc.stdout.read() or b'')
        feed(b'\n')
        launch.update(ended=ended, exit_code=proc.returncode, result=result,
                      elapsed_seconds=time.monotonic() - start)
    finally:
        if proc and proc.poll() is None:
            proc.kill()
            proc.wait()
        if xvfb:
            xvfb.terminate()
            xvfb.wait()
        (output / 'run.log').write_text('\n'.join(lines) + '\n')
        launch['end_unix_ns'] = time.time_ns()
        if (capture / 'metadata.json').is_file():
            launch['capture'] = analyze.integrity(capture)
        (output / 'launch.json').write_text(json.dumps(launch, indent=2) + '\n')

    print(json.dumps({k: launch.get(k) for k in ('result', 'ended', 'exit_code', 'capture')},
                     indent=1))
    if 'capture' not in launch or result is None:
        return 2
    return 1 if result == 'FAIL' else 0


if __name__ == '__main__':
    raise SystemExit(main())
