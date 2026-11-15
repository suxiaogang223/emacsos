#!/usr/bin/env python3
"""Boot real ring-3 ELF workers; this does not prove preemptive scheduling."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/process-probe"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    image = (ROOT / "build/toolchains/kernel.id").read_text().strip()
    flags = ("--target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror "
             "-ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone "
             "-mgeneral-regs-only")
    lines = ["nasm -f elf64 tests/kernel/worker-context.asm -o build/process-probe/worker-context.o"]
    fixtures = ["section .rodata"]
    for n in range(16):
        lines += [f"clang {flags} -Wno-unused-function -DWORKER_CASE={n} -c tests/kernel/worker.c -o build/process-probe/worker-{n}.o",
                  f"ld.lld -s -T tests/kernel/worker.ld -o build/process-probe/worker-{n}.elf build/process-probe/worker-{n}.o build/process-probe/worker-context.o"]
        fixtures += [f"global worker_{n}_start, worker_{n}_end", f"worker_{n}_start:",
                     f'incbin "build/process-probe/worker-{n}.elf"', f"worker_{n}_end:"]
    fixtures += ["section .note.GNU-stack noalloc noexec nowrite progbits"]
    (OUT / "fixtures.asm").write_text("\n".join(fixtures) + "\n")
    for source in ("kernel/memory.c", "kernel/hardware.c", "kernel/process.c", "tests/kernel/process-probe.c"):
        lines.append(f"clang {flags} -Ikernel -DEMACSOS_MEMORY_TEST -c {source} -o build/process-probe/{Path(source).stem}.o")
    for name in ("entry", "interrupts", "process_entry"):
        lines.append(f"nasm -f elf64 kernel/{name}.asm -o build/process-probe/{name}.o")
    lines += ["nasm -f elf64 build/process-probe/fixtures.asm -o build/process-probe/fixtures.o",
              "ld.lld -T kernel/kernel.ld -o build/process-probe/kernel.elf build/process-probe/memory.o build/process-probe/hardware.o build/process-probe/process.o build/process-probe/process-probe.o build/process-probe/entry.o build/process-probe/interrupts.o build/process-probe/process_entry.o build/process-probe/fixtures.o",
              "llvm-objcopy -O binary build/process-probe/kernel.elf build/process-probe/kernel.bin",
              "size=$(wc -c < build/process-probe/kernel.bin)", "sectors=$(((size+511)/512))",
              'test "$sectors" -le 896',
              "nasm -f bin -DKERNEL_SECTORS=$sectors kernel/boot.asm -o build/process-probe/boot.bin",
              "cat build/process-probe/boot.bin build/process-probe/kernel.bin > build/process-probe/system.img",
              "truncate -s $(((sectors+1)*512)) build/process-probe/system.img"]
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "--user",
                    f"{os.getuid()}:{os.getgid()}", "-e", "HOME=/tmp", "-v", f"{ROOT}:/work:z",
                    "-w", "/work", image, "sh", "-eu", "-c", "\n".join(lines)], check=True)
    disk = OUT / "system.img"
    command = ["qemu-system-x86_64", "-M", "pc", "-m", "1G", "-nic", "none",
               "-display", "none", "-monitor", "none", "-serial", "stdio", "-no-reboot",
               "-drive", f"file={disk},format=raw,if=ide,index=0,snapshot=on",
               "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    try:
        run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        log, status = run.stdout, run.returncode
    except subprocess.TimeoutExpired as error:
        log, status = error.stdout or b"", "timeout"
    (OUT / "serial.log").write_bytes(log)
    print(log.decode(errors="replace"), end="", flush=True)
    passed = b"PROCESS-PROBE-PASS" in log and status == 33
    record = {"parent": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
              "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)),
              "container": image, "passed": passed, "status": status, "command": command,
              "image_sha256": hashlib.sha256(disk.read_bytes()).hexdigest(),
              "workers": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in OUT.glob("worker-*.elf")},
              "scope": "Actual native ring-3 ELF workers, checked syscall/copy/context/exit boundaries; no preemption, blocking I/O or Emacs worker API claim"}
    (OUT / "results.json").write_text(json.dumps(record, indent=2) + "\n")
    if not passed:
        raise SystemExit(f"Process probe failed with status {status}")


if __name__ == "__main__":
    main()
