# BIOS Linux baseline

## Boot path

1. The disk bootstrap reads the core from an embedded LBA recorded at byte 8.
   CD tooling instead patches the El Torito boot-information table at bytes
   8–63; the core follows the first 2048 bytes of that boot image. Stage 1 keeps
   BIOS `DL`, verifies EDD, reads bounded blocks and retries failures.
2. `loader_entry.asm` enables A20, enters protected mode and clears BSS. C calls
   BIOS through a serialized bridge that saves the complete i386 C register
   state and uses a separate real-mode stack. Its interrupt number is patched
   into the bridge's `int` instruction, so that small section is intentionally
   writable and executable.
3. The core checks A20, collects E820, discovers the boot drive's partitions,
   probes filesystems and reads `arkboot.yaml`/`arkboot.yml` (or legacy
   `arkboot.cfg`). A bounded parser builds up to 16 boot entries. The text menu
   polls BIOS INT 16h and COM1 input; INT 1Ah ticks drive the optional countdown,
   including midnight wrap. Any key disables automatic boot. The selected
   entry supplies the kernel, initramfs and command line to the Linux loader.
4. The Linux loader validates the bzImage setup header, selects usable RAM,
   reads the protected-mode payload and optional initramfs, prepares a zeroed
   `boot_params`, then changes to Linux's required GDT selectors and jumps to
   the kernel's 32-bit entry with ESI pointing to the parameters.

## Memory ownership

| Physical range | Use |
| --- | --- |
| `0x00000..0x004ff` | BIOS IVT/BDA, retained |
| `0x00500..0x00503` | A20 probe, saved/restored before disk loading |
| Below `0x07000` | BIOS bridge stack, grows down |
| `0x07c00..0x07dff` | Disk/CD bootstrap |
| `0x08000..__bss_end` (at most `0x60000`) | Core, static state and boot parameters |
| `0x60000..0x67fff` | BIOS bounce buffer, at most 32 KiB per request |
| `0x68000..0x681ff` | EDD packet/parameters and E820 scratch |
| Below `0x78000` | Protected-mode stack, grows down; separated from BIOS buffers |
| At/above `0x100000` | Kernel/initramfs, selected from usable E820 ranges |

The linker rejects a bridge outside the first 64 KiB or a core overlapping the
BIOS buffer. INT 12h must report at least 512 KiB conventional RAM. The core has
no heap, and uses bounded stack/static buffers. No BIOS calls occur after the
kernel handoff. Filesystem metadata checks protect bounds but are not intended
as a complete hostile-media security boundary.

## References

* [Linux/x86 boot protocol](https://www.kernel.org/doc/html/latest/arch/x86/boot.html)
  — setup fields, placement, initramfs limit and 32-bit entry requirements.
* [Linux boot parameter zero page](https://docs.kernel.org/arch/x86/zero-page.html)
  — E820 and `boot_params` offsets.
* [ext4 on-disk format](https://docs.kernel.org/filesystems/ext4/index.html)
  — superblocks, inodes, directory records, indirect blocks and extent trees.
* [ECMA-167](https://ecma-international.org/publications-and-standards/standards/ecma-167/)
  and the [OSTA UDF specifications](https://www.osta.org/specs/) — UDF descriptors,
  partition maps, allocation and compressed Unicode names.
* [Microsoft FAT specification](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/fatgen103)
  — BPB, FAT type thresholds, chains and long directory names.
* [ECMA-119](https://ecma-international.org/publications-and-standards/standards/ecma-119/)
  — ISO9660 volume and directory records.
* [xorrisofs manual](https://www.gnu.org/software/xorriso/man_1_xorrisofs.html)
  — El Torito and the boot-information table.

This implementation uses the documented disk structures directly; it does not
link GRUB, Linux filesystem drivers, a C library or a resident operating system.
