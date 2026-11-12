#!/usr/bin/env python3
"""Record the source and components of the complete fixed-machine boot image."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args], text=True).strip()


def main():
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
    for path in (ROOT / "lisp").glob("*.el"):
        installed = runtime / "root/usr/share/emacs/site-lisp" / path.name
        if not installed.is_file() or digest(path) != digest(installed):
            raise SystemExit(f"Stale installed system Lisp: {path.name}")
    components = ["build/skeleton/kernel.elf", "build/skeleton/emacsos.img",
                  "build/runtime/emacs", "build/runtime/runtime.cpio",
                  "build/runtime/manifest.json", "build/mvp/emacsos.img"]
    record = {
        "format": 1, "scope": "Build identity only; guest acceptance is separate",
        "parent": git("rev-parse", "HEAD"), "tree": git("rev-parse", "HEAD^{tree}"),
        "fork": git("rev-parse", "HEAD:emacs"),
        "dirty": bool(git("status", "--porcelain")),
        "machine": {"qemu": "pc", "ram": "1G", "console": "COM1", "network": "none"},
        "components": {name: digest(ROOT / name) for name in components},
        "containers": {
            image: subprocess.check_output(["docker", "image", "inspect", image,
                                             "--format", "{{.Id}}"], text=True).strip()
            for image in ("emacsos-runtime:alpine3.22", "emacsos-skeleton:alpine3.22")
        },
    }
    (out / "manifest.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"MVP image built from {record['parent']}; dirty={record['dirty']}")


if __name__ == "__main__":
    main()
