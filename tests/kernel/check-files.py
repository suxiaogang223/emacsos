#!/usr/bin/env python3
"""Compile and exercise the real file backend on a host, including bad archives."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def entry(name, mode, data=b""):
    fields = (1, mode, 0, 0, 1, 1700000000, len(data), 0, 0, 0, 0,
              len(name.encode()) + 1, 0)
    header = b"070701" + b"".join(f"{value:08x}".encode() for value in fields)
    record = header + name.encode() + b"\0"
    record += bytes(-len(record) % 4)
    record += data
    return record + bytes(-len(record) % 4)


with tempfile.TemporaryDirectory(prefix="emacsos-files-") as directory:
    temporary = Path(directory)
    program = temporary / "files-test"
    subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O1", "-g",
                    "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I", str(ROOT / "kernel"), str(ROOT / "kernel/files.c"),
                    str(ROOT / "kernel/file_abi.c"),
                    str(ROOT / "tests/kernel/files-test.c"), "-o", str(program)], check=True)
    archive = (entry("usr", 0o40755) + entry("usr/readonly", 0o100644, b"upstream")
               + entry("usr/alias", 0o120777, b"readonly")
               + entry("TRAILER!!!", 0))
    fixture = temporary / "runtime.cpio"
    fixture.write_bytes(archive)
    env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}
    subprocess.run([str(program), str(fixture)], check=True, env=env)
    for name, damaged in {
        "truncated": archive[:-128],
        "magic": b"x" + archive[1:],
        "parent": entry("../escape", 0o100644, b"no") + entry("TRAILER!!!", 0),
    }.items():
        fixture.write_bytes(damaged)
        result = subprocess.run([str(program), str(fixture)], capture_output=True, env=env)
        if result.returncode != 2 or b"Rejected archive:" not in result.stderr:
            raise SystemExit(f"Invalid archive was not rejected safely: {name}: {result.stderr!r}")
    print("Malformed archive rejection checks passed")
