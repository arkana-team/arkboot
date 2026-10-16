# Arkboot

A BIOS Linux bootloader with a small disk/CD bootstrap and a freestanding
32-bit C core. It reads a configuration file, loads an x86 Linux `bzImage` and
optional initramfs, and enters Linux through its 32-bit boot protocol. Suitable
64-bit kernels use that entry point too; Arkboot does not enter long mode.

## Build

Required: GCC with i386 code generation, NASM, GNU binutils, Make and Python 3.
No libc or 32-bit userspace libraries are needed for the loader.

```sh
make -j2
```

This produces `boot.bin`, `cdboot.bin`, `loader.bin`, and the inspectable
`build/loader.elf` / `build/loader.map`. The core is loaded at physical `0x8000`;
its assembly BIOS bridge stays below 64 KiB. Code and BSS, the BIOS transfer
buffer, real-mode stack and protected-mode stack occupy separate memory areas.

## Boot tree and configuration

Create a directory such as `root/` with:

```text
root/
  boot/
    arkboot.cfg
    vmlinuz
    initrd.img
```

`boot/arkboot.cfg` (or `/arkboot.cfg`) uses literal `key=value` lines:

```ini
kernel=/boot/vmlinuz
initrd=/boot/initrd.img
cmdline=console=tty0 root=UUID=YOUR-ROOT-FILESYSTEM-UUID ro
```

The kernel, initramfs and config must be on the same filesystem on the BIOS boot
drive. Arkboot checks the whole drive and its partitions, then boots the first
volume with a configuration. It does not search other drives or discover Linux
installations automatically. Kernel command-line parameters, including `root=`,
are passed unchanged to Linux. Linux/initramfs provide the root filesystem drivers.

Paths are relative to the selected filesystem's root, so a separate `/boot`
partition may use `/vmlinuz` rather than `/boot/vmlinuz`. Spaces in values are
literal, and lines beginning with `#` are comments. Quotes and inline comments
are not interpreted. Unknown keys and excessive lengths are errors. Defaults are
`/boot/vmlinuz`, `/boot/initrd.img` and `console=tty0`; a missing default initramfs
is allowed. An explicit `initrd=` disables it; a nonempty explicit path must exist.
Only one initramfs file is loaded (concatenate microcode/initramfs archives first
if needed). Configuration is limited to 4095 bytes, paths to 511 bytes and the
command line to 2047 bytes or the kernel's smaller limit.

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
entry-array CRCs are checked. BIOS extended disk services (EDD) and E820 are
required; there is no CHS fallback or native SATA/NVMe/USB driver. When BIOS omits optical capacity, an ISO primary volume descriptor is required
(including on UDF bridge CDs). Initial disk
boot currently assumes 512-byte BIOS blocks, and CD boot 2048-byte blocks.

The Linux loader requires boot protocol 2.10 or newer, loads below 4 GiB, checks
RAM availability/alignment and initialization size, reserves the kernel's whole
initialization region, places the initramfs below the kernel's limit, and passes
E820, text screen information and the command line. This is a baseline boot path,
without UEFI, menus, graphics, encryption, RAID/LVM or signature verification.

## Validation

`make check` compares production filesystem/partition readers against files in
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

See [docs/design.md](docs/design.md) for implementation references and layout,
and [docs/validation.md](docs/validation.md) for the executed validation matrix.
