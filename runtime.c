#include "bios.h"

static inline void outb(u16 port, u8 value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
static inline u8 inb(u16 port) {
    u8 value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static u32 cursor;
void console_init(void) {
    outb(0x3f9, 0); outb(0x3fb, 0x80);
    outb(0x3f8, 1); outb(0x3f9, 0);
    outb(0x3fb, 3); outb(0x3fa, 0xc7); outb(0x3fc, 3);
    struct bios_regs r = { .eax = 3 };
    bios_call(0x10, &r);
    cursor = 0;
}
static void character(char c) {
    volatile u16 *vga = (volatile u16 *)0xb8000;
    for (u32 i = 0; i < 100000; i++) {
        if (inb(0x3fd) & 0x20) { outb(0x3f8, c); break; }
    }
    if (c == '\r') cursor -= cursor % 80;
    else if (c == '\n') cursor += 80 - cursor % 80;
    else vga[cursor++] = 0x0700 | (u8)c;
    if (cursor >= 80 * 25) {
        for (u32 i = 0; i < 80 * 24; i++) vga[i] = vga[i + 80];
        for (u32 i = 80 * 24; i < 80 * 25; i++) vga[i] = 0x0720;
        cursor -= 80;
    }
}
void print(const char *s) {
    while (*s) { if (*s == '\n') character('\r'); character(*s++); }
}
void console_clear(void) {
    /* ANSI clear/home for serial terminals, direct text buffer for VGA. */
    print("\033[2J\033[H");
    volatile u16 *vga = (volatile u16 *)0xb8000;
    for (u32 i = 0; i < 80 * 25; i++) vga[i] = 0x0720;
    cursor = 0;
}
int console_key(void) {
    static u32 escape;
    if (inb(0x3fd) & 1) {
        u8 c = inb(0x3f8);
        if (c == 27) { escape = 1; return 27; }
        if (escape == 1 && (c == '[' || c == 'O')) { escape = 2; return -1; }
        if (escape == 2) {
            escape = 0;
            if (c == 'A') return 0x4800;
            if (c == 'B') return 0x5000;
            if (c == 'H') return 0x4700;
            if (c == 'F') return 0x4f00;
        }
        escape = 0;
        return c;
    }
    struct bios_regs r = { .eax = 0x100 };
    bios_call(0x16, &r);
    if (r.flags & 0x40) return -1;
    r.eax = 0;
    bios_call(0x16, &r);
    return (r.eax & 0xff) ? r.eax & 0xff : r.eax & 0xff00;
}
u32 console_millis(void) {
    static u32 previous, ticks;
    struct bios_regs r = {0};
    bios_call(0x1a, &r);
    u32 now = ((r.ecx & 0xffff) << 16) | (r.edx & 0xffff);
    ticks += now >= previous ? now - previous : 0x1800b0 - previous + now;
    previous = now;
    return (u64)ticks * 10000 / 182;
}
