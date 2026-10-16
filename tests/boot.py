#!/usr/bin/env python3
"""Boot one 128 MiB QEMU guest at a time and require the PID 1 success marker."""
import argparse
import os
import pathlib
import selectors
import shutil
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parents[1]


def available_kib():
    for line in pathlib.Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemAvailable:"):
            return int(line.split()[1])
    raise RuntimeError("Cannot determine available host memory")


def boot(image, optical, expected, logfile, timeout):
    if available_kib() < 768 * 1024:
        raise RuntimeError("Less than 768 MiB host RAM available; refusing another guest")
    command = ["qemu-system-x86_64", "-machine", "pc,accel=tcg", "-cpu", "qemu64", "-m", "128",
               "-display", "none", "-monitor", "none", "-serial", "stdio", "-nic", "none",
               "-no-reboot", "-no-shutdown"]
    command += (["-cdrom", str(image), "-boot", "order=d"] if optical else
                ["-drive", f"format=raw,file={image}", "-boot", "order=c"])
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    tail = bytearray()
    success = False
    try:
        with selectors.DefaultSelector() as selector, logfile.open("wb") as log:
            selector.register(process.stdout, selectors.EVENT_READ)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                for key, _ in selector.select(timeout=0.2):
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if not chunk:
                        break
                    log.write(chunk)
                    log.flush()
                    tail.extend(chunk)
                    if len(tail) > 8 * 1024 * 1024:
                        raise RuntimeError("Guest exceeded the 8 MiB log limit")
                    if b"ARKBOOT_USERSPACE_OK" in tail:
                        success = b"Starting Linux" in tail and f"Found {expected}".encode() in tail
                        break
                if success or process.poll() is not None:
                    break
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        process.stdout.close()
    if not success:
        raise RuntimeError(f"Boot failed: {image}; see {logfile}\n{tail[-4000:].decode(errors='replace')}")
    print(f"PASS {image.stem}: BIOS -> {expected} -> Linux -> PID 1", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=REPO / "build/validation/root")
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--case", help="Run just one named case, e.g. ext4-gpt or udf")
    parser.add_argument("--mkisofs", default=os.environ.get("MKISOFS", "mkisofs"))
    args = parser.parse_args()
    os.chdir(REPO)
    root = args.root.resolve(strict=True)
    work = REPO / "build/boot-tests"
    work.mkdir(parents=True, exist_ok=True)
    cases = [(f"{fs}-mbr", fs, size, "mbr") for fs, size in
             (("fat12", 8), ("fat16", 32), ("fat32", 96), ("ext2", 32), ("ext3", 32), ("ext4", 32))]
    cases += [("fat32-gpt", "fat32", 96, "gpt"), ("ext4-gpt", "ext4", 32, "gpt"),
              ("iso9660", "iso9660", 0, ""), ("udf", "udf", 0, "")]
    if args.case:
        cases = [case for case in cases if case[0] == args.case]
        if not cases:
            parser.error("Unknown test case")
    if any(case[1] == "udf" for case in cases) and not shutil.which(args.mkisofs):
        raise SystemExit("UDF tests need cdrtools mkisofs; set MKISOFS or --mkisofs")
    for name, fs, size, table in cases:
        image = work / f"{name}.{'iso' if not table else 'img'}"
        command = [sys.executable, "scripts/mkimage.py", "disk" if table else "iso",
                   "--root", str(root), "--output", str(image)]
        if table:
            command += ["--fs", fs, "--size-mib", str(size), "--partition-table", table]
        elif fs == "udf":
            command += ["--udf", "--mkisofs", args.mkisofs]
        with (work / f"{name}-image.log").open("wb") as log:
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
        boot(image, not table, fs.upper() if fs.startswith("fat") else
             "ext2/3/4" if fs.startswith("ext") else "UDF" if fs == "udf" else "ISO9660",
             work / f"{name}-serial.log", args.timeout)
        image.unlink()
    print(f"All {len(cases)} boot cases reached userspace")


if __name__ == "__main__":
    main()
