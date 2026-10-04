import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import Mock
from deathloop import Controller,BANK,SAVE0,SAVE10
from fs_watch import FileObserver

class HarnessTests(unittest.TestCase):
    def controller(self,n=2):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        c=Controller(temp.name,n,plan={'death_axis_key':'s','cross_key':'n','observation_hold_seconds':0})
        self.addCleanup(c.close);c.bridge=Mock();c.screenshot=Mock(return_value='synthetic.png')
        c.start_ready('synthetic control');c.tick()
        return c
    def feed(self,c,kind,path):c.feed(dict(event=kind,path=path,ns=time.monotonic_ns()))
    def cycle(self,c):
        self.feed(c,'unlink','/savedata0/backup0000')
        self.feed(c,'write_close',SAVE0)
        self.feed(c,'read_close',BANK)
        self.feed(c,'open',BANK)
        self.feed(c,'unlink','/savedata0/backup0010')
        self.feed(c,'write_close',SAVE10)
    def test_every_iteration_releases_then_explicitly_restarts(self):
        c=self.controller();self.cycle(c)
        self.assertEqual(len(c.cycles),1);self.assertEqual(c.state,'ready');self.assertFalse(c.completed)
        c.tick();self.assertEqual(c.state,'moving');self.cycle(c)
        self.assertEqual(len(c.cycles),2);self.assertTrue(c.completed)
        presses=[x for x in c.bridge.call.call_args_list if x.args==('press',)]
        releases=[x for x in c.bridge.call.call_args_list if x.args==('release',)]
        self.assertEqual(len(presses),2);self.assertEqual(len(releases),2)
    def test_autosave_without_reload_does_not_count_death(self):
        c=self.controller();self.feed(c,'unlink','/savedata0/backup0000');self.feed(c,'write_close',SAVE0)
        self.assertEqual(c.state,'moving');self.assertEqual(len(c.cycles),0)
    def test_bank_reopen_without_death_candidate_does_not_count(self):
        c=self.controller();self.feed(c,'read_close',BANK);self.feed(c,'open',BANK)
        self.assertEqual(c.state,'moving');self.assertFalse(c.cycles)
    def test_next_start_requires_matching_write_close(self):
        c=self.controller();self.feed(c,'unlink','/savedata0/backup0000');self.feed(c,'write_close',SAVE0)
        self.feed(c,'read_close',BANK);self.feed(c,'open',BANK)
        self.feed(c,'unlink','/savedata0/backup0010');self.feed(c,'read_close',SAVE10)
        self.assertEqual(c.state,'await_next_start');self.assertFalse(c.cycles)
        self.feed(c,'write_close','/savedata0/userdata0001');self.assertFalse(c.cycles)
    def test_timeout_fails_visibly_and_does_not_fabricate_cycle(self):
        c=self.controller();c.entered-=100
        with self.assertRaisesRegex(RuntimeError,'stalled'):c.tick()
        self.assertFalse(c.cycles)
    def test_real_host_notifications_are_observed(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);save=root/'profile/home/1000/savedata/SPRJ0005';save.mkdir(parents=True)
            for name in ('menu/title.tpf.dcx','menu/logo_fromsoft.gfx','menu/nowloading2.gfx','sound/sprj_xm24.fsb'):
                p=root/'target/app/dvdroot_ps4'/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('synthetic')
            observer=FileObserver(root/'target',root/'profile');self.addCleanup(observer.close)
            backup=save/'backup0000';backup.write_text('synthetic');backup.unlink()
            (save/'userdata0000').write_text('synthetic')
            with (root/'target/app/dvdroot_ps4/sound/sprj_xm24.fsb').open('rb') as source:source.read()
            rows=observer.poll();seen={(r['event'],r['path']) for r in rows}
            self.assertIn(('unlink','/savedata0/backup0000'),seen)
            self.assertIn(('write_close',SAVE0),seen)
            self.assertIn(('open',BANK),seen);self.assertIn(('read_close',BANK),seen)

if __name__=='__main__':unittest.main()
