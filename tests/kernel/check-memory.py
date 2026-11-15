#!/usr/bin/env python3
"""Exercise real CR3 mappings, quotas, rollback and reclamation in native QEMU."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/memory-probe"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    image = (ROOT / "build/toolchains/kernel.id").read_text().strip()
    results = []
    for mode, define, marker in (
        ("normal", "", "MEMORY-PROBE-PASS"),
        ("readonly", "-DMEMORY_PROBE_READONLY", "error=0x0000000000000003"),
        ("nx", "-DMEMORY_PROBE_NX", "error=0x0000000000000011"),
        ("direct-nx", "-DMEMORY_PROBE_DIRECT_NX", "error=0x0000000000000011"),
    ):
        directory = f"build/memory-probe/{mode}"
        flags = ("--target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror "
                 "-Wno-unused-function -Ikernel -DEMACSOS_MEMORY_TEST -ffreestanding "
                 "-fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only")
        lines = [f"mkdir -p {directory}"]
        for source in ("kernel/memory.c", "kernel/hardware.c", "tests/kernel/memory-probe.c"):
            lines.append(f"clang {flags} {define} -c {source} -o {directory}/{Path(source).stem}.o")
        for source in ("entry", "interrupts"):
            lines.append(f"nasm -f elf64 kernel/{source}.asm -o {directory}/{source}.o")
        lines += [f"ld.lld -T kernel/kernel.ld -o {directory}/kernel.elf {directory}/*.o",
                  f"llvm-objcopy -O binary {directory}/kernel.elf {directory}/kernel.bin",
                  f"size=$(wc -c < {directory}/kernel.bin)", "sectors=$(((size+511)/512))",
                  f"nasm -f bin -DKERNEL_SECTORS=$sectors kernel/boot.asm -o {directory}/boot.bin",
                  f"cat {directory}/boot.bin {directory}/kernel.bin > {directory}/system.img",
                  f"truncate -s $(((sectors+1)*512)) {directory}/system.img"]
        subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "--user",
                        f"{os.getuid()}:{os.getgid()}", "-e", "HOME=/tmp", "-v", f"{ROOT}:/work:z",
                        "-w", "/work", image, "sh", "-eu", "-c", "\n".join(lines)], check=True)
        disk = ROOT / directory / "system.img"
        command = ["qemu-system-x86_64", "-M", "pc", "-m", "1G", "-nic", "none",
                   "-display", "none", "-monitor", "none", "-serial", "stdio", "-no-reboot",
                   "-drive", f"file={disk},format=raw,if=ide,index=0,snapshot=on",
                   "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
        try:
            run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 timeout=60 if mode == "normal" else 5)
            log, status = run.stdout, run.returncode
        except subprocess.TimeoutExpired as error:
            log, status = error.stdout or b"", "timeout"
        (OUT / f"{mode}.log").write_bytes(log)
        print(log.decode(errors="replace"), end="", flush=True)
        if marker.encode() not in log or (mode == "normal" and status != 33):
            raise SystemExit(f"Memory probe {mode} failed with status {status}")
        results.append({"mode": mode, "status": status, "command": command,
                        "image_sha256": hashlib.sha256(disk.read_bytes()).hexdigest()})
    record = {"parent": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
              "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)),
              "container": image, "passed": True, "results": results,
              "scope": "Real native kernel memory/MMU probes; no protected process or Emacs GC claim"}
    (OUT / "results.json").write_text(json.dumps(record, indent=2) + "\n")


if __name__ == "__main__":
    main()
