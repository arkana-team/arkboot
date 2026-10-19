#include "config.h"

static void decimal(u32 n) {
    char digits[11];
    u32 length = 0;
    do { digits[length++] = '0' + n % 10; n /= 10; } while (n);
    while (length) { char s[2] = {digits[--length], 0}; print(s); }
}
static void draw(const struct boot_config *c, u32 selected, int remaining) {
    console_clear();
    print("ARKBOOT - Boot menu\n\n");
    for (u32 i = 0; i < c->count; i++) {
        print(i == selected ? "  > " : "    ");
        print(c->entries[i].title); print("\n");
    }
    print("\nUp/Down or W/S: select    Enter: boot\n");
    if (remaining >= 0) {
        print("Booting selected entry in "); decimal(remaining);
        print(" seconds. Any key stops the timer.\n");
    } else print("Automatic boot stopped. Select an entry and press Enter.\n");
}
u32 menu_select(const struct boot_config *c) {
    u32 selected = c->default_entry;
    if (!c->timeout) return selected;
    u32 last = console_millis(), elapsed = 0;
    u32 duration = c->timeout > 0 ? (u32)c->timeout * 1000 : 0;
    int remaining = c->timeout;
    draw(c, selected, remaining);
    for (;;) {
        int key = console_key();
        if (key >= 0) {
            duration = 0; remaining = -1;
            if (key == '\r' || key == '\n') break;
            if (key == 0x4800 || key == 'w' || key == 'W')
                selected = selected ? selected - 1 : c->count - 1;
            else if (key == 0x5000 || key == 's' || key == 'S')
                selected = (selected + 1) % c->count;
            else if (key == 0x4700) selected = 0;
            else if (key == 0x4f00) selected = c->count - 1;
            draw(c, selected, remaining);
        }
        if (duration) {
            u32 now = console_millis();
            elapsed += now - last;
            last = now;
            if (elapsed >= duration) break;
            int seconds = (duration - elapsed + 999) / 1000;
            if (seconds != remaining) { remaining = seconds; draw(c, selected, remaining); }
        }
    }
    print("\nBooting "); print(c->entries[selected].title); print("\n");
    return selected;
}
