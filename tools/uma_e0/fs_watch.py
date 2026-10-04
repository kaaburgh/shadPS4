#!/usr/bin/env python3
"""External Linux host-file notifications; no emulator or guest-memory hooks."""
import ctypes
import os
from pathlib import Path
import struct
import time

OPEN=0x20
CLOSE_WRITE=0x8
CLOSE_READ=0x10
CREATE=0x100
DELETE=0x200
MOVED_FROM=0x40
MOVED_TO=0x80
OVERFLOW=0x4000
ISDIR=0x40000000
HEADER=struct.Struct('iIII')

class FileObserver:
    def __init__(self, target, profile):
        self.libc=ctypes.CDLL(None,use_errno=True)
        self.libc.inotify_init1.argtypes=[ctypes.c_int]
        self.libc.inotify_add_watch.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.c_uint32]
        self.fd=self.libc.inotify_init1(os.O_NONBLOCK|os.O_CLOEXEC)
        if self.fd<0:raise OSError(ctypes.get_errno(),'inotify_init1')
        self.watches={}
        self.active_save_bundle=None
        root=Path(profile)/'home/1000/savedata'
        if not root.is_dir():raise ValueError('prepared save directory missing')
        self.save_root=root
        self.watch(root,'save-directory',True)
        for p in root.rglob('*'):
            if p.is_dir():self.watch(p,'save-directory',True)
        assets=['menu/title.tpf.dcx','menu/logo_fromsoft.gfx','menu/nowloading2.gfx','sound/sprj_xm24.fsb']
        for relative in assets:
            found=False
            for app in ('app','app-UPDATE'):
                path=Path(target)/app/'dvdroot_ps4'/relative
                if path.is_file():self.watch(path,'/app0/dvdroot_ps4/'+relative,False);found=True
            if not found:raise ValueError('required lifecycle asset absent: '+relative)

    def watch(self,path,label,is_dir):
        wd=self.libc.inotify_add_watch(self.fd,os.fsencode(path),OPEN|CLOSE_READ|CLOSE_WRITE|CREATE|DELETE|MOVED_FROM|MOVED_TO)
        if wd<0:raise OSError(ctypes.get_errno(),'inotify_add_watch',str(path))
        self.watches[wd]=(Path(path),label,is_dir)

    def poll(self):
        result=[]
        while True:
            try:blob=os.read(self.fd,65536)
            except BlockingIOError:break
            offset=0
            while offset<len(blob):
                wd,mask,cookie,length=HEADER.unpack_from(blob,offset);offset+=HEADER.size
                name=os.fsdecode(blob[offset:offset+length].split(b'\0',1)[0]);offset+=length
                if mask&OVERFLOW:raise RuntimeError('inotify queue overflow; lifecycle coverage lost')
                if wd not in self.watches:continue
                path,label,is_dir=self.watches[wd]
                if is_dir:
                    path=path/name
                    if mask&ISDIR:
                        if mask&(CREATE|MOVED_TO) and path.is_dir():
                            self.watch(path,'save-directory',True)
                            for sub in path.rglob('*'):
                                if sub.is_dir():self.watch(sub,'save-directory',True)
                        continue
                    if not (name.startswith('userdata') or name.startswith('backup')):continue
                    # Only prepared SPRJ0005 bundle files, not unrelated profile saves.
                    if 'SPRJ0005' not in path.parts:continue
                    if self.active_save_bundle is None and name=='userdata0010' and mask&OPEN:
                        self.active_save_bundle=path.parent
                    if self.active_save_bundle is not None and path.parent!=self.active_save_bundle:continue
                    label='/savedata0/'+name
                for bit,kind in [(OPEN,'open'),(CLOSE_WRITE,'write_close'),(CLOSE_READ,'read_close'),(DELETE,'unlink'),(MOVED_FROM,'unlink'),(CREATE,'create'),(MOVED_TO,'create')]:
                    if mask&bit:result.append(dict(ns=time.monotonic_ns(),event=kind,path=label,host_path=str(path),mask=mask,cookie=cookie))
        return result

    def close(self):
        if self.fd is not None:os.close(self.fd);self.fd=None
