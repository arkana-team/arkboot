bits 16

global bios_print_char
global bios_carriage

bios_carriage:
    push bp
    mov bp, sp
    pusha

    mov ah, 0x03
    mov bh, 0x00
    int 0x10
    
    mov dl, 0x00
    mov ah, 0x02
    mov bh, 0x00
    int 0x10
    
    popa
    pop bp
    ret
    
bios_print_char:
    pusha
    mov ah, 0x0E
    mov bh, 0x00
    mov bl, 0x07
    int 0x10
    popa
    ret
