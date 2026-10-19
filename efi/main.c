#include "arkboot.h"

EFI_SYSTEM_TABLE *efi_system;
EFI_BOOT_SERVICES *efi_boot;

void efi_check(EFI_STATUS status, const char *message) {
    if (!EFI_ERROR(status)) return;
    print("UEFI status "); print_hex((u32)status); print("\n");
    panic(message);
}
void *efi_alloc(UINTN size) {
    void *buffer;
    efi_check(efi_boot->AllocatePool(EfiLoaderData, size, &buffer), "UEFI allocation failed");
    return buffer;
}
/* GNU-EFI's startup relocates the image and calls this with the SysV ABI. */
EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system) {
    efi_system = system;
    efi_boot = system->BootServices;
    console_init();
    print("ARKBOOT UEFI x86-64\n");
    EFI_GUID loaded_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE *loaded;
    efi_check(efi_boot->HandleProtocol(image, &loaded_guid, (void **)&loaded), "missing loaded image protocol");
    struct disk disk;
    if (!efi_boot_disk(loaded->DeviceHandle, &disk)) panic(fs_error);
    struct partition candidates[32];
    u32 count = partitions(&disk, candidates, 32);
    if (!count) panic(fs_error ? fs_error : "invalid partition table");
    for (u32 i = 0; i < count; i++) {
        struct volume v;
        if (!fs_mount(&v, &candidates[i])) {
            if (fs_error) { print("Skipping volume: "); print(fs_error); print("\n"); }
            continue;
        }
        print("Found "); print(fs_name(&v)); print("\n");
        struct boot_entry entry;
        if (config_load(&v, &entry)) efi_linux(image, &v, &entry);
    }
    panic("no boot configuration found on boot drive");
}

/* GNU-EFI 4 names the C entry _entry; older releases call efi_main directly. */
EFI_STATUS _entry(EFI_HANDLE image, EFI_SYSTEM_TABLE *system) {
    return efi_main(image, system);
}
