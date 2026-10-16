OUTPUT_FORMAT(elf32-i386)
ENTRY(loader_start)
SECTIONS {
    . = 0x8000;
    .start : { KEEP(*(.start)) }
    ASSERT(. < 0x10000, "BIOS bridge must remain below 64 KiB")
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    . = ALIGN(16);
    __bss_start = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
    __bss_end = .;
    ASSERT(. <= 0x60000, "core overlaps BIOS transfer buffer")
    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment) }
}
