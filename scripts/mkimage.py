#!/usr/bin/env python3
"""Build BIOS disk/CD images as regular files; never install to a device."""
import argparse
import os
import pathlib
import shutil
import stat
import struct
import subprocess
import tempfile
import uuid
import zlib

REPO = pathlib.Path(__file__).resolve().parents[1]
FILESYSTEMS = ("fat12", "fat16", "fat32", "ext2", "ext3", "ext4")


def run(*args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def tool(name):
    found = shutil.which(name)
    if not found:
        raise ValueError(f"Required image tool is missing: {name}")
    return found


def check_output(path):
    if path.is_symlink() or (path.exists() and not stat.S_ISREG(path.stat().st_mode)):
        raise ValueError("Output must be a regular file, never a device or symlink")
    path.parent.mkdir(parents=True, exist_ok=True)


def partition(args, output, length):
    with output.open("wb") as stream:
        stream.truncate(length)
    if args.fs.startswith("fat"):
        run(tool("mkfs.fat"), "-F", args.fs[3:], output)
        for child in sorted(args.root.iterdir()):
            run(tool("mcopy"), "-i", output, "-s", child, "::/")
    else:
        run(tool(f"mkfs.{args.fs}"), "-q", "-F", "-d", args.root, output)


def gpt(stream, blocks, fs):
    """Reserve sectors 34..2047 for the core; data starts at the 1 MiB mark."""
    entries = bytearray(128 * 128)
    bios_type = uuid.UUID("21686148-6449-6e6f-744e-656564454649")
    data_type = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7" if fs.startswith("fat")
                          else "0fc63daf-8483-4772-8e79-3d69d8477de4")
    last = blocks - 34
    for index, kind, first, end, name in (
            (0, bios_type, 34, 2047, "Arkboot core"),
            (1, data_type, 2048, last, "Arkboot files")):
        struct.pack_into("<16s16sQQQ72s", entries, index * 128,
                         kind.bytes_le, uuid.uuid4().bytes_le, first, end, 0,
                         name.encode("utf-16le"))
    disk_guid = uuid.uuid4().bytes_le
    for current, backup, table in ((1, blocks - 1, 2), (blocks - 1, 1, blocks - 33)):
        header = bytearray(512)
        struct.pack_into("<8sIIIIQQQQ16sQIII", header, 0, b"EFI PART", 0x10000,
                         92, 0, 0, current, backup, 34, last, disk_guid,
                         table, 128, 128, zlib.crc32(entries))
        struct.pack_into("<I", header, 16, zlib.crc32(header[:92]))
        stream.seek(current * 512)
        stream.write(header)
        stream.seek(table * 512)
        stream.write(entries)


def disk(args):
    blocks = args.size_mib * 2048
    if args.size_mib < 4:
        raise ValueError("Disk image must be at least 4 MiB")
    is_gpt = args.partition_table == "gpt"
    count = blocks - 2048 - (33 if is_gpt else 0)
    core_lba = 34 if is_gpt else 1
    core = (REPO / "loader.bin").read_bytes()
    if core_lba * 512 + len(core) > 1024 * 1024:
        raise ValueError("Core exceeds its reserved embedding area")
    boot = bytearray((REPO / "boot.bin").read_bytes())
    struct.pack_into("<I", boot, 8, core_lba)
    kind = {"fat12": 1, "fat16": 0x0e, "fat32": 0x0c}.get(args.fs, 0x83)
    struct.pack_into("<B3sB3sII", boot, 446, 0 if is_gpt else 0x80, b"\xfe\xff\xff",
                     0xee if is_gpt else kind, b"\xfe\xff\xff",
                     1 if is_gpt else 2048, min(blocks - 1, 0xffffffff) if is_gpt else count)
    with tempfile.TemporaryDirectory(prefix="arkboot-", dir=args.output.parent) as tmp:
        part = pathlib.Path(tmp) / "partition.img"
        image = pathlib.Path(tmp) / "disk.img"
        partition(args, part, count * 512)
        with image.open("w+b") as stream:
            stream.truncate(blocks * 512)
            stream.write(boot)
            if is_gpt:
                gpt(stream, blocks, args.fs)
            stream.seek(core_lba * 512)
            stream.write(core)
            stream.seek(1024 * 1024)
            with part.open("rb") as source:
                shutil.copyfileobj(source, stream, 1024 * 1024)
        os.replace(image, args.output)


def iso(args):
    with tempfile.TemporaryDirectory(prefix="arkboot-", dir=args.output.parent) as tmp:
        tree = pathlib.Path(tmp) / "root"
        shutil.copytree(args.root, tree)
        (tree / "boot").mkdir(exist_ok=True)
        image = tree / "boot" / "arkboot-cd.bin"
        boot = (REPO / "cdboot.bin").read_bytes().ljust(2048, b"\0")
        core = (REPO / "loader.bin").read_bytes()
        image.write_bytes(boot + core + bytes(-len(core) % 2048))
        generated = pathlib.Path(tmp) / "image.iso"
        if args.udf:
            command = [tool(args.mkisofs), "-udf"]
        else:
            command = [tool("xorriso"), "-as", "mkisofs"]
        run(*command, "-iso-level", "3", "-R", "-J", "-b", "boot/arkboot-cd.bin",
            "-c", "boot/boot.cat", "-no-emul-boot", "-boot-load-size", "4",
            "-boot-info-table", "-o", generated, tree)
        os.replace(generated, args.output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="kind", required=True)
    for name in ("disk", "iso"):
        p = sub.add_parser(name)
        p.add_argument("--root", required=True, type=pathlib.Path)
        p.add_argument("--output", required=True, type=pathlib.Path)
        if name == "disk":
            p.add_argument("--fs", choices=FILESYSTEMS, default="fat32")
            p.add_argument("--size-mib", type=int, default=128)
            p.add_argument("--partition-table", choices=("mbr", "gpt"), default="mbr")
        else:
            p.add_argument("--udf", action="store_true")
            p.add_argument("--mkisofs", default="mkisofs",
                           help="cdrtools mkisofs executable for UDF (xorriso does not create UDF)")
    args = parser.parse_args()
    args.root = args.root.resolve(strict=True)
    check_output(args.output)
    if args.root == args.output.resolve() or args.root in args.output.resolve().parents:
        raise ValueError("Output must be outside the input tree")
    if not args.root.is_dir():
        raise ValueError("Root must be a directory")
    for path in args.root.rglob("*"):
        if path.is_symlink() or not (path.is_dir() or path.is_file()):
            raise ValueError(f"Only regular files/directories are supported in boot trees: {path}")
    if not ((args.root / "boot/arkboot.cfg").is_file() or (args.root / "arkboot.cfg").is_file()):
        raise ValueError("Boot tree must contain boot/arkboot.cfg or arkboot.cfg")
    for binary in ("boot.bin", "cdboot.bin", "loader.bin"):
        if not (REPO / binary).is_file():
            raise ValueError("Run make before generating images")
    (disk if args.kind == "disk" else iso)(args)
    print(f"Created {args.output}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(str(exc)) from exc
