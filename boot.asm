bits 16

global start
global bios_print_char
global bios_read_sector
extern main

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    
    mov ah, 0x02
    mov al, 10
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov dl, 0x80
    mov bx, 0x7e00
    int 0x13

    jc load_error

    jmp 0x0000:0x7e00

load_error:
	mov si, errmsg
	call print_string
	jmp $

print_string:
	pusha
	mov ah, 0x0e
.loop:
	lodsb
	test al, al
	jz .done
	int 0x10
	jmp .loop
.done:
	popa
	ret

errmsg db 'E', 0

times 510-($-$$) db 0
dw 0xAA55
