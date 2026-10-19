#include "arkboot.h"

static void *initrd_data;
static UINTN initrd_size;
static EFI_GUID path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static EFI_GUID load_guid = EFI_LOAD_FILE2_PROTOCOL_GUID;
static struct {
    VENDOR_DEVICE_PATH vendor;
    EFI_DEVICE_PATH end;
} initrd_path = {
    .vendor = {
        .Header = {MEDIA_DEVICE_PATH, MEDIA_VENDOR_DP, {sizeof(VENDOR_DEVICE_PATH), 0}},
        .Guid = {0x5568e427, 0x68fc, 0x4f3d, {0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68}}
    },
    .end = {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {4, 0}}
};
_Static_assert(sizeof initrd_path == 24, "Linux initrd device path layout changed");

static EFI_STATUS EFIAPI load_initrd(EFI_LOAD_FILE2_PROTOCOL *self, EFI_DEVICE_PATH *path,
                                     BOOLEAN boot_policy, UINTN *size, void *buffer) {
    (void)self;
    if (!path || !size) return EFI_INVALID_PARAMETER;
    if (boot_policy) return EFI_UNSUPPORTED;
    if (path->Type != END_DEVICE_PATH_TYPE || path->SubType != END_ENTIRE_DEVICE_PATH_SUBTYPE ||
        rd16(path->Length) != 4) return EFI_NOT_FOUND;
    if (!buffer || *size < initrd_size) { *size = initrd_size; return EFI_BUFFER_TOO_SMALL; }
    memcpy(buffer, initrd_data, initrd_size);
    *size = initrd_size;
    return EFI_SUCCESS;
}
static EFI_LOAD_FILE2_PROTOCOL initrd_protocol = {.LoadFile = load_initrd};

static void *read_file(struct volume *v, const struct node *file) {
    if (file->directory || !file->size || file->size > UINT32_MAX) panic("invalid kernel or initramfs file");
    void *data = efi_alloc(file->size);
    if (!fs_read(v, file, 0, data, file->size)) panic(fs_error);
    return data;
}
static void command_line(CHAR16 *out, const char *input) {
    /* LoadOptions is UTF-16. Reject malformed UTF-8 rather than changing it. */
    const u8 *s = (const u8 *)input;
    while (*s) {
        u32 cp = *s++, extra = 0, minimum = 0;
        if (cp >= 0xc2 && cp <= 0xdf) { cp &= 31; extra = 1; minimum = 0x80; }
        else if (cp >= 0xe0 && cp <= 0xef) { cp &= 15; extra = 2; minimum = 0x800; }
        else if (cp >= 0xf0 && cp <= 0xf4) { cp &= 7; extra = 3; minimum = 0x10000; }
        else if (cp >= 0x80) panic("invalid UTF-8 in kernel command line");
        for (u32 i = 0; i < extra; i++) {
            if ((*s & 0xc0) != 0x80) panic("invalid UTF-8 in kernel command line");
            cp = (cp << 6) | (*s++ & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            panic("invalid UTF-8 in kernel command line");
        if (cp > 0xffff) { cp -= 0x10000; *out++ = 0xd800 | (cp >> 10); *out++ = 0xdc00 | (cp & 1023); }
        else *out++ = cp;
    }
    *out = 0;
}
_Noreturn void efi_linux(EFI_HANDLE parent, struct volume *v, const struct boot_entry *entry) {
    struct node kernel, initrd;
    if (!fs_open(v, entry->kernel, &kernel)) panic(fs_error);
    u8 *data = read_file(v, &kernel);
    /* UEFI LoadImage validates the PE image; give a useful architecture error first. */
    if (kernel.size < 64 || rd16(data) != 0x5a4d) panic("requires an x86-64 Linux kernel with CONFIG_EFI_STUB=y");
    u32 pe = rd32(data + 0x3c);
    if (pe > kernel.size - 26 || rd32(data + pe) != 0x4550 ||
        rd16(data + pe + 4) != 0x8664 || rd16(data + pe + 24) != 0x20b)
        panic("requires an x86-64 Linux kernel with CONFIG_EFI_STUB=y");
    EFI_HANDLE child;
    print("Loading Linux EFI image\n");
    efi_check(efi_boot->LoadImage(FALSE, parent, NULL, data, kernel.size, &child), "cannot load Linux EFI image");
    efi_boot->FreePool(data);
    EFI_GUID loaded_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE *loaded;
    efi_check(efi_boot->HandleProtocol(child, &loaded_guid, (void **)&loaded), "missing Linux loaded image protocol");
    static CHAR16 options[2048];
    command_line(options, entry->cmdline);
    UINTN length = 0;
    while (options[length]) length++;
    loaded->LoadOptions = options;
    loaded->LoadOptionsSize = (length + 1) * sizeof options[0];
    EFI_HANDLE initrd_handle = NULL;
    if (entry->initrd[0]) {
        if (!fs_open(v, entry->initrd, &initrd)) {
            if (!fs_error || entry->initrd_explicit || strcmp(fs_error, "file not found")) panic(fs_error);
        } else {
            initrd_data = read_file(v, &initrd);
            initrd_size = initrd.size;
            efi_check(efi_boot->InstallProtocolInterface(&initrd_handle, &path_guid, EFI_NATIVE_INTERFACE,
                       &initrd_path), "cannot install Linux initrd device path");
            efi_check(efi_boot->InstallProtocolInterface(&initrd_handle, &load_guid, EFI_NATIVE_INTERFACE,
                       &initrd_protocol), "cannot install Linux initrd provider");
        }
    }
    /* The EFI stub owns GetMemoryMap/ExitBootServices and the final Linux handoff. */
    efi_boot->CloseEvent(efi_timer);
    print("Starting Linux\n");
    EFI_STATUS status = efi_boot->StartImage(child, NULL, NULL);
    if (initrd_handle) {
        efi_boot->UninstallProtocolInterface(initrd_handle, &load_guid, &initrd_protocol);
        efi_boot->UninstallProtocolInterface(initrd_handle, &path_guid, &initrd_path);
        efi_boot->FreePool(initrd_data);
    }
    efi_boot->UnloadImage(child);
    efi_check(status, "Linux EFI image failed");
    panic("Linux EFI image returned unexpectedly");
}
