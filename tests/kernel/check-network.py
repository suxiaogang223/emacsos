#!/usr/bin/env python3
"""Build the actual freestanding network backend and exercise native QEMU TCP."""
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import threading

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/network-probe"
SOURCES = ["kernel/network.c", "kernel/ne2000.c", "kernel/hardware.c", "kernel/memory.c",
           "tests/kernel/network-probe.c"]
SOURCES += [f"third_party/lwip/src/core/{name}.c" for name in
            "init def inet_chksum ip mem memp netif pbuf stats sys tcp tcp_in tcp_out timeouts".split()]
SOURCES += [f"third_party/lwip/src/core/ipv4/{name}.c" for name in "etharp icmp ip4 ip4_addr".split()]
SOURCES += ["third_party/lwip/src/netif/ethernet.c"]


def build(mode, port):
    """Use the shared kernel toolchain, keeping every artifact in this checkout."""
    toolchain = (ROOT / "build/toolchains/kernel.id").read_text().strip()
    command = ["docker", "run", "--rm", "--platform", "linux/amd64", "--user",
               f"{os.getuid()}:{os.getgid()}", "-e", "HOME=/tmp", "-v", f"{ROOT}:/work:z",
               "-w", "/work", toolchain, "sh", "-eu", "-c"]
    directory = f"build/network-probe/{mode}"
    flags = ("--target=x86_64-unknown-none-elf -std=c11 -O2 -g -Wall -Wextra -Werror "
             "-Ikernel/lwip-libc -Ikernel -Ithird_party/lwip/src/include -ffreestanding "
             "-fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mgeneral-regs-only")
    define = "-DNETWORK_PROBE_ABSENT" if mode == "absent" else f"-DNETWORK_PROBE_PORT={port}"
    lines = [f"mkdir -p {directory}"]
    for source in SOURCES:
        lines.append(f"clang {flags} {define} -c {source} -o {directory}/{Path(source).stem}.o")
    for source in ("entry", "interrupts"):
        lines.append(f"nasm -f elf64 kernel/{source}.asm -o {directory}/{source}.o")
    lines += [f"ld.lld -T kernel/kernel.ld -o {directory}/kernel.elf {directory}/*.o",
              f"llvm-objcopy -O binary {directory}/kernel.elf {directory}/kernel.bin",
              f"size=$(wc -c < {directory}/kernel.bin)", "sectors=$(((size+511)/512))",
              f"nasm -f bin -DKERNEL_SECTORS=$sectors kernel/boot.asm -o {directory}/boot.bin",
              f"cat {directory}/boot.bin {directory}/kernel.bin > {directory}/system.img",
              f"truncate -s $(((sectors+1)*512)) {directory}/system.img"]
    subprocess.run(command + ["\n".join(lines)], cwd=ROOT, check=True)
    return OUT / mode / "system.img", toolchain


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for attempt in range(100):
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
        # Keep a bound, non-listening adjacent port for the refusal case.
        refused = socket.socket()
        try:
            if port == 65535:
                raise OSError("No adjacent port")
            refused.bind(("127.0.0.1", port + 1))
            break
        except OSError:
            listener.close()
            refused.close()
    else:
        raise SystemExit("Could not reserve the local fixture port pair")
    listener.listen()
    listener.settimeout(30)
    fixture = {}

    def echo():
        try:
            connection, _ = listener.accept()
            connection.settimeout(20)
            total = 0
            with connection:
                while True:
                    data = connection.recv(777)
                    if not data:
                        break
                    total += len(data)
                    connection.sendall(data)
                connection.shutdown(socket.SHUT_WR)
            fixture["echo_bytes"] = total
        except Exception as error:
            fixture["error"] = repr(error)

    results = []
    try:
        image, toolchain = build("tcp", port)
        thread = threading.Thread(target=echo, daemon=True)
        thread.start()
        for mode in ("tcp", "absent"):
            if mode == "absent":
                image, _ = build(mode, port)
            args = ["qemu-system-x86_64", "-M", "pc", "-m", "1G", "-display", "none",
                    "-monitor", "none", "-serial", "stdio", "-no-reboot", "-drive",
                    f"file={image},format=raw,if=ide,index=0,media=disk,snapshot=on",
                    "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
            if mode == "tcp":
                args += ["-netdev", "user,id=n0", "-device",
                         "ne2k_isa,netdev=n0,iobase=0x300,irq=9,mac=52:54:00:12:34:56",
                         "-object", f"filter-dump,id=packets,netdev=n0,file={OUT}/tcp.pcap"]
            else:
                args += ["-nic", "none"]
            run = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=35)
            log = run.stdout.decode(errors="replace")
            (OUT / f"{mode}.log").write_text(log)
            marker = "NETWORK-TIMEOUT-PASS" if mode == "tcp" else "NETWORK-ABSENT-PASS"
            print(log, end="")
            if run.returncode != 33 or marker not in log:
                raise AssertionError(f"{mode}: QEMU status {run.returncode}; {log}")
            results.append({"mode": mode, "status": run.returncode, "command": args,
                            "image_sha256": hashlib.sha256(image.read_bytes()).hexdigest()})
        thread.join(timeout=1)
        if fixture != {"echo_bytes": 65536}:
            raise AssertionError(fixture)
    finally:
        listener.close()
        refused.close()
    report = {"parent": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
              "lwip": subprocess.check_output(["git", "-C", "third_party/lwip", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
              "container_id": toolchain, "fixture": fixture, "results": results}
    (OUT / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Native network backend probes passed; no Emacs or browser acceptance is claimed.")


if __name__ == "__main__":
    main()
