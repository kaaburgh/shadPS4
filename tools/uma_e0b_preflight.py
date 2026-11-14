#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pinned host-only E0b probe and evidence gate for AMD/RADV dma-buf imports.

This does NOT run PS4 guest code or assert that the E3 importer already works.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import subprocess
import sys

PROBE_COMMIT = "e35713c7e56e751a01c7d3a6606c3d6c1a65ac5b"
# Durable branch at PROBE_COMMIT, fetched when a clone does not have the commit.
PROBE_BRANCH = "research/uma-e0b-probe"
TESTS = ("T4b.planb_coherence_stress", "T5b.planb_remap_identity")
VARIANTS = ("dmabuf/bda", "dmabuf/ssbo")


def judge(report: dict, mode: str, probe_returncode: int | None = None) -> dict:
    if not isinstance(report, dict) or report.get("tool") != "e0b_uma_probe" or report.get("format") != 1:
        return {"verdict": "INVALID_REPORT", "problems": ["unknown probe report format"]}
    facts_list = report.get("facts")
    results = report.get("results")
    if (not isinstance(facts_list, list) or not isinstance(results, list) or
            any(not isinstance(f, dict) or not isinstance(f.get("key"), str) or
                not isinstance(f.get("value"), str) for f in facts_list)):
        return {"verdict": "INVALID_REPORT", "problems": ["malformed facts or results"]}
    facts = {f["key"]: f["value"] for f in facts_list}
    problems = []
    blocked = []
    fatal = []
    optional_failures = []
    if len(facts) != len(facts_list):
        fatal.append("duplicate fact keys")

    device = facts.get("device.type", "")
    driver = facts.get("device.driver_name", "")
    vendor = facts.get("device.vendor_id", "").lower()
    source = facts.get("dmabuf.source", "")
    if mode == "lavapipe":
        if device != "cpu" or source != "self-test memfd (fake)":
            problems.append("lavapipe mode must use CPU Vulkan and FAKE memfd, never real udmabuf")
    elif mode == "radv":
        if device not in ("discrete", "integrated") or "radv" not in driver.lower() or vendor != "0x1002":
            problems.append("RADV mode must select an actual AMD RADV GPU (no lavapipe/AMDVLK)")
        if source != "/dev/udmabuf":
            blocked.append("real /dev/udmabuf is unavailable: " + (source or "<not reported>"))
    else:
        return {"verdict": "INVALID_REPORT", "problems": ["unknown mode"]}

    if facts.get("device_lost") != "no":
        fatal.append("Vulkan device lost")
    if facts.get("validation_errors") not in ("0", "not enabled"):
        fatal.append("Vulkan validation errors or missing evidence: " + str(facts.get("validation_errors")))

    indexed = {}
    for entry in results:
        if (not isinstance(entry, dict) or
                any(not isinstance(entry.get(k), str) for k in ("test", "variant", "status", "detail")) or
                entry["status"] not in ("PASS", "FAIL", "ERROR", "UNSUPPORTED", "SKIP", "INFO")):
            fatal.append("malformed result: " + repr(entry))
            continue
        key = (entry["test"], entry["variant"])
        if key in indexed:
            fatal.append("duplicate result: " + str(key))
        indexed[key] = entry
        if entry["status"] in ("FAIL", "ERROR"):
            message = "%s/%s: %s: %s" % (*key, entry["status"], entry["detail"])
            expected = (mode == "radv" and device in ("integrated", "discrete") and
                        vendor == "0x1002" and "radv" in driver.lower() and
                        key[0] in TESTS and key[1] in ("host/bda", "host/ssbo") and
                        entry["status"] == "ERROR" and
                        entry["detail"] == "vkAllocateMemory(host import) -> VK_ERROR_UNKNOWN")
            (optional_failures if expected else fatal).append(message)

    # The pinned probe returns 1 for any FAIL/ERROR. Only attribute it to host
    # refusals when all eight expected Plan-B results and run facts are present.
    expected_keys = {(test, variant) for test in TESTS
                     for variant in (*VARIANTS, "host/bda", "host/ssbo")}
    complete = (set(indexed) == expected_keys and len(results) == len(expected_keys) and
                facts.get("device_lost") == "no" and
                facts.get("validation_errors") in ("0", "not enabled"))
    if probe_returncode is None:
        blocked.append("native probe return code not supplied")
        rc_explanation = "unknown: native probe return code not supplied"
    elif type(probe_returncode) is not int:
        fatal.append("invalid native probe return code")
        rc_explanation = "invalid return code"
    elif probe_returncode == 0 and not optional_failures:
        rc_explanation = "success"
    elif probe_returncode == 1 and optional_failures and not fatal and complete:
        rc_explanation = "explained solely by documented optional host-import errors"
    else:
        fatal.append("unexplained or inconsistent native probe return code: " + str(probe_returncode))
        rc_explanation = "unexplained or inconsistent"

    checked = {}
    for test in TESTS:
        for variant in VARIANTS:
            row = indexed.get((test, variant))
            status = row.get("status") if row else "MISSING"
            checked[test + ":" + variant] = status
            if status != "PASS":
                blocked.append(test + "/" + variant + ": " + status +
                               (": " + str(row.get("detail", "")) if row else ""))

    if problems:
        verdict = "WRONG_DEVICE"
    elif fatal:
        verdict = "FAIL"
    elif blocked:
        verdict = "BLOCKED"
    else:
        verdict = "SELFTEST_PASS_NOT_REAL_DMABUF" if mode == "lavapipe" else "RADV_IMPORT_PROTOTYPE_GO"
    return {
        "verdict": verdict,
        "mode": mode,
        "device": {"type": device, "driver": driver, "vendor": vendor,
                   "name": facts.get("device.name"), "dmabuf_source": source},
        "checked": checked,
        "problems": problems,
        "failures": fatal,
        "expected_optional_failures": optional_failures,
        "probe_returncode": probe_returncode,
        "probe_returncode_explanation": rc_explanation,
        "blocked": blocked,
        "note": ("lavapipe uses a fake memfd fd; this never establishes actual "
                 "udmabuf/RADV support or production performance" if mode == "lavapipe"
                 else "GO means 64-KiB-class imported-buffer correctness, NOT large "
                      "import scaling, pin budget, or shadPS4 E3 integration"),
    }


def exit_code(verdict: str) -> int:
    if verdict in ("RADV_IMPORT_PROTOTYPE_GO", "SELFTEST_PASS_NOT_REAL_DMABUF"):
        return 0
    return 1 if verdict == "FAIL" else 2


def command(args: list[str], log: Path, cwd: Path | None = None,
            env: dict[str, str] | None = None) -> int:
    with log.open("w", encoding="utf-8") as stream:
        run = subprocess.run(args, cwd=cwd, env=env, stdout=stream,
                             stderr=subprocess.STDOUT, check=False)
    return run.returncode


def prepare_probe(root: Path, out: Path) -> Path:
    def git(*args: str) -> subprocess.CompletedProcess:
        return subprocess.run(["git", "-C", str(root), *args],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)

    commit = git("cat-file", "-e", PROBE_COMMIT + "^{commit}")
    if commit.returncode:
        fetch = git("fetch", "origin", PROBE_BRANCH)
        if fetch.returncode or git("cat-file", "-e", PROBE_COMMIT + "^{commit}").returncode:
            raise RuntimeError("pinned E0b source is unavailable; fetch origin/" + PROBE_BRANCH +
                               " (wanted " + PROBE_COMMIT + ")")
    archive = out / "e0b-source.tar"
    with archive.open("wb") as stream:
        export = subprocess.run(["git", "-C", str(root), "archive", PROBE_COMMIT,
                                 "tools/e0b_uma_probe"], stdout=stream, check=False)
    if export.returncode:
        raise RuntimeError("git archive of pinned E0b sources failed")
    unpack = out / "source"
    unpack.mkdir()
    if command(["tar", "-xf", str(archive), "-C", str(unpack)],
               out / "extract.log"):
        raise RuntimeError("cannot unpack E0b sources (see extract.log)")
    archive.unlink()
    return unpack / "tools" / "e0b_uma_probe"


def run_probe(args: argparse.Namespace) -> int:
    root = Path(__file__).resolve().parent.parent
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = (Path(args.out) if args.out else
           Path.cwd() / ("e0b-preflight-%s-%s" % (args.mode, stamp))).resolve()
    try:
        out.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        print("Output directory already exists: " + str(out), file=sys.stderr)
        return 2

    manifest = {"mode": args.mode, "probe_source_sha": PROBE_COMMIT,
                "host": platform.platform(), "device_selector": args.device,
                "validation_requested": args.validate, "selected_icd": args.icd}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    try:
        source = prepare_probe(root, out)
        if command(["cmake", "-S", str(source), "-B", str(out / "build"),
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo"], out / "configure.log"):
            raise RuntimeError("CMake configure failed")
        if command(["cmake", "--build", str(out / "build"), "--parallel", "2"],
                   out / "build.log"):
            raise RuntimeError("E0b compile failed")

        env = os.environ.copy()
        flags = []
        if args.mode == "lavapipe":
            icd = Path(args.icd or "/usr/share/vulkan/icd.d/lvp_icd.json")
            if not icd.is_file():
                raise RuntimeError("missing lavapipe ICD: " + str(icd))
            env["VK_ICD_FILENAMES"] = str(icd.resolve())
            env["VK_DRIVER_FILES"] = str(icd.resolve())
            flags += ["--allow-cpu", "--selftest-memfd-as-dmabuf"]
        else:
            env["AMD_VULKAN_ICD"] = "RADV"
        if args.validate:
            flags.append("--validate")
        if args.device is not None:
            flags += ["--device", str(args.device)]
        probe_args = [str(out / "build" / "e0b_uma_probe"),
                      "--tests", "caps,t4b,t5b", "--t4-iters", str(args.t4_iters),
                      "--json", str(out / "probe.json"), *flags]
        probe_rc = command(probe_args, out / "probe.log", env=env)
        if not (out / "probe.json").is_file():
            # The reason, e.g. no matching Vulkan device, is only in the log.
            lines = (out / "probe.log").read_text(errors="replace").strip().splitlines()
            raise RuntimeError("probe did not write JSON (return code %d)%s" %
                               (probe_rc, ": " + lines[-1].strip() if lines else ""))
        result = judge(json.loads((out / "probe.json").read_text()), args.mode, probe_rc)
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        result = {"verdict": "INFRA_FAIL", "problems": [str(error)]}

    (out / "decision.json").write_text(json.dumps(result, indent=2) + "\n")
    print("E0b verdict: " + result["verdict"])
    print("Evidence: " + str(out))
    for reason in result.get("problems", []) + result.get("failures", []) + result.get("blocked", []) + result.get("expected_optional_failures", []):
        print("  - " + reason)
    return exit_code(result["verdict"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="operation", required=True)
    run = sub.add_parser("run", help="extract pinned E0b probe, build and execute")
    run.add_argument("--mode", required=True, choices=("lavapipe", "radv"))
    run.add_argument("--out", help="new evidence directory (must not exist)")
    run.add_argument("--device", type=int, help="Vulkan physical device index")
    run.add_argument("--icd", help="lavapipe ICD path (lavapipe mode only)")
    run.add_argument("--validate", action="store_true",
                     help="require installed Vulkan validation layers")
    run.add_argument("--t4-iters", type=int, default=32)
    classify = sub.add_parser("classify", help="classify a saved probe JSON without a GPU")
    classify.add_argument("--mode", required=True, choices=("lavapipe", "radv"))
    classify.add_argument("--probe-returncode", type=int, help="native probe process exit code (required for GO)")
    classify.add_argument("report", type=Path)
    args = parser.parse_args()
    if args.operation == "run":
        if args.t4_iters < 1 or args.t4_iters > 256:
            parser.error("--t4-iters must be 1..256")
        return run_probe(args)
    try:
        result = judge(json.loads(args.report.read_text()), args.mode, args.probe_returncode)
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return exit_code(result["verdict"])


if __name__ == "__main__":
    raise SystemExit(main())
