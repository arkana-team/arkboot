; The same bootstrap is assembled for disks (512-byte blocks) and El Torito
; no-emulation CDs (2048-byte blocks). The installer reserves the whole core.
bits 16
org 0x7c00

jmp short start
nop
times 8-($-$$) db 0
; xorriso -boot-info-table fills these fields in the CD boot image.
boot_info:
    dd 1                    ; disk core LBA; CD tooling replaces with PVD LBA
    dd 0                    ; boot image LBA
    dd 0                    ; boot image size
    dd 0                    ; checksum
    times 40 db 0
start:
    jmp 0:normalized
normalized:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x7c00
    cld
    sti
    mov [drive], dl
    mov ah, 0x41
    mov bx, 0x55aa
    int 0x13
    jc error
    cmp bx, 0xaa55
    jne error
    test cx, 1
    jz error
%ifdef CD_BOOT
    mov eax, [boot_info+4]
    inc eax
    mov [dap_lba], eax
%else
    mov eax, [boot_info]
    mov [dap_lba], eax
%endif
    mov bp, CORE_BLOCKS
.next:
    mov byte [tries], 3
.retry:
    mov word [dap_count], 1
    mov si, dap
    mov dl, [drive]
    mov ah, 0x42
    int 0x13
    jnc .loaded
    mov dl, [drive]
    xor ah, ah
    int 0x13
    dec byte [tries]
    jnz .retry
    jmp error
.loaded:
    add word [dap_segment], BLOCK_SIZE / 16
    add dword [dap_lba], 1
    adc dword [dap_lba+4], 0
    dec bp
    jnz .next
    mov dl, [drive]
    mov cx, BLOCK_SIZE
    jmp 0:0x8000
error:
    mov si, message
.print:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0e
    mov bx, 7
    int 0x10
    jmp .print
.halt:
    cli
    hlt
    jmp .halt
message db 'ARKBOOT: BIOS disk read failed', 0
align 4
dap:
    db 16, 0
dap_count: dw 1
    dw 0
dap_segment: dw 0x0800
dap_lba: dq 1
drive db 0
tries db 0
; Preserve the MBR disk signature and partition-table area for image tooling.
times 440-($-$$) db 0
times 70 db 0
dw 0xaa55
