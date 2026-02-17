#include "bios.h"
__asm__(".code16gcc\n");

void print(const char *str) {
    while (*str) {
        if (*str == '\n') {
            bios_carriage();
            bios_print_char('\n');
        } else {
            bios_print_char(*str);
        }
        str++;
    }
}

int main() {
    print("ARKBOOT\n");
    print("Please implement proper loading mechanisms.\n");  // stub

    for (;;) {
        __asm__ volatile("hlt");
    }
}
