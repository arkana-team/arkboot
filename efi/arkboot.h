#ifndef ARKBOOT_EFI_H
#define ARKBOOT_EFI_H
#include <efi.h>
#include "../config.h"

#if !defined(__x86_64__)
#error Arkboot UEFI requires x86-64
#endif

extern EFI_SYSTEM_TABLE *efi_system;
extern EFI_BOOT_SERVICES *efi_boot;
extern EFI_EVENT efi_timer;
void efi_check(EFI_STATUS status, const char *message);
void *efi_alloc(UINTN size);
bool efi_boot_disk(EFI_HANDLE device, struct disk *disk);
_Noreturn void efi_linux(EFI_HANDLE parent, struct volume *v, const struct boot_entry *entry);
#endif
