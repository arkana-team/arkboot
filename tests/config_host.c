#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../config.h"

static struct boot_config config;
static const char *parse(const char *source, bool yaml) {
    static char text[BOOT_CONFIG_MAX];
    assert(strlen(source) < sizeof text);
    strcpy(text, source);
    u32 line;
    return config_parse(&config, text, yaml, &line);
}
int main(void) {
    assert(!parse("default: 1\ntimeout: -1\nentries:\n"
                  "  - title: 'Linux: normal' # comment\n"
                  "    kernel: /vmlinuz\n"
                  "    cmdline: \"console=ttyS0 path=\\\"a\\\"\"\n"
                  "  - title: 'It''s rescue'\n"
                  "    kernel: /rescue\n"
                  "    initrd: ''\n", true));
    assert(config.count == 2 && config.default_entry == 1 && config.timeout == -1);
    assert(!strcmp(config.entries[0].title, "Linux: normal"));
    assert(!strcmp(config.entries[0].cmdline, "console=ttyS0 path=\"a\""));
    assert(!strcmp(config.entries[1].title, "It's rescue"));
    assert(!config.entries[0].initrd[0] && config.entries[0].initrd_explicit);
    assert(!parse("kernel=/linux\ncmdline=hi # literal\n", false));
    assert(config.count == 1 && !config.timeout && !config.entries[0].initrd_explicit);
    assert(!strcmp(config.entries[0].initrd, "/boot/initrd.img"));
    assert(!strcmp(config.entries[0].cmdline, "hi # literal"));
    assert(!parse("\tkernel \t= /linux\t\ncmdline=one\ttwo\n", false));
    assert(!strcmp(config.entries[0].cmdline, "one\ttwo"));
    const char *bad[] = {
        "\tentries:\n  - title: Linux\n    kernel: /linux\n",
        "", "entries:\n", "entries: []\n", "default: 1\nentries:\n  - title: Linux\n    kernel: /linux\n",
        "timeout: 3601\n", "timeout: -2\n", "timeout: 99999999999999999999999999\n",
        "entries:\n  - title: Linux\n", "entries:\n  - kernel: /linux\n",
        "entries:\n  - title: Linux\n    kernel: /linux\n    kernel: /other\n",
        "entries:\nentries:\n", "entries:\n    kernel: /linux\n", "  - title: Linux\n",
        "entries:\n  - title: 'unterminated\n", "entries:\n  - title: \"bad\\n\"\n",
        "entries:\n  - title: Linux\n    kernel: /linux\n    extra: bad\n",
        "entries:\n  - title: Linux\n    kernel: &anchor\n", "entries:\n\t- title: Linux\n",
        "entries:\n  - title: Linux\n    kernel: /linux\n    cmdline: nested: value\n",
        "entries:\n  - title: Linux\n    kernel: /linux\n    cmdline: [a, b]\n",
    };
    for (u32 i = 0; i < sizeof bad / sizeof bad[0]; i++) assert(parse(bad[i], true));
    char many[4096] = "entries:\n";
    for (u32 i = 0; i < BOOT_ENTRIES_MAX; i++) strcat(many, "  - title: Linux\n    kernel: /linux\n");
    assert(!parse(many, true) && config.count == BOOT_ENTRIES_MAX);
    strcat(many, "  - title: Excess\n    kernel: /linux\n");
    assert(parse(many, true));
    char long_value[4096] = "entries:\n  - title: Linux\n    kernel: ";
    u32 n = strlen(long_value);
    memset(long_value + n, 'a', FS_PATH_MAX + 1);
    long_value[n + FS_PATH_MAX + 1] = 0;
    assert(parse(long_value, true));
    puts("PASS boot configuration parsing and bounds");
}
