#!/usr/bin/env python3
"""Build and run the Linux system shared by native QEMU and the browser."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/machine"
SYSTEM = ROOT / "system"
IMAGE = "emacsos-linux:alpine3.22"


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def inputs():
    paths = sorted(path for path in SYSTEM.iterdir() if path.is_file())
    paths.append(ROOT / "scripts/machine.py")
    return {str(path.relative_to(ROOT)): digest(path) for path in paths}


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n")


def create_disk(path, certificate=None):
    """Create a new home disk; never format an existing user disk."""
    with path.open("xb") as disk:
        disk.truncate(64 * 1024 * 1024)
    with tempfile.TemporaryDirectory(prefix=".seed-", dir=OUT) as temporary:
        seed = Path(temporary)
        (seed / "emacs/work").mkdir(parents=True)
        if certificate:
            shutil.copyfile(certificate, seed / "emacs/work/fixture-ca.pem")
        run("mkfs.ext4", "-q", "-F", "-m", "0", "-L", "EMACSOS_DATA", "-d", seed, path)
    for directory in ("/emacs", "/emacs/work"):
        for field in ("uid", "gid"):
            run("debugfs", "-w", "-R", f"set_inode_field {directory} {field} 1000", path,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    selected_inputs = inputs()
    pin = git("ls-files", "-s", "emacs").split()[1]
    fork = ROOT / "emacs"
    actual = subprocess.check_output(["git", "-C", str(fork), "rev-parse", "HEAD"], text=True).strip()
    if pin != actual or subprocess.check_output(["git", "-C", str(fork), "status", "--porcelain"]):
        raise RuntimeError("Initialize a clean Emacs checkout matching the parent gitlink")
    context = OUT / "context"
    context.mkdir(exist_ok=True)
    system_files = [path for path in SYSTEM.iterdir() if path.is_file()]
    retained = {path.name for path in system_files} | {"source.tar", "source-pin"}
    for path in context.iterdir():
        if path.name not in retained and path.is_file():
            path.unlink()
    for path in system_files:
        shutil.copyfile(path, context / path.name)
    source = context / "source.tar"
    if not source.exists() or not (context / "source-pin").exists() or (context / "source-pin").read_text() != pin:
        with source.open("wb") as stream:
            run("git", "-C", fork, "archive", "--format=tar", pin, stdout=stream)
        (context / "source-pin").write_text(pin)
    epoch = subprocess.check_output(["git", "-C", str(fork), "show", "-s", "--format=%ct", pin], text=True).strip()
    run("docker", "build", "--platform", "linux/amd64", "--build-arg", f"SOURCE_DATE_EPOCH={epoch}", "-t", IMAGE, context)
    container = subprocess.check_output(["docker", "image", "inspect", IMAGE, "--format", "{{.Id}}"], text=True).strip()
    key = {"inputs": selected_inputs, "fork_pin": pin, "container": container}
    receipt = OUT / "package-key.json"
    artifacts = ("vmlinuz", "initramfs.cpio.gz", "runtime.squashfs", "packages.txt", "emacs.txt")
    cached = json.loads(receipt.read_text()) if receipt.exists() else {}
    if cached.get("key") != key or any(not (OUT / name).exists() or digest(OUT / name) != cached.get("artifacts", {}).get(name) for name in artifacts):
        package = r'''
set -eu
set -o pipefail
cp /boot/vmlinuz-virt /out/vmlinuz
cp /boot/usr.squashfs /out/runtime.squashfs
find bin etc home init lib media mnt opt root run sbin srv tmp var -xdev -print0 > /out/archive.list
find usr/lib -xdev -print0 >> /out/archive.list
printf 'dev\0proc\0sys\0usr\0' >> /out/archive.list
sort -z /out/archive.list | cpio --null -o -H newc --owner=0:0 2>/out/cpio.log | gzip -n > /out/initramfs.cpio.gz
apk list --installed > /out/packages.txt
emacs --batch -Q --eval '(princ (format "%s\n%s\n" emacs-version system-configuration-options))' > /out/emacs.txt
chown "$OUTPUT_UID:$OUTPUT_GID" /out/vmlinuz /out/initramfs.cpio.gz /out/runtime.squashfs /out/packages.txt /out/emacs.txt /out/cpio.log /out/archive.list
'''
        run("docker", "run", "--rm", "--platform", "linux/amd64",
            "-e", f"OUTPUT_UID={os.getuid()}", "-e", f"OUTPUT_GID={os.getgid()}",
            "-v", f"{OUT}:/out:z", IMAGE, "/bin/sh", "-c", package)
        cached = {"key": key, "artifacts": {name: digest(OUT / name) for name in artifacts}}
        write_json(receipt, cached)
    for name in ("data-template.ext4", "data.ext4"):
        if not (OUT / name).exists():
            create_disk(OUT / name)
    if inputs() != selected_inputs:
        raise RuntimeError("Build inputs changed during compilation; rebuild before recording identity")
    write_json(OUT / "manifest.json", {
        "parent": git("rev-parse", "HEAD"), "dirty": bool(git("status", "--porcelain")),
        "fork_pin": pin, "runtime": "GNU Emacs 30.2, pinned fork; Alpine Linux 6.12.117",
        "container": container, "inputs": selected_inputs,
        "artifacts": {**cached["artifacts"], "data-template.ext4": digest(OUT / "data-template.ext4")},
        "machine": "QEMU pc, x86-64 TCG, 1 GiB, COM1, virtio data disk",
        "native_network": "QEMU user network", "browser_network": "disabled",
    })
    print(f"System ready: {OUT}", flush=True)


def verify():
    manifest = json.loads((OUT / "manifest.json").read_text())
    if manifest["inputs"] != inputs() or manifest["fork_pin"] != git("ls-files", "-s", "emacs").split()[1]:
        raise RuntimeError("System sources changed; run make build")
    for name, expected in manifest["artifacts"].items():
        if digest(OUT / name) != expected:
            raise RuntimeError(f"System artifact changed: {name}; run make build")
    return manifest


def command(disk):
    return ["qemu-system-x86_64", "-machine", "pc", "-accel", "tcg", "-m", "1024",
            "-kernel", str(OUT / "vmlinuz"), "-initrd", str(OUT / "initramfs.cpio.gz"),
            "-append", "console=ttyS0,115200 rdinit=/init panic=-1 quiet",
            "-drive", f"file={disk},format=raw,if=virtio",
            "-drive", f"file={OUT / 'runtime.squashfs'},format=raw,if=virtio,readonly=on",
            "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0",
            "-display", "none", "-serial", "stdio", "-monitor", "none", "-no-reboot"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("build", "run"))
    args = parser.parse_args()
    if args.action == "build":
        build()
    else:
        verify()
        run(*command(OUT / "data.ext4"))


if __name__ == "__main__":
    main()
