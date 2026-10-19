# Arkboot

A Linux bootloader for BIOS and **x86-64 UEFI**, with shared YAML boot menus,
partition parsing and read-only filesystem drivers. The BIOS frontend uses a
32-bit freestanding C core and Linux's 32-bit boot protocol. The UEFI frontend
is a 64-bit EFI application and launches Linux through its EFI stub.

## Build

Required: GCC with i386/x86-64 code generation, NASM, GNU binutils, Make,
Python 3 and **GNU-EFI** headers/startup/relocation library (for example the
`gnu-efi` package). No libc or 32-bit userspace libraries are needed.
`EFI_INCLUDE` and `EFI_LIB` can override `/usr/include/efi` and `/usr/lib` when
GNU-EFI is installed elsewhere.

```sh
make -j2
```

This builds both firmware paths: `BOOTX64.EFI`, `boot.bin`, `cdboot.bin` and
`loader.bin`. Use `make uefi` or `make bios` to build just one frontend; the BIOS
build does not need GNU-EFI. Inspectable UEFI output is in
`build/uefi/arkboot.so` / `build/uefi/arkboot.map`; BIOS output is in
`build/loader.elf` / `build/loader.map`. The BIOS core is loaded at physical `0x8000`;
its assembly BIOS bridge stays below 64 KiB. Code and BSS, the BIOS transfer
buffer, real-mode stack and protected-mode stack occupy separate memory areas.

## Boot tree and configuration

Create a directory such as `root/` with:

```text
root/
  boot/
    arkboot.yaml
    vmlinuz
    initrd.img
```

Use `boot/arkboot.yaml` for a menu:

```yaml
default: 0
timeout: 5
entries:
  - title: Linux
    kernel: /boot/vmlinuz
    initrd: /boot/initrd.img
    cmdline: console=tty0 root=UUID=YOUR-ROOT-FILESYSTEM-UUID ro
  - title: Linux rescue
    kernel: /boot/vmlinuz
    initrd: /boot/initrd.img
    cmdline: console=tty0 root=UUID=YOUR-ROOT-FILESYSTEM-UUID ro single
```

Select with **Up/Down** (or **W/S**) and press **Enter** to boot. Home/End select
the first/last entry. BIOS uses VGA and COM1 at 115200 baud; UEFI uses the
firmware's text console and keyboard, including serial when configured by the
firmware. BIOS serial terminals support W/S, Enter and ANSI arrow keys.
Any key cancels the countdown.
`default` is a zero-based entry index (default `0`). `timeout` defaults to five
seconds; `0` boots immediately and `-1` waits indefinitely. Positive timeouts
are limited to 3600 seconds.

Arkboot searches each volume for `/boot/arkboot.yaml`, `/boot/arkboot.yml`,
`/arkboot.yaml`, `/arkboot.yml`, `/boot/arkboot.cfg`, then `/arkboot.cfg`, in that
order. It boots from the first volume containing a configuration. Invalid
configuration stops boot with an error instead of falling back to another file.
The kernel, initramfs and config must be on the same filesystem on the BIOS boot
drive (the physical parent of the loaded EFI partition/CD under UEFI).
Arkboot does not search other drives or discover Linux installations
automatically. Linux/initramfs provide the root filesystem drivers.

YAML uses a bounded block format: `entries` is a sequence with **two spaces**
before `- title:` and **four spaces** before subsequent fields. Up to 16 entries
are supported. Each requires a printable ASCII `title` (up to 63 characters) and
a nonempty `kernel` path. `initrd` is optional; omitting it or setting it to `""`
disables initramfs loading. A nonempty path must exist. `cmdline` defaults to
`console=tty0` and is passed to Linux unchanged.

Values may be plain, single quoted (use `''` for a literal quote), or double
quoted (only `\\` and `\"` escapes are supported). `#` starts a comment outside
quotes when preceded by a space. Quote literal values containing `: ` or YAML
special characters. Tabs, multiline scalars, flow collections, anchors, aliases,
tags, document markers and nested structures beyond the entry list are not
supported. Unknown or duplicate keys and excessive lengths are errors.
See [examples/arkboot.yaml](examples/arkboot.yaml) for a complete example.

Paths are relative to the selected filesystem's root, so a separate `/boot`
partition may use `/vmlinuz` rather than `/boot/vmlinuz`. Only one initramfs file
is loaded (concatenate microcode/initramfs archives first if needed).
Configuration is limited to 16383 bytes, paths to 511 bytes and each command line
to 2047 bytes or the kernel's smaller limit.

Existing `arkboot.cfg` files boot directly without a menu and retain their
`key=value` format and defaults:

```ini
kernel=/boot/vmlinuz
initrd=/boot/initrd.img
cmdline=console=tty0 root=UUID=YOUR-ROOT-FILESYSTEM-UUID ro
```

In `.cfg`, quotes and inline comments are literal; lines beginning with `#` are
comments. Defaults are `/boot/vmlinuz`, `/boot/initrd.img` and `console=tty0`;
a missing default initramfs is allowed. An explicit `initrd=` disables it;
a nonempty explicit path must exist.

## Generate images

The tools create **regular image files only**. They do not install to physical
disks. Disk images reserve space for the core before the filesystem partition;
GPT images have a dedicated BIOS boot partition. Existing output images are
replaced after successful creation. Boot trees must contain regular files and
directories, with no symlinks.

FAT images require `mkfs.fat` and `mcopy` (dosfstools and mtools); ext images
require `mkfs.ext2`, `mkfs.ext3` or `mkfs.ext4` (e2fsprogs).

```sh
python3 scripts/mkimage.py disk --root root --fs fat32 --output disk.img
python3 scripts/mkimage.py disk --root root --fs ext4 --partition-table gpt --output gpt.img
qemu-system-x86_64 -m 128 -drive format=raw,file=disk.img -serial stdio
```

Supported `--fs` values: `fat12`, `fat16`, `fat32`, `ext2`, `ext3`, `ext4`.
Use an appropriately sized image for FAT: for example `--size-mib 8` for FAT12,
32 for FAT16, and 96 or larger for FAT32. `--size-mib` includes the reserved area.
The default is 128 MiB. `make disk.img ROOT=root FS=ext4` is a convenience target.

ISO9660 CDs require `xorriso`. UDF CDs require **cdrtools `mkisofs`**; xorriso does
not create UDF. Both use El Torito no-emulation boot images and the same core.

```sh
python3 scripts/mkimage.py iso --root root --output arkboot.iso
python3 scripts/mkimage.py iso --root root --udf --output udf.iso
qemu-system-x86_64 -m 128 -cdrom arkboot.iso -boot order=d -serial stdio
```

`--mkisofs /path/to/mkisofs` can select a non-system cdrtools binary. UDF bridge
images are mounted through the UDF driver, rather than their ISO9660 fallback tree.

## x86-64 UEFI images

UEFI requires an **x86-64 Linux kernel built with `CONFIG_EFI_STUB=y`**.
Initramfs loading also requires support for Linux's Load File 2 provider protocol;
this path is validated with Linux 6.12. The same kernel can also boot through BIOS. Arkboot loads it with UEFI image services,
passes the entry's command line as UTF-16 load options and supplies its initramfs
through Linux's `EFI_LOAD_FILE2` protocol. The kernel's EFI stub performs the
final memory-map and `ExitBootServices` handoff. IA32 UEFI is unsupported.

Add `--uefi` to generate an image that boots through either BIOS or UEFI:

```sh
python3 scripts/mkimage.py disk --root root --fs ext4 --partition-table gpt --uefi --output disk.img
python3 scripts/mkimage.py iso --root root --uefi --output arkboot.iso
python3 scripts/mkimage.py iso --root root --udf --uefi --output udf.iso
```

Disk images gain a **64 MiB FAT32 EFI System Partition** containing
`/EFI/BOOT/BOOTX64.EFI`; the chosen data filesystem holds the config, kernel and
initramfs. `--size-mib` includes the ESP: allow at least 68 MiB total, plus enough
space for boot files and filesystem metadata. GPT retains its BIOS boot
partition; MBR images use a type-`0xef` ESP. CD images include an additional EFI
El Torito entry backed by an 8 MiB FAT16 image. The EFI partition contains only
Arkboot, so filesystem support is provided by the same drivers as BIOS.

For example, with OVMF installed (adjust paths for your distribution):

```sh
cp /usr/share/edk2/x64/OVMF_VARS.4m.fd build/OVMF_VARS.fd
qemu-system-x86_64 -machine q35 -m 256 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
  -drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
  -drive format=raw,file=disk.img -serial stdio
```

`make disk.img UEFI=1` and `make arkboot.iso UEFI=1` are convenience targets.

`BOOTX64.EFI` can also be copied to an existing ESP at `/EFI/BOOT/BOOTX64.EFI`.
Arkboot searches the filesystems on that same physical drive for configuration.
UEFI uses firmware Block I/O and allocated memory; it does not invoke BIOS
interrupts or use the BIOS core's fixed physical addresses. Builds are unsigned;
Secure Boot trust and signing are not implemented.

## Filesystem baseline

All drivers are read-only. Unsupported features cause an error, and files must be
regular files reached through directories; symlinks are not followed.

| Filesystem | Implemented baseline | Limits |
| --- | --- | --- |
| FAT12/16/32 | Fixed and clustered roots, subdirectories, fragmented chains, active FAT selection, short and long names | ASCII case folding; BMP UTF-8 long names; no surrogate pairs |
| ext2/3/4 | 1–64 KiB blocks, variable inode/descriptor sizes, 64-bit block addresses, direct/indirect blocks, extent trees, sparse files | Clean filesystems only; no journal replay, `meta_bg`, bigalloc, inline data, encryption, casefold or verity files; metadata checksums are not verified |
| ISO9660 | Primary volume descriptor, directories, contiguous files, case-insensitive names and `;version` stripping | Uses primary ISO names, not Rock Ridge or Joliet; no multi-extent/interleaved files |
| UDF | One physical type-1 partition map; FE/EFE; short/long/embedded allocation, continuation extents, holes; CS0 8/16-bit names | No VAT, sparable/metadata maps, indirect ICBs, symlinks or surrogate pairs; anchor at block 256 |

On disk, MBR primary/logical partitions and GPT are supported. GPT header and
entry-array CRCs are checked. For BIOS, extended disk services (EDD) and E820 are
required; there is no CHS fallback or native SATA/NVMe/USB driver. When BIOS omits optical capacity, an ISO primary volume descriptor is required
(including on UDF bridge CDs). Initial disk
boot currently assumes 512-byte BIOS blocks, and CD boot 2048-byte blocks.

The BIOS Linux loader requires boot protocol 2.10 or newer, loads below 4 GiB, checks
RAM availability/alignment and initialization size, reserves the kernel's whole
initialization region, places the initramfs below the kernel's limit, and passes
E820, text screen information and the command line. This is a baseline boot path,
with a text boot menu, without a graphical menu, encryption, RAID/LVM or signature verification.

## Validation

`make check` checks configuration parsing with sanitizers and compares production filesystem/partition readers against files in
images made by normal image tools. It uses bounded buffers and tests unaligned,
sequential and random reads, fragmented FAT files, sparse files, missing files,
file bounds, damaged metadata rejection, unknown BIOS CD capacity, and MBR/GPT images.
It needs all filesystem image tools, including cdrtools for UDF:

```sh
MKISOFS=/path/to/mkisofs make check
```

Compile the pinned minimal Linux 6.12 kernel and a static, libc-free PID 1:

```sh
scripts/build-test-linux.sh
MKISOFS=/path/to/mkisofs python3 tests/boot.py
```

The source and build outputs stay on disk under ignored `build/validation/`,
not in RAM-backed `/tmp`. The kernel build requires 1.5 GiB available RAM before
starting, pins the C dialect to GNU C11 for newer compilers, permits at most two jobs (`JOBS=1` for less parallelism), and bounds
individual build processes to 1 GiB of address space. QEMU tests run sequentially
with 128 MiB assigned per guest, a host-memory preflight, and per-guest deadlines.
Serial/image logs are retained in `build/boot-tests/`; successful images are
removed. A pass requires the kernel handoff message **and** `ARKBOOT_USERSPACE_OK` from PID 1,
using the expected filesystem driver. Use `--case ext4-gpt` to run one case.

With the test kernel and initramfs available, exercise YAML menus, countdowns,
serial selection, BIOS keyboard selection and invalid configuration rejection:

```sh
python3 tests/menu.py
```

These cases run one 128 MiB QEMU guest at a time and retain logs under
`build/menu-tests/`. Successful boot cases require the selected entry's command
line and `ARKBOOT_USERSPACE_OK`; they cover FAT12 and ISO9660 menu paths.

The test kernel config enables EFI stub support and disables IA32 mixed mode
and the deprecated EFI handover protocol. With OVMF installed, run:

```sh
MKISOFS=/path/to/mkisofs python3 tests/uefi.py
```

`--ovmf-code` and `--ovmf-vars` override firmware discovery; `--case ext4-gpt`
runs one case. UEFI tests run sequentially with 256 MiB guest RAM and a host
memory preflight. They cover all supported filesystems, MBR/GPT, keyboard and
serial selection, countdowns, invalid configuration, and IA32 kernel rejection.
Logs are retained under `build/uefi-tests/`. To check BIOS boot from images
containing both firmware entries:

```sh
MKISOFS=/path/to/mkisofs python3 tests/boot.py --dual-firmware
```

These BIOS logs are retained under `build/boot-tests-dual/`.

See [docs/design.md](docs/design.md) for implementation references and layout,
and [docs/validation.md](docs/validation.md) for the executed validation matrix.
