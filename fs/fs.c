#include "../fs.h"

bool name_equal(const char *a, const char *b, bool fold) {
    while (*a && *b) {
        u8 x = *a++, y = *b++;
        if (fold) { if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; }
        if (x != y) return false;
    }
    return *a == *b;
}
bool name_utf8(char *out, u32 *len, u32 cp) {
    if (!cp || cp == '/' || cp > 0xffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    u32 n = cp < 128 ? 1 : cp < 2048 ? 2 : 3;
    if (*len + n > FS_NAME_MAX) return false;
    if (n == 3) out[(*len)++] = 0xe0 | (cp >> 12);
    if (n >= 2) out[(*len)++] = (n == 2 ? 0xc0 : 0x80) | ((cp >> 6) & (n == 2 ? 31 : 63));
    out[(*len)++] = n == 1 ? cp : 0x80 | (cp & 63);
    out[*len] = 0;
    return true;
}
const char *fs_name(const struct volume *v) {
    switch (v->kind) {
    case FS_FAT: return v->u.fat.bits == 12 ? "FAT12" : v->u.fat.bits == 16 ? "FAT16" : "FAT32";
    case FS_EXT: return "ext2/3/4";
    case FS_ISO: return "ISO9660";
    case FS_UDF: return "UDF";
    default: return "unknown";
    }
}
bool fs_mount(struct volume *v, const struct partition *p) {
    memset(v, 0, sizeof *v); v->part = *p; fs_error = NULL;
    /* UDF first: a bridge disc may contain both UDF and ISO9660 trees. */
    bool (*const drivers[])(struct volume *) = {udf_mount, ext_mount, fat_mount, iso_mount};
    for (u32 i = 0; i < sizeof drivers / sizeof drivers[0]; i++) {
        if (drivers[i](v)) { fs_error = NULL; return true; }
        if (fs_error) return false;
    }
    return false;
}
bool fs_read(struct volume *v, const struct node *f, u64 off, void *dst, u32 n) {
    if (off > f->size || n > f->size - off) return error("read outside file");
    switch (v->kind) {
    case FS_FAT: return fat_read(v, f, off, dst, n);
    case FS_EXT: return ext_read(v, f, off, dst, n);
    case FS_ISO: return iso_read(v, f, off, dst, n);
    case FS_UDF: return udf_read(v, f, off, dst, n);
    default: return error("no filesystem mounted");
    }
}
bool fs_open(struct volume *v, const char *path, struct node *out) {
    if (strlen(path) > FS_PATH_MAX) return error("path too long");
    struct node node = v->root;
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        char name[FS_NAME_MAX + 1];
        u32 len = 0;
        while (*path && *path != '/') {
            if (len == FS_NAME_MAX) return error("filename too long");
            name[len++] = *path++;
        }
        name[len] = 0;
        if (!strcmp(name, ".")) continue;
        if (!strcmp(name, "..")) return error("parent paths are not supported");
        if (!node.directory) return error("path component is not a directory");
        struct node next;
        bool ok = false;
        switch (v->kind) {
        case FS_FAT: ok = fat_lookup(v, &node, name, &next); break;
        case FS_EXT: ok = ext_lookup(v, &node, name, &next); break;
        case FS_ISO: ok = iso_lookup(v, &node, name, &next); break;
        case FS_UDF: ok = udf_lookup(v, &node, name, &next); break;
        default: break;
        }
        if (!ok) return false;
        node = next;
    }
    *out = node;
    return true;
}
