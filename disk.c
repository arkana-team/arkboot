#include "disk.h"

const char *fs_error;
bool error(const char *s) { fs_error = s; return false; }

bool part_read(const struct partition *p, u64 offset, void *buffer, u32 size) {
    if (offset > p->size || size > p->size - offset) return error("read outside partition");
    return disk_read(p->disk, p->base + offset, buffer, size);
}

static u32 crc32(const u8 *p, u32 len) {
    u32 crc = ~0u;
    while (len--) {
        crc ^= *p++;
        for (u32 i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
    }
    return ~crc;
}
static bool add_partition(struct disk *d, struct partition *out, u32 *n, u32 cap,
                          u64 start, u64 blocks) {
    if (!blocks || start >= d->blocks || blocks > d->blocks - start)
        return error("invalid partition bounds");
    if (*n == cap) return error("too many partitions");
    out[(*n)++] = (struct partition){ d, start * d->block_size, blocks * d->block_size };
    return true;
}
u32 partitions(struct disk *d, struct partition *out, u32 cap) {
    u8 sector[512];
    u32 n = 0;
    if (!cap || !add_partition(d, out, &n, cap, 0, d->blocks) ||
        !disk_read(d, 0, sector, sizeof sector)) return 0;
    if (rd16(sector + 510) != 0xaa55) return n;
    bool gpt = false;
    for (u32 i = 0; i < 4; i++) if (sector[446 + i * 16 + 4] == 0xee) gpt = true;
    if (gpt) {
        u8 h[512], entry[128];
        if (!disk_read(d, d->block_size, h, sizeof h) || memcmp(h, "EFI PART", 8)) {
            error("invalid GPT header"); return 0;
        }
        u32 hs = rd32(h + 12), expected = rd32(h + 16);
        wr32(h + 16, 0);
        if (hs < 92 || hs > sizeof h || crc32(h, hs) != expected) {
            error("invalid GPT header checksum"); return 0;
        }
        u64 table = rd64(h + 72), first = rd64(h + 40), last = rd64(h + 48);
        u32 count = rd32(h + 80), esize = rd32(h + 84);
        if (count > 4096 || esize < 128 || esize > 4096 ||
            !power2(esize) || table >= d->blocks ||
            (u64)count * esize > (d->blocks - table) * d->block_size ||
            first > last || last >= d->blocks) { error("invalid GPT layout"); return 0; }
        /* Stream the entire entry array, including unused entries, for its CRC. */
        u32 crc = ~0u;
        for (u64 pos = 0; pos < (u64)count * esize;) {
            u32 len = min32(sizeof sector, (u64)count * esize - pos);
            if (!disk_read(d, table * d->block_size + pos, sector, len)) return 0;
            for (u32 j = 0; j < len; j++) {
                crc ^= sector[j];
                for (u32 k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
            }
            pos += len;
        }
        if (~crc != rd32(h + 88)) { error("invalid GPT partition checksum"); return 0; }
        for (u32 i = 0; i < count; i++) {
            if (!disk_read(d, table * d->block_size + (u64)i * esize, entry, sizeof entry)) return 0;
            u8 used = 0;
            for (u32 j = 0; j < 16; j++) used |= entry[j];
            if (!used) continue;
            u64 start = rd64(entry + 32), end = rd64(entry + 40);
            if (start < first || end > last || start > end ||
                !add_partition(d, out, &n, cap, start, end - start + 1)) return 0;
        }
        return n;
    }
    for (u32 i = 0; i < 4; i++) {
        u8 *e = sector + 446 + i * 16;
        u8 type = e[4];
        u64 start = rd32(e + 8), length = rd32(e + 12);
        if (!type) continue;
        if (type != 5 && type != 0x0f && type != 0x85) {
            if (!add_partition(d, out, &n, cap, start, length)) return 0;
            continue;
        }
        if (!length || start >= d->blocks || length > d->blocks - start) return 0;
        u64 ebr = start;
        for (u32 links = 0; ; links++) {
            u8 b[512];
            if (links >= 128 || ebr < start || ebr >= start + length ||
                !disk_read(d, ebr * d->block_size, b, sizeof b) || rd16(b + 510) != 0xaa55)
                return 0;
            u32 relative = rd32(b + 454), blocks = rd32(b + 458);
            if (b[450] && (relative >= start + length - ebr ||
                blocks > start + length - ebr - relative ||
                !add_partition(d, out, &n, cap, ebr + relative, blocks))) return 0;
            if (!b[466]) break;
            ebr = start + rd32(b + 470);
        }
    }
    return n;
}
