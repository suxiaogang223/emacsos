#!/usr/bin/env python3
"""Fetch the released guest image without touching the native retained disk."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def fetch():
    release = json.loads((ROOT / "web/runtime.json").read_text())
    archive = ROOT / "build/publication/guest-runtime.tar.gz"
    archive.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["curl", "--fail", "--location", "--retry", "3", release["url"], "-o", str(archive)], check=True)
    with archive.open("rb") as stream:
        if hashlib.file_digest(stream, "sha256").hexdigest() != release["sha256"]:
            raise ValueError("Public runtime archive checksum mismatch")
    allowed = {"vmlinuz", "initramfs.cpio.gz", "data-template.ext4", "manifest.json"}
    payloads = {}
    with tarfile.open(archive) as bundle:
        for member in bundle:
            if member.name not in allowed or not member.isfile() or member.name in payloads:
                raise ValueError(f"Unexpected public runtime entry: {member.name}")
            payloads[member.name] = bundle.extractfile(member).read()
    if payloads.keys() != allowed:
        raise ValueError("Public runtime archive is incomplete")
    manifest = json.loads(payloads["manifest.json"])
    if manifest["dirty"]:
        raise ValueError("Public runtime must identify a clean source checkout")
    for name in allowed - {"manifest.json"}:
        if hashlib.sha256(payloads[name]).hexdigest() != manifest["artifacts"][name]:
            raise ValueError(f"Guest checksum mismatch: {name}")
    spec = importlib.util.spec_from_file_location("machine", ROOT / "scripts/machine.py")
    machine = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(machine)
    if manifest["inputs"] != machine.inputs() or manifest["fork_pin"] != machine.git("ls-files", "-s", "emacs").split()[1]:
        raise ValueError("Published guest does not match system sources; prepare a new runtime release")
    machine.OUT.mkdir(parents=True, exist_ok=True)
    for name, data in payloads.items():
        (machine.OUT / name).write_bytes(data)
    print("Verified released guest; native data.ext4 was not accessed")


if __name__ == "__main__":
    fetch()
