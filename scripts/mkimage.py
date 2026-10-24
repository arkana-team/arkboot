#!/usr/bin/env python3
"""Build BIOS and optional x86-64 UEFI images; never install to a device."""
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


def copy_root(root, tree):
    """Snapshot a resolved root without following mutable source pathnames."""
    directory_flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW

    def copy_directory(source, destination):
        destination.mkdir()
        for name in os.listdir(source):
            mode = os.stat(name, dir_fd=source, follow_symlinks=False).st_mode
            if not (stat.S_ISDIR(mode) or stat.S_ISREG(mode)):
                raise ValueError(f"Only regular files/directories are supported in boot trees: {name}")
            flags = directory_flags if stat.S_ISDIR(mode) else os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK
            descriptor = os.open(name, flags, dir_fd=source)
            try:
                mode = os.fstat(descriptor).st_mode
                target = destination / name
                if stat.S_ISDIR(mode):
                    copy_directory(descriptor, target)
                elif stat.S_ISREG(mode):
                    with os.fdopen(descriptor, "rb", closefd=False) as src, target.open("wb") as dst:
                        shutil.copyfileobj(src, dst)
                    shutil.copystat(descriptor, target)
                else:
                    raise ValueError(f"Only regular files/directories are supported in boot trees: {name}")
            finally:
                os.close(descriptor)
        shutil.copystat(source, destination)

    # Pin every component, including root's parents, before walking its entries.
    source = os.open(root.anchor, directory_flags)
    try:
        for name in root.parts[1:]:
            descriptor = os.open(name, directory_flags, dir_fd=source)
            os.close(source)
            source = descriptor
        copy_directory(source, tree)
    finally:
        os.close(source)


def partition(args, output, length):
    with output.open("wb") as stream:
        stream.truncate(length)
    if args.fs.startswith("fat"):
        run(tool("mkfs.fat"), "-F", args.fs[3:], output)
        for child in sorted(args.root.iterdir()):
            run(tool("mcopy"), "-i", output, "-s", child, "::/")
    else:
        run(tool(f"mkfs.{args.fs}"), "-q", "-F", "-d", args.root, output)


def efi_partition(output, optical=False):
    # FAT32 ESP for disks; a small removable FAT16 image for El Torito CDs.
    with output.open("wb") as stream:
        stream.truncate((8 if optical else 64) * 1024 * 1024)
    run(tool("mkfs.fat"), "-F", "16" if optical else "32",
        *(["-s", "1"] if optical else []), output)
    run(tool("mmd"), "-i", output, "::/EFI", "::/EFI/BOOT")
    run(tool("mcopy"), "-i", output, REPO / "BOOTX64.EFI", "::/EFI/BOOT/BOOTX64.EFI")


def gpt(stream, blocks, fs, uefi=False):
    """Reserve a BIOS core area, optional ESP, and the selected data filesystem."""
    entries = bytearray(128 * 128)
    bios_type = uuid.UUID("21686148-6449-6e6f-744e-656564454649")
    data_type = uuid.UUID("ebd0a0a2-b9e5-4433-87c0-68b6b72699c7" if fs.startswith("fat")
                          else "0fc63daf-8483-4772-8e79-3d69d8477de4")
    last = blocks - 34
    layout = [(bios_type, 34, 2047, "Arkboot core")]
    data_start = 2048
    if uefi:
        data_start += 64 * 2048
        layout.append((uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b"),
                       2048, data_start - 1, "EFI System Partition"))
    layout.append((data_type, data_start, last, "Arkboot files"))
    for index, (kind, first, end, name) in enumerate(layout):
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
    data_start = 2048 + (64 * 2048 if args.uefi else 0)
    count = blocks - data_start - (33 if is_gpt else 0)
    if args.uefi and args.size_mib < 68:
        raise ValueError("UEFI disk images must be at least 68 MiB, including the 64 MiB ESP")
    core_lba = 34 if is_gpt else 1
    core = (REPO / "loader.bin").read_bytes()
    if core_lba * 512 + len(core) > 1024 * 1024:
        raise ValueError("Core exceeds its reserved embedding area")
    boot = bytearray((REPO / "boot.bin").read_bytes())
    struct.pack_into("<I", boot, 8, core_lba)
    kind = {"fat12": 1, "fat16": 0x0e, "fat32": 0x0c}.get(args.fs, 0x83)
    struct.pack_into("<B3sB3sII", boot, 446, 0 if is_gpt else 0x80, b"\xfe\xff\xff",
                     0xee if is_gpt else kind, b"\xfe\xff\xff",
                     1 if is_gpt else data_start, min(blocks - 1, 0xffffffff) if is_gpt else count)
    if args.uefi and not is_gpt:
        struct.pack_into("<B3sB3sII", boot, 462, 0, b"\xfe\xff\xff", 0xef,
                         b"\xfe\xff\xff", 2048, 64 * 2048)
    with tempfile.TemporaryDirectory(prefix="arkboot-", dir=args.output.parent) as tmp:
        part = pathlib.Path(tmp) / "partition.img"
        image = pathlib.Path(tmp) / "disk.img"
        partition(args, part, count * 512)
        esp = pathlib.Path(tmp) / "esp.img"
        if args.uefi:
            efi_partition(esp)
        with image.open("w+b") as stream:
            stream.truncate(blocks * 512)
            stream.write(boot)
            if is_gpt:
                gpt(stream, blocks, args.fs, args.uefi)
            stream.seek(core_lba * 512)
            stream.write(core)
            if args.uefi:
                stream.seek(1024 * 1024)
                with esp.open("rb") as source:
                    shutil.copyfileobj(source, stream, 1024 * 1024)
            stream.seek(data_start * 512)
            with part.open("rb") as source:
                shutil.copyfileobj(source, stream, 1024 * 1024)
        os.replace(image, args.output)


def iso(args):
    with tempfile.TemporaryDirectory(prefix="arkboot-", dir=args.output.parent) as tmp:
        tree = args.root
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
        if args.uefi:
            efi_partition(tree / "boot/arkboot-efi.img", optical=True)
        # Explicit platform + -b works with both cdrtools and xorriso.
        efi_options = (["-eltorito-alt-boot", "-eltorito-platform", "efi",
                        "-b", "boot/arkboot-efi.img", "-no-emul-boot"]
                       if args.uefi else [])
        run(*command, "-iso-level", "3", "-R", "-J", "-b", "boot/arkboot-cd.bin",
            "-c", "boot/boot.cat", "-no-emul-boot", "-boot-load-size", "4",
            "-boot-info-table", *efi_options, "-o", generated, tree)
        os.replace(generated, args.output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="kind", required=True)
    for name in ("disk", "iso"):
        p = sub.add_parser(name)
        p.add_argument("--root", required=True, type=pathlib.Path)
        p.add_argument("--output", required=True, type=pathlib.Path)
        p.add_argument("--uefi", action="store_true", help="Add x86-64 UEFI boot alongside BIOS")
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
    for binary in ("boot.bin", "cdboot.bin", "loader.bin") + (("BOOTX64.EFI",) if args.uefi else ()):
        if not (REPO / binary).is_file():
            raise ValueError("Run make before generating images")
    with tempfile.TemporaryDirectory(prefix="arkboot-", dir=args.output.parent) as tmp:
        tree = pathlib.Path(tmp) / "root"
        copy_root(args.root, tree)
        args.root = tree
        if not any((args.root / directory / f"arkboot.{ext}").is_file()
                   for directory in ("boot", "") for ext in ("yaml", "yml", "cfg")):
            raise ValueError("Boot tree must contain arkboot.yaml, arkboot.yml or arkboot.cfg in boot/ or root")
        (disk if args.kind == "disk" else iso)(args)
    print(f"Created {args.output}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(str(exc)) from exc
