bits 16

global loader_start
extern main
extern bios_print_char

loader_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x8000
    sti

    call main

    jmp $
