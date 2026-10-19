#ifndef ARKBOOT_CONFIG_H
#define ARKBOOT_CONFIG_H
#include "fs.h"

#define BOOT_ENTRIES_MAX 16
#define BOOT_CONFIG_MAX 16384
struct boot_entry {
    char title[64];
    char kernel[FS_PATH_MAX + 1];
    char initrd[FS_PATH_MAX + 1];
    char cmdline[2048];
    bool initrd_explicit;
};
struct boot_config {
    struct boot_entry entries[BOOT_ENTRIES_MAX];
    u32 count, default_entry;
    int timeout;
};
/* Parses in place. Returns an error string, or NULL on success. */
const char *config_parse(struct boot_config *config, char *text, bool yaml, u32 *line);
u32 menu_select(const struct boot_config *config);
#endif
