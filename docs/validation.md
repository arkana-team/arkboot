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
