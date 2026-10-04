#!/usr/bin/env python3
"""Run E1A source characterization and deterministic regression scaffolding."""
import argparse
from pathlib import Path
import unittest

def cases(suite):
    for item in suite:
        if isinstance(item,unittest.TestSuite):yield from cases(item)
        else:yield item

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--strict-current',action='store_true',help='show desired helper assertions as real failures at E0, exit1')
    args=p.parse_args()
    suite=unittest.defaultTestLoader.discover(str(Path(__file__).parent),pattern='test_*.py')
    if args.strict_current:
        for case in cases(suite):
            method=getattr(type(case),case._testMethodName)
            if getattr(method,'__unittest_expecting_failure__',False):
                method.__unittest_expecting_failure__=False
    result=unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if result.wasSuccessful() else 1)
