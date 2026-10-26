#include "../fs.h"

/* On-disk fields follow Documentation/filesystems/ext4. No journal replay. */
bool ext_mount(struct volume *v) {
    u8 s[1024];
    if (v->part.size < 2048) return false;
    if (!part_read(&v->part, 1024, s, sizeof s) || rd16(s + 56) != 0xef53) return false;
    u32 incompat = rd32(s + 96), ro = rd32(s + 100);
    /* filetype, extents, 64bit, flex_bg, checksum_seed, large_dir */
    if (incompat & ~0x62c2u) return error("unsupported ext incompat feature or dirty journal");
    /* sparse_super, large_file, btree_dir, huge_file, gdt_csum, dir_nlink,
       extra_isize, quota, metadata_csum, read-only and project quota */
    if (ro & ~0x357fu) return error("unsupported ext read-only feature (e.g. bigalloc)");
    if (!(rd16(s + 58) & 1)) return error("ext filesystem is not clean");
    u32 log = rd32(s + 24);
    if (log > 6) return error("unsupported ext block size");
    u32 block = 1024u << log;
    u64 blocks = rd32(s + 4);
    if (incompat & 0x80) blocks |= (u64)rd32(s + 336) << 32;
    u32 first = rd32(s + 20), per_group = rd32(s + 32), ipg = rd32(s + 40);
    u32 inode_size = rd32(s + 76) ? rd16(s + 88) : 128;
    u32 desc_size = incompat & 0x80 ? rd16(s + 254) : 32;
    if (!blocks || blocks > v->part.size / block || first >= blocks || !per_group || !ipg ||
        !power2(inode_size) || inode_size < 128 || inode_size > block ||
        desc_size < 32 || desc_size > block || desc_size % 8 ||
        ((incompat & 0x80) && desc_size < 64)) return error("invalid ext geometry");
    u64 groups = (blocks - first + per_group - 1) / per_group;
    if (groups > UINT32_MAX) return error("too many ext block groups");
    v->block_size = block;
    v->u.ext.inodes = rd32(s);
    v->u.ext.inodes_per_group = ipg;
    v->u.ext.inode_size = inode_size;
    v->u.ext.desc_size = desc_size;
    v->u.ext.groups = groups;
    v->u.ext.blocks = blocks;
    v->u.ext.descriptors = (u64)(first + 1) * block;
    v->kind = FS_EXT;
    if (!ext_lookup(v, NULL, NULL, &v->root)) return false;
    return v->root.directory || error("invalid ext root directory");
}
static bool inode_read(struct volume *v, u32 ino, struct node *out) {
    u8 desc[64];
    if (!ino || ino > v->u.ext.inodes) return error("invalid ext inode number");
    u32 group = (ino - 1) / v->u.ext.inodes_per_group;
    if (group >= v->u.ext.groups) return error("invalid ext inode group");
    if (!part_read(&v->part, v->u.ext.descriptors + (u64)group * v->u.ext.desc_size,
                   desc, min32(sizeof desc, v->u.ext.desc_size))) return false;
    u64 table = rd32(desc + 8);
    if (v->u.ext.desc_size >= 64) table |= (u64)rd32(desc + 40) << 32;
    if (!table || table >= v->u.ext.blocks) return error("invalid ext inode table");
    memset(out, 0, sizeof *out);
    out->id = ino;
    if (!part_read(&v->part, table * v->block_size +
                   (u64)((ino - 1) % v->u.ext.inodes_per_group) * v->u.ext.inode_size,
                   out->meta, min32(sizeof out->meta, v->u.ext.inode_size))) return false;
    u32 type = rd16(out->meta) & 0xf000;
    if (type != 0x4000 && type != 0x8000) return error("ext symlinks and special files are unsupported");
    if (rd32(out->meta + 32) & (0x800u | 0x10000000u | 0x100000u))
        return error("encrypted, inline or verity ext inode is unsupported");
    out->directory = type == 0x4000;
    out->size = rd32(out->meta + 4);
    if (!out->directory) out->size |= (u64)rd32(out->meta + 108) << 32;
    return true;
}
/* Shared by all mappings in one read or directory lookup. Lookups also charge
   records and data sectors to bound discovery work independently of block size. */
static bool ext_work(u32 *budget, u32 cost) {
    if (*budget < cost) return error("ext work budget exceeded");
    *budget -= cost;
    return true;
}
static bool ext_block(struct volume *v, const struct node *f, u32 logical, u64 *physical,
                      u32 *run, u32 *budget) {
    u32 bs = v->block_size;
    *run = 1;
    if (rd32(f->meta + 32) & 0x80000) {
        u8 header[12], entry[12];
        const u8 *root = f->meta + 40;
        u64 base = 0;
        u32 capacity = 60, previous_depth = 6;
        for (u32 level = 0; level < 6; level++) {
            if (!ext_work(budget, 1)) return false;
            if (!level) memcpy(header, root, 12);
            else if (!part_read(&v->part, base, header, 12)) return false;
            u32 count = rd16(header + 2), max = rd16(header + 4), depth = rd16(header + 6);
            if (rd16(header) != 0xf30a || count > max || max > (capacity - 12) / 12 ||
                depth >= previous_depth || (level && depth + 1 != previous_depth))
                return error("invalid ext extent tree");
            previous_depth = depth;
            bool found = false;
            u32 last_start = 0;
            for (u32 i = 0; i < count; i++) {
                if (!ext_work(budget, 1)) return false;
                if (!level) memcpy(entry, root + 12 + i * 12, 12);
                else if (!part_read(&v->part, base + 12 + i * 12, entry, 12)) return false;
                u32 start = rd32(entry);
                if (i && start <= last_start) return error("unordered ext extents");
                last_start = start;
                if (start > logical) break;
                if (depth) { *physical = (u64)rd16(entry + 8) << 32 | rd32(entry + 4); found = true; }
                else {
                    u32 length = rd16(entry + 4);
                    bool unwritten = length > 32768;
                    if (unwritten) length -= 32768;
                    if (!length) return error("empty ext extent");
                    if (logical - start < length) {
                        *physical = unwritten ? 0 : ((u64)rd16(entry + 6) << 32 | rd32(entry + 8)) + logical - start;
                        *run = length - (logical - start);
                        return true;
                    }
                }
            }
            if (!depth || !found) { *physical = 0; return true; }
            if (!*physical || *physical >= v->u.ext.blocks) return error("invalid ext extent index");
            base = *physical * bs; capacity = bs;
        }
        return error("ext extent tree too deep");
    }
    u32 per = bs / 4, depth = 0;
    u64 index = logical, span = per;
    u32 pointer;
    if (index < 12) { *physical = rd32(f->meta + 40 + index * 4); return true; }
    index -= 12;
    for (depth = 1; depth <= 3; depth++) {
        if (index < span) break;
        index -= span; span *= per;
    }
    if (depth > 3) return error("ext logical block exceeds indirect tree");
    pointer = rd32(f->meta + 40 + (11 + depth) * 4);
    while (depth--) {
        if (!pointer) { *physical = 0; return true; }
        if (pointer >= v->u.ext.blocks) return error("invalid ext indirect pointer");
        span /= per;
        u32 slot = index / span;
        u8 word[4];
        if (!ext_work(budget, 1)) return false;
        if (!part_read(&v->part, (u64)pointer * bs + slot * 4, word, 4)) return false;
        pointer = rd32(word); index %= span;
    }
    *physical = pointer;
    return true;
}
static bool ext_data(struct volume *v, u64 physical, u32 within, void *dst, u32 size) {
    if (!physical) { memset(dst, 0, size); return true; }
    if (physical >= v->u.ext.blocks || size > (v->u.ext.blocks - physical) * v->block_size - within)
        return error("ext data outside filesystem");
    return part_read(&v->part, physical * v->block_size + within, dst, size);
}
bool ext_read(struct volume *v, const struct node *f, u64 off, void *dst, u32 size) {
    u8 *out = dst;
    u32 budget = 65536;
    while (size) {
        u64 logical = off / v->block_size, physical = 0;
        if (logical > UINT32_MAX) return error("ext logical offset exceeds 32 bits");
        u32 run, within = off % v->block_size;
        if (!ext_block(v, f, logical, &physical, &run, &budget)) return false;
        u32 n = min32(size, min32(run, UINT32_MAX / v->block_size) * v->block_size - within);
        if (!ext_data(v, physical, within, out, n)) return false;
        out += n; off += n; size -= n;
    }
    return true;
}
bool ext_lookup(struct volume *v, const struct node *dir, const char *name, struct node *out) {
    if (!dir) return inode_read(v, 2, out);
    /* Filesystem reads are serialized. Keep the largest supported block off
       the BIOS stack; mapping state and budget are fresh for each lookup. */
    static u8 block[65536];
    u32 budget = 65536, run = 0;
    u64 physical = 0;
    for (u64 off = 0; off + 8 <= dir->size;) {
        if (!ext_work(&budget, v->block_size / 512)) return false;
        if (!run) {
            u64 logical = off / v->block_size;
            if (logical > UINT32_MAX) return error("ext logical offset exceeds 32 bits");
            if (!ext_block(v, dir, logical, &physical, &run, &budget)) return false;
        }
        u32 n = min32(v->block_size, dir->size - off);
        if (!ext_data(v, physical, 0, block, n)) return false;
        u32 within = 0;
        for (; within + 8 <= n;) {
            if (!ext_work(&budget, 1)) return false;
            const u8 *h = block + within;
            u32 ino = rd32(h), len = rd16(h + 4), name_len = h[6];
            if (v->block_size == 65536 && (len == 65535 || !len)) len = 65536;
            if (len < 8 || len % 4 || len > n - within || name_len > len - 8)
                return error("invalid ext directory record");
            if (ino) {
                char filename[256];
                memcpy(filename, h + 8, name_len);
                filename[name_len] = 0;
                if (name_equal(filename, name, false)) return inode_read(v, ino, out);
            }
            within += len;
        }
        if (within != n && dir->size - off - within >= 8)
            return error("invalid ext directory record");
        off += n; run--;
        if (physical) physical++;
    }
    return error("file not found");
}
