#include "arkboot.h"

static EFI_GUID block_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
static EFI_GUID path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
static u8 *bounce;

/* Device paths come from firmware; bound traversal and reject malformed nodes. */
static u32 path_size(const EFI_DEVICE_PATH *path) {
    u32 size = 0;
    while (size <= 4092) {
        u32 length = rd16(path->Length);
        if (length < 4 || length > 4096 - size) return 0;
        size += length;
        if (path->Type == END_DEVICE_PATH_TYPE)
            return path->SubType == END_ENTIRE_DEVICE_PATH_SUBTYPE && length == 4 ? size : 0;
        path = (const void *)((const u8 *)path + length);
    }
    return 0;
}
bool efi_boot_disk(EFI_HANDLE device, struct disk *disk) {
    EFI_DEVICE_PATH *source;
    EFI_STATUS status = efi_boot->HandleProtocol(device, &path_guid, (void **)&source);
    if (EFI_ERROR(status)) return error("boot device has no UEFI device path");
    u32 source_size = path_size(source);
    if (!source_size) return error("invalid boot device path");
    EFI_HANDLE *handles;
    UINTN count;
    status = efi_boot->LocateHandleBuffer(ByProtocol, &block_guid, NULL, &count, &handles);
    if (EFI_ERROR(status)) return error("no UEFI block devices");
    EFI_BLOCK_IO *best = NULL;
    u32 longest = 0;
    for (UINTN i = 0; i < count; i++) {
        EFI_BLOCK_IO *io;
        EFI_DEVICE_PATH *path;
        if (EFI_ERROR(efi_boot->HandleProtocol(handles[i], &block_guid, (void **)&io)) ||
            !io->Media->MediaPresent || io->Media->LogicalPartition ||
            EFI_ERROR(efi_boot->HandleProtocol(handles[i], &path_guid, (void **)&path))) continue;
        u32 size = path_size(path);
        if (size <= 4 || size > source_size || size <= longest) continue;
        /* Select the physical parent of the loaded ESP/CD, never another drive. */
        if (!memcmp(path, source, size - 4)) { longest = size; best = io; }
    }
    efi_boot->FreePool(handles);
    if (!best) return error("cannot find the UEFI boot drive's physical block device");
    EFI_BLOCK_IO_MEDIA *m = best->Media;
    if (!power2(m->BlockSize) || m->BlockSize < 512 || m->BlockSize > 65536 ||
        m->LastBlock == UINT64_MAX || m->LastBlock + 1 > UINT64_MAX / m->BlockSize)
        return error("unsupported UEFI block size or disk capacity");
    u32 alignment = m->IoAlign > 1 ? m->IoAlign : 1;
    if (!power2(alignment) || alignment > 1024 * 1024) return error("unsupported UEFI I/O alignment");
    void *allocation = efi_alloc(65536 + alignment - 1);
    bounce = (void *)(((uintptr_t)allocation + alignment - 1) & ~((uintptr_t)alignment - 1));
    *disk = (struct disk){.block_size = m->BlockSize, .blocks = m->LastBlock + 1, .context = best};
    return true;
}
bool disk_read(struct disk *disk, u64 offset, void *buffer, u32 size) {
    EFI_BLOCK_IO *io = disk->context;
    if (offset > disk->blocks * disk->block_size || size > disk->blocks * disk->block_size - offset)
        return error("read outside disk");
    u8 *out = buffer;
    while (size) {
        u32 skip = offset % disk->block_size;
        u32 blocks = min32(((u64)size + skip + disk->block_size - 1) / disk->block_size,
                           65536 / disk->block_size);
        EFI_STATUS status = io->ReadBlocks(io, io->Media->MediaId, offset / disk->block_size,
                                           blocks * disk->block_size, bounce);
        if (EFI_ERROR(status)) return error("UEFI block read failed");
        u32 length = min32(size, blocks * disk->block_size - skip);
        memcpy(out, bounce + skip, length);
        out += length; offset += length; size -= length;
    }
    return true;
}
