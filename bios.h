#ifndef BIOS_H
#define BIOS_H

#include <stdint.h>

extern void bios_print_char(char c);
extern void bios_carriage();

void print(const char *str);

#endif
