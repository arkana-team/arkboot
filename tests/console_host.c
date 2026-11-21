#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../platform.h"

/* Host harness for common.c's number formatting: print_hex and print_u32. */
static char output[256];
static size_t used;
void print(const char *s) {
    size_t n = strlen(s);
    if (used + n + 1 < sizeof output) {
        memcpy(output + used, s, n);
        used += n;
        output[used] = 0;
    }
}
static void reset(void) { used = 0; output[0] = 0; }
static void check(const char *label, const char *actual, const char *expected) {
    if (strcmp(actual, expected)) {
        fprintf(stderr, "%s: got \"%s\", want \"%s\"\n", label, actual, expected);
        exit(1);
    }
}
int main(void) {
    reset(); print_u32(0); check("print_u32(0)", output, "0");
    reset(); print_u32(16); check("print_u32(16)", output, "16");
    reset(); print_u32(123456789); check("print_u32(123456789)", output, "123456789");
    reset(); print_u32(4294967295u); check("print_u32(UINT32_MAX)", output, "4294967295");
    reset(); print_hex(16); check("print_hex(16)", output, "0x00000010");
    reset(); print_hex(0xffffffffu); check("print_hex(UINT32_MAX)", output, "0xffffffff");
    puts("console formatting checks passed");
    return 0;
}
