#!/usr/bin/env python3
"""Compile/run the production synchronization adapters without a Vulkan device."""
import os
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='shadps4-e1b-') as temporary:
    for source in ('prefix_test.cpp', 'completion_test.cpp'):
        binary = Path(temporary)/source.removesuffix('.cpp')
        subprocess.run([os.environ.get('CXX', 'clang++-19'), '-std=c++23', '-O2', '-pthread',
                        '-I', str(ROOT/'tools/e1a_tests/shims'), '-I', str(ROOT/'src'),
                        str(Path(__file__).parent/source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)
