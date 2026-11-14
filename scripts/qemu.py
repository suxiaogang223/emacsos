#!/usr/bin/env python3
"""Launch the fixed native machine profile shared by interactive and test runs."""
import argparse
import os
from pathlib import Path


def command(image, *, interactive=False, debug_exit=False):
    args = ["qemu-system-x86_64", "-M", "pc", "-m", "1G", "-nic", "none",
            "-display", "none", "-monitor", "none", "-serial",
            "mon:stdio" if interactive else "stdio", "-no-reboot",
            "-drive", f"file={image},format=raw,if=ide,index=0,media=disk,snapshot=on"]
    if debug_exit:
        args += ["-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"]
    return args


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path,
                        default=os.environ.get("EMACSOS_IMAGE", "build/mvp/emacsos.img"))
    args, extra = parser.parse_known_args()
    native = command(args.image, interactive=True) + extra
    os.execvp(native[0], native)
