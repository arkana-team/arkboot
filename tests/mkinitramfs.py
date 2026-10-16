#!/usr/bin/env python3
"""Make a deterministic newc initramfs without root privileges or libc."""
import argparse
import pathlib
import stat
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    init = args.output.parent / "init"
    subprocess.run(["gcc", "-m64", "-Os", "-nostdlib", "-static", "-no-pie",
                    "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
                    "-Wl,--build-id=none", str(pathlib.Path(__file__).with_name("init.c")),
                    "-o", str(init)], check=True)
    archive = bytearray()

    def entry(name, mode, data=b"", major=0, minor=0):
        fields = (1, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(name) + 1, 0)
        archive.extend(b"070701" + b"".join(f"{v:08x}".encode() for v in fields))
        archive.extend(name.encode() + b"\0")
        archive.extend(b"\0" * (-len(archive) % 4))
        archive.extend(data)
        archive.extend(b"\0" * (-len(archive) % 4))

    entry("dev", stat.S_IFDIR | 0o755)
    entry("dev/console", stat.S_IFCHR | 0o600, major=5, minor=1)
    entry("init", stat.S_IFREG | 0o755, init.read_bytes())
    entry("TRAILER!!!", 0)
    args.output.write_bytes(archive)
    print(f"Built static /init and {args.output} ({len(archive)} bytes)")


if __name__ == "__main__":
    main()
