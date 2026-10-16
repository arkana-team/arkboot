#include "../fs.h"

static u8 fat_cache[4096];
static const struct volume *cache_volume;
static u64 cache_offset = UINT64_MAX;

static bool chain_size(struct volume *v, u32 start, u64 *size);

bool fat_mount(struct volume *v) {
    u8 b[512];
    if (v->part.size < 512) return false;
    if (!part_read(&v->part, 0, b, sizeof b) || rd16(b + 510) != 0xaa55 ||
        (b[0] != 0xeb && b[0] != 0xe9)) return false;
    u32 sector = rd16(b + 11), spc = b[13], reserved = rd16(b + 14), fats = b[16];
    u32 roots = rd16(b + 17), total = rd16(b + 19), fat_size = rd16(b + 22);
    if (!total) total = rd32(b + 32);
    if (!fat_size) fat_size = rd32(b + 36);
    if (!power2(sector) || sector < 512 || sector > 4096 || !power2(spc) || spc > 128 ||
        !reserved || !fats || fats > 2 || !fat_size || !total) return false;
    u32 root_sectors = (roots * 32 + sector - 1) / sector;
    u64 data_sector = (u64)reserved + (u64)fats * fat_size + root_sectors;
    if (data_sector >= total || (u64)total * sector > v->part.size) return error("invalid FAT layout");
    u32 clusters = (total - data_sector) / spc;
    u32 bits = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    if ((u64)(clusters + 2) * bits > (u64)fat_size * sector * 8 || clusters >= 0x0ffffff5)
        return error("FAT table too small");
    if (bits == 32 && (roots || rd16(b + 22) || rd16(b + 42)))
        return error("unsupported FAT32 layout/version");
    if (bits != 32 && !roots) return error("missing FAT root directory");
    u32 active = 0;
    if (bits == 32 && (rd16(b + 40) & 0x80)) active = b[40] & 15;
    if (active >= fats) return error("invalid active FAT");
    v->block_size = sector;
    v->u.fat.bits = bits;
    v->u.fat.cluster_size = sector * spc;
    v->u.fat.clusters = clusters;
    v->u.fat.fat_size = fat_size;
    v->u.fat.root_entries = roots;
    v->u.fat.root_cluster = bits == 32 ? rd32(b + 44) & 0x0fffffff : 0;
    v->u.fat.fat = ((u64)reserved + (u64)active * fat_size) * sector;
    v->u.fat.root = ((u64)reserved + (u64)fats * fat_size) * sector;
    v->u.fat.data = data_sector * sector;
    v->root = (struct node){ .id = v->u.fat.root_cluster, .directory = true,
        .size = bits == 32 ? (u64)clusters * v->u.fat.cluster_size : roots * 32 };
    if (bits == 32 && (v->root.id < 2 || v->root.id >= clusters + 2))
        return error("invalid FAT32 root cluster");
    cache_offset = UINT64_MAX; cache_volume = NULL;
    if (bits == 32 && !chain_size(v, v->root.id, &v->root.size)) return false;
    v->kind = FS_FAT;
    return true;
}
static bool fat_byte(struct volume *v, u32 offset, u8 *byte) {
    u64 sector = v->u.fat.fat + (offset / v->block_size) * (u64)v->block_size;
    if (cache_volume != v || cache_offset != sector) {
        if (!part_read(&v->part, sector, fat_cache, v->block_size)) return false;
        cache_volume = v; cache_offset = sector;
    }
    *byte = fat_cache[offset % v->block_size];
    return true;
}
static bool next_cluster(struct volume *v, u32 cluster, u32 *next) {
    if (cluster < 2 || cluster >= v->u.fat.clusters + 2) return error("invalid FAT cluster");
    u32 bits = v->u.fat.bits, offset = bits == 12 ? cluster + cluster / 2 : cluster * (bits / 8);
    u8 bytes[4] = {0};
    for (u32 i = 0; i < (bits == 32 ? 4u : 2u); i++)
        if (!fat_byte(v, offset + i, &bytes[i])) return false;
    u32 n = rd32(bytes);
    if (bits == 12) n = (cluster & 1 ? n >> 4 : n) & 0xfff;
    if (bits == 16) n &= 0xffff;
    if (bits == 32) n &= 0x0fffffff;
    u32 end = bits == 12 ? 0xff8 : bits == 16 ? 0xfff8 : 0x0ffffff8;
    if (n >= end) n = 0;
    else if (n < 2 || n >= v->u.fat.clusters + 2) return error("bad FAT cluster chain");
    *next = n;
    return true;
}
static bool chain_size(struct volume *v, u32 start, u64 *size) {
    u32 cluster = start;
    *size = 0;
    for (u32 i = 0; i < v->u.fat.clusters; i++) {
        if (!next_cluster(v, cluster, &cluster)) return false;
        *size += v->u.fat.cluster_size;
        if (!cluster) return true;
    }
    return error("cyclic FAT directory chain");
}
bool fat_read(struct volume *v, const struct node *f, u64 off, void *dst, u32 size) {
    if (!f->id && f->directory && v->u.fat.bits != 32)
        return part_read(&v->part, v->u.fat.root + off, dst, size);
    u32 cluster = f->id, cluster_size = v->u.fat.cluster_size, budget = v->u.fat.clusters;
    u64 skip = off / cluster_size;
    if (skip >= budget) return error("FAT offset exceeds cluster count");
    while (skip--) {
        if (!next_cluster(v, cluster, &cluster) || !cluster) return error("short FAT cluster chain");
        budget--;
    }
    u32 within = off % cluster_size;
    u8 *out = dst;
    while (size) {
        if (!budget || cluster < 2 || cluster >= v->u.fat.clusters + 2)
            return error("invalid/cyclic FAT cluster chain");
        u32 run = 1, last = cluster;
        u32 wanted = (size + (u64)within + cluster_size - 1) / cluster_size;
        u32 maximum = min32(wanted, cluster_size < 32768 ? 32768 / cluster_size : 1);
        while (run < maximum && run < budget) {
            u32 next;
            if (!next_cluster(v, last, &next)) return false;
            if (next != last + 1) break;
            last = next; run++;
        }
        budget -= run;
        u32 n = min32(size, run * cluster_size - within);
        if (!part_read(&v->part, v->u.fat.data + (u64)(cluster - 2) * cluster_size + within, out, n))
            return false;
        size -= n; out += n; within = 0;
        if (size && (!next_cluster(v, last, &cluster) || !cluster))
            return error("short FAT cluster chain");
    }
    return true;
}
static u8 short_checksum(const u8 *s) {
    u8 sum = 0;
    for (u32 i = 0; i < 11; i++) sum = ((sum & 1) << 7) + (sum >> 1) + s[i];
    return sum;
}
bool fat_lookup(struct volume *v, const struct node *dir, const char *name, struct node *out) {
    u8 e[32];
    u16 long_name[260];
    long_name[0] = 0xffff;
    u32 expected = 0;
    u8 checksum = 0;
    for (u64 offset = 0; offset + 32 <= dir->size; offset += 32) {
        if (!fat_read(v, dir, offset, e, sizeof e)) return false;
        if (!e[0]) break;
        if (e[0] == 0xe5) { expected = 0; long_name[0] = 0xffff; continue; }
        if (e[11] == 0x0f) {
            u32 seq = e[0] & 31;
            if ((e[0] & 0x40) && seq && seq <= 20) {
                for (u32 i = 0; i < 260; i++) long_name[i] = 0xffff;
                expected = seq; checksum = e[13];
            }
            if (!seq || seq != expected || seq > 20 || checksum != e[13] || e[12] || rd16(e + 26)) {
                expected = 0; long_name[0] = 0xffff; continue;
            }
            static const u8 positions[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
            for (u32 i = 0; i < 13; i++) long_name[(seq - 1) * 13 + i] = rd16(e + positions[i]);
            expected--;
            continue;
        }
        char filename[FS_NAME_MAX + 1] = {0};
        u32 len = 0;
        bool valid_long = !expected && checksum == short_checksum(e) && long_name[0] != 0xffff;
        /* long_name is only consulted after a complete, checksummed LFN chain. */
        if (valid_long) {
            for (u32 i = 0; i < 260 && long_name[i] && long_name[i] != 0xffff; i++) {
                if (!name_utf8(filename, &len, long_name[i])) { valid_long = false; break; }
            }
        }
        bool match = valid_long && name_equal(filename, name, true);
        if (!match) {
            len = 0;
            for (u32 i = 0; i < 8 && e[i] != ' '; i++) filename[len++] = e[i] == 5 ? 0xe5 : e[i];
            if (e[8] != ' ') {
                filename[len++] = '.';
                for (u32 i = 8; i < 11 && e[i] != ' '; i++) filename[len++] = e[i];
            }
            filename[len] = 0;
            match = name_equal(filename, name, true);
        }
        expected = 0; checksum = 0; long_name[0] = 0xffff;
        if ((e[11] & 8) || !match) continue;
        u32 cluster = rd16(e + 26);
        if (v->u.fat.bits == 32) cluster |= (u32)(rd16(e + 20) & 0x0fff) << 16;
        *out = (struct node){ .id = cluster, .directory = !!(e[11] & 0x10), .size = rd32(e + 28) };
        if (out->directory && !chain_size(v, cluster, &out->size)) return false;
        return true;
    }
    return error("file not found");
}
