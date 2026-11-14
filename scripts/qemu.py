#!/usr/bin/env python3
"""Launch the fixed native machine profile shared by interactive and test runs."""
import argparse
import os
import shutil
from pathlib import Path


DATA_SIZE = 64 * 1024 * 1024


def drive_path(path):
    return str(path).replace(",", ",,")


def prepare_data(path, template=Path("build/image/data-empty.img")):
    """Create only a missing disk; existing bytes are never reformatted."""
    path = Path(path)
    if not path.exists():
        if not template.is_file() or template.stat().st_size != DATA_SIZE:
            raise ValueError("Build the initialized DATA template with make build")
        path.parent.mkdir(parents=True, exist_ok=True)
        try:
            with path.open("xb") as output, template.open("rb") as source:
                shutil.copyfileobj(source, output)
        except FileExistsError:
            pass
        except BaseException:
            path.unlink(missing_ok=True)
            raise
    if not path.is_file() or path.stat().st_size != DATA_SIZE:
        raise ValueError("DATA must be a regular 64 MiB image; existing bytes were preserved")
    return path


def command(image, *, interactive=False, debug_exit=False, data=None, control=None):
    args = ["qemu-system-x86_64", "-M", "pc", "-m", "1G", "-nic", "none",
            "-display", "none", "-monitor", "none", "-serial",
            "mon:stdio" if interactive else "stdio", "-no-reboot",
            "-drive", f"file={drive_path(image)},format=raw,if=ide,index=0,media=disk,snapshot=on"]
    if data is not None:
        args += ["-drive", f"file={drive_path(data)},format=raw,if=ide,index=1,media=disk,cache=writeback"]
    if control is not None:
        args += ["-chardev", f"pipe,id=storage,path={drive_path(control)}", "-serial", "chardev:storage"]
    if debug_exit:
        args += ["-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    return args


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path,
                        default=os.environ.get("EMACSOS_IMAGE", "build/image/emacsos.img"))
    parser.add_argument("--data", type=Path,
                        default=os.environ.get("EMACSOS_DATA", "build/data/emacsos-data.img"))
    parser.add_argument("--no-data", action="store_true", help="Boot with volatile working files")
    args, extra = parser.parse_known_args()
    try:
        data = None if args.no_data else prepare_data(args.data)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    native = command(args.image, interactive=True, data=data) + extra
    os.execvp(native[0], native)
