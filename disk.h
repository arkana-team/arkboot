#ifndef ARKBOOT_DISK_H
#define ARKBOOT_DISK_H
#include "platform.h"
struct disk { u8 drive; u32 block_size; u64 blocks; void *context; };
struct partition { struct disk *disk; u64 base, size; };
extern const char *fs_error;
bool error(const char *s);
bool disk_init(struct disk *d, u8 drive, u32 block_size);
bool disk_read(struct disk *d, u64 offset, void *buffer, u32 size);
bool part_read(const struct partition *p, u64 offset, void *buffer, u32 size);
/* Includes the unpartitioned device; returns 0 on invalid partition metadata. */
u32 partitions(struct disk *d, struct partition *out, u32 capacity);
#endif
