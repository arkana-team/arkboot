#include "../fs.h"

/* ECMA-167 descriptors. Baseline: one physical (type 1) partition map.
   Short/long allocation descriptors, continuation extents, holes, FE/EFE and
   embedded files are supported. VAT, sparing and metadata maps are rejected. */
static u16 crc16(const u8 *data, u32 n) {
    u16 crc = 0;
    while (n--) {
        crc ^= (u16)*data++ << 8;
        for (u32 i = 0; i < 8; i++) crc = (crc << 1) ^ (crc & 0x8000 ? 0x1021 : 0);
    }
    return crc;
}
static bool descriptor(struct volume *v, u32 physical, u32 location, u8 *b, u16 tag) {
    if (!part_read(&v->part, (u64)physical * v->block_size, b, v->block_size)) return false;
    u8 sum = 0;
    for (u32 i = 0; i < 16; i++) if (i != 4) sum += b[i];
    u32 len = rd16(b + 10);
    if (rd16(b) != tag || (rd16(b + 2) != 2 && rd16(b + 2) != 3) || sum != b[4] ||
        rd32(b + 12) != location || len > v->block_size - 16 ||
        crc16(b + 16, len) != rd16(b + 8)) return error("invalid UDF descriptor");
    return true;
}
static bool partition_block(struct volume *v, u32 lbn, u32 *physical) {
    if (lbn >= v->u.udf.partition_length) return error("UDF block outside physical partition");
    *physical = v->u.udf.partition_start + lbn;
    return true;
}
static bool udf_inode(struct volume *v, u32 icb, struct node *out) {
    u8 b[4096]; u32 physical = 0;
    if (!partition_block(v, icb, &physical) ||
        !part_read(&v->part, (u64)physical * v->block_size, b, 2)) return false;
    u16 tag = rd16(b);
    if (tag != 261 && tag != 266) return error("unsupported UDF ICB type");
    if (!descriptor(v, physical, icb, b, tag)) return false;
    if (rd16(b + 20) != 4 || (b[27] != 4 && b[27] != 5))
        return error("unsupported UDF file type/ICB strategy");
    u32 fixed = tag == 261 ? 176 : 216;
    u32 attrs = rd32(b + fixed - 8), ads = rd32(b + fixed - 4), type = rd16(b + 34) & 7;
    if ((type != 0 && type != 1 && type != 3) || attrs > v->block_size - fixed ||
        ads > v->block_size - fixed - attrs ||
        (type == 0 && ads % 8) || (type == 1 && ads % 16)) return error("unsupported UDF allocation descriptors");
    *out = (struct node){ .id = icb, .size = rd64(b + 56), .directory = b[27] == 4 };
    wr32(out->meta, fixed + attrs); wr32(out->meta + 4, ads); wr32(out->meta + 8, type);
    if (type == 3 && out->size > ads) return error("short UDF embedded file");
    return true;
}
bool udf_mount(struct volume *v) {
    u8 b[4096];
    static const u32 sizes[] = {2048, 512, 1024, 4096};
    for (u32 candidate = 0; candidate < 4; candidate++) {
        u32 bs = sizes[candidate];
        if (v->part.size < 257u * bs) continue;
        if (!part_read(&v->part, (u64)256 * bs, b, 16)) return false;
        if (rd16(b) != 2) continue;
        v->block_size = bs;
        if (!descriptor(v, 256, 256, b, 2)) return false;
        u32 start = rd32(b + 20), length = rd32(b + 16);
        if (!length || length % bs || length / bs > 256 || start >= v->part.size / bs ||
            length > v->part.size - (u64)start * bs ||
            (u64)start + length / bs > 0x100000000ull) return error("invalid UDF descriptor sequence");
        u32 map_number = UINT32_MAX, fileset = 0;
        for (u32 i = 0; i < length / bs; i++) {
            if (!part_read(&v->part, (u64)(start + i) * bs, b, bs)) return false;
            if (rd16(b) == 8) break;
            if (rd16(b) != 6) continue;
            if (!descriptor(v, start + i, start + i, b, 6)) return false;
            if (rd32(b + 212) != bs || rd32(b + 264) != 6 || rd32(b + 268) != 1 ||
                b[440] != 1 || b[441] != 6 || rd16(b + 256) || (rd32(b + 248) >> 30))
                return error("UDF requires a single physical partition map");
            map_number = rd16(b + 444); fileset = rd32(b + 252);
            break;
        }
        if (map_number == UINT32_MAX) return error("missing UDF logical volume");
        bool found = false;
        for (u32 i = 0; i < length / bs; i++) {
            if (!part_read(&v->part, (u64)(start + i) * bs, b, bs)) return false;
            if (rd16(b) != 5 || rd16(b + 22) != map_number) continue;
            if (!descriptor(v, start + i, start + i, b, 5)) return false;
            u32 pos = rd32(b + 188), blocks = rd32(b + 192);
            if (!blocks || pos >= v->part.size / bs || blocks > v->part.size / bs - pos ||
                (u64)pos + blocks > 0x100000000ull)
                return error("invalid UDF partition geometry");
            v->u.udf.partition_start = pos; v->u.udf.partition_length = blocks;
            found = true; break;
        }
        if (!found) return error("missing UDF partition");
        u32 physical = 0;
        if (!partition_block(v, fileset, &physical) || !descriptor(v, physical, fileset, b, 256)) return false;
        if (rd16(b + 408) || (rd32(b + 400) >> 30)) return error("unsupported UDF root ICB");
        if (!udf_inode(v, rd32(b + 404), &v->root)) return false;
        if (!v->root.directory) return error("invalid UDF root directory");
        v->kind = FS_UDF;
        return true;
    }
    return false;
}
bool udf_read(struct volume *v, const struct node *f, u64 off, void *dst, u32 size) {
    u32 physical = 0;
    if (!partition_block(v, f->id, &physical)) return false;
    u32 type = rd32(f->meta + 8), ad_size = type == 0 ? 8 : 16;
    u64 base = (u64)physical * v->block_size + rd32(f->meta);
    u32 ads = rd32(f->meta + 4), cursor = 0, continuations = 0;
    if (type == 3) return part_read(&v->part, base + off, dst, size);
    u8 *out = dst;
    while (size) {
        u8 ad[16];
        if (cursor > ads || ad_size > ads - cursor) return error("short UDF extent list");
        if (!part_read(&v->part, base + cursor, ad, ad_size)) return false;
        cursor += ad_size;
        u32 raw = rd32(ad), kind = raw >> 30, len = raw & 0x3fffffff, lbn = rd32(ad + 4);
        if (!len || (type == 1 && rd16(ad + 8))) return error("invalid UDF extent/partition reference");
        if (kind == 3) {
            u8 b[4096];
            if (++continuations > 64 || !partition_block(v, lbn, &physical) ||
                !descriptor(v, physical, lbn, b, 258)) return error("invalid/cyclic UDF continuation");
            ads = rd32(b + 20); cursor = 0;
            if (ads > v->block_size - 24 || ads % ad_size) return error("invalid UDF continuation length");
            base = (u64)physical * v->block_size + 24;
            continue;
        }
        if (off >= len) { off -= len; continue; }
        u32 n = min32(size, len - off);
        if (kind == 1 || kind == 2) memset(out, 0, n);
        else {
            if (!partition_block(v, lbn, &physical) ||
                len > (u64)(v->u.udf.partition_length - lbn) * v->block_size)
                return error("UDF extent outside partition");
            if (!part_read(&v->part, (u64)physical * v->block_size + off, out, n)) return false;
        }
        size -= n; out += n; off = 0;
    }
    return true;
}
bool udf_lookup(struct volume *v, const struct node *dir, const char *name, struct node *out) {
    for (u64 off = 0; off + 38 <= dir->size;) {
        u8 h[38], encoded[255];
        if (!udf_read(v, dir, off, h, sizeof h)) return false;
        u32 imp = rd16(h + 36), len = h[19], record = (38 + imp + len + 3) & ~3u;
        u8 sum = 0;
        for (u32 i = 0; i < 16; i++) if (i != 4) sum += h[i];
        if (rd16(h) != 257 || sum != h[4] || record > dir->size - off)
            return error("invalid UDF file identifier");
        if (len && !(h[18] & (4 | 8))) {
            if (!udf_read(v, dir, off + 38 + imp, encoded, len)) return false;
            char decoded[256] = {0}; u32 n = 0, step = encoded[0] == 8 ? 1 : encoded[0] == 16 ? 2 : 0;
            if (!step || (len - 1) % step) return error("unsupported UDF filename encoding");
            for (u32 i = 1; i < len; i += step) {
                u32 cp = step == 1 ? encoded[i] : (u32)encoded[i] << 8 | encoded[i + 1];
                if (!name_utf8(decoded, &n, cp)) return error("unsupported UDF filename");
            }
            if (name_equal(decoded, name, false)) {
                if (rd16(h + 28) || (rd32(h + 20) >> 30)) return error("unsupported UDF file ICB");
                return udf_inode(v, rd32(h + 24), out);
            }
        }
        off += record;
    }
    return error("file not found");
}
