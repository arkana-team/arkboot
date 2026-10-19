#ifndef ARKBOOT_BIOS_H
#define ARKBOOT_BIOS_H
#include "platform.h"

struct bios_regs {
    u32 eax, ebx, ecx, edx, esi, edi, ebp;
    u16 ds, es;
    u32 flags;
};

_Static_assert(sizeof(struct bios_regs) == 36 && offsetof(struct bios_regs, flags) == 32,
               "BIOS bridge register layout changed");

void bios_call(u32 vector, struct bios_regs *regs);
_Noreturn void linux_enter(u32 entry, void *params);
#endif
