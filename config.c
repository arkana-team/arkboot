#include "config.h"

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = 0;
    return s;
}
static bool copy(char *out, u32 capacity, const char *value) {
    u32 length = strlen(value);
    if (length >= capacity) return false;
    memcpy(out, value, length + 1);
    return true;
}
/* Deliberately bounded YAML block syntax; no implicit scalar conversions. */
static const char *scalar(char *s) {
    char *out = s;
    if (*s == '\'' || *s == '"') {
        char quote = *s++;
        while (*s) {
            if (*s == quote) {
                s++;
                if (quote == '\'' && *s == '\'') { *out++ = *s++; continue; }
                s = trim(s);
                if (*s && *s != '#') return "unexpected text after quoted value";
                *out = 0;
                return NULL;
            }
            if (quote == '"' && *s == '\\') {
                s++;
                if (*s != '\\' && *s != '"') return "only escaped quote and backslash are supported";
            }
            if ((u8)*s < 32 || (u8)*s == 127) return "control character in YAML value";
            *out++ = *s++;
        }
        return "unterminated quoted value";
    }
    if (*s == '[' || *s == '{' || *s == '&' || *s == '*' || *s == '!' ||
        *s == '|' || *s == '>' || *s == '@' || *s == '`' || *s == '%' ||
        (*s == '-' && (!s[1] || s[1] == ' ')) || *s == '?' || *s == ':')
        return "unsupported YAML scalar; quote literal special characters";
    for (char *p = s; *p; p++) {
        if (*p == '#' && (p == s || p[-1] == ' ')) { *p = 0; break; }
        if (*p == ':' && (!p[1] || p[1] == ' ')) return "quote values containing colon followed by space";
        if ((u8)*p < 32 || (u8)*p == 127) return "control character in YAML value";
    }
    trim(s);
    return NULL;
}
static bool number(const char *s, u32 maximum, u32 *value) {
    u32 n = 0;
    if (!*s) return false;
    while (*s) {
        if (*s < '0' || *s > '9') return false;
        u32 digit = *s++ - '0';
        if (digit > maximum || n > (maximum - digit) / 10) return false;
        n = n * 10 + digit;
    }
    *value = n;
    return true;
}
const char *config_parse(struct boot_config *c, char *text, bool yaml, u32 *line_number) {
    memset(c, 0, sizeof *c);
    c->timeout = yaml ? 5 : 0;
    u32 root_seen = 0, fields[BOOT_ENTRIES_MAX] = {0};
    bool in_entries = false;
    if (!yaml) {
        c->count = 1;
        copy(c->entries[0].title, sizeof c->entries[0].title, "Linux");
        copy(c->entries[0].kernel, sizeof c->entries[0].kernel, "/boot/vmlinuz");
        copy(c->entries[0].initrd, sizeof c->entries[0].initrd, "/boot/initrd.img");
        copy(c->entries[0].cmdline, sizeof c->entries[0].cmdline, "console=tty0");
    }
    *line_number = 0;
    for (char *line = text; *line;) {
        (*line_number)++;
        char *next = line;
        while (*next && *next != '\n') next++;
        if (*next) *next++ = 0;
        if (yaml) for (char *p = line; *p; p++)
            if (*p == '\t') return "tabs are not supported in YAML";
        u32 indent = 0;
        while (line[indent] == ' ') indent++;
        char *key = trim(line + indent);
        line = next;
        if (!*key || *key == '#') continue;
        if (yaml && indent == 2 && key[0] == '-' && key[1] == ' ') {
            if (!in_entries) return "entry outside entries list";
            if (c->count == BOOT_ENTRIES_MAX) return "too many boot entries (maximum 16)";
            struct boot_entry *e = &c->entries[c->count++];
            copy(e->cmdline, sizeof e->cmdline, "console=tty0");
            e->initrd_explicit = true;
            key += 2;
        } else if (yaml && indent && (indent != 4 || !in_entries || !c->count)) {
            return "YAML requires two spaces before - and four before entry fields";
        }
        char *value = key;
        while (*value && *value != (yaml ? ':' : '=')) value++;
        if (!*value) return yaml ? "expected key: value" : "configuration requires key=value";
        *value++ = 0; key = trim(key); value = trim(value);
        if (yaml) {
            const char *err = scalar(value);
            if (err) return err;
        }
        if (yaml && !indent) {
            u32 bit, n;
            in_entries = false;
            if (!strcmp(key, "entries")) {
                bit = 1;
                if (*value) return "entries must be a block sequence";
                in_entries = true;
            } else if (!strcmp(key, "default")) {
                bit = 2;
                if (!number(value, BOOT_ENTRIES_MAX - 1, &n)) return "default must be an entry index (0-15)";
                c->default_entry = n;
            } else if (!strcmp(key, "timeout")) {
                bit = 4;
                if (!strcmp(value, "-1")) c->timeout = -1;
                else {
                    if (!number(value, 3600, &n)) return "timeout must be -1 or 0-3600 seconds";
                    c->timeout = n;
                }
            } else return "unknown menu key";
            if (root_seen & bit) return "duplicate menu key";
            root_seen |= bit;
            continue;
        }
        struct boot_entry *e = &c->entries[c->count - 1];
        u32 bit, capacity;
        char *dest;
        if (!strcmp(key, "title") && yaml) { bit = 1; dest = e->title; capacity = sizeof e->title; }
        else if (!strcmp(key, "kernel")) { bit = 2; dest = e->kernel; capacity = sizeof e->kernel; }
        else if (!strcmp(key, "initrd")) {
            bit = 4; dest = e->initrd; capacity = sizeof e->initrd; e->initrd_explicit = true;
        } else if (!strcmp(key, "cmdline")) { bit = 8; dest = e->cmdline; capacity = sizeof e->cmdline; }
        else return "unknown boot entry key";
        if (yaml && (fields[c->count - 1] & bit)) return "duplicate boot entry key";
        fields[c->count - 1] |= bit;
        if (!copy(dest, capacity, value)) return "configuration value too long";
    }
    if (!c->count) return "entries list is empty or missing";
    if (c->default_entry >= c->count) return "default entry index is outside entries list";
    for (u32 i = 0; i < c->count; i++) {
        struct boot_entry *e = &c->entries[i];
        if (yaml && !e->title[0]) return "each entry requires a nonempty title";
        if (!e->kernel[0]) return "each entry requires a nonempty kernel path";
        for (char *p = e->title; *p; p++)
            if ((u8)*p < 32 || (u8)*p > 126) return "menu titles must use printable ASCII";
    }
    return NULL;
}
