#!/usr/bin/env python3
"""Assemble, verify and identify the complete fixed-machine boot image."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import struct

ROOT = Path(__file__).resolve().parent.parent


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def pack_image(boot_path, elf_path, archive_path, output):
    boot = boot_path.read_bytes()
    sizes = (elf_path.stat().st_size, archive_path.stat().st_size)
    if len(boot) > 1024 * 1024:
        raise ValueError("Kernel image overlaps the runtime header")
    if not sizes[0] or any(size > 256 * 1024 * 1024 for size in sizes):
        raise ValueError("Runtime payload exceeds fixed RAM contract")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".part")
    try:
        with temporary.open("wb") as stream:
            stream.write(boot + bytes(1024 * 1024 - len(boot)))
            stream.write(struct.pack("<8sQQ", b"EMACSOS1", *sizes) + bytes(512 - 24))
            for path, size in zip((elf_path, archive_path), sizes):
                with path.open("rb") as component:
                    for chunk in iter(lambda: component.read(1024 * 1024), b""):
                        stream.write(chunk)
                stream.write(bytes(-size % 512))
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)


def verify_image(image_path, boot_path, elf_path, archive_path):
    """Bind the booted disk bytes to the individually identified components."""
    boot = boot_path.read_bytes()
    if len(boot) > 1024 * 1024:
        raise SystemExit("Kernel image overlaps the runtime header")
    sizes = (elf_path.stat().st_size, archive_path.stat().st_size)
    with image_path.open("rb") as image:
        if image.read(1024 * 1024) != boot + bytes(1024 * 1024 - len(boot)):
            raise SystemExit("Boot image does not contain the identified kernel")
        expected = struct.pack("<8sQQ", b"EMACSOS1", *sizes) + bytes(512 - 24)
        if image.read(512) != expected:
            raise SystemExit("Boot image runtime header differs")
        for path, size in zip((elf_path, archive_path), sizes):
            with path.open("rb") as component:
                for chunk in iter(lambda: component.read(1024 * 1024), b""):
                    if image.read(len(chunk)) != chunk:
                        raise SystemExit(f"Boot image payload differs: {path.name}")
            if image.read(-size % 512) != bytes(-size % 512):
                raise SystemExit("Boot image sector padding differs")
        if image.read(1):
            raise SystemExit("Boot image has unexpected trailing bytes")


def build():
    out = ROOT / "build/mvp"
    runtime = ROOT / "build/runtime"
    manifest = json.loads((runtime / "manifest.json").read_text())
    if manifest["emacs_commit"] != git("rev-parse", "HEAD:emacs"):
        raise SystemExit("Runtime source pin does not match the parent")
    for name, record in (("emacs", manifest["elf"]), ("runtime.cpio", manifest["archive"])):
        if digest(runtime / name) != record["sha256"]:
            raise SystemExit(f"Stale runtime component: {name}")
    if manifest["parent_commit"] != git("rev-parse", "HEAD"):
        raise SystemExit("Repackage the runtime at the current parent commit")
    entries = {}
    for entry in manifest["files"]:
        if entry["path"] in entries:
            raise SystemExit(f"Duplicate runtime manifest path: {entry['path']}")
        entries[entry["path"]] = entry
    sources = list((ROOT / "lisp").glob("*.el"))
    for path in sources:
        name = "/usr/share/emacs/site-lisp/" + path.name
        installed = runtime / ("root" + name)
        entry = entries.get(name, {})
        expected = digest(path)
        if (not installed.is_file() or installed.is_symlink()
                or digest(installed) != expected
                or entry.get("sha256") != expected
                or entry.get("size") != path.stat().st_size
                or "symlink" in entry or "directory" in entry):
            raise SystemExit(f"Stale installed/archived system Lisp: {path.name}")
    image_path = out / "emacsos.img"
    inputs = (ROOT / "build/skeleton/emacsos.img", runtime / "emacs", runtime / "runtime.cpio")
    try:
        verify_image(image_path, *inputs)
    except (OSError, SystemExit):
        pack_image(*inputs, image_path)
        verify_image(image_path, *inputs)
        print("Assembled MVP image")
    if git("-C", "emacs", "rev-parse", "HEAD") != manifest["emacs_commit"]:
        raise SystemExit("Initialize the matching Emacs submodule")
    components = ["build/skeleton/kernel.elf", "build/skeleton/emacsos.img",
                  "build/runtime/emacs", "build/runtime/runtime.cpio",
                  "build/runtime/manifest.json", "build/mvp/emacsos.img"]
    record = {
        "format": 1, "scope": "Build identity only; guest acceptance is separate",
        "parent": git("rev-parse", "HEAD"), "tree": git("rev-parse", "HEAD^{tree}"),
        "fork": git("rev-parse", "HEAD:emacs"),
        "dirty": bool(git("status", "--porcelain")) or manifest["parent_dirty"],
        "machine": {"qemu": "pc", "ram": "1G", "console": "COM1", "network": "none"},
        "components": {name: digest(ROOT / name) for name in components},
        "containers": {
            "emacsos-runtime:alpine3.22": manifest["container_id"],
            "emacsos-skeleton:alpine3.22": subprocess.check_output([
                "docker", "image", "inspect", "emacsos-skeleton:alpine3.22",
                "--format", "{{.Id}}"], text=True).strip(),
        },
    }
    manifest_path = out / "manifest.json"
    encoded = json.dumps(record, indent=2) + "\n"
    if not manifest_path.exists() or manifest_path.read_text() != encoded:
        manifest_path.write_text(encoded)
    print(f"MVP image built from {record['parent']}; dirty={record['dirty']}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    commands.add_parser("build", help="Build or reuse the complete image and record its identity")
    pack = commands.add_parser("pack", help="Assemble a standalone probe image")
    for name in ("boot", "elf", "archive", "output"):
        pack.add_argument(name, type=Path)
    args = parser.parse_args()
    if args.action == "build":
        build()
    else:
        pack_image(args.boot, args.elf, args.archive, args.output)
