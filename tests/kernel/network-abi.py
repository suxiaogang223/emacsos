#!/usr/bin/env python3
"""Check the actual ABI objects with fake streams, independently of guest TCP."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def entry(name, mode, data=b""):
    fields = (1, mode, 0, 0, 1, 1700000000, len(data), 0, 0, 0, 0,
              len(name.encode()) + 1, 0)
    record = b"070701" + b"".join(f"{n:08x}".encode() for n in fields)
    record += name.encode() + b"\0"
    record += bytes(-len(record) % 4) + data
    return record + bytes(-len(record) % 4)


with tempfile.TemporaryDirectory(prefix="emacsos-network-abi-") as directory:
    temporary = Path(directory)
    program = temporary / "network-abi"
    sources = ["kernel/file_abi.c", "kernel/runtime.c", "kernel/files.c",
               "kernel/littlefs_backend.c", "third_party/littlefs/lfs.c",
               "tests/kernel/network-abi-test.c"]
    subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O1", "-g",
                    "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-DLFS_CONFIG=littlefs_config.h", "-DEMACSOS_HOST_TEST",
                    "-I", str(ROOT / "kernel"),
                    *[str(ROOT / source) for source in sources],
                    "-o", str(program)], check=True)
    fixture = temporary / "runtime.cpio"
    fixture.write_bytes(entry("usr", 0o40755)
                        + entry("usr/readonly", 0o100644, b"upstream")
                        + entry("TRAILER!!!", 0))
    subprocess.run([str(program), str(fixture)], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
