#!/usr/bin/env python3
"""Persistent external input using historical X11 helper functions, bound to child PID."""
import importlib.util
import json
from pathlib import Path
import sys
import time
from Xlib import X, XK, display, protocol
from Xlib.ext import xtest
from PIL import Image

HELPER=Path('/media/ubuntu/UsbSSD447G/shadps4/work/bb-1.09-exploration/diagnostics/x11_control.py')
spec=importlib.util.spec_from_file_location('historical_x11',HELPER)
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
dpy=display.Display();root=dpy.screen().root
pid=int(sys.argv[1]);pressed=set()

def target():
    # Unlike name-only keyboard commands, never select another emulator's window.
    windows=helper.pid_windows(dpy,root,pid)
    windows=[w for w in windows if 'CUSA03173' in (w.get_wm_name() or '')]
    if len(windows)!=1:raise RuntimeError(f'expected one CUSA03173 window for PID {pid}; found {len(windows)}')
    return windows[0]

def key(window,name,down):
    code=dpy.keysym_to_keycode(XK.string_to_keysym(name))
    if not code:raise ValueError('unmapped keysym '+name)
    event=protocol.event.KeyPress if down else protocol.event.KeyRelease
    window.send_event(event(time=X.CurrentTime,root=root,window=window,same_screen=1,child=X.NONE,
        root_x=1,root_y=1,event_x=1,event_y=1,state=0,detail=code),
        event_mask=X.KeyPressMask if down else X.KeyReleaseMask)
    dpy.sync()
    if down:pressed.add(name)
    else:pressed.discard(name)

try:
    for line in sys.stdin:
        req=json.loads(line)
        try:
            window=target()
            action=req['action']
            if action=='focus':helper.activate(dpy,root,window)
            elif action in ('press','release'):key(window,req['key'],action=='press')
            elif action=='tap':
                key(window,req['key'],True);time.sleep(req.get('hold',.1));key(window,req['key'],False)
            elif action=='screenshot':
                g=window.get_geometry();raw=window.get_image(0,0,g.width,g.height,X.ZPixmap,0xffffffff)
                Image.frombytes('RGB',(g.width,g.height),raw.data,'raw','BGRX').save(req['path'])
            elif action=='release_all':
                for name in list(pressed):key(window,name,False)
            elif action=='close':
                for name in list(pressed):key(window,name,False)
                print(json.dumps(dict(ok=True)),flush=True);break
            else:raise ValueError('unknown action')
            print(json.dumps(dict(ok=True,ns=time.monotonic_ns())),flush=True)
        except Exception as exc:print(json.dumps(dict(ok=False,error=str(exc))),flush=True)
finally:
    try:
        window=target()
        for name in list(pressed):key(window,name,False)
    except Exception:pass
    dpy.close()
