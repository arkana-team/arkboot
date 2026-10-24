#!/usr/bin/env python3
"""Check real filesystem images against the production readers; no kernel needed."""
import os
import pathlib
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "scripts"))
import mkimage
from fixtures import fragment_fat, flip


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def main():
    os.chdir(REPO)
    run(sys.executable, "tests/image_builder.py")
    run(sys.executable, "tests/write_media.py")
    work = REPO / "build/check"
    root = work / "root"
    (root / "boot").mkdir(parents=True, exist_ok=True)
    (root / "boot/arkboot.cfg").write_text("kernel=/boot/payload.bin\ninitrd=\n")
    # Enough data to use double-indirect addressing with ext's 1 KiB blocks.
    with (root / "boot/payload.bin").open("wb") as stream:
        for i in range(40):
            stream.write(bytes((j * 7 + i * 31) % 256 for j in range(65536)))
    (root / "boot/long-file-name.txt").write_text("Long filename and unaligned read fixture\n" * 53)
    with (root / "boot/sparse.bin").open("wb") as stream:
        stream.write(b"start of sparse file")
        stream.seek(2 * 1024 * 1024 - 23)
        stream.write(b"end of sparse file data")
    config_harness = work / "config_host"
    run("gcc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-Wall", "-Wextra", "-Werror",
        "tests/config_host.c", "config.c", "-o", config_harness)
    run(config_harness)
    fat_harness = work / "fat_host"
    run("gcc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-Wall", "-Wextra", "-Werror",
        "tests/fat_host.c", "disk.c", *sorted((REPO / "fs").glob("*.c")), "-o", fat_harness)
    run(fat_harness)
    ext_harness = work / "ext_lookup_host"
    run("gcc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-Wall", "-Wextra", "-Werror",
        "tests/ext_lookup_host.c", "disk.c", *sorted((REPO / "fs").glob("*.c")), "-o", ext_harness)
    run(ext_harness)
    harness = work / "fs_host"
    run("gcc", "-std=c11", "-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=undefined", "-fno-sanitize-recover=all",
        "-Wall", "-Wextra", "-Werror", "tests/fs_host.c", "disk.c", "bios_disk.c", *sorted((REPO / "fs").glob("*.c")), "-o", harness)
    cases = [(fs, size, "mbr") for fs, size in
             (("fat12", 8), ("fat16", 32), ("fat32", 96), ("ext2", 32), ("ext3", 32), ("ext4", 32))]
    cases += [("fat32", 96, "gpt"), ("ext4", 32, "gpt")]
    for fs, size, table in cases:
        image = work / f"{fs}-{table}.img"
        run(sys.executable, "scripts/mkimage.py", "disk", "--root", root, "--output", image,
            "--fs", fs, "--size-mib", size, "--partition-table", table)
        if fs.startswith("fat"):
            fragment_fat(image)
        run(harness, image, 512, fs.upper() if fs.startswith("fat") else "ext2/3/4",
            "/boot/arkboot.cfg", root / "boot/arkboot.cfg", "/boot/payload.bin", root / "boot/payload.bin",
            "/boot/long-file-name.txt", root / "boot/long-file-name.txt",
            "/boot/sparse.bin", root / "boot/sparse.bin")
        if fs == "ext4":
            if table == "gpt":
                flip(image, 512 + 16)
                run(harness, image, 512, "reject", "GPT header checksum")
                flip(image, 512 + 16)
                flip(image, 2 * 512 + 64)
                run(harness, image, 512, "reject", "GPT partition checksum")
            else:
                flip(image, 1024 * 1024 + 1024 + 96, 4)
                run(harness, image, 512, "reject", "dirty journal")
        image.unlink()
    for udf in (False, True):
        tool = os.environ.get("MKISOFS", "mkisofs")
        if udf and not shutil.which(tool):
            raise SystemExit("UDF checks require cdrtools mkisofs; set MKISOFS to its path")
        image = work / ("udf.iso" if udf else "iso9660.iso")
        command = [sys.executable, "scripts/mkimage.py", "iso", "--root", root, "--output", image]
        if udf:
            command += ["--udf", "--mkisofs", tool]
        run(*command)
        # Plain ISO uses ISO names, with long names converted by the image tool.
        arguments = [harness, image, 2048, "UDF" if udf else "ISO9660",
                     "/boot/arkboot.cfg", root / "boot/arkboot.cfg",
                     "/boot/payload.bin", root / "boot/payload.bin"]
        if udf:
            arguments += ["/boot/long-file-name.txt", root / "boot/long-file-name.txt"]
        arguments += ["/boot/sparse.bin", root / "boot/sparse.bin"]
        run(*arguments)
        subprocess.run([str(arg) for arg in arguments], check=True,
                       env={**os.environ, "ARKBOOT_TEST_UNKNOWN_CAPACITY": "1"})
        if udf:
            flip(image, 256 * 2048 + 4)
            run(harness, image, 2048, "reject", "invalid UDF descriptor")
        image.unlink()
    # Image tools must never overwrite a device through a symlink.
    link = work / "output-link"
    link.unlink(missing_ok=True)
    link.symlink_to("/dev/null")
    try:
        try:
            mkimage.check_output(link)
        except ValueError:
            pass
        else:
            raise AssertionError("symlink output accepted")
    finally:
        link.unlink()
    print("All filesystem image checks passed")


if __name__ == "__main__":
    main()
