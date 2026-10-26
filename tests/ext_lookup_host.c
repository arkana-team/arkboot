#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../fs.h"

/* Small synthetic images can alias one directory block across many extents.
   Count actual disk reads so an I/O amplification regression fails promptly. */
static u8 image[32 * 65536];
static u32 reads, metadata_reads, data_reads, read_limit;
static struct volume volume;
static struct node directory;

bool disk_read(struct disk *disk, u64 off, void *dst, u32 size) {
    (void)disk;
    assert(++reads <= read_limit);
    assert(off <= sizeof image && size <= sizeof image - off);
    if (off >= 2 * volume.block_size && off < 3 * volume.block_size) metadata_reads++;
    if (off >= 3 * volume.block_size && off < 4 * volume.block_size) data_reads++;
    memcpy(dst, image + off, size);
    return true;
}
static void header(u8 *p, u32 count, u32 max, u32 depth) {
    wr16(p, 0xf30a); wr16(p + 2, count); wr16(p + 4, max); wr16(p + 6, depth);
}
static void extent(u8 *p, u32 logical, u32 length, u32 physical) {
    wr32(p, logical); wr16(p + 4, length); wr32(p + 8, physical);
}
static void fixture(u32 bs, u32 count, u32 length) {
    memset(image, 0, sizeof image);
    volume = (struct volume){.part = {.size = sizeof image}, .kind = FS_EXT, .block_size = bs};
    volume.u.ext.blocks = sizeof image / bs;
    directory = (struct node){.directory = true, .size = (u64)count * length * bs};
    wr32(directory.meta + 32, 0x80000);
    header(directory.meta + 40, 1, 4, 1);
    wr32(directory.meta + 56, 2);
    header(image + 2 * bs, count, (bs - 12) / 12, 0);
    for (u32 i = 0; i < count; i++) extent(image + 2 * bs + 12 + i * 12, i * length, length, 3);
    /* Nonmatching live records require both header and name handling. */
    for (u32 off = 0; off + 12 <= bs;) {
        u8 *p = image + 3 * bs + off;
        u32 len = bs - off < 24 ? bs - off : 12;
        wr32(p, 3); wr16(p + 4, len); p[6] = 1; p[8] = 'x';
        off += len;
    }
    fs_error = NULL; reads = metadata_reads = data_reads = 0; read_limit = 10000;
}
static void missing(void) {
    struct node out;
    assert(!ext_lookup(&volume, &directory, "missing", &out));
    assert(fs_error && !strcmp(fs_error, "file not found"));
}
static void budget_exceeded(void) {
    struct node out;
    assert(!ext_lookup(&volume, &directory, "missing", &out));
    assert(fs_error && !strcmp(fs_error, "ext directory lookup budget exceeded"));
}
static void invalid_record(void) {
    struct node out;
    assert(!ext_lookup(&volume, &directory, "missing", &out));
    assert(fs_error && !strcmp(fs_error, "invalid ext directory record"));
}
/* Exercise regular reads through the public bounds check with aliased data. */
static void file_bytes(u64 off, u32 size, bool zero) {
    u8 *out = malloc(size ? size : 1);
    assert(out);
    assert(fs_read(&volume, &directory, off, out, size));
    for (u32 i = 0; i < size; i++) {
        u8 expected = zero ? 0 : image[3 * volume.block_size + (off + i) % volume.block_size];
        assert(out[i] == expected);
    }
    free(out);
}
static void file_budget_exceeded(u64 off, u32 size) {
    u8 *out = malloc(size);
    assert(out);
    assert(!fs_read(&volume, &directory, off, out, size));
    assert(fs_error && !strcmp(fs_error, "ext file mapping budget exceeded"));
    free(out);
}
static void file_checks(void) {
    for (u32 bs = 1024; bs <= 65536; bs *= 2) {
        fixture(bs, 84, 1);
        directory.directory = false;
        /* Previously 3,654 metadata reads for 84 one-block extents. */
        read_limit = 128;
        file_bytes(0, directory.size, false);
        assert(metadata_reads < 8 && data_reads == 84);
        reads = metadata_reads = data_reads = 0;
        file_bytes(17, directory.size - 34, false);
        assert(metadata_reads < 8 && data_reads == 84);
        /* A random read and a different inode get fresh traversal state. */
        reads = metadata_reads = data_reads = 0;
        file_bytes(70 * (u64)bs + 7, bs + 3, false);
        file_bytes(3, bs + 5, false);
        assert(metadata_reads < 12);
        header(directory.meta + 40, 1, 4, 0);
        memset(directory.meta + 52, 0, 12);
        extent(directory.meta + 52, 0, 1, 3);
        file_bytes(0, bs, false);
        /* Sparse and unwritten extents keep their zero-fill semantics. */
        header(directory.meta + 40, 2, 4, 0);
        extent(directory.meta + 52, 2, 32769, 3);
        extent(directory.meta + 64, 4, 1, 3);
        file_bytes(1, 4 * bs - 1, true);
        file_bytes(4 * (u64)bs, bs, false);
    }

    /* Full 64 KiB leaves must use batches, including a random read near EOF. */
    fixture(65536, (65536 - 12) / 12, 1);
    directory.directory = false;
    read_limit = 300;
    file_bytes(0, 256 * 65536, false);
    assert(metadata_reads < 12);
    reads = metadata_reads = data_reads = 0;
    file_bytes(directory.size - 65536 - 17, 65536 + 17, false);
    assert(metadata_reads < 175);

    /* Cross leaves at depth two, then at the maximum supported depth. Each
       index selects two leaves that reuse the same physical data block. */
    for (u32 depth = 2; depth <= 5; depth += 3) {
        fixture(1024, 84, 1);
        directory.directory = false;
        header(directory.meta + 40, 1, 4, depth);
        wr32(directory.meta + 56, 4);
        for (u32 level = depth - 1; level > 1; level--) {
            u32 block = 4 + depth - 1 - level;
            header(image + block * 1024, 1, 84, level);
            wr32(image + block * 1024 + 16, block + 1);
        }
        u8 *index = image + (depth + 2) * 1024;
        header(index, 2, 84, 1);
        wr32(index + 16, 2);
        wr32(index + 24, 84); wr32(index + 28, 9);
        memcpy(image + 9 * 1024, image + 2 * 1024, 1024);
        for (u32 i = 0; i < 84; i++) wr32(image + 9 * 1024 + 12 + i * 12, 84 + i);
        directory.size *= 2;
        read_limit = 200;
        file_bytes(0, directory.size, false);
        /* Cached paths must not hide work: a long sparse tail still stops. */
        directory.size = 16384 * 1024;
        reads = 0; read_limit = 1000;
        file_budget_exceeded(0, directory.size);
        assert(reads < 200);
        /* Neither cache nor exhausted budget leaks into the next call. */
        reads = 0;
        file_bytes(0, 1024, false);
    }

    /* Legacy indirect mapping shares the cumulative file-read budget. */
    fixture(1024, 1, 1);
    directory.directory = false;
    wr32(directory.meta + 32, 0);
    memset(directory.meta + 40, 0, 60);
    wr32(directory.meta + 92, 2);
    for (u32 i = 0; i < 256; i++) {
        wr32(image + 2 * 1024 + i * 4, 4);
        wr32(image + 4 * 1024 + i * 4, 3);
    }
    directory.size = (u64)(12 + 256 + 32769) * 1024;
    read_limit = 100000;
    file_budget_exceeded((12 + 256) * 1024, 32769 * 1024);
    assert(reads == 65536 + data_reads && data_reads == 32768);

    /* Resuming from a cursor must still reject an unordered leaf entry. */
    fixture(1024, 84, 1);
    directory.directory = false;
    wr32(image + 2 * 1024 + 12 + 40 * 12, 39);
    u8 out[42 * 1024];
    assert(!fs_read(&volume, &directory, 0, out, sizeof out));
    assert(fs_error && !strcmp(fs_error, "unordered ext extents"));
    puts("ext regular-file mapping checks passed");
}
int main(void) {
    /* 84 ordered extents alias the same physical block. Previously this
       incurred over 600,000 reads for a directory stored in two blocks. */
    fixture(1024, 84, 1);
    missing();
    assert(data_reads == 84 && metadata_reads < 4000);

    /* One resolved range must survive across directory blocks. */
    fixture(1024, 1, 4);
    memcpy(image + 4 * 1024, image + 3 * 1024, 1024);
    memcpy(image + 5 * 1024, image + 3 * 1024, 1024);
    memcpy(image + 6 * 1024, image + 3 * 1024, 1024);
    missing();
    assert(metadata_reads == 2);

    /* A huge logical directory still has bounded cumulative work, including
       when mappings are inline or use legacy indirect blocks. */
    for (u32 mode = 0; mode < 3; mode++) {
        fixture(65536, (65536 - 12) / 12, 1);
        if (mode == 1) {
            header(directory.meta + 40, 1, 4, 0);
            memset(directory.meta + 52, 0, 12);
            extent(directory.meta + 52, 0, 24, 3);
            directory.size = (u64)24 * 65536;
            for (u32 i = 4; i < 27; i++) memcpy(image + i * 65536, image + 3 * 65536, 65536);
        } else if (mode == 2) {
            wr32(directory.meta + 32, 0);
            memset(directory.meta + 40, 0, 60);
            for (u32 i = 0; i < 12; i++) wr32(directory.meta + 40 + i * 4, 3);
            wr32(directory.meta + 88, 2);
            for (u32 i = 0; i < 65536 / 4; i++) wr32(image + 2 * 65536 + i * 4, 3);
        }
        budget_exceeded();
        assert(reads < 1000);
    }

    /* Few records per block must not bypass the metadata or data budget. */
    fixture(65536, (65536 - 12) / 12, 1);
    memset(image + 3 * 65536, 0, 65536);
    wr16(image + 3 * 65536 + 4, 65535);
    read_limit = 65536;
    budget_exceeded();
    assert(metadata_reads < 65536 && data_reads < 512);
    fixture(65536, 1, 1);
    wr32(directory.meta + 32, 0);
    memset(directory.meta + 40, 0, 60);
    for (u32 i = 0; i < 12; i++) wr32(directory.meta + 40 + i * 4, 3);
    wr32(directory.meta + 88, 2);
    for (u32 i = 0; i < 65536 / 4; i++) wr32(image + 2 * 65536 + i * 4, 3);
    directory.size = (u64)1024 * 65536;
    memset(image + 3 * 65536, 0, 65536);
    budget_exceeded();
    assert(data_reads < 512);

    /* Preserve successful lookup and the supported block-size range. */
    for (u32 bs = 1024; bs <= 65536; bs *= 2) {
        fixture(bs, 1, 1);
        volume.u.ext.inodes = volume.u.ext.inodes_per_group = 4;
        volume.u.ext.groups = 1; volume.u.ext.inode_size = 128;
        volume.u.ext.desc_size = 32; volume.u.ext.descriptors = bs;
        wr32(image + bs + 8, 7);
        wr16(image + 7 * bs + 2 * 128, 0x8000);
        volume.root = directory;
        struct node out;
        assert(fs_open(&volume, "/x", &out) && out.id == 3 && !out.directory);
        missing();
        assert(data_reads == 2);  /* A new lookup gets a fresh block. */
        if (bs == 65536) {
            for (u32 encoded = 0; encoded <= 65535; encoded += 65535) {
                memset(image + 3 * bs, 0, bs);
                wr16(image + 3 * bs + 4, encoded);
                missing();
            }
        }
    }

    fixture(1024, 1, 1);
    wr16(image + 3 * 1024 + 4, 1028);
    invalid_record();
    /* A record header in the last four bytes must not be skipped on the way
       to the next directory block. A short final tail retains EOF behavior. */
    fixture(1024, 1, 2);
    wr16(image + 3 * 1024 + 4, 1020);
    invalid_record();
    directory.size = 1024;
    missing();
    puts("ext directory lookup checks passed");
    file_checks();
    return 0;
}
