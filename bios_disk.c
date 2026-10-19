#include "bios.h"
#include "disk.h"

bool disk_init(struct disk *d, u8 drive, u32 block_size) {
    u8 *info = (u8 *)0x68000;
    struct bios_regs r = { .eax = 0x4800, .edx = drive, .ds = 0x6800 };
    memset(info, 0, 64);
    wr16(info, 30);
    bios_call(0x13, &r);
    if (r.flags & 1) return error("BIOS EDD drive parameters unavailable");
    u16 reported = rd16(info + 24);
    if (reported != block_size || (reported != 512 && reported != 2048 && reported != 4096))
        return error("unsupported BIOS block size");
    *d = (struct disk){.drive = drive, .block_size = reported, .blocks = rd64(info + 16)};
    if ((!d->blocks || d->blocks > UINT64_MAX / reported) && reported == 2048) {
        /* Some BIOSes (including SeaBIOS) do not expose optical capacity in
           AH=48h. Both ISO and UDF bridge CDs carry an ISO volume size. Use
           that bounded size instead of assuming a fictitious disk capacity. */
        u8 descriptor[2048];
        d->blocks = UINT32_MAX;
        bool found = false;
        for (u32 block = 16; block < 80; block++) {
            if (!disk_read(d, (u64)block * 2048, descriptor, sizeof descriptor)) return false;
            if (memcmp(descriptor + 1, "CD001", 5) || descriptor[6] != 1 || descriptor[0] == 255) break;
            if (descriptor[0] == 1) {
                d->blocks = rd32(descriptor + 80);
                found = d->blocks >= block + 1;
                break;
            }
        }
        if (!found) return error("BIOS omitted CD capacity and no ISO volume size was found");
    }
    if (!d->blocks || d->blocks > UINT64_MAX / reported)
        return error("invalid BIOS disk capacity");
    return true;
}

bool disk_read(struct disk *d, u64 offset, void *buffer, u32 size) {
    u8 *out = buffer, *bounce = (u8 *)0x60000, *dap = (u8 *)0x68000;
    if (offset > d->blocks * d->block_size || size > d->blocks * d->block_size - offset)
        return error("read outside disk");
    while (size) {
        u64 lba = offset / d->block_size;
        u32 skip = offset % d->block_size;
        /* Up to 32 KiB per BIOS request; never cross a 64 KiB boundary. */
        u32 count = min32((size + (u64)skip + d->block_size - 1) / d->block_size,
                          32768 / d->block_size);
        bool ok = false;
        for (u32 attempt = 0; attempt < 3; attempt++) {
            memset(dap, 0, 16);
            dap[0] = 16; wr16(dap + 2, count); wr16(dap + 6, 0x6000);
            memcpy(dap + 8, &lba, 8);
            struct bios_regs r = { .eax = 0x4200, .edx = d->drive, .ds = 0x6800 };
            bios_call(0x13, &r);
            if (!(r.flags & 1)) { ok = true; break; }
            r = (struct bios_regs){ .edx = d->drive };
            bios_call(0x13, &r);
        }
        if (!ok) return error("BIOS disk read failed");
        u32 n = min32(size, count * d->block_size - skip);
        memcpy(out, bounce + skip, n);
        out += n; offset += n; size -= n;
    }
    return true;
}
