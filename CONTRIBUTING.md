# Contributing to Arkboot

Thanks for your interest. This repository builds a BIOS/UEFI Linux bootloader
from a small C core plus two thin firmware frontends. Production code stays
bounded and dependency-free: no libc in the loaded core, explicit buffer
limits, and no heap.

## What a change needs

- **Keep the shared core shared.** BIOS and UEFI code live in this repo;
  filesystem, GPT/MBR, configuration parsing and menu logic are common code
  and should not fork between frontends.
- **Add host-side tests for shared-core logic.** The host harnesses in `tests/`
  (config, FAT, ext, UDF, image staging) compile the production sources with
  sanitizers and run without firmware. If a bug is fixed in shared logic, the
  regression test belongs with the harness.
- **Run the checks.** `make check` needs the image tools (see the README and
  `docs/validation.md`); the fast subset that runs anywhere is:

  ```sh
  gcc -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
      -Wall -Wextra -Werror tests/config_host.c config.c -o /tmp/config_host
  /tmp/config_host

  python3 tests/image_builder.py
  ```

  For the full firmware validation (QEMU/OVMF, cdrtools) see
  `docs/validation.md`.
- **Do not weaken validation to make a change pass.** If a check needs to
  change, explain why in the PR (a different on-disk contract, a documented
  new/unsupported feature, etc.).
- Keep examples, README and `docs/` consistent with behavior changes.

## Coding conventions

- C11, `-Wall -Wextra -Werror`; 32-bit freestanding for the BIOS core
  (`-m32`), 64-bit MS ABI for UEFI (GNU-EFI). Bounds-check every untrusted
  length and offset; filesystem metadata is parsed, checked and rejected on
  unsupported or ambiguous input.
- Keep relocations and generated artifacts (`build/`, images, ISO, `*.bin`)
  out of the repository.
- Documentation follows the validation docs: report measured results, not
  hopeful ones.

## Pull requests

Open PRs against `main`. Describe the problem, the intended effect, and how
you validated it. Images and large generated files do not belong in diffs.
