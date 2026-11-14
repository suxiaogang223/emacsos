#!/usr/bin/env python3
"""Boot the real ATA/COM2 driver in QEMU; no filesystem acceptance is implied."""
import json
import os
import pathlib
import select
import socket
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "build/storage-device"
OUT.mkdir(parents=True, exist_ok=True)
# Simulated status failures verify deadlines which successful QEMU I/O cannot.
subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-DEMACSOS_BLOCK_TEST", "-Ikernel", "kernel/block.c",
                "tests/kernel/block-error-tests.c", "-o", str(OUT / "block-errors")],
               cwd=ROOT, check=True)
subprocess.run([str(OUT / "block-errors")], check=True)
IMAGE = subprocess.check_output(
    ["docker", "image", "inspect", "emacsos-kernel:alpine3.22", "--format", "{{.Id}}"],
    text=True).strip()
CFLAGS = ["--target=x86_64-unknown-none-elf", "-std=c11", "-O2", "-Wall", "-Wextra",
          "-Werror", "-ffreestanding", "-fno-stack-protector", "-fno-pic", "-fno-pie",
          "-mno-red-zone", "-mgeneral-regs-only", "-Ikernel"]
def tool(*args):
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/amd64", "--user",
                    f"{os.getuid()}:{os.getgid()}", "-v", f"{ROOT}:/work:z", "-w", "/work",
                    IMAGE, *args], check=True)
def relative(path):
    return str(path.relative_to(ROOT))
def build(mode, define):
    objects = []
    for name, source in (("probe", "tests/kernel/storage-device-probe.c"),
                         ("hardware", "kernel/hardware.c"), ("memory", "kernel/memory.c"),
                         ("block", "kernel/block.c"), ("control", "kernel/storage_control.c")):
        obj = OUT / f"{mode}-{name}.o"
        tool("clang", *CFLAGS, *define, "-c", source, "-o", relative(obj))
        objects.append(relative(obj))
    for name in ("entry", "interrupts"):
        obj = OUT / f"{name}.o"
        tool("nasm", "-f", "elf64", f"kernel/{name}.asm", "-o", relative(obj))
        objects.append(relative(obj))
    elf, binary, boot = (OUT / f"{mode}{suffix}" for suffix in (".elf", ".bin", "-boot.bin"))
    tool("ld.lld", "-T", "kernel/kernel.ld", "-o", relative(elf), *objects)
    tool("llvm-objcopy", "-O", "binary", relative(elf), relative(binary))
    body = binary.read_bytes()
    tool("nasm", "-f", "bin", f"-DKERNEL_SECTORS={(len(body) + 511) // 512}",
         "kernel/boot.asm", "-o", relative(boot))
    disk = OUT / f"{mode}.img"
    disk.write_bytes(boot.read_bytes() + body + bytes(-len(body) % 512))
    return disk
results = []
for mode, define, size, marker in (
        ("normal", [], 64 * 1024 * 1024, "DATA-DEVICE-PASS CONTROL-READY"),
        ("absent", ["-DPROBE_NO_DATA"], None, "DATA-ABSENT-PASS"),
        ("wrong-size", ["-DPROBE_WRONG_SIZE"], 32 * 1024 * 1024, "DATA-SIZE-PASS")):
    disk = build(mode, define)
    with tempfile.TemporaryDirectory(prefix="emacsos-device-") as temporary:
        temp = pathlib.Path(temporary)
        data = temp / "data.img"
        if size:
            with data.open("wb") as file:
                file.truncate(size)
        channel = temp / "control.sock"
        command = ["qemu-system-x86_64", "-M", "pc", "-m", "1G", "-nic", "none",
                   "-display", "none", "-monitor", "none", "-serial", "stdio",
                   "-serial", f"unix:{channel},server=on,wait=off", "-no-reboot",
                   "-drive", f"file={disk},format=raw,if=ide,index=0,snapshot=on"]
        if size:
            command += ["-drive", f"file={data},format=raw,if=ide,index=1"]
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        transcript = b""
        try:
            deadline = time.monotonic() + 15
            while marker.encode() not in transcript:
                if time.monotonic() > deadline or process.poll() is not None:
                    raise AssertionError(f"{mode} did not boot: {transcript!r}")
                if select.select([process.stdout], [], [], .2)[0]:
                    transcript += os.read(process.stdout.fileno(), 4096)
            if mode == "normal":
                with socket.socket(socket.AF_UNIX) as control:
                    control.settimeout(3)
                    control.connect(str(channel))
                    stream = control.makefile("rb")
                    def request(session, identifier, verb, expected, errno=0, frozen=0):
                        control.sendall(f"EOS1 {session:016x} {identifier:016x} {verb}\n".encode())
                        line = stream.readline().decode().strip()
                        assert line == (f"EOS1 {session:016x} {identifier:016x} {expected} "
                                        f"0000000000000007 {errno} 1 {frozen}"), line
                        return line
                    request(1, 1, "STATUS", "STATUS")
                    request(1, 2, "FREEZE", "FROZEN", frozen=1)
                    request(1, 3, "STATUS", "STATUS", frozen=1)
                    request(2, 4, "THAW", "ERROR", errno=116, frozen=1)
                    request(1, 2, "THAW", "ERROR", errno=116, frozen=1)
                    request(1, 4, "THAW", "THAWED")
                    control.sendall(b"x" * 256 + b"\n")
                    request(1, 5, "STATUS", "STATUS")
                    control.sendall(b"EOS1 ")
                    time.sleep(1.1)
                    control.sendall(b"bad-tail\n")
                    request(1, 6, "STATUS", "STATUS")
            results.append({"mode": mode, "boot": marker, "control": mode == "normal"})
        finally:
            process.terminate()
            process.communicate(timeout=5)
            (OUT / f"{mode}.log").write_bytes(transcript)
        if mode == "normal":
            expected = bytes((i * 17 + 29) & 255 for i in range(1024))
            with data.open("rb") as file:
                file.seek(7 * 512)
                assert file.read(1024) == expected, "host disk bytes differ"
                file.seek(size - 512)
                assert file.read(512) == expected[:512], "host final-sector bytes differ"
(OUT / "results.json").write_text(json.dumps({"toolchain": IMAGE, "results": results}, indent=2) + "\n")
print("ATA write/read/flush/bounds and COM2 freeze/thaw/stale/partial-frame probes passed.")
