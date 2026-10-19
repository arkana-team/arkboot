#include "platform.h"

void *memcpy(void *dst, const void *src, size_t n) {
    u8 *d = dst;
    const u8 *s = src;
    while (n--) *d++ = *s++;
    return dst;
}
void *memset(void *dst, int c, size_t n) {
    u8 *d = dst;
    while (n--) *d++ = c;
    return dst;
}
int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; x++; y++; }
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}

/* Freestanding i386 has no implicit libgcc dependency. */
#ifdef __i386__
static u64 divide(u64 n, u64 d, u64 *rem) {
    u64 q = 0, r = 0;
    if (!d) panic("division by zero");
    for (int i = 63; i >= 0; i--) {
        bool carry = r >> 63;
        r = (r << 1) | ((n >> i) & 1);
        if (carry || r >= d) { r -= d; q |= (u64)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}
u64 __udivdi3(u64 n, u64 d) { return divide(n, d, NULL); }
u64 __umoddi3(u64 n, u64 d) { u64 r; divide(n, d, &r); return r; }
u64 __udivmoddi4(u64 n, u64 d, u64 *r) { return divide(n, d, r); }

#endif
void print_hex(u32 n) {
    char s[11] = "0x00000000";
    for (u32 i = 0; i < 8; i++) s[9 - i] = "0123456789abcdef"[(n >> (4 * i)) & 15];
    print(s);
}
_Noreturn void panic(const char *s) {
    print("ARKBOOT: "); print(s ? s : "unspecified boot failure"); print("\n");
    for (;;) __asm__ volatile("cli; hlt");
}
