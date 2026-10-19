#ifndef ARKBOOT_BIOS_H
#define ARKBOOT_BIOS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

struct bios_regs {
    u32 eax, ebx, ecx, edx, esi, edi, ebp;
    u16 ds, es;
    u32 flags;
};

_Static_assert(sizeof(struct bios_regs) == 36 && offsetof(struct bios_regs, flags) == 32,
               "BIOS bridge register layout changed");

void bios_call(u32 vector, struct bios_regs *regs);
_Noreturn void linux_enter(u32 entry, void *params);
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
void console_init(void);
void console_clear(void);
int console_key(void);
u32 console_ticks(void);
void print(const char *s);
void print_hex(u32 n);
_Noreturn void panic(const char *s);

static inline u16 rd16(const void *p) {
    const u8 *b = p;
    return (u16)b[0] | (u16)b[1] << 8;
}
static inline u32 rd32(const void *p) {
    const u8 *b = p;
    return (u32)rd16(b) | (u32)rd16(b + 2) << 16;
}
static inline u64 rd64(const void *p) {
    const u8 *b = p;
    return (u64)rd32(b) | (u64)rd32(b + 4) << 32;
}
static inline void wr16(void *p, u16 n) { memcpy(p, &n, 2); }
static inline void wr32(void *p, u32 n) { memcpy(p, &n, 4); }
static inline u32 min32(u32 a, u32 b) { return a < b ? a : b; }
static inline bool power2(u32 n) { return n && !(n & (n - 1)); }
#endif
