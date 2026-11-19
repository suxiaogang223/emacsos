#!/usr/bin/env python3
"""Collect pinned upstream source and package the offline emulator build inputs."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def add_bytes(bundle, name, data, epoch):
    member = tarfile.TarInfo("emulator/" + name)
    member.size = len(data)
    member.mtime = epoch
    member.mode = 0o644
    bundle.addfile(member, io.BytesIO(data))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, default=ROOT / "build/wasm-inputs")
    parser.add_argument("--output", type=Path, default=ROOT / "build/publication/emulator-sources.tar.gz")
    args = parser.parse_args()
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    inputs = json.loads((ROOT / "system/wasm/inputs.json").read_text())
    for repository in inputs["repositories"].values():
        path = cache / repository["checkout"]
        if not path.exists():
            subprocess.run(["git", "clone", "--filter=blob:none", "--no-checkout", repository["url"], str(path)], check=True)
            subprocess.run(["git", "-C", str(path), "fetch", "--depth", "1", "origin", repository["commit"]], check=True)
        subprocess.run(["git", "-C", str(path), "cat-file", "-e", repository["commit"] + "^{commit}"], check=True)
    for name, source in inputs["archives"].items():
        path = cache / name
        if path.exists() and digest(path) == source["sha256"]:
            continue
        partial = path.with_suffix(path.suffix + ".partial")
        subprocess.run(["curl", "--fail", "--location", "--retry", "3", source["url"], "-o", str(partial)], check=True)
        if digest(partial) != source["sha256"]:
            raise ValueError(f"Source checksum mismatch: {name}")
        partial.replace(path)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, "w:gz") as output:
        for name, repository in inputs["repositories"].items():
            prefix = "emulator/" + repository["path"] + "/"
            process = subprocess.Popen(["git", "-C", str(cache / repository["checkout"]), "archive", "--format=tar",
                                        "--prefix=" + prefix, repository["commit"]], stdout=subprocess.PIPE)
            with tarfile.open(fileobj=process.stdout, mode="r|") as source:
                for member in source:
                    # Exclude the unused precompiled firmware shipped by QEMU.
                    # This release builds its serial-only firmware separately.
                    if name == "qemu" and member.isfile() and Path(member.name).parent == Path(prefix) / "pc-bios":
                        if Path(member.name).name not in ("README", "meson.build") and not member.name.endswith(".dts"):
                            continue
                    output.addfile(member, source.extractfile(member) if member.isfile() else None)
            if process.wait() != 0:
                raise RuntimeError(f"Source archive failed: {name}")
        # Meson normally applies these build definitions after fetching its
        # Berkeley subprojects. Supply the same pinned overlays offline.
        qemu = inputs["repositories"]["qemu"]
        for project in ("berkeley-softfloat-3", "berkeley-testfloat-3"):
            prefix = f"subprojects/packagefiles/{project}/"
            files = subprocess.check_output(["git", "-C", str(cache / qemu["checkout"]), "ls-tree", "-r", "--name-only", qemu["commit"], prefix], text=True).splitlines()
            for path in files:
                data = subprocess.check_output(["git", "-C", str(cache / qemu["checkout"]), "show", qemu["commit"] + ":" + path])
                add_bytes(output, f"qemu/subprojects/{project}/" + path.removeprefix(prefix), data, inputs["source_date_epoch"])
        for name in inputs["archives"]:
            output.add(cache / name, arcname="emulator/" + name)
        # Avoid references to omitted, unused EDK2 binaries during configure.
        data = subprocess.check_output(["git", "-C", str(cache / qemu["checkout"]), "show", qemu["commit"] + ":meson.build"])
        original = b"  if target in target_dirs\n    bzip2 = find_program('bzip2'"
        replacement = b"  if get_option('install_blobs') and target in target_dirs\n    bzip2 = find_program('bzip2'"
        if data.count(original) != 1:
            raise ValueError("The no-install-blobs patch needs updating")
        add_bytes(output, "qemu/meson.build", data.replace(original, replacement), inputs["source_date_epoch"])
        # Apply the parent cache policy to the exact pinned upstream source.
        with tempfile.TemporaryDirectory(prefix="emacsos-wasm-patch-") as temporary:
            path = Path(temporary) / "tcg/wasm32.c"
            path.parent.mkdir()
            path.write_bytes(subprocess.check_output([
                "git", "-C", str(cache / qemu["checkout"]), "show", qemu["commit"] + ":tcg/wasm32.c"]))
            subprocess.run(["patch", "--batch", "-p1", "-d", temporary],
                           input=(ROOT / "system/wasm/bounded-cache.patch").read_bytes(), check=True)
            add_bytes(output, "qemu/tcg/wasm32.c", path.read_bytes(), inputs["source_date_epoch"])
        add_bytes(output, "sources.json", (ROOT / "system/wasm/inputs.json").read_bytes(), inputs["source_date_epoch"])
        for name in ("Toolchain.Dockerfile", "build.sh", "no-install-blobs.patch", "bounded-cache.patch"):
            add_bytes(output, name, (ROOT / "system/wasm" / name).read_bytes(), inputs["source_date_epoch"])
    print(json.dumps({"archive": str(args.output), "sha256": digest(args.output)}))


if __name__ == "__main__":
    main()
