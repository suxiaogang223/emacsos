#!/usr/bin/env python3
"""Compile and exercise the real file backend on a host, including bad archives."""
from pathlib import Path
import os
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import image

image.verify_littlefs()


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
                    "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-fsanitize=address,undefined",
                    "-DLFS_CONFIG=littlefs_config.h", "-DEMACSOS_HOST_TEST",
                    "-I", str(ROOT / "kernel"), str(ROOT / "kernel/files.c"),
                    str(ROOT / "kernel/file_abi.c"), str(ROOT / "kernel/littlefs_backend.c"),
                    str(ROOT / "third_party/littlefs/lfs.c"),
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

    persistent = temporary / "persistent-test"
    subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O1", "-g",
                    "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-fsanitize=address,undefined", "-DLFS_CONFIG=littlefs_config.h",
                    "-DEMACSOS_HOST_TEST", "-I", str(ROOT / "kernel"),
                    str(ROOT / "kernel/files.c"), str(ROOT / "kernel/file_abi.c"),
                    str(ROOT / "kernel/littlefs_backend.c"),
                    str(ROOT / "third_party/littlefs/lfs.c"),
                    str(ROOT / "tests/kernel/persistent-test.c"), "-o", str(persistent)], check=True)
    baseline = temporary / "baseline.img"
    trial = temporary / "trial.img"

    def run(mode, image=trial, expected=0, **fault):
        result = subprocess.run([str(persistent), str(image), mode], capture_output=True,
                                text=True, env={**env, **fault})
        if result.returncode != expected:
            raise SystemExit(f"Persistent test {mode}, {fault}: {result.returncode}: {result.stderr}")
        return result.stdout

    run("format", baseline)
    print(run("seed", baseline), end="")
    print(run("verify", baseline), end="")
    cuts = flush_failures = 0
    for operation, recovery in [("replace", "recovered"),
                                ("create-regular", "recover-regular"),
                                ("create-directory", "recover-directory"),
                                ("create-symlink", "recover-symlink")]:
        shutil.copyfile(baseline, trial)
        output = run(operation)
        count = int(re.search(r"WRITES (\d+)", output)[1])
        for torn in (False, True):
            for cut in range(1, count + 1):
                shutil.copyfile(baseline, trial)
                run(operation, expected=77, CUT_AFTER=str(cut),
                    **({"TEAR_CUT": "1"} if torn else {}))
                run(recovery)
                cuts += 1
        if operation.startswith("create-"):
            count = int(re.search(r"FLUSHES (\d+)", output)[1])
            for failure in range(1, count + 1):
                shutil.copyfile(baseline, trial)
                run(operation, FAIL_FLUSH=str(failure))
                run(recovery)
                flush_failures += 1
    print(f"Persistent recovery passed {cuts} sector/torn cuts and {flush_failures} sync failures")
    shutil.copyfile(baseline, trial)
    print(run("io-failure"), end="")
    shutil.copyfile(baseline, trial)
    print(run("stream"), end="")
    shutil.copyfile(baseline, trial)
    print(run("capacity"), end="")
    with trial.open("wb") as damaged:
        damaged.truncate(64 * 1024 * 1024)
    before = hashlib.sha256(trial.read_bytes()).digest()
    print(run("reject"), end="")
    assert hashlib.sha256(trial.read_bytes()).digest() == before
