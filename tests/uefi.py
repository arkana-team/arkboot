#!/usr/bin/env python3
"""Boot x86-64 Arkboot under OVMF, one bounded guest at a time."""
import argparse
import os
import pathlib
import selectors
import shutil
import socket
import subprocess
import sys
import time

from boot import REPO, available_kib


def firmware_pair():
    for directory, code, variables in (
        ("/usr/share/edk2/x64", "OVMF_CODE.4m.fd", "OVMF_VARS.4m.fd"),
        ("/usr/share/OVMF", "OVMF_CODE_4M.fd", "OVMF_VARS_4M.fd"),
        ("/usr/share/OVMF", "OVMF_CODE.fd", "OVMF_VARS.fd"),
    ):
        paths = tuple(pathlib.Path(directory) / name for name in (code, variables))
        if all(path.is_file() for path in paths):
            return paths
    return None, None


def keypress(monitor, key):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(2)
        client.connect(str(monitor))
        client.sendall(f"sendkey {key}\n".encode())


def boot(image, optical, name, expected_fs, work, args, action=None, error=None):
    if available_kib() < 768 * 1024:
        raise RuntimeError("Less than 768 MiB host RAM available; refusing another guest")
    variables = work / "vars.fd"
    shutil.copyfile(args.ovmf_vars, variables)
    monitor = work / "monitor.sock"
    monitor.unlink(missing_ok=True)
    command = ["qemu-system-x86_64", "-machine", "q35,accel=tcg", "-cpu", "qemu64", "-m", "256",
               "-drive", f"if=pflash,format=raw,readonly=on,file={args.ovmf_code}",
               "-drive", f"if=pflash,format=raw,file={variables}",
               "-display", "none", "-monitor", f"unix:{monitor},server=on,wait=off",
               "-serial", "stdio", "-nic", "none", "-no-reboot", "-no-shutdown"]
    command += (["-cdrom", str(image), "-boot", "order=d"] if optical else
                ["-drive", f"format=raw,file={image}", "-boot", "order=c"])
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, bufsize=0)
    output = bytearray()
    selection_time = None
    try:
        with selectors.DefaultSelector() as selector, (work / f"{name}-serial.log").open("wb") as log:
            selector.register(process.stdout, selectors.EVENT_READ)
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                for key, _ in selector.select(0.1):
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    if not chunk:
                        selector.unregister(key.fileobj)
                    log.write(chunk)
                    log.flush()
                    output.extend(chunk)
                if len(output) > 8 * 1024 * 1024:
                    raise RuntimeError("Guest exceeded the 8 MiB log limit")
                if action and selection_time is None and b"Up/Down or W/S:" in output:
                    if action == "serial":
                        process.stdin.write(b"s")
                    else:
                        keypress(monitor, "down")
                    selection_time = time.monotonic()
                if action and selection_time is not None and time.monotonic() - selection_time >= 2:
                    assert b"Starting Linux" not in output, "input failed to cancel countdown"
                    if action == "serial":
                        process.stdin.write(b"\r")
                    else:
                        keypress(monitor, "ret")
                    action = None
                if error and error.encode() in output:
                    assert b"Starting Linux" not in output
                    break
                if b"ARKBOOT_USERSPACE_OK" in output:
                    for marker in ("ARKBOOT UEFI x86-64", f"Found {expected_fs}",
                                   "Starting Linux", "arkboot.uefi=Selected", "arkboot.unicode=café",
                                   "Loaded initrd from LINUX_EFI_INITRD_MEDIA_GUID"):
                        assert marker.encode() in output, f"missing {marker}"
                    break
                if process.poll() is not None:
                    raise RuntimeError("QEMU exited before the expected result")
            else:
                raise RuntimeError(f"UEFI boot timed out: {name}: {output[-4000:]!r}")
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        process.stdin.close()
        process.stdout.close()
        monitor.unlink(missing_ok=True)
        variables.unlink(missing_ok=True)
    print(f"PASS UEFI {name}: {error or 'Linux -> PID 1'}", flush=True)


def main():
    code, variables = firmware_pair()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ovmf-code", type=pathlib.Path, default=code)
    parser.add_argument("--ovmf-vars", type=pathlib.Path, default=variables)
    parser.add_argument("--root", type=pathlib.Path, default=REPO / "build/validation/root")
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("--case", help="Run one case, e.g. ext4-gpt or keyboard")
    parser.add_argument("--mkisofs", default=os.environ.get("MKISOFS", "mkisofs"))
    args = parser.parse_args()
    if not args.ovmf_code or not args.ovmf_vars:
        parser.error("OVMF not found; supply --ovmf-code and --ovmf-vars")
    args.ovmf_code = args.ovmf_code.resolve(strict=True)
    args.ovmf_vars = args.ovmf_vars.resolve(strict=True)
    source = args.root.resolve(strict=True)
    os.chdir(REPO)
    work = REPO / "build/uefi-tests"
    root = work / "root"
    (root / "boot").mkdir(parents=True, exist_ok=True)
    for name in ("vmlinuz", "initrd.img"):
        shutil.copyfile(source / "boot" / name, root / "boot" / name)
    cases = [(f"{fs}-mbr", fs, size, "mbr", None, None) for fs, size in
             (("fat12", 72), ("fat16", 96), ("fat32", 160), ("ext2", 80), ("ext3", 80), ("ext4", 80))]
    cases += [("fat32-gpt", "fat32", 160, "gpt", None, None),
              ("ext4-gpt", "ext4", 80, "gpt", None, None),
              ("iso9660", "ISO9660", 0, "", None, None), ("udf", "UDF", 0, "", None, None),
              ("serial", "fat12", 72, "gpt", "serial", None),
              ("keyboard", "fat12", 72, "gpt", "keyboard", None),
              ("invalid-config", "fat12", 72, "gpt", None, "default entry index is outside entries list"),
              ("invalid-kernel", "fat12", 72, "gpt", None, "requires an x86-64 Linux kernel with CONFIG_EFI_STUB=y")]
    if args.case:
        cases = [case for case in cases if case[0] == args.case]
        if not cases:
            parser.error("Unknown UEFI test case")
    if any(case[0] == "udf" for case in cases) and not shutil.which(args.mkisofs):
        parser.error("UDF requires cdrtools mkisofs; supply --mkisofs")
    for name, fs, size, table, action, error in cases:
        selected_kernel = "/boot/vmlinuz"
        if name == "invalid-kernel":
            # An IA32 PE header must be rejected before firmware executes it.
            image32 = bytearray(128)
            image32[:2] = b"MZ"
            image32[0x3c:0x40] = (64).to_bytes(4, "little")
            image32[64:68] = b"PE\0\0"
            image32[68:70] = (0x14c).to_bytes(2, "little")
            image32[88:90] = (0x10b).to_bytes(2, "little")
            (root / "boot/ia32.efi").write_bytes(image32)
            selected_kernel = "/boot/ia32.efi"
        default = 3 if name == "invalid-config" else 0 if action else 1
        text = (f"default: {default}\ntimeout: {1 if action or name == 'ext4-gpt' else 0}\nentries:\n"
                "  - title: Unselected\n    kernel: /missing-kernel\n"
                f"  - title: Selected\n    kernel: {selected_kernel}\n    initrd: /boot/initrd.img\n"
                "    cmdline: console=ttyS0,115200 panic=-1 arkboot.uefi=Selected arkboot.unicode=café\n")
        (root / "boot/arkboot.yaml").write_text(text)
        image = work / f"{name}.{'img' if table else 'iso'}"
        command = [sys.executable, "scripts/mkimage.py", "disk" if table else "iso", "--uefi",
                   "--root", str(root), "--output", str(image)]
        if table:
            command += ["--fs", fs, "--size-mib", str(size), "--partition-table", table]
        elif fs == "UDF":
            command += ["--udf", "--mkisofs", args.mkisofs]
        with (work / f"{name}-image.log").open("wb") as log:
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
        expected_fs = fs.upper() if fs.startswith("fat") else "ext2/3/4" if fs.startswith("ext") else fs
        boot(image, not table, name, expected_fs, work, args, action, error)
        image.unlink()
    print(f"All {len(cases)} UEFI cases passed")


if __name__ == "__main__":
    main()
