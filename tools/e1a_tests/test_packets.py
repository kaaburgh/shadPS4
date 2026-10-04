"""E1B real owned-packet/lane adapters; Release-GDS retains its E1C expected failure."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
PROBE_DIR=Path(__file__).resolve().parent

class CurrentPacketHelpers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=os.environ.get('CXX') or shutil.which('clang++-19') or shutil.which('clang++') or shutil.which('g++')
        if not compiler:raise RuntimeError('C++23 compiler required for real packet regression probe')
        cls.temp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.temp.cleanup)
        binary=Path(cls.temp.name)/'packet-probe'
        subprocess.run([compiler,'-std=c++23','-O2','-pthread','-I',str(PROBE_DIR/'shims'),'-I',str(ROOT/'src'),
                        str(PROBE_DIR/'packet_probe.cpp'),'-o',str(binary)],check=True)
        output=subprocess.check_output([str(binary)],text=True)
        cls.observed={line.split()[0]:[int(v) for v in line.split()[1:]] for line in output.splitlines()}

    def test_eop_and_release_current_store_before_irq_order(self):
        self.assertEqual(self.observed['eop_after'],[1,1,1])
        self.assertEqual(self.observed['release_scalar_after'],[1,1,1])

    def test_eos_gds_helper_does_not_do_the_external_finish_path(self):
        self.assertEqual(self.observed['eos_gds_helper_no_store'],[0])

    def test_desired_eop_store_not_before_host_completion(self):
        self.assertEqual(self.observed['eop'][0],0,'production adapter must retain the store until completion')

    def test_desired_eos_store_not_before_host_completion(self):
        self.assertEqual(self.observed['eos'][0],0,'production adapter must retain the store until completion')

    def test_desired_release_scalar_not_before_host_completion(self):
        self.assertEqual(self.observed['release_scalar'][0],0,'production adapter must retain the store until completion')

    def test_desired_irq_only_not_before_host_completion(self):
        self.assertEqual(self.observed['eop_irq_only'],[0,0],'production adapter must retain the IRQ until completion')

    @unittest.expectedFailure
    def test_desired_release_gds_irq_waits_for_transfer_and_guest_visibility(self):
        self.assertEqual(self.observed['release_gds'],[1,0,0],'recording GDS transfer is not completion')

if __name__=='__main__':unittest.main()
