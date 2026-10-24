#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../fs.h"

/* A tiny, checksum-valid volume can alias a directory block many times.
   Count reads through the production mount/open/read path, without firmware. */
static u8 image[272 * 4096];
static u32 bs, reads;
static bool bounded;

bool disk_read(struct disk *d, u64 offset, void *buffer, u32 size) {
    (void)d;
    if (bounded) assert(++reads <= 2 * 65536 + 2);
    if (offset > 272u * bs || size > 272u * bs - offset) return false;
    memcpy(buffer, image + offset, size);
    return true;
}
static u8 *block(u32 n) { return image + n * bs; }
static void seal(u8 *b, u16 tag, u32 location, u32 size) {
    wr16(b, tag); wr16(b + 2, 3); wr16(b + 10, size - 16); wr32(b + 12, location);
    u16 crc = 0;
    for (u32 i = 16; i < size; i++) {
        crc ^= (u16)b[i] << 8;
        for (u32 j = 0; j < 8; j++) crc = (crc << 1) ^ (crc & 0x8000 ? 0x1021 : 0);
    }
    wr16(b + 8, crc);
    b[4] = 0;
    for (u32 i = 0; i < 16; i++) if (i != 4) b[4] += b[i];
}
static void extent(u8 *ad, u32 length, u32 lbn) {
    wr32(ad, length); wr32(ad + 4, lbn);
}
static void inode(u8 *b, u32 lbn, bool efe, u32 type, bool directory, u64 size, u32 ads) {
    u32 fixed = efe ? 216 : 176;
    wr16(b + 20, 4); b[27] = directory ? 4 : 5; wr16(b + 34, type);
    memcpy(b + 56, &size, sizeof size); wr32(b + fixed - 4, ads);
    seal(b, efe ? 266 : 261, lbn, fixed + ads);
}
static void fixture(struct volume *v, u32 size, u32 type, bool efe, bool continued,
                    bool attack, bool deleted) {
    memset(image, 0, sizeof image); bs = size;
    u8 *b = block(256);
    wr32(b + 16, 3 * bs); wr32(b + 20, 257); seal(b, 2, 256, 24);
    b = block(257);
    wr32(b + 212, bs); wr32(b + 248, bs); wr32(b + 264, 6); wr32(b + 268, 1);
    b[440] = 1; b[441] = 6; seal(b, 6, 257, 446);
    b = block(258);
    wr32(b + 188, 260); wr32(b + 192, 12); seal(b, 5, 258, 196);
    wr16(block(259), 8);
    b = block(260);
    wr32(b + 400, bs); wr32(b + 404, 1); seal(b, 256, 0, 410);
    /* The matching inode is empty, embedded, and independently validated. */
    inode(block(262), 2, efe, 3, false, 0, 0);
    u32 fixed = efe ? 216 : 176, ad_size = type == 0 ? 8 : 16;
    u8 *ads = block(261) + fixed;
    u32 count = attack ? (bs - (continued ? 24 : fixed)) / ad_size : 1;
    if (continued) {
        extent(ads, 0xc0000000u | bs, 3);
        ads = block(263) + 24;
    }
    u64 length = attack ? (u64)count * bs : 64;
    if (type == 3) length = 64;
    if (type != 3) {
        for (u32 i = 0; i < count; i++) extent(ads + i * ad_size, attack ? bs : 64, 4);
    }
    u8 *data = type == 3 ? block(261) + fixed : block(264);
    for (u32 off = 0; off < (type == 3 ? 64 : bs); off += 64) {
        b = data + off;
        b[18] = deleted ? 4 : 0; b[19] = 7;
        wr32(b + 20, bs); wr32(b + 24, 2); wr16(b + 36, 16);
        memcpy(b + 54, "\10target", 7);
        seal(b, 257, 4, 64);
    }
    if (continued) {
        wr32(block(263) + 20, count * ad_size);
        seal(block(263), 258, 3, 24 + count * ad_size);
    }
    inode(block(261), 1, efe, type, true, length,
          type == 3 ? 64 : continued ? ad_size : count * ad_size);
    static struct disk d;
    struct partition p = {&d, 0, 272u * bs};
    fs_error = NULL; bounded = false;
    assert(fs_mount(v, &p) && v->kind == FS_UDF);
    reads = 0; bounded = true;
}
int main(void) {
    struct volume v;
    struct node n;
    for (u32 type = 0; type <= 3; type++) {
        if (type == 2) continue;
        for (u32 efe = 0; efe < 2; efe++) {
            for (u32 continued = 0; continued < (type == 3 ? 1u : 2u); continued++) {
                for (u32 bs_index = 0; bs_index < 3; bs_index++) {
                    const u32 sizes[] = {512, 2048, 4096};
                    fixture(&v, sizes[bs_index], type, efe, continued, false, false);
                    assert(fs_open(&v, "/target", &n) && n.id == 2 && !n.directory);
                    assert(!fs_open(&v, "/missing", &n));
                    assert(fs_error && !strcmp(fs_error, "file not found"));
                    /* A fresh lookup gets its own budget. */
                    assert(fs_open(&v, "/target", &n));
                }
                if (type == 3) continue;
                for (u32 deleted = 0; deleted < 2; deleted++) {
                    fixture(&v, 2048, type, efe, continued, true, deleted);
                    assert(!fs_open(&v, "/missing", &n));
                    assert(fs_error && !strcmp(fs_error, "UDF directory lookup work limit exceeded"));
                    reads = 0;
                    assert(fs_open(&v, deleted ? "/missing" : "/target", &n) == !deleted);
                    /* Random regular reads keep their existing extent semantics. */
                    bounded = false;
                    u8 h[38];
                    assert(fs_read(&v, &v.root, v.root.size - 64, h, sizeof h));
                    assert(rd16(h) == 257);
                }
            }
        }
    }
    puts("UDF lookup work-limit checks passed");
    return 0;
}
