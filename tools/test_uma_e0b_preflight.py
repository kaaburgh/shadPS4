#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""No-Vulkan classifier tests: python3 -m unittest tools/test_uma_e0b_preflight.py"""
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from uma_e0b_preflight import judge


def report(mode="radv", status="PASS"):
    device = ("cpu", "llvmpipe", "0x10005", "self-test memfd (fake)") if mode == "lavapipe" else (
        "integrated", "radv", "0x1002", "/dev/udmabuf")
    facts = [{"key": key, "value": value} for key, value in [
        ("device.type", device[0]),
        ("device.driver_name", device[1]),
        ("device.vendor_id", device[2]),
        ("dmabuf.source", device[3]),
        ("device_lost", "no"),
        ("validation_errors", "0"),
    ]]
    return {"tool": "e0b_uma_probe", "format": 1, "facts": facts,
            "results": [{"test": test, "variant": variant, "status": status,
                         "detail": "fake sample"} for test in (
                             "T4b.planb_coherence_stress", "T5b.planb_remap_identity")
                        for variant in ("dmabuf/bda", "dmabuf/ssbo")]}


class VerdictTests(unittest.TestCase):
    def test_radv_success_is_prototype_gate_only(self):
        self.assertEqual(judge(report(), "radv")["verdict"], "RADV_IMPORT_PROTOTYPE_GO")

    def test_lavapipe_success_does_not_claim_real_dma_buf(self):
        self.assertEqual(judge(report("lavapipe"), "lavapipe")["verdict"],
                         "SELFTEST_PASS_NOT_REAL_DMABUF")

    def test_reject_cpu_when_claiming_radv(self):
        self.assertEqual(judge(report("lavapipe"), "radv")["verdict"], "WRONG_DEVICE")

    def test_reject_amdvlk_when_claiming_radv(self):
        data = report()
        next(f for f in data["facts"] if f["key"] == "device.driver_name")["value"] = "AMD proprietary"
        self.assertEqual(judge(data, "radv")["verdict"], "WRONG_DEVICE")

    def test_reject_fake_memfd_in_radv_mode(self):
        data = report()
        next(f for f in data["facts"] if f["key"] == "dmabuf.source")["value"] = "self-test memfd (fake)"
        self.assertEqual(judge(data, "radv")["verdict"], "BLOCKED")

    def test_missing_variant_is_not_success(self):
        data = report()
        data["results"].pop()
        self.assertEqual(judge(data, "radv")["verdict"], "BLOCKED")

    def test_unsupported_is_not_success(self):
        self.assertEqual(judge(report(status="UNSUPPORTED"), "radv")["verdict"], "BLOCKED")

    def test_fail_is_not_success(self):
        self.assertEqual(judge(report(status="FAIL"), "radv")["verdict"], "FAIL")

    def test_error_is_not_success(self):
        self.assertEqual(judge(report(status="ERROR"), "radv")["verdict"], "FAIL")

    def test_validation_error_fails_even_when_tests_pass(self):
        data = report()
        next(f for f in data["facts"] if f["key"] == "validation_errors")["value"] = "1"
        self.assertEqual(judge(data, "radv")["verdict"], "FAIL")

    def test_duplicate_variant_is_not_success(self):
        data = report()
        data["results"].append(data["results"][0])
        self.assertEqual(judge(data, "radv")["verdict"], "FAIL")

    def test_unknown_format(self):
        self.assertEqual(judge({}, "radv")["verdict"], "INVALID_REPORT")


if __name__ == "__main__":
    unittest.main()
