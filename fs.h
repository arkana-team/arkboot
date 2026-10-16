#ifndef ARKBOOT_FS_H
#define ARKBOOT_FS_H
#include "disk.h"
#define FS_NAME_MAX 255
#define FS_PATH_MAX 511

enum fs_kind { FS_NONE, FS_FAT, FS_EXT, FS_ISO, FS_UDF };
struct node {
    u64 size;
    u32 id;
    bool directory;
    u8 meta[160];
};
struct volume {
    struct partition part;
    enum fs_kind kind;
    u32 block_size;
    struct node root;
    union {
        struct { u32 bits, cluster_size, clusters, fat_size, root_entries, root_cluster;
                 u64 fat, data, root; } fat;
        struct { u32 inodes, inodes_per_group, inode_size, desc_size, groups;
                 u64 blocks, descriptors; } ext;
        struct { u32 partition_start, partition_length; } udf;
    } u;
};

bool fs_mount(struct volume *v, const struct partition *p);
bool fs_open(struct volume *v, const char *path, struct node *out);
bool fs_read(struct volume *v, const struct node *file, u64 offset, void *dst, u32 size);
const char *fs_name(const struct volume *v);
bool name_equal(const char *a, const char *b, bool fold);
bool name_utf8(char *out, u32 *len, u32 codepoint);

bool fat_mount(struct volume *v);
bool fat_lookup(struct volume *v, const struct node *dir, const char *name, struct node *out);
bool fat_read(struct volume *v, const struct node *file, u64 off, void *dst, u32 size);
bool ext_mount(struct volume *v);
bool ext_lookup(struct volume *v, const struct node *dir, const char *name, struct node *out);
bool ext_read(struct volume *v, const struct node *file, u64 off, void *dst, u32 size);
bool iso_mount(struct volume *v);
bool iso_lookup(struct volume *v, const struct node *dir, const char *name, struct node *out);
bool iso_read(struct volume *v, const struct node *file, u64 off, void *dst, u32 size);
bool udf_mount(struct volume *v);
bool udf_lookup(struct volume *v, const struct node *dir, const char *name, struct node *out);
bool udf_read(struct volume *v, const struct node *file, u64 off, void *dst, u32 size);
#endif
