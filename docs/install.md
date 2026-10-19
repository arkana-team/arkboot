# Install Arkboot on a CD or HDD

This guide covers BIOS and x86-64 UEFI. Commands run from the repository root on
Linux. Choose the method that matches your goal:

| Goal | Method | Effect on existing data |
| --- | --- | --- |
| Boot from CD-R or CD-RW | Generate an ISO and burn it as a disc image | Uses a blank optical disc |
| Make a dedicated HDD, SSD or USB boot disk | Generate a disk image and write it to the whole device | Replaces the target's partition table and existing partitions |
| Add Arkboot to a disk that already contains an OS | Copy the UEFI application to its existing EFI System Partition | Preserves partitions; adds files and an optional firmware boot entry |

**The whole-disk method destroys the existing layout.** Use the existing-ESP
method when preserving an installed OS. There is currently no BIOS installer
that embeds Arkboot into an existing partition layout while preserving it.

## 1. Prepare the boot files

Build both firmware versions:

```sh
make -j2
mkdir -p root/boot
cp /path/to/vmlinuz root/boot/vmlinuz
cp /path/to/initrd.img root/boot/initrd.img
cp examples/arkboot.yaml root/boot/arkboot.yaml
```

Replace the source paths with the actual kernel and its matching initramfs.
Use regular files: the image builder rejects symlinks. UEFI needs an x86-64
EFI-stub kernel; initramfs loading uses Linux's Load File 2 protocol, validated
with Linux 6.12. Build dependencies are in the [README](../README.md#build).

Edit `root/boot/arkboot.yaml`, especially the `cmdline` values. For an existing
Linux root filesystem, replace `YOUR-ROOT-FILESYSTEM-UUID` with its actual UUID
from `lsblk --fs`. The initramfs must support that root filesystem and its storage
hardware. For a self-contained live CD, supply an initramfs containing the live
userspace and `/init` instead, with the command line it needs.

**Copying a kernel and initramfs does not install an operating system.** A disk
image built from these boot files contains only those files. For the whole-disk
method, an existing OS root must survive on another device; do not point `root=`
at a filesystem on the disk you are about to replace. The existing-ESP method
can boot an OS already on that same disk.

Kernel, initramfs and configuration must be on one filesystem on the boot disk.
Linux's root filesystem can be elsewhere. See the
[configuration reference](../README.md#boot-tree-and-configuration) for paths,
menu defaults and supported YAML syntax.

## 2. Burn a CD

Create an ISO with BIOS and x86-64 UEFI boot entries:

```sh
python3 scripts/mkimage.py iso --root root --uefi --output arkboot.iso
```

This needs `xorriso`, plus `mkfs.fat`, `mmd` and `mcopy` for the EFI boot image.
Omit `--uefi` for a BIOS-only CD. For UDF, use cdrtools `mkisofs`:

```sh
python3 scripts/mkimage.py iso --root root --udf --uefi --output arkboot-udf.iso
```

Insert a blank CD-R or an already-blanked CD-RW. Identify the writer, for example
`/dev/sr0`, with:

```sh
lsblk -o PATH,TYPE,SIZE,MODEL,MOUNTPOINTS
xorriso -devices
```

Unmount it if your desktop mounted a disc there. Preview the burn, then perform
it; replace `/dev/sr0` with your actual optical drive:

```sh
python3 scripts/write-media.py cd arkboot.iso /dev/sr0 --dry-run
sudo python3 scripts/write-media.py cd arkboot.iso /dev/sr0 --write
```

The helper prints the image and drive details and requires you to type the full
resolved device path. It checks for a blank medium, burns one finalized data
session with xorriso, and ejects the disc. `--speed 8` requests 8x writing; choose
a speed supported by your drive/media. The helper does not erase CD-RW media.

To burn manually to a confirmed blank disc:

```sh
sudo xorriso -no_rc -as cdrecord -v dev=/dev/sr0 -sao -eject -data arkboot.iso
```

**Burn the ISO as a disc image.** Copying `arkboot.iso`, `boot.bin` or
`BOOTX64.EFI` onto an ordinary data CD does not install the El Torito boot
catalog. On another OS, use the optical application's “burn disc image” option.
The ISO must fit the medium; a CD is typically around 700 MB.

Reinsert the disc and optionally compare exactly the ISO's byte count on Linux:

```sh
sudo cmp --bytes="$(stat -c %s arkboot.iso)" arkboot.iso /dev/sr0
```

No output and exit status zero means those bytes match. Then select the optical
drive in the machine's firmware boot menu. Physical burning and firmware
compatibility have not been tested in this repository's QEMU validation.

## 3. Write a dedicated HDD, SSD or USB disk

Generate a complete disk image. GPT with `--uefi` supports both BIOS and
x86-64 UEFI:

```sh
python3 scripts/mkimage.py disk --root root --fs ext4 --partition-table gpt \
  --uefi --size-mib 1024 --output arkboot.img
```

Choose enough image space for all boot files. The EFI partition uses 64 MiB;
`--size-mib` includes it. FAT12/16/32 and ext2/3/4 data filesystems are available.
MBR is supported with `--partition-table mbr`. The image builder only creates
regular files; the writer is the separate step that modifies a physical disk.

Identify the target by model, serial and size:

```sh
lsblk -o PATH,TYPE,SIZE,MODEL,SERIAL,LOG-SEC,FSTYPE,MOUNTPOINTS
ls -l /dev/disk/by-id/
```

Use the **whole disk**, such as `/dev/sdb` or `/dev/nvme1n1`, not `/dev/sdb1` or
`/dev/nvme1n1p1`. A `/dev/disk/by-id/...` link is also accepted; the helper shows
and asks you to type its resolved path. Back up the target's data first. Unmount
all its partitions and disable any swap/RAID/LVM/encryption users before writing.
For a system disk, boot a separate live Linux environment first.

Dependencies for writing are `lsblk`, `wipefs` and `blockdev` (util-linux), GNU
`dd` and `cmp`, and **`sgdisk` (GPT fdisk)** for GPT images. These images use
512-byte logical sectors; the helper rejects native 4 KiB logical-sector disks.
512e disks with 512-byte logical sectors are accepted.

Preview, inspect the target details, then write:

```sh
python3 scripts/write-media.py disk arkboot.img /dev/sdb --dry-run
sudo python3 scripts/write-media.py disk arkboot.img /dev/sdb --write
```

There is no unattended `--yes` mode. `--write` requires root, an interactive
terminal, and the exact target path. The helper refuses partitions, mounted or
active devices, read-only disks and disks smaller than the image. It rechecks
the device after confirmation and holds an exclusive block-device open during
writing to reject mounted devices, including mounts outside the current
namespace. Keep automounters and other disk tools stopped during installation.

After confirmation it:

1. Removes old signatures on the whole target with `wipefs`, including obsolete
   GPT signatures that may otherwise survive at the disk's far end.
2. Writes the full disk image from byte zero and flushes it.
3. Compares the written image bytes against the source.
4. For GPT, moves the backup GPT to the physical end of the target with `sgdisk`.
5. Requests that Linux reread the partition table.

The GPT relocation changes metadata after the byte comparison. If the target is
larger than the image, extra space remains unallocated; partitions/filesystems
are not grown automatically. This is not a secure erase: old bytes outside the
new image may remain recoverable. A failure or interruption can leave an
incomplete disk; the helper reports failure and does not resume automatically.

Inspect the finished layout with `lsblk`. Select the target disk in the firmware
boot menu. For UEFI, the image contains `/EFI/BOOT/BOOTX64.EFI` in its FAT32 ESP;
for BIOS, the MBR/bootstrap and embedded core are already placed correctly.

**Use `.img` for a disk and `.iso` for optical media.** Arkboot's El Torito ISOs
are not hybrid USB/HDD disk images. Do not write an ISO to an HDD using this
procedure. Do not write `boot.bin` or `loader.bin` directly onto a disk: they are
individual stages, and the image builder must patch their placement and preserve
space for the loader.

## 4. Add UEFI Arkboot to an existing HDD

This method preserves the disk's partitions. Work from Linux booted in UEFI mode
if you want to register a firmware boot entry. Identify the existing FAT EFI
System Partition using `lsblk --fs` and the partition table. In this example it
is `/dev/nvme0n1p1`; **replace it with your actual ESP**:

```sh
make uefi
sudo mkdir -p /mnt/arkboot-esp
sudo mount /dev/nvme0n1p1 /mnt/arkboot-esp
sudo mkdir -p /mnt/arkboot-esp/EFI/Arkboot
sudo cp -i BOOTX64.EFI /mnt/arkboot-esp/EFI/Arkboot/BOOTX64.EFI
sudo umount /mnt/arkboot-esp
```

Check for an existing `EFI/Arkboot/BOOTX64.EFI` first and back it up if present.
If the ESP is already mounted, use its existing mount point rather than mounting
it again. This procedure does not format it or replace another loader's files.

Put your kernel, initramfs and `arkboot.yaml` on a supported filesystem on that
**same physical disk**. Paths in YAML are relative to that filesystem's root.
For a dedicated `/boot` partition containing `vmlinuz` and `initrd.img` at its
root, put `arkboot.yaml` there and use:

```yaml
timeout: 5
entries:
  - title: Linux
    kernel: /vmlinuz
    initrd: /initrd.img
    cmdline: console=tty0 root=UUID=YOUR-ROOT-FILESYSTEM-UUID ro
```

Use real filenames or regular copies, not distribution-managed symlinks. Replace
the root UUID and paths with your installation's values. Cleanly unmount any
filesystem edited from a live environment before booting Arkboot; it does not
replay ext journals. Each filesystem is searched in turn, and the first one
with an Arkboot configuration is used.

Register the EFI application using `efibootmgr` (example disk and ESP number):

```sh
sudo efibootmgr --create --disk /dev/nvme0n1 --part 1 \
  --label Arkboot --loader '\EFI\Arkboot\BOOTX64.EFI'
sudo efibootmgr --verbose
```

This adds a firmware entry and may change the boot order. You can select that
entry from the firmware boot menu or adjust the order there. Firmware that
supports “boot from file” can also start `EFI/Arkboot/BOOTX64.EFI` directly.

For a dedicated removable ESP, the fallback location is
`/EFI/BOOT/BOOTX64.EFI`. Check and back up any existing file before replacing it;
that path may already belong to another loader. A unique `EFI/Arkboot/` directory
and firmware entry avoid that collision on an existing OS disk.

Arkboot builds are unsigned, so disable Secure Boot to use them. Signing/trust
setup and IA32 UEFI are not implemented.

## Validation and references

The writer's argument handling, image checks, device refusal rules and planned
commands are covered by `python3 tests/write_media.py`, using temporary regular
files and simulated device metadata. It does not write to real block devices or
burn discs during those checks. Boot images themselves are validated in QEMU;
see [validation results](validation.md). Physical installation remains untested.

* [GNU xorriso / xorrecord manual](https://www.gnu.org/software/xorriso/man_1_xorrecord.html)
  documents writing an ISO as a data track and disc finalization.
* [GPT fdisk sgdisk documentation](https://www.rodsbooks.com/gdisk/sgdisk.html)
  documents moving backup GPT structures to the end of the target disk.
* [util-linux wipefs manual](https://github.com/util-linux/util-linux/blob/master/misc-utils/wipefs.8.adoc)
  describes signature removal; it does not securely erase file data.
* [efibootmgr upstream documentation](https://github.com/rhboot/efibootmgr)
  describes registering an EFI application and its effect on the boot order.
