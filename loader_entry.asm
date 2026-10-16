bits 16
section .start progbits alloc exec write align=16
global loader_start, bios_call, linux_enter
extern boot_main, __bss_start, __bss_end

loader_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x7000
    mov [boot_drive], dl
    mov [boot_block_size], cx
    ; Ask the BIOS first, then use the fast A20 gate as a fallback.
    mov ax, 0x2401
    int 0x15
    cli
    in al, 0x92
    and al, 0xfe
    or al, 2
    out 0x92, al
    xor ax, ax
    mov ds, ax
    lgdt [gdt_pointer]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:protected_start

align 8
gdt:
    dq 0
    dq 0x00cf9a000000ffff      ; 08: flat 32-bit executable
    dq 0x00cf92000000ffff      ; 10: flat 32-bit writable
    dq 0x00009a000000ffff      ; 18: 16-bit executable, base 0
    dq 0x000092000000ffff      ; 20: 16-bit writable, base 0
.end:
gdt_pointer:
    dw gdt.end-gdt-1
    dd gdt
boot_drive db 0
boot_block_size dw 0
align 4
saved_sp dd 0
saved_arg dd 0
saved_regs times 36 db 0

bits 32
protected_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x78000
    xor eax, eax
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    rep stosb
    movzx eax, word [boot_block_size]
    push eax
    movzx eax, byte [boot_drive]
    push eax
    call boot_main
.halt:
    cli
    hlt
    jmp .halt

; cdecl bios_call(vector, regs). Keep the bridge and its state below 64 KiB.
; BIOS calls are serialized; both the real-mode stack and transfer buffer are
; separate from core code, BSS, the protected-mode stack, and kernel memory.
bios_call:
    pushfd
    pushad
    cld
    mov [saved_sp], esp
    mov eax, [esp+40]
    mov [interrupt+1], al
    mov esi, [esp+44]
    mov [saved_arg], esi
    mov edi, saved_regs
    mov ecx, 9
    rep movsd
    jmp 0x18:protected16
bits 16
protected16:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr0
    and eax, 0xfffffffe
    mov cr0, eax
    jmp 0:real_mode
real_mode:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x7000
    mov eax, [cs:saved_regs]
    mov ebx, [cs:saved_regs+4]
    mov ecx, [cs:saved_regs+8]
    mov edx, [cs:saved_regs+12]
    mov esi, [cs:saved_regs+16]
    mov edi, [cs:saved_regs+20]
    mov ebp, [cs:saved_regs+24]
    mov ds, [cs:saved_regs+28]
    mov es, [cs:saved_regs+30]
    sti
interrupt:
    int 0x13
    pushfd
    cli
    pop dword [cs:saved_regs+32]
    mov [cs:saved_regs], eax
    mov [cs:saved_regs+4], ebx
    mov [cs:saved_regs+8], ecx
    mov [cs:saved_regs+12], edx
    mov [cs:saved_regs+16], esi
    mov [cs:saved_regs+20], edi
    mov [cs:saved_regs+24], ebp
    mov [cs:saved_regs+28], ds
    mov [cs:saved_regs+30], es
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    lgdt [cs:gdt_pointer]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:protected_return
bits 32
protected_return:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    cld
    mov esp, [saved_sp]
    mov esi, saved_regs
    mov edi, [saved_arg]
    mov ecx, 9
    rep movsd
    popad
    popfd
    ret

; Linux boot protocol selectors differ from the core's: CS=10, DS=18.
linux_enter:
    cli
    mov edx, [esp+4]
    mov esi, [esp+8]
    lgdt [linux_gdt_pointer]
    jmp 0x10:.linux_segments
.linux_segments:
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    xor ebp, ebp
    xor edi, edi
    xor ebx, ebx
    cld
    jmp edx
align 8
linux_gdt:
    dq 0, 0
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
.end:
linux_gdt_pointer:
    dw linux_gdt.end-linux_gdt-1
    dd linux_gdt
section .note.GNU-stack noalloc noexec nowrite progbits
