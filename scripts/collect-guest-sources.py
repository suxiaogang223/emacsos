#!/usr/bin/env python3
"""Collect verified Alpine source packages from the actual guest's APK database."""
import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def installed_database(image):
    with gzip.open(image, "rb") as stream:
        while True:
            header = stream.read(110)
            if len(header) != 110 or header[:6] not in (b"070701", b"070702"):
                raise ValueError("Invalid newc initramfs")
            size = int(header[54:62], 16)
            name_size = int(header[94:102], 16)
            name = stream.read(name_size).rstrip(b"\0").decode()
            stream.read(-(110 + name_size) % 4)
            data = stream.read(size)
            stream.read(-size % 4)
            if name.lstrip("./") == "lib/apk/db/installed":
                return data.decode()
            if name == "TRAILER!!!":
                raise ValueError("Guest package database is missing")


def inventory(database):
    result = []
    for block in database.strip().split("\n\n"):
        fields = dict(line.split(":", 1) for line in block.splitlines() if ":" in line)
        if "P" not in fields:
            continue
        package = {key: fields.get(field, "") for key, field in
                   (("package", "P"), ("version", "V"), ("origin", "o"), ("commit", "c"), ("license", "L"))}
        if package["origin"] in (".", "..") or not re.fullmatch(r"[a-zA-Z0-9+_.-]+", package["origin"]) or not re.fullmatch(r"[0-9a-f]{40}", package["commit"]):
            raise ValueError(f"Missing source identity: {package['package']}")
        result.append(package)
    if not result:
        raise ValueError("Empty guest package inventory")
    return result


def api(path):
    # Maintainer tooling uses authenticated GitHub reads to avoid the anonymous
    # API's 60-request limit when collecting the complete patch closure.
    return json.loads(subprocess.check_output(["gh", "api", path], timeout=120))


def blob_digest(data):
    return hashlib.sha1(f"blob {len(data)}\0".encode() + data).hexdigest()


def collect_recipe(item, destination):
    origin, commit = item
    prefix = "community" if origin == "cpio" else "main"
    target = destination / "aports" / origin
    target.mkdir(parents=True, exist_ok=True)
    entries = api(f"repos/alpinelinux/aports/contents/{prefix}/{origin}?ref={commit}")
    for entry in entries:
        if entry["type"] != "file" or Path(entry["name"]).name != entry["name"]:
            raise ValueError(f"Unsupported recipe entry: {entry['path']}")
        path = target / entry["name"]
        if path.is_file() and blob_digest(path.read_bytes()) == entry["sha"]:
            continue
        data = base64.b64decode(api(f"repos/alpinelinux/aports/git/blobs/{entry['sha']}")["content"])
        if blob_digest(data) != entry["sha"]:
            raise ValueError(f"Recipe blob mismatch: {entry['path']}")
        path.write_bytes(data)
    (target / ".recipe-complete").touch()
    print(f"Recipe: {prefix}/{origin} at {commit}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=ROOT / "build/machine/initramfs.cpio.gz")
    parser.add_argument("--output", type=Path, default=ROOT / "build/publication/guest-sources")
    args = parser.parse_args()
    destination = args.output.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    database = installed_database(args.image)
    packages = inventory(database)
    origins = {p["origin"]: p["commit"] for p in packages}
    if any(p["commit"] != origins[p["origin"]] for p in packages):
        raise ValueError("Conflicting source commits for one origin")
    (destination / "installed.txt").write_text(database)
    (destination / "packages.json").write_text(json.dumps(packages, indent=2) + "\n")
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda item: collect_recipe(item, destination), origins.items()))
    subprocess.run(["docker", "build", "-t", "emacsos-source-tools:v1", "-f",
                    str(ROOT / "system/wasm/source-tools.Dockerfile"), str(ROOT / "system/wasm")], check=True)
    subprocess.run(["docker", "run", "--rm", "-e", f"OUTPUT_UID={os.getuid()}", "-e", f"OUTPUT_GID={os.getgid()}",
                    "-v", f"{destination}:/out:z", "-v", f"{ROOT / 'system/wasm/collect-guest.sh'}:/collect.sh:ro,z",
                    "emacsos-source-tools:v1", "sh", "/collect.sh"], check=True)
    verified = {p.parent.name for p in (destination / "aports").glob("*/.verified")}
    if verified != set(origins):
        raise ValueError("Source package verification is incomplete")
    print(f"Verified sources for {len(packages)} installed packages / {len(origins)} origins")


if __name__ == "__main__":
    main()
