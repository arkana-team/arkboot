#!/usr/bin/env python3
"""Exercise YAML menus through BIOS keyboard, serial input and timed boot."""
import os
import pathlib
import selectors
import shutil
import socket
import subprocess
import sys
import time

from boot import REPO, available_kib


def exercise(image, optical, name, expected, action=None, error=None):
    if available_kib() < 768 * 1024:
        raise RuntimeError("Less than 768 MiB host RAM available")
    work = image.parent
    monitor = work / "monitor.sock"
    monitor.unlink(missing_ok=True)
    command = ["qemu-system-x86_64", "-machine", "pc,accel=tcg", "-m", "128",
               "-display", "none", "-monitor", f"unix:{monitor},server=on,wait=off",
               "-serial", "stdio", "-nic", "none", "-no-reboot", "-no-shutdown"]
    command += (["-cdrom", str(image), "-boot", "order=d"] if optical else
                ["-drive", f"format=raw,file={image}"])
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, bufsize=0)
    output = bytearray()
    sent = False
    menu_time = None
    try:
        with selectors.DefaultSelector() as selector, (work / f"{name}.log").open("wb") as log:
            selector.register(process.stdout, selectors.EVENT_READ)
            deadline = time.monotonic() + 40
            while time.monotonic() < deadline:
                for key, _ in selector.select(0.1):
                    chunk = os.read(key.fileobj.fileno(), 65536)
                    log.write(chunk)
                    log.flush()
                    output.extend(chunk)
                if len(output) > 2 * 1024 * 1024:
                    raise RuntimeError("Excessive guest output")
                if b"ARKBOOT - Boot menu" in output and menu_time is None:
                    menu_time = time.monotonic()
                if action and menu_time is not None and not sent:
                    # Stop the one-second timer, then wait beyond its deadline.
                    if action in ("serial", "arrows"):
                        process.stdin.write(b"s" if action == "serial" else b"\x1b[B")
                        sent = True
                    else:
                        with socket.socket(socket.AF_UNIX) as client:
                            client.settimeout(2)
                            client.connect(str(monitor))
                            client.sendall(b"sendkey down\n")
                        sent = True
                if action and sent and menu_time is not None and time.monotonic() - menu_time > 2:
                    assert b"Starting Linux" not in output, "key did not cancel countdown"
                    if action in ("serial", "arrows"):
                        process.stdin.write(b"\r")
                    else:
                        with socket.socket(socket.AF_UNIX) as client:
                            client.settimeout(2)
                            client.connect(str(monitor))
                            client.sendall(b"sendkey ret\n")
                    action = None
                if error and error.encode() in output:
                    assert b"Starting Linux" not in output
                    break
                if b"ARKBOOT_USERSPACE_OK" in output:
                    assert f"arkboot.menu={expected}".encode() in output
                    if menu_time is not None:
                        assert f"Booting {expected}".encode() in output
                    break
                if process.poll() is not None:
                    raise RuntimeError("QEMU exited unexpectedly")
            else:
                raise RuntimeError(f"Menu case {name} timed out: {output[-3000:]!r}")
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
    print(f"PASS menu {name}", flush=True)


def main():
    os.chdir(REPO)
    work = REPO / "build/menu-tests"
    root = work / "root"
    root.mkdir(parents=True, exist_ok=True)
    source = REPO / "build/validation/root/boot"
    (root / "boot").mkdir(exist_ok=True)
    for filename in ("vmlinuz", "initrd.img"):
        shutil.copyfile(source / filename, root / "boot" / filename)
    entries = "entries:\n"
    for title in ("Linux", "Rescue"):
        entries += (f"  - title: {title}\n    kernel: /boot/vmlinuz\n"
                    "    initrd: /boot/initrd.img\n"
                    f"    cmdline: console=ttyS0,115200 panic=-1 arkboot.menu={title}\n")
    cases = [("timeout-default", "default: 1\ntimeout: 1\n" + entries, "Rescue", None, None),
             ("serial-select", "timeout: 1\n" + entries, "Rescue", "serial", None),
             ("serial-arrows", "timeout: 1\n" + entries, "Rescue", "arrows", None),
             ("keyboard-select", "timeout: 1\n" + entries, "Rescue", "keyboard", None),
             ("immediate", "default: 1\ntimeout: 0\n" + entries, "Rescue", None, None),
             ("wait", "timeout: -1\n" + entries, "Rescue", "keyboard", None),
             ("invalid", "default: 3\n" + entries, "", None, "default entry index is outside entries list"),
             ("iso-yml", "default: 1\ntimeout: 1\n" + entries, "Rescue", None, None)]
    for name, text, expected, action, error in cases:
        for config in (root / "boot").glob("arkboot.*"):
            config.unlink()
        optical = name == "iso-yml"
        config = root / ("arkboot.yml" if optical else "boot/arkboot.yaml")
        config.write_text(text)
        image = work / ("menu.iso" if optical else "menu.img")
        command = [sys.executable, "scripts/mkimage.py", "iso" if optical else "disk",
                   "--root", str(root), "--output", str(image)]
        if not optical:
            command += ["--fs", "fat12", "--size-mib", "8"]
        with (work / f"{name}-image.log").open("wb") as log:
            subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
        exercise(image, optical, name, expected, action, error)
        image.unlink()
    print(f"All {len(cases)} menu boot cases passed")


if __name__ == "__main__":
    main()
