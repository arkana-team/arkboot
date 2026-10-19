# Baseline validation

Validated on 2026-10-16 using QEMU's `pc` machine, TCG and `qemu64` CPU.
The test kernel was compiled from Linux 6.12 with `tests/linux.config` using
`scripts/build-test-linux.sh`. The static PID 1 was compiled from `tests/init.c`
and packaged into a 9624-byte initramfs. No host root filesystem or userspace
was used inside the guest.

Executed successfully:

```sh
make -j2
scripts/build-test-linux.sh
MKISOFS="$PWD/build/validation/cdrtools/usr/bin/mkisofs" make check
MKISOFS="$PWD/build/validation/cdrtools/usr/bin/mkisofs" python3 tests/boot.py
```

For this run cdrtools was extracted locally, without installing a system package.
Use an installed `mkisofs` or set `MKISOFS` to another cdrtools executable.

| Boot medium | Result |
| --- | --- |
| MBR disk: FAT12, FAT16, FAT32 | All reached Linux and PID 1 |
| MBR disk: ext2, ext3, ext4 | All reached Linux and PID 1 |
| GPT disk: FAT32, ext4 | Both reached Linux and PID 1 |
| El Torito CD: ISO9660 | Reached Linux and PID 1 using ISO9660 |
| El Torito CD: UDF bridge | Reached Linux and PID 1 using UDF |

Each of the ten cases printed `ARKBOOT_USERSPACE_OK` from the compiled `/init`
running as PID 1. Serial logs are retained locally in ignored
`build/boot-tests/*-serial.log`; the final aggregate is
`build/boot-tests/results.log`. Filesystem checks also passed for fragmented FAT
allocation, sparse files, sequential/unaligned/random reads, long names, missing
files, file bounds, unknown BIOS optical capacity, and rejection of dirty ext
journals and damaged GPT/UDF metadata. Their log is `build/check.log`.

The kernel build used two jobs with a 1 GiB per-process virtual-memory limit.
QEMU ran one guest at a time with 128 MiB assigned; available host RAM was
checked throughout. Source, build output and image staging stayed on disk,
including compiler temporary files after the C-dialect compatibility adjustment.

An additional smoke test reached the same PID 1 with the host's existing kernel
from FAT32, ISO9660 and UDF. The acceptance matrix above used the newly compiled
minimal Linux 6.12 kernel. Its SHA-256 was:

```text
714a25936dd58be5829e612f3c881733c347c5794ad42db53279badc5ca6d619
```

These results establish the documented baseline on QEMU. Physical firmware and
all optional filesystem features have not been exhaustively tested; see the
[filesystem limits](../README.md#filesystem-baseline).

## YAML boot menus (2026-10-19)

Built with `make -j2` and validated using the existing Linux 6.12 test kernel
and static PID 1. `make check` passed configuration parsing with AddressSanitizer
and UndefinedBehaviorSanitizer, plus the filesystem/partition image checks.
The configuration harness additionally passed after the final legacy-tab
compatibility and YAML-tab rejection changes.

`python3 tests/menu.py` passed all eight cases:

| Menu case | Result |
| --- | --- |
| Countdown with a nonzero default entry | Selected Rescue and reached PID 1 |
| Serial W/S selection | Canceled countdown, selected Rescue and reached PID 1 |
| Serial ANSI arrow selection | Canceled countdown, selected Rescue and reached PID 1 |
| BIOS keyboard Down/Enter | Canceled countdown, selected Rescue and reached PID 1 |
| Zero timeout | Booted default entry immediately and reached PID 1 |
| Indefinite wait | Accepted BIOS keyboard selection and reached PID 1 |
| Default index outside entry list | Rejected configuration before Linux handoff |
| Root-level `.yml` on ISO9660 CD | Booted default entry and reached PID 1 |

Disk menu cases used FAT12. Successful cases checked the chosen entry's Linux
command line and `ARKBOOT_USERSPACE_OK`. Input cases waited beyond the original
timeout before pressing Enter to check cancellation. Logs are retained in
`build/menu-tests/`. The complete ten-case legacy `.cfg` matrix also passed via
`tests/boot.py`, covering all filesystems and both disk partition tables listed
above. Each guest ran sequentially with 128 MiB RAM and a host memory preflight.
Physical hardware remains untested.

## x86-64 UEFI (2026-10-19)

Built both frontends with `make -B -j2`, using GNU-EFI 4.0.4 for its x86-64
headers, startup and relocation library. `BOOTX64.EFI` is a PE32+ AMD64 EFI
application. The final rebuild produced the same EFI executable as the tested
build. There is no IA32 frontend.

Rebuilt Linux 6.12 with `scripts/build-test-linux.sh`, enabling ACPI, EFI and the
EFI stub while disabling mixed-mode EFI and the deprecated handover protocol.
The test kernel was 1254400 bytes, with SHA-256:

```text
f7306b970f0aa84b01b4a86633dbbb79d2d489c8a9dd2c3a72ed81366d7f4ee5
```

The 14 UEFI cases passed across matrix and focused runs of `tests/uefi.py`.
OVMF used `/usr/share/edk2/x64/OVMF_CODE.4m.fd` and a fresh copy of the matching
variable store for each guest, with Secure Boot disabled. Guests used QEMU
`q35,accel=tcg`, `qemu64` and 256 MiB RAM, running sequentially with a host
memory preflight and bounded deadlines/output.

| UEFI case | Result |
| --- | --- |
| MBR: FAT12, FAT16, FAT32 | All reached Linux PID 1 |
| MBR: ext2, ext3, ext4 | All reached Linux PID 1 |
| GPT: FAT32, ext4 | Both reached Linux PID 1 |
| EFI El Torito CD: ISO9660, UDF | Both reached Linux PID 1 using the expected driver |
| Serial W/S/Enter selection | Canceled countdown, selected second entry and reached PID 1 |
| Firmware keyboard Down/Enter selection | Canceled countdown, selected second entry and reached PID 1 |
| Invalid default index | Rejected configuration before Linux handoff |
| IA32 PE kernel image | Rejected architecture before Linux handoff |

Successful cases required `ARKBOOT_USERSPACE_OK` and the selected entry's
command-line marker. All successful logs additionally confirmed preservation of
`arkboot.unicode=café` and Linux's message that it loaded the initramfs from the
`LINUX_EFI_INITRD_MEDIA_GUID` device path. This exercises Arkboot's Load File 2
provider with kernels/initramfs on the data filesystem, separate from the ESP.
The ext4 GPT case exercised timed default selection; input cases waited beyond
the original countdown before pressing Enter. Logs are in
`build/uefi-tests/*-serial.log` and `*-image.log`.

CD validation caught and corrected FAT16 cluster sizing for the small EFI boot
image and the differing EFI boot-entry flags in xorriso/cdrtools. Final ISO9660
and UDF images were tested with the portable explicit-platform boot options.
`make check` also passed its sanitizer-backed configuration parsing and
filesystem/partition checks after separating BIOS I/O from shared code.

BIOS regression validation also passed:

```sh
MKISOFS="$PWD/build/validation/cdrtools/usr/bin/mkisofs" python3 tests/boot.py --dual-firmware
python3 tests/menu.py
```

All ten combined-image BIOS cases reached Linux PID 1, covering FAT12/16/32,
ext2/3/4, MBR/GPT and both ISO9660/UDF CDs. Their serial logs are retained in
`build/boot-tests-dual/`. All eight original BIOS menu cases passed, including
countdowns, serial/keyboard selection, indefinite wait and invalid configuration
rejection; logs are in `build/menu-tests/`.

These results cover QEMU/OVMF with unsigned images. Physical UEFI firmware and
Secure Boot trust/signing remain untested/unimplemented respectively.

## Installation guide and media writer (2026-10-19)

`python3 tests/write_media.py` passed 12 focused checks using temporary files
and simulated block-device metadata. These cover image/media type mismatch,
whole-device and sector-size requirements, mounted devices and swap, active
RAID/LVM/encryption holders, read-only/undersized disks, missing sysfs metadata,
confirmation and terminal requirements, changed devices/images, failure handling,
and verification before GPT relocation. The helper defaults to preview.

An additional integration check generated real Arkboot FAT12 MBR, ext4 GPT/UEFI
and ISO9660/UEFI images with `scripts/mkimage.py`; the helper recognized each,
rejected the wrong media kind, and its CLI refused regular files as destinations.
Generation logs are retained under `build/write-media-tests/integration/`. Python
compilation and CLI help checks passed. `make check` now includes the focused
writer checks. No physical disk was written or optical disc burned; the writer's
physical I/O, GPT relocation and optical media handling remain hardware-untested.
