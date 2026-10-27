#!/usr/bin/env python3
"""Preview or write an Arkboot disk/CD image on Linux. Default: preview only."""
import argparse
import contextlib
import json
import os
import pathlib
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile


COLUMNS = "PATH,TYPE,SIZE,RO,MODEL,SERIAL,LOG-SEC,MOUNTPOINTS,MAJ:MIN"


def run(command, **kwargs):
    return subprocess.run(command, check=True, env={**os.environ, "LC_ALL": "C"}, **kwargs)


def file_identity(fd):
    info = os.fstat(fd)
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns


@contextlib.contextmanager
def open_image(image):
    # Pin each ancestor without following symlinks, as in mkimage.copy_root.
    directory_flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    directory = os.open(image.anchor, directory_flags)
    try:
        for name in image.parts[1:-1]:
            descriptor = os.open(name, directory_flags, dir_fd=directory)
            os.close(directory)
            directory = descriptor
        fd = os.open(image.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
                     dir_fd=directory)
    finally:
        os.close(directory)
    with os.fdopen(fd, "rb") as source:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise ValueError("Image must be a nonempty regular file")
        yield source


@contextlib.contextmanager
def image_snapshot(source, expected_identity):
    fd = source.fileno()
    if file_identity(fd) != expected_identity:
        raise ValueError("Image changed during confirmation")
    source.seek(0)
    # Ignore TMPDIR: the snapshot's parent must not be attacker-controlled.
    # /tmp's sticky bit protects our private directory from replacement.
    with tempfile.TemporaryDirectory(prefix="arkboot-media-", dir="/tmp") as directory:
        snapshot = pathlib.Path(directory) / "image"
        with snapshot.open("xb") as target:
            shutil.copyfileobj(source, target, 1024 * 1024)
        if file_identity(fd) != expected_identity:
            raise ValueError("Image changed while copying")
        yield snapshot


def image_info(stream, kind):
    info = os.fstat(stream.fileno())
    if not stat.S_ISREG(info.st_mode) or not info.st_size:
        raise ValueError("Image must be a nonempty regular file")
    stream.seek(0)
    if kind == "disk":
        header = stream.read(1024)
        if (info.st_size % 512 or len(header) < 1024 or header[510:512] != b"\x55\xaa"
                or b"ARKBOOT: BIOS disk read failed" not in header[:440]):
            raise ValueError("Expected a complete Arkboot .img from scripts/mkimage.py disk")
        protective = any(header[446 + i * 16 + 4] == 0xee for i in range(4))
        gpt = header[512:520] == b"EFI PART"
        if protective != gpt:
            raise ValueError("Inconsistent protective MBR/GPT in image")
        return info.st_size, gpt
    if info.st_size % 2048:
        raise ValueError("ISO size must be a multiple of 2048 bytes")
    for block in range(16, 80):
        stream.seek(block * 2048)
        descriptor = stream.read(2048)
        if len(descriptor) != 2048 or descriptor[1:6] != b"CD001" or descriptor[0] == 255:
            break
        if descriptor[0] == 0 and descriptor[7:30] == b"EL TORITO SPECIFICATION":
            return info.st_size, False
    raise ValueError("Expected a bootable El Torito ISO from scripts/mkimage.py iso")


def device_tree(device):
    if not stat.S_ISBLK(device.stat().st_mode):
        raise ValueError("Target must be a Linux block device, not a regular file")
    result = run(["lsblk", "--json", "--bytes", "--tree", "--output", COLUMNS, "--", str(device)],
                 capture_output=True, text=True)
    roots = json.loads(result.stdout)["blockdevices"]
    if len(roots) != 1 or pathlib.Path(roots[0]["path"]).resolve() != device:
        raise ValueError("Cannot identify the selected device uniquely")
    return roots[0]


def identity(node):
    return tuple(node.get(key) for key in ("path", "type", "size", "model", "serial", "maj:min", "log-sec"))


def flatten(node):
    yield node
    for child in node.get("children", []):
        yield from flatten(child)


def active_swaps():
    devices = set()
    for line in pathlib.Path("/proc/swaps").read_text().splitlines()[1:]:
        # Kernel proc paths escape whitespace as octal sequences.
        name = line.split()[0]
        for code, char in ((r"\040", " "), (r"\011", "\t"), (r"\012", "\n"), (r"\134", "\\")):
            name = name.replace(code, char)
        info = os.stat(name)
        number = info.st_rdev if stat.S_ISBLK(info.st_mode) else info.st_dev
        devices.add(f"{os.major(number)}:{os.minor(number)}")
    return devices


def validate_target(tree, kind, image_size, swaps, sysfs=pathlib.Path("/sys/dev/block")):
    expected = "disk" if kind == "disk" else "rom"
    if tree["type"] != expected:
        raise ValueError(f"Target must be a whole {'HDD/SSD/USB disk' if kind == 'disk' else 'optical drive'}")
    for node in flatten(tree):
        mounts = node.get("mountpoints") or []
        if any(mounts) or node["maj:min"] in swaps:
            raise ValueError(f"Device is mounted or has active swap: {node['path']}")
        holders = sysfs / node["maj:min"] / "holders"
        if not holders.is_dir():
            raise ValueError(f"Cannot inspect active device users: {node['path']}")
        if any(holders.iterdir()):
            raise ValueError(f"Device has active RAID/LVM/encryption users: {node['path']}")
    if kind == "disk":
        if tree["ro"]:
            raise ValueError("Target disk is read-only")
        if int(tree["log-sec"]) != 512:
            raise ValueError("These images require a disk with 512-byte logical sectors")
        if int(tree["size"]) < image_size:
            raise ValueError("Target disk is smaller than the image")


def commands(kind, image, device, size, gpt, speed=None):
    if kind == "cd":
        return [["xorriso", "-no_rc", "-as", "cdrecord", "-v", f"dev={device}",
                 "-sao", "-eject", *([f"speed={speed}"] if speed else []), "-data", str(image)]]
    # The parent holds O_EXCL. --force prevents wipefs taking a second claim;
    # the parent's claim supplies the mounted-device protection.
    result = [
        ["wipefs", "--all", "--force", str(device)],
        ["dd", f"if={image}", f"of={device}", "bs=4M", "conv=fsync", "status=progress"],
        ["cmp", f"--bytes={size}", str(image), str(device)],
    ]
    if gpt:
        result.append(["sgdisk", "--move-second-header", str(device)])
    result.append(["blockdev", "--rereadpt", str(device)])
    return result


def blank_cd(device):
    result = run(["xorriso", "-no_rc", "-indev", str(device), "-toc"],
                 capture_output=True, text=True)
    # Drive acquisition and -toc can repeat records. Require agreement on the
    # complete records so a volume label cannot authorize a nonblank disc.
    lines = result.stdout.splitlines() + result.stderr.splitlines()
    statuses = {line for line in lines if line.startswith("Media status :")}
    profiles = {line for line in lines if line.startswith("Media current:")}
    if (statuses != {"Media status : is blank"}
            or profiles not in ({"Media current: CD-R"}, {"Media current: CD-RW"})):
        raise ValueError("Insert a blank CD-R or already-blanked CD-RW; this helper does not erase discs")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("disk", "cd"))
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("device", type=pathlib.Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true", help="Write after interactive device confirmation")
    mode.add_argument("--dry-run", action="store_true", help="Preview only (the default)")
    parser.add_argument("--speed", type=int, help="Requested optical write speed, e.g. 8")
    args = parser.parse_args()
    if sys.platform != "linux":
        raise ValueError("This helper supports Linux; see docs/install.md for other systems")
    if args.speed is not None and (args.kind != "cd" or args.speed < 1):
        parser.error("--speed must be a positive integer and is only for CD burning")
    # Preserve symlinks for the descriptor walk to reject, rather than resolve.
    image = pathlib.Path(os.path.abspath(args.image))
    device = args.device.resolve(strict=True)
    with open_image(image) as source:
        write_media(args, image, source, device)


def write_media(args, image, source, device):
    source_identity = file_identity(source.fileno())
    size, gpt = image_info(source, args.kind)
    tree = device_tree(device)
    number = device.stat().st_rdev
    validate_target(tree, args.kind, size, active_swaps())
    plan = commands(args.kind, image, device, size, gpt, args.speed)
    print(f"Image:  {image} ({size} bytes)")
    print(f"Target: {device} ({tree['size']} bytes)")
    print(f"Model:  {tree.get('model') or 'unknown'}; serial: {tree.get('serial') or 'unknown'}")
    print("Replaces the ENTIRE DISK and its partition table." if args.kind == "disk" else "Burns a blank optical disc.")
    for command in plan:
        print("  " + shlex.join(command))
    if not args.write:
        print("Preview only. Add --write to write after typing the target device path.")
        return
    for command in plan:
        if not shutil.which(command[0]):
            raise ValueError(f"Required tool is missing: {command[0]}")
    if os.geteuid() != 0:
        raise ValueError("Run with sudo for --write")
    if not sys.stdin.isatty():
        raise ValueError("--write requires interactive confirmation in a terminal")
    if args.kind == "cd":
        blank_cd(device)
    if input(f"Type the full target path {device} to continue: ").strip() != str(device):
        raise ValueError("Confirmation did not match; nothing was written")
    # Recheck after confirmation; keep a disk exclusive-open during the write so
    # the kernel refuses mounted devices (including mounts in other namespaces).
    with image_snapshot(source, source_identity) as snapshot:
        with snapshot.open("rb") as snapshot_source:
            if image_info(snapshot_source, args.kind) != (size, gpt):
                raise ValueError("Image changed during confirmation")
        plan = commands(args.kind, snapshot, device, size, gpt, args.speed)
        current = device_tree(device)
        if identity(current) != identity(tree) or device.stat().st_rdev != number:
            raise ValueError("Target device changed during confirmation")
        validate_target(current, args.kind, size, active_swaps())
        if args.kind == "cd":
            blank_cd(device)
        fd = None
        try:
            if args.kind == "disk":
                fd = os.open(device, os.O_RDWR | os.O_EXCL)
                if os.fstat(fd).st_rdev != number:
                    raise ValueError("Device changed during confirmation")
            for command in plan:
                run(command)
        finally:
            if fd is not None:
                os.close(fd)
    print("Done. Disk bytes were compared before GPT relocation." if args.kind == "disk" else
          "Burn completed and disc ejected. Reinsert it to verify or boot it.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as exc:
        raise SystemExit(str(exc)) from exc
    except EOFError:
        raise SystemExit("Confirmation canceled; nothing was written.") from None
    except KeyboardInterrupt:
        raise SystemExit("Interrupted; if writing had started, treat the medium as incomplete.") from None
