#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../fs.h"

/* Small synthetic images can alias one directory block across many extents.
   Count actual disk reads so an I/O amplification regression fails promptly. */
static u8 image[32 * 65536];
static u8 file_data[33 * 1024 * 1024];
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
    assert(fs_error && !strcmp(fs_error, "ext work budget exceeded"));
}
static void invalid_record(void) {
    struct node out;
    assert(!ext_lookup(&volume, &directory, "missing", &out));
    assert(fs_error && !strcmp(fs_error, "invalid ext directory record"));
}
static void read_budget_exceeded(void) {
    directory.directory = false;
    directory.size = sizeof file_data;
    read_limit = 131072;
    assert(!fs_read(&volume, &directory, 0, file_data, sizeof file_data));
    assert(fs_error && !strcmp(fs_error, "ext work budget exceeded"));
    assert(reads < 131072);
    /* Exhaustion must not leak into a subsequent independent request. */
    assert(fs_read(&volume, &directory, 12 * volume.block_size, file_data, 1));
}
int main(void) {
    /* Regular-file reads must share a budget across mappings. Small extent
       trees can otherwise cause millions of reads, including across holes. */
    for (u32 bs = 1024; bs <= 65536; bs *= 2) {
        fixture(bs, (bs - 12) / 12, 1);
        read_budget_exceeded();
    }
    fixture(1024, 1, 1);
    extent(image + 2 * 1024 + 12, sizeof file_data / 1024, 1, 3);
    read_budget_exceeded();
    fixture(1024, 1, 1);
    header(directory.meta + 40, 1, 4, 0);
    extent(directory.meta + 52, sizeof file_data / 1024, 1, 3);
    read_budget_exceeded();

    /* Legacy indirect pointers are subject to the same cumulative limit. */
    fixture(1024, 1, 1);
    memset(directory.meta + 32, 0, 68);
    wr32(directory.meta + 88, 2);
    wr32(directory.meta + 92, 2);
    for (u32 i = 0; i < 1024 / 4; i++) wr32(image + 2 * 1024 + i * 4, 2);
    read_budget_exceeded();

    /* A large unwritten extent costs metadata work, not its byte length. */
    fixture(2048, 1, 1);
    directory.directory = false;
    directory.size = sizeof file_data;
    extent(image + 2 * 2048 + 12, 0, 32768 + sizeof file_data / 2048, 0);
    memset(file_data, 0xff, sizeof file_data);
    assert(fs_read(&volume, &directory, 0, file_data, sizeof file_data));
    assert(metadata_reads == 2 && data_reads == 0);
    for (u32 i = 0; i < sizeof file_data; i++) assert(file_data[i] == 0);

    /* Fresh requests retain normal unaligned reads, sparse zero filling and
       error propagation for invalid extent metadata. */
    for (u32 bs = 1024; bs <= 65536; bs *= 2) {
        fixture(bs, 1, 1);
        directory.directory = false;
        directory.size = 3 * bs;
        extent(image + 2 * bs + 12, 1, 1, 3);
        memset(file_data, 0xff, 3 * bs);
        assert(fs_read(&volume, &directory, 1, file_data, 3 * bs - 2));
        for (u32 i = 0; i < 3 * bs - 2; i++) {
            u32 pos = i + 1;
            assert(file_data[i] == (pos >= bs && pos < 2 * bs ? image[3 * bs + pos - bs] : 0));
        }
        assert(fs_read(&volume, &directory, bs + 7, file_data, 17));
        assert(!memcmp(file_data, image + 3 * bs + 7, 17));
        wr16(image + 2 * bs, 0);
        assert(!fs_read(&volume, &directory, 0, file_data, 1));
        assert(fs_error && !strcmp(fs_error, "invalid ext extent tree"));
    }

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
    puts("ext directory lookup and file read checks passed");
    return 0;
}
