#!/usr/bin/env python3
"""Exercise static-musl startup and actual page protection in QEMU."""
import hashlib
import json
import os
import pathlib
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
out = root / "build/kernel"
toolchain = (root / "build/toolchains/kernel.id").read_text().strip()
sys.path.insert(0, str(root / "scripts"))
import image as disk_image
import qemu
results = []
for mode, define, marker in (
    ("normal", [], "MUSL-GUEST-PROBE-PASS"),
    ("readonly", ["-DPROBE_WRITE_PROTECTED"], "FAULT vector=0x000000000000000e error=0x0000000000000003"),
    ("noexecute", ["-DPROBE_EXECUTE_PROTECTED"], "FAULT vector=0x000000000000000e error=0x0000000000000011"),
):
    elf = out / f"probe-{mode}"
    disk = out / f"probe-{mode}.img"
    archive = out / "empty.cpio"
    archive.write_bytes(b"")
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64",
                    "--user", f"{os.getuid()}:{os.getgid()}",
                    "-v", f"{root}:/work:z", "-w", "/work", toolchain,
                    "gcc", "-static", "-no-pie", "-O2", *define, "tests/kernel/runtime-probe.c", "-o", str(elf.relative_to(root))], check=True)
    disk_image.pack_image(out / "emacsos.img", elf, archive, disk)
    command = qemu.command(disk, debug_exit=True)
    try:
        run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
        output, status = run.stdout.decode(errors="replace"), run.returncode
    except subprocess.TimeoutExpired as error:
        output, status = (error.stdout or b"").decode(errors="replace"), "timeout"
    (out / f"probe-{mode}.log").write_text(output)
    print(output, end="")
    if marker not in output or (mode == "normal" and status != 33):
        raise SystemExit(f"{mode} failed: status {status}")
    results.append({"mode": mode, "status": status, "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(), "disk_sha256": hashlib.sha256(disk.read_bytes()).hexdigest()})
manifest = {"parent": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(), "fork": subprocess.check_output(["git", "rev-parse", "HEAD:emacs"], cwd=root, text=True).strip(), "container_id": toolchain, "kernel_sha256": hashlib.sha256((out / "kernel.elf").read_bytes()).hexdigest(), "results": results}
(out / "probe-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print("All three kernel probes passed; no Emacs acceptance is claimed.")
