#include "config.h"

static char config_text[BOOT_CONFIG_MAX];
static struct boot_config boot_config;

bool config_load(struct volume *v, struct boot_entry *selected) {
    static const char *const paths[] = {
        "/boot/arkboot.yaml", "/boot/arkboot.yml", "/arkboot.yaml", "/arkboot.yml",
        "/boot/arkboot.cfg", "/arkboot.cfg"
    };
    struct node cfg;
    u32 path;
    for (path = 0; path < sizeof paths / sizeof paths[0]; path++) {
        if (fs_open(v, paths[path], &cfg)) break;
        if (!fs_error || strcmp(fs_error, "file not found")) panic(fs_error);
    }
    if (path == sizeof paths / sizeof paths[0]) return false;
    print("Configuration: "); print(paths[path]); print("\n");
    if (cfg.directory) panic("boot configuration is a directory");
    if (cfg.size >= sizeof config_text || !fs_read(v, &cfg, 0, config_text, cfg.size))
        panic("cannot read boot configuration (maximum 16383 bytes)");
    for (u32 i = 0; i < cfg.size; i++)
        if (!config_text[i]) panic("NUL byte in boot configuration");
    config_text[cfg.size] = 0;
    u32 line;
    const char *err = config_parse(&boot_config, config_text, path < 4, &line);
    if (err) { print("Configuration error near line "); print_hex(line); print("\n"); panic(err); }
    *selected = boot_config.entries[menu_select(&boot_config)];
    return true;
}
