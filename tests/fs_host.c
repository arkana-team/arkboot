#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "../fs.h"

/* Emulate only BIOS disk I/O. Partition and filesystem code is the actual core
   implementation, exercised with normal image-tool output and bounded memory. */
static FILE *image;
static u64 image_bytes;
static u32 block_size;
static bool unknown_capacity;
void bios_call(u32 vector, struct bios_regs *r) {
    r->flags = 1;
    if (vector != 0x13) abort();
    u8 *p = (u8 *)(uintptr_t)(((u32)r->ds << 4) + (r->esi & 0xffff));
    if ((r->eax >> 8) == 0x48) {
        wr16(p, 30); memcpy(p + 16, &(u64){unknown_capacity ? UINT64_MAX : image_bytes / block_size}, 8);
        wr16(p + 24, block_size); r->flags = 0;
    } else if ((r->eax >> 8) == 0x42) {
        u64 start = rd64(p + 8) * block_size;
        u32 size = rd16(p + 2) * block_size;
        void *dest = (void *)(uintptr_t)(((u32)rd16(p + 6) << 4) + rd16(p + 4));
        if (start <= image_bytes && size <= image_bytes - start &&
            !fseeko(image, start, SEEK_SET) && fread(dest, 1, size, image) == size) r->flags = 0;
    } else if (!(r->eax & 0xff00)) r->flags = 0;
}
static void die(const char *s) { fprintf(stderr, "%s: %s\n", s, fs_error ? fs_error : ""); exit(1); }
int main(int argc, char **argv) {
    bool reject = argc == 5 && !strcmp(argv[3], "reject");
    if (!reject && (argc < 6 || argc % 2)) { fprintf(stderr, "usage: fs_host image block-size fs-name path reference [path reference ...]\n"); return 2; }
    block_size = strtoul(argv[2], NULL, 0);
    unknown_capacity = getenv("ARKBOOT_TEST_UNKNOWN_CAPACITY") != NULL;
    image = fopen(argv[1], "rb");
    if (!image) { perror(argv[1]); return 1; }
    struct stat st;
    if (fstat(fileno(image), &st)) return 1;
    image_bytes = st.st_size;
    if (mmap((void *)0x60000, 0x10000, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED) { perror("BIOS memory"); return 1; }
    struct disk d;
    struct partition p[32];
    if (!disk_init(&d, 0x80, block_size)) die("disk_init");
    u32 count = partitions(&d, p, 32);
    if (reject) {
        bool rejected = !count && fs_error && strstr(fs_error, argv[4]);
        for (u32 i = 0; i < count; i++) {
            struct volume candidate;
            if (fs_mount(&candidate, &p[i])) die("corrupt filesystem mounted");
            if (fs_error && strstr(fs_error, argv[4])) rejected = true;
        }
        if (!rejected) die("expected metadata rejection missing");
        puts("metadata rejection check passed");
        fclose(image);
        return 0;
    }
    if (!count) die("partitions");
    struct volume v;
    bool found = false;
    for (u32 i = 0; i < count; i++) {
        if (!fs_mount(&v, &p[i])) continue;
        struct node n;
        if (fs_open(&v, argv[4], &n)) { found = true; break; }
    }
    if (!found) die("mount/open");
    if (strcmp(fs_name(&v), argv[3])) die("wrong filesystem driver");
    u8 actual[65536], expected[65536];
    for (int i = 4; i < argc; i += 2) {
        struct node n;
        if (!fs_open(&v, argv[i], &n) || n.directory) die("fs_open");
        FILE *ref = fopen(argv[i + 1], "rb");
        if (!ref) die("reference open");
        if (fstat(fileno(ref), &st) || n.size != (u64)st.st_size) die("file size");
        /* Unaligned chunks cross sectors, clusters, ext indirect boundaries and
           UDF extents, while keeping test buffers below 1 MiB. */
        for (u64 off = 0; off < n.size;) {
            u32 size = min32(65521, n.size - off);
            if (fread(expected, 1, size, ref) != size || !fs_read(&v, &n, off, actual, size) ||
                memcmp(actual, expected, size)) die("file contents");
            off += size;
        }
        u32 seed = 0x12345678;
        for (u32 j = 0; j < 32 && n.size; j++) {
            seed = seed * 1664525 + 1013904223;
            u64 off = seed % n.size;
            u32 size = min32(4093, n.size - off);
            if (fseeko(ref, off, SEEK_SET) || fread(expected, 1, size, ref) != size ||
                !fs_read(&v, &n, off, actual, size) || memcmp(actual, expected, size)) die("random file contents");
        }
        if (fs_read(&v, &n, n.size, actual, 1)) die("file bounds not enforced");
        fclose(ref);
    }
    struct node missing;
    if (fs_open(&v, "/this-file-does-not-exist", &missing)) die("nonexistent file opened");
    fclose(image);
    puts("filesystem read checks passed");
    return 0;
}
