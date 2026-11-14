#!/usr/bin/env python3
"""Fetch hash-pinned browser assets atomically; cached bytes are verified too."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def valid(path, expected):
    if not path.is_file():
        return False
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest() == expected


def fetch_assets(manifest, destination):
    destination.mkdir(parents=True, exist_ok=True)
    for name, record in manifest["assets"].items():
        if Path(name).name != name or name in (".", ".."):
            raise ValueError("Asset names must be plain filenames")
        if not record["url"].startswith("https://") or not re.fullmatch("[a-f0-9]{64}", record["sha256"]):
            raise ValueError(f"Invalid pinned asset: {name}")
        target = destination / name
        if not valid(target, record["sha256"]):
            with tempfile.NamedTemporaryFile(prefix=name + ".", suffix=".part", dir=destination, delete=False) as stream:
                temporary = Path(stream.name)
            try:
                subprocess.run(["curl", "-fsSL", "--retry", "2", "--connect-timeout", "15", "--max-time", "300", record["url"], "-o", str(temporary)], check=True)
                if not valid(temporary, record["sha256"]):
                    raise ValueError(f"Unexpected checksum for {name}")
                temporary.chmod(0o644)
                temporary.replace(target)
            finally:
                temporary.unlink(missing_ok=True)
        print(f"Verified {name}")


def main():
    manifest = json.loads((ROOT / "web/assets.json").read_text())
    fetch_assets(manifest, ROOT / "build/web-vendor")


if __name__ == "__main__":
    main()
