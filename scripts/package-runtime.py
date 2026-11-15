#!/usr/bin/env python3
"""Package a bounded, deterministic newc runtime and its artifact manifest."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_newc(root, archive):
    entries = sorted(root.rglob("*"), key=lambda p: p.relative_to(root).as_posix())
    if len(entries) > 8190:
        raise ValueError("Runtime exceeds the kernel's 8192-node archive bound")
    manifest = []
    with archive.open("wb") as output:
        for ino, path in enumerate(entries, 1):
            name = path.relative_to(root).as_posix()
            if len(name.encode()) >= 1024 or any(len(p.encode()) > 255 for p in Path(name).parts):
                raise ValueError(f"Runtime path exceeds kernel bound: {name}")
            mode = path.lstat().st_mode
            if path.is_symlink():
                data = os.readlink(path).encode()
                record = {"path": "/" + name, "symlink": data.decode()}
            elif path.is_file():
                data = path.read_bytes()
                record = {"path": "/" + name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            elif path.is_dir():
                data = b""
                record = {"path": "/" + name, "directory": True}
            else:
                raise ValueError(f"Unsupported runtime entry: {name}")
            record["mode"] = mode
            encoded = name.encode() + b"\0"
            values = [ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(encoded), 0]
            header = b"070701" + b"".join(f"{v:08x}".encode() for v in values)
            output.write(header + encoded)
            output.write(b"\0" * (-(110 + len(encoded)) % 4))
            output.write(data)
            output.write(b"\0" * (-len(data) % 4))
            manifest.append(record)
        name = b"TRAILER!!!\0"
        values = [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0]
        output.write(b"070701" + b"".join(f"{v:08x}".encode() for v in values) + name)
        output.write(b"\0" * (-(110 + len(name)) % 4))
        output.write(b"\0" * (-output.tell() % 512))
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("build/runtime"))
    parser.add_argument("--site-lisp", type=Path, action="append", default=[], help="Include parent system Lisp from an additional directory")
    args = parser.parse_args()
    out = args.output.resolve()
    root = out / "root"
    parent = Path(__file__).resolve().parent.parent
    git = lambda *args: subprocess.check_output(["git", "-C", str(parent), *args], text=True).strip()
    identity = {"parent_commit": git("rev-parse", "HEAD"),
                "parent_dirty": bool(git("status", "--porcelain"))}
    sources = {}
    for directory in [parent / "lisp", *args.site_lisp]:
        if directory.is_dir():
            for path in directory.glob("*.el"):
                sources[path.name] = path
    # Developer manuals and external Unix helpers are outside this runtime profile.
    for relative in ("usr/share/info", "usr/share/man", "usr/share/icons", "usr/share/applications", "usr/share/metainfo", "usr/share/emacs/site-lisp", "usr/share/emacs/30.2/site-lisp", "usr/var", "var"):
        shutil.rmtree(root / relative, ignore_errors=True)
    # Install one canonical executable rather than duplicating versioned bytes.
    executable = root / "usr/bin/emacs"
    contents = executable.read_bytes()
    executable.unlink()
    executable.write_bytes(contents)
    executable.chmod(0o755)
    for path in executable.parent.iterdir():
        if path.name != "emacs":
            path.unlink()
    for path in (root / "usr/libexec").rglob("*"):
        if path.is_file() and path.suffix != ".pdmp":
            path.unlink()
    for path in (root / "usr/share/emacs/30.2/lisp").rglob("*.el"):
        if path.with_suffix(".elc").is_file():
            path.unlink()
    site = root / "usr/share/emacs/site-lisp"
    site.mkdir(parents=True, exist_ok=True)
    for name, path in sources.items():
        shutil.copyfile(path, site / name)
    etc = root / "etc"
    etc.mkdir(exist_ok=True)
    # Match the DATA template; these immutable defaults also serve recovery.
    # This uid/gid 0 runtime identity does not implement user isolation or a shell.
    (etc / "passwd").write_text("root:x:0:0:emacsOS:/home/user:\n")
    (etc / "group").write_text("root:x:0:\n")
    dumps = [p for p in root.rglob("*.pdmp") if not p.is_symlink()]
    if len(dumps) != 1:
        raise ValueError("Expected exactly one built portable dump")
    alias = dumps[0].parent / "emacs.pdmp"
    if alias != dumps[0]:
        alias.unlink(missing_ok=True)
        alias.symlink_to(dumps[0].name)
    # Expose a standalone artifact for the kernel ELF loader.
    shutil.copyfile(executable, out / "emacs")
    os.chmod(out / "emacs", 0o755)
    entries = write_newc(root, out / "runtime.cpio")
    if (out / "runtime.cpio").stat().st_size > 128 * 1024 * 1024:
        raise ValueError("Runtime archive exceeds the 128 MiB packaging budget")
    manifest = {
        "format": 1,
        "scope": "Host-built static runtime; no guest acceptance implied",
        **identity,
        "emacs_commit": git("-C", "emacs", "rev-parse", "HEAD"),
        "source_archive_sha256": sha256(out / "source.tar"),
        "profile": "GNU Emacs30.2 / static musl / x86-64 / terminal / pdumper",
        "source_date_epoch": (out / "source-date-epoch.txt").read_text().strip(),
        "build_key": (out / "build-key").read_text().strip(),
        "container_id": (out / "container-id").read_text().strip(),
        "container_recipe_sha256": sha256(parent / "scripts/Dockerfile.runtime"),
        "package_lock_sha256": sha256(parent / "scripts/runtime-packages.lock"),
        "archive_mtime": 0,
        "configure_arguments": (out / "configure-arguments.txt").read_text().strip(),
        "cflags": "-O2 -g0 -fno-pie",
        "ldflags": "-static -no-pie",
        "compiler": (out / "compiler.txt").read_text(),
        "packages": (out / "packages.txt").read_text().splitlines(),
        "elf": {"path": "emacs", "sha256": sha256(out / "emacs")},
        "archive": {"path": "runtime.cpio", "size": (out / "runtime.cpio").stat().st_size, "sha256": sha256(out / "runtime.cpio")},
        "files": entries,
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Packaged {len(entries)} entries, {manifest['archive']['size']} bytes")


if __name__ == "__main__":
    main()
