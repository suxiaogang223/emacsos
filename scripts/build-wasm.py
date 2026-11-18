#!/usr/bin/env python3
"""Rebuild QEMU/Wasm and firmware from the release's verified source bundle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ("out.js", "qemu-system-x86_64.wasm", "qemu-system-x86_64.worker.js", "load-rom.js", "load-rom.data")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "build/wasm")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = json.loads((ROOT / "web/sources.json").read_text())["emulator"]
    archive = args.archive or output / "emulator-sources.tar.gz"
    if not args.archive:
        subprocess.run(["curl", "--fail", "--location", "--retry", "3", source["url"], "-o", str(archive)], check=True)
    if digest(archive) != source["sha256"]:
        raise ValueError("Emulator source archive checksum mismatch")
    with tarfile.open(archive) as bundle:
        bundle.extractall(output / "source", filter="data")
    source_root = output / "source/emulator"
    for name in ("Toolchain.Dockerfile", "build.sh", "no-install-blobs.patch"):
        if digest(source_root / name) != digest(ROOT / "system/wasm" / name):
            raise ValueError(f"Source bundle and checkout disagree: {name}")
    image = "emacsos-wasm-builder:3.1.50"
    subprocess.run(["docker", "build", "--platform", "linux/amd64", "-t", image,
                    "-f", str(source_root / "Toolchain.Dockerfile"), str(source_root)], check=True)
    artifacts = output / "artifacts"
    artifacts.mkdir()
    subprocess.run(["docker", "run", "--rm", "--network", "none", "--platform", "linux/amd64",
                    "-e", f"OUTPUT_UID={os.getuid()}", "-e", f"OUTPUT_GID={os.getgid()}",
                    "-v", f"{source_root}:/source:ro,z", "-v", f"{artifacts}:/out:z",
                    "-v", f"{source_root / 'build.sh'}:/build-emulator.sh:ro,z", image, "bash", "/build-emulator.sh"], check=True)
    receipt = {"source_archive_sha256": source["sha256"],
               "toolchain_image": subprocess.check_output(["docker", "image", "inspect", image, "--format", "{{.Id}}"], text=True).strip(),
               "sources": json.loads((source_root / "sources.json").read_text()),
               "recipe": {name: digest(source_root / name) for name in ("Toolchain.Dockerfile", "build.sh", "no-install-blobs.patch")},
               "tool_versions": (artifacts / "tool-versions.txt").read_text(),
               "artifacts": {name: digest(artifacts / name) for name in ASSETS},
               "firmware": {path.name: digest(path) for path in sorted((artifacts / "firmware").glob("*.bin"))}}
    (artifacts / "wasm-build.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"Built emulator and firmware: {artifacts}")


if __name__ == "__main__":
    main()
