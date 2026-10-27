#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../fs.h"

/* Count actual production-reader I/O, rather than relying on timing. */
static u8 *image;
static u64 image_size, reads, read_limit;
bool disk_read(struct disk *d, u64 offset, void *buffer, u32 size) {
    (void)d;
    if (++reads > read_limit) return error("directory lookup I/O budget exceeded");
    assert(offset <= image_size && size <= image_size - offset);
    memcpy(buffer, image + offset, size);
    return true;
}
static u32 chain_cluster(u32 i, u32 clusters) {
    /* Alternate FAT sectors on every link, even for small directories. */
    return 2 + i / 2 + (i % 2 ? clusters / 2 : 0);
}
static void set_link(struct volume *v, u32 cluster, u32 next) {
    u32 bits = v->u.fat.bits;
    u8 *p = image + v->u.fat.fat + cluster * bits / 8;
    if (bits == 12) {
        u32 shift = cluster % 2 ? 4 : 0;
        wr16(p, (rd16(p) & ~(0xfff << shift)) | ((next & 0xfff) << shift));
    } else if (bits == 16) wr16(p, next);
    else wr32(p, next);
}
static u8 *cluster_data(struct volume *v, u32 cluster) {
    return image + v->u.fat.data + (u64)(cluster - 2) * v->u.fat.cluster_size;
}
static void entry(u8 *p, const char *name, u32 cluster, bool directory) {
    memset(p, 0, 32);
    memcpy(p, name, 11);
    p[11] = directory ? 0x10 : 0x20;
    wr16(p + 26, cluster); wr16(p + 20, cluster >> 16);
}
static void make_image(struct volume *v, u32 bits, u32 sector, u32 spc, u32 length) {
    u32 clusters = bits == 12 ? 1024 : bits == 16 ? 8192 : 131072;
    u32 fat_sectors = (((clusters + 2) * bits + 7) / 8 + sector - 1) / sector;
    u32 roots = bits == 32 ? 0 : 17;
    u32 root_sectors = (roots * 32 + sector - 1) / sector;
    u32 total = 1 + fat_sectors + root_sectors + clusters * spc;
    image_size = (u64)total * sector;
    image = calloc(1, image_size);
    assert(image);
    image[0] = 0xe9; image[13] = spc; image[16] = 1;
    wr16(image + 11, sector); wr16(image + 14, 1);
    wr16(image + 17, roots); wr32(image + 32, total);
    if (bits == 32) { wr32(image + 36, fat_sectors); wr32(image + 44, 2); }
    else wr16(image + 22, fat_sectors);
    wr16(image + 510, 0xaa55);
    *v = (struct volume){ .part = { .size = image_size }, .block_size = sector,
        .u.fat = { .bits = bits, .clusters = clusters, .cluster_size = sector * spc,
            .fat = sector, .root = (u64)(1 + fat_sectors) * sector,
            .data = (u64)(1 + fat_sectors + root_sectors) * sector } };
    for (u32 i = 0; i < length; i++) {
        u32 cluster = chain_cluster(i, clusters);
        set_link(v, cluster, i + 1 < length ? chain_cluster(i + 1, clusters) : UINT32_MAX);
        memset(cluster_data(v, cluster), 0xe5, sector * spc);
    }
    if (roots) {
        memset(image + v->u.fat.root, 0xe5, roots * 32);
        /* Last entry is in a partial sector for 512-byte sectors. */
        entry(image + v->u.fat.root + (roots - 1) * 32, "BOOT       ", 2, true);
    }
    reads = 0; read_limit = 2 * (u64)length + 8;
    assert(fat_mount(v) && v->u.fat.bits == bits);
}
static void lookup_checks(u32 bits, u32 sector, u32 spc, u32 length) {
    struct volume v;
    make_image(&v, bits, sector, spc, length);
    u32 last = chain_cluster(length - 1, v.u.fat.clusters);
    u8 *tail = cluster_data(&v, last) + v.u.fat.cluster_size - 32;
    entry(tail, "ARKBOOT CFG", 0, false);
    /* LFN at the end of the preceding cluster: state must cross both buffers
       and fragmented cluster boundaries. */
    u8 *lfn = cluster_data(&v, chain_cluster(length - 2, v.u.fat.clusters)) + v.u.fat.cluster_size - 32;
    memset(lfn, 0xff, 32);
    lfn[0] = 0x41; lfn[11] = 0x0f; lfn[12] = 0; wr16(lfn + 26, 0);
    u8 sum = 0;
    for (u32 i = 0; i < 11; i++) sum = ((sum & 1) << 7) + (sum >> 1) + tail[i];
    lfn[13] = sum;
    const u8 positions[] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    const char long_name[] = "boot-menu.cfg";
    for (u32 i = 0; i < 13; i++) wr16(lfn + positions[i], long_name[i]);
    /* Put the associated short entry immediately after the LFN. */
    memcpy(cluster_data(&v, last), tail, 32);
    struct node out;
    reads = 0; read_limit = (u64)length * (spc + 3) + 8;
    assert(fs_open(&v, bits == 32 ? "/boot-menu.cfg" : "/boot/boot-menu.cfg", &out));
    assert(!out.directory && !out.size);
    reads = 0;
    assert(fs_open(&v, bits == 32 ? "/ARKBOOT.CFG" : "/BOOT/ARKBOOT.CFG", &out));
    reads = 0;
    assert(!fs_open(&v, bits == 32 ? "/missing" : "/boot/missing", &out));
    assert(!strcmp(fs_error, "file not found"));
    struct node dir = { .id = 2, .directory = true,
        .size = ((u64)length + 1) * v.u.fat.cluster_size };
    reads = 0;
    assert(!fat_lookup(&v, &dir, "missing", &out));
    assert(strstr(fs_error, "short FAT directory chain"));
    /* A 64-bit remaining size must not truncate the buffered read length. */
    dir.size = UINT64_C(1) << 32;
    reads = 0;
    assert(fat_lookup(&v, &dir, "ARKBOOT.CFG", &out));
    /* A zero entry stops scanning without following the rest of the chain. */
    cluster_data(&v, 2)[0] = 0;
    reads = 0; read_limit = 1;
    assert(!fat_lookup(&v, &dir, "missing", &out));
    assert(!strcmp(fs_error, "file not found"));
    cluster_data(&v, 2)[0] = 0xe5;
    set_link(&v, last, 2);
    /* Remount after mutation to invalidate the read-only driver's FAT cache. */
    reads = 0; read_limit = 4 * (u64)length + 8;
    assert(fat_mount(&v) == (bits != 32));
    /* Direct lookup bounds total work even for an oversized cyclic node. */
    dir.size = ((u64)v.u.fat.clusters + 1) * v.u.fat.cluster_size;
    reads = 0; read_limit = (u64)v.u.fat.clusters * (spc + 1) + 8;
    assert(!fat_lookup(&v, &dir, "missing", &out));
    assert(strstr(fs_error, "cyclic"));
    /* Mount/subdirectory sizing must still reject cycles and invalid links. */
    reads = 0; read_limit = 4 * (u64)length + 8;
    assert(!(bits == 32 ? fat_mount(&v) : fs_open(&v, "/boot", &out)));
    assert(strstr(fs_error, "cyclic"));
    set_link(&v, 2, 1);
    reads = 0;
    assert(fat_mount(&v) == (bits != 32));
    assert(!(bits == 32 ? fat_mount(&v) : fs_open(&v, "/boot", &out)));
    assert(strstr(fs_error, "bad FAT cluster chain"));
    free(image);
}
static void cycle_checks(u32 bits, u32 prefix, u32 cycle) {
    struct volume v;
    u32 length = prefix + cycle;
    make_image(&v, bits, 512, 1, length);
    set_link(&v, chain_cluster(length - 1, v.u.fat.clusters),
             chain_cluster(prefix, v.u.fat.clusters));
    reads = 0; read_limit = 4 * (u64)length + 8;
    assert(fat_mount(&v) == (bits != 32));
    struct node out;
    if (bits != 32) assert(!fs_open(&v, "/boot", &out));
    assert(!strcmp(fs_error, "cyclic FAT directory chain"));
    if (bits == 32) {
        /* A separate one-cluster root reaches the same cycle as a child. */
        u32 root = v.u.fat.clusters + 1;
        set_link(&v, root, UINT32_MAX);
        wr32(image + 44, root);
        entry(cluster_data(&v, root), "BOOT       ", 2, true);
        reads = 0;
        assert(fat_mount(&v));
        assert(!fs_open(&v, "/boot", &out));
        assert(!strcmp(fs_error, "cyclic FAT directory chain"));
    }
    free(image);
}
static void sizing_limit_checks(void) {
    for (u32 length = 65536; length <= 65537; length++) {
        struct volume v;
        /* Mount a short root first, then build a fragmented subdirectory. */
        make_image(&v, 32, 512, 1, 1);
        entry(cluster_data(&v, 2), "BOOT       ", chain_cluster(1, v.u.fat.clusters), true);
        for (u32 i = 1; i <= length; i++)
            set_link(&v, chain_cluster(i, v.u.fat.clusters),
                     i < length ? chain_cluster(i + 1, v.u.fat.clusters) : UINT32_MAX);
        reads = 0; read_limit = 65536 + 3;
        assert(fat_mount(&v));
        struct node out;
        assert(fs_open(&v, "/boot", &out) == (length == 65536));
        if (length == 65536) assert(out.size == (u64)length * 512);
        else assert(!strcmp(fs_error, "FAT directory chain work limit exceeded"));
        /* Exercise the same boundary through FAT32 root sizing. */
        wr32(image + 44, chain_cluster(1, v.u.fat.clusters));
        reads = 0; read_limit = 65536 + 1;
        assert(fat_mount(&v) == (length == 65536));
        if (length == 65536) assert(v.root.size == (u64)length * 512);
        else assert(!strcmp(fs_error, "FAT directory chain work limit exceeded"));
        free(image);
    }
}
int main(void) {
    for (u32 bits = 12; bits <= 32; bits = bits == 12 ? 16 : bits == 16 ? 32 : 33) {
        cycle_checks(bits, 0, 1);
        cycle_checks(bits, 0, 2);
        cycle_checks(bits, 31, 3);
        lookup_checks(bits, 512, 1, 128);
        lookup_checks(bits, 512, 2, 256);
    }
    lookup_checks(12, 4096, 2, 128);
    sizing_limit_checks();
    puts("PASS FAT directory lookup: bounded I/O, fragmented chains, fixed roots, LFN and corrupt metadata");
}
