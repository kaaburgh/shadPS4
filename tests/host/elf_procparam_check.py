#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic ELF fixtures for ReadCompiledSdkVersion / PT_SCE_PROCPARAM.

No commercial game, Sony SDK, PS4 runtime, or homebrew compiler required.
The test checks that the loader diagnoses rejected segment sizes before
attempting a stack copy. Later guest-boot failures do not count as this test.
"""

import argparse
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

ELF_IDENT = b"\x7fELF" + bytes([2, 1, 1, 9, 0]) + b"\0" * 7
PT_SCE_PROCPARAM = 0x61000001


def fixture(file_size: int, memory_size: int) -> bytes:
    header = ELF_IDENT + struct.pack(
        "<HHIQQQIHHHHHH",
        0xFE10, 0x3E, 1, 0, 0x40, 0, 0, 0x40, 0x38, 1, 0x38, 0, 0,
    )
    phdr = struct.pack(
        "<IIQQQQQQ",
        PT_SCE_PROCPARAM, 4, 120, 0, 0, file_size, memory_size, 0x1000,
    )
    return header + phdr + b"B" * max(file_size, 24)


def run_case(executable: pathlib.Path, name: str, image: pathlib.Path,
             should_reject: bool, env: dict[str, str]) -> bool:
    command = [str(executable), str(image)]
    try:
        proc = subprocess.run(
            command, capture_output=True, timeout=20, env=env, check=False
        )
        log = (proc.stdout + proc.stderr).decode("utf-8", "replace")
        ret = proc.returncode
    except subprocess.TimeoutExpired as exc:
        output = (exc.stdout or b"") + (exc.stderr or b"")
        log = output.decode("utf-8", "replace")
        ret = "timeout after loader window"

    diagnostic = "Invalid PT_SCE_PROCPARAM segment size" in log
    sanitizer = any(s in log for s in (
        "AddressSanitizer", "stack-buffer-overflow", "heap-buffer-overflow",
        "runtime error: member access",
    ))
    # An intentionally incomplete synthetic ELF can fail at later startup
    # stages; only the early loader size check is the oracle here.
    good = diagnostic == should_reject and not sanitizer
    print(f"{name}: {'PASS' if good else 'FAIL'} reject={diagnostic} "
          f"sanitizer={sanitizer} exit={ret}")
    if not good:
        print(log[-5000:], file=sys.stderr)
    return good


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--shadps4", type=pathlib.Path, required=True)
    args = parser.parse_args()
    exe = args.shadps4.resolve()
    if not exe.is_file():
        parser.error(f"Not an executable file: {exe}")

    # OrbisProcParam is 96 bytes on x64; sdk_version ends at byte 24.
    scenarios = {
        "valid_96": (96, 96, False),
        "valid_min_24": (24, 24, False),
        "too_small_23": (23, 23, True),
        "oversized_4096": (4096, 4096, True),
        "bad_memsz": (96, 64, True),
    }
    with tempfile.TemporaryDirectory(prefix="shadps4-procparam-") as tmp:
        root = pathlib.Path(tmp)
        env = os.environ.copy()
        env["XDG_DATA_HOME"] = str(root / "data")
        good = True
        for name, (file_size, mem_size, reject) in scenarios.items():
            directory = root / name
            directory.mkdir()
            filename = directory / "eboot.bin"
            filename.write_bytes(fixture(file_size, mem_size))
            good = run_case(exe, name, filename, reject, env) and good

    return 0 if good else 1


if __name__ == "__main__":
    raise SystemExit(main())
