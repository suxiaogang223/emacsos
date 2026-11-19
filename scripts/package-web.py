#!/usr/bin/env python3
"""Stage the public browser computer from the development server's allowlist."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("serve_web", ROOT / "scripts/serve-web.py")
server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)


def package(root, destination):
    # Build a fresh directory; never recursively delete a caller-selected path.
    destination.mkdir(parents=True, exist_ok=False)
    assets = json.loads((root / "web/assets.json").read_text())
    manifest = json.loads((root / "build/machine/manifest.json").read_text())
    sources = json.loads((root / "web/sources.json").read_text())
    emulator = json.loads((root / "web/wasm-build.json").read_text())
    if emulator["source_archive_sha256"] != sources["emulator"]["sha256"]:
        raise ValueError("Emulator source archive and build receipt disagree")
    if set(emulator["artifacts"]) != set(assets["assets"]) - {"xterm.js", "xterm.css", "xterm-pty.js"}:
        raise ValueError("Emulator build receipt does not cover every emulator asset")
    for name, digest in emulator["artifacts"].items():
        if assets["assets"][name]["sha256"] != digest:
            raise ValueError(f"Emulator asset and build receipt disagree: {name}")
    expected = {**{"/vendor/" + name: item["sha256"] for name, item in assets["assets"].items()},
                **{"/" + name: manifest["artifacts"][name] for name in
                   ("vmlinuz", "initramfs.cpio.gz", "runtime.squashfs", "data-template.ext4")}}
    for route, (source, _mime) in server.routes(root).items():
        if route.endswith("/"):
            continue
        target = destination / route.lstrip("/")
        if route in expected:
            with source.open("rb") as stream:
                actual = hashlib.file_digest(stream, "sha256").hexdigest()
            if actual != expected[route]:
                raise ValueError(f"Publication checksum mismatch: {route}")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    (destination / ".nojekyll").touch()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    package(ROOT, args.destination)


if __name__ == "__main__":
    main()
