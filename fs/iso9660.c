#include "../fs.h"

static bool iso_node(const u8 *r, struct node *out) {
    if (r[0] < 34 || r[1] || r[26] || r[27]) return error("unsupported ISO9660 extended/interleaved file");
    if (r[25] & 0x80) return error("ISO9660 multi-extent files are unsupported");
    *out = (struct node){ .id = rd32(r + 2), .size = rd32(r + 10), .directory = !!(r[25] & 2) };
    return true;
}
bool iso_mount(struct volume *v) {
    u8 d[2048];
    if (v->part.size < 17 * 2048) return false;
    for (u32 block = 16; block < 80; block++) {
        if (!part_read(&v->part, (u64)block * 2048, d, sizeof d)) return false;
        if (memcmp(d + 1, "CD001", 5) || d[6] != 1) return false;
        if (d[0] == 255) return false;
        if (d[0] != 1) continue;
        if (rd16(d + 128) != 2048 || (u64)rd32(d + 80) * 2048 > v->part.size)
            return error("invalid ISO9660 volume");
        if (!iso_node(d + 156, &v->root)) return false;
        v->block_size = 2048; v->kind = FS_ISO;
        return v->root.directory || error("invalid ISO9660 root directory");
    }
    return false;
}
bool iso_read(struct volume *v, const struct node *f, u64 off, void *dst, u32 size) {
    return part_read(&v->part, (u64)f->id * 2048 + off, dst, size);
}
bool iso_lookup(struct volume *v, const struct node *dir, const char *name, struct node *out) {
    for (u64 off = 0; off < dir->size;) {
        u8 r[255];
        if (!iso_read(v, dir, off, r, 1)) return false;
        if (!r[0]) { off = (off / 2048 + 1) * 2048; continue; }
        u32 len = r[0];
        if (len < 34 || len > dir->size - off || len > 2048 - off % 2048 ||
            !iso_read(v, dir, off, r, len)) return error("invalid ISO9660 directory record");
        u32 name_len = r[32];
        if (name_len > len - 33) return error("invalid ISO9660 filename");
        char filename[256]; u32 n = 0;
        for (u32 i = 0; i < name_len && r[33 + i] != ';'; i++) filename[n++] = r[33 + i];
        if (n && filename[n - 1] == '.') n--;
        filename[n] = 0;
        if (name_len && r[33] > 1 && name_equal(filename, name, true)) return iso_node(r, out);
        off += len;
    }
    return error("file not found");
}
