#!/usr/bin/env python3
"""Append a bounded ELF/newc runtime payload at the fixed IDE disk offset."""
import argparse
import pathlib
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("skeleton", type=pathlib.Path)
parser.add_argument("elf", type=pathlib.Path)
parser.add_argument("archive", type=pathlib.Path)
parser.add_argument("output", type=pathlib.Path)
args = parser.parse_args()
boot = args.skeleton.read_bytes()
elf = args.elf.read_bytes()
archive = args.archive.read_bytes()
if len(boot) > 1024 * 1024:
    parser.error("kernel image overlaps runtime header")
if not elf or len(elf) > 256 * 1024 * 1024 or len(archive) > 256 * 1024 * 1024:
    parser.error("runtime payload exceeds fixed RAM contract")
with args.output.open("wb") as stream:
    stream.write(boot)
    stream.write(bytes(1024 * 1024 - len(boot)))
    stream.write(struct.pack("<8sQQ", b"EMACSOS1", len(elf), len(archive)))
    stream.write(bytes(512 - 24))
    for data in (elf, archive):
        stream.write(data)
        stream.write(bytes(-len(data) % 512))
