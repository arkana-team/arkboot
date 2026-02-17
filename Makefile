CC = gcc
AS = nasm
LD = ld

CFLAGS = -ffreestanding -nostdlib -nostartfiles -m16 -Os -fno-pie -fno-stack-protector
ASFLAGS = -f elf32
LDFLAGS = -nostdlib --oformat binary -m elf_i386

all: disk.img

disk.img: boot.bin loader.bin
	cat boot.bin loader.bin > disk.img
	truncate -s 10240 disk.img

boot.bin: boot.o
	$(LD) $(LDFLAGS) -T boot_linker.ld -o $@ $^

loader.bin: loader_entry.o loader.o loader_asm.o
	$(LD) $(LDFLAGS) -T loader_linker.ld -o $@ $^

loader_entry.o: loader_entry.asm
	$(AS) $(ASFLAGS) $< -o $@

loader_asm.o: loader_asm.asm
	$(AS) $(ASFLAGS) $< -o $@

loader.o: loader.c
	$(CC) $(CFLAGS) -c $< -o $@

boot.o: boot.asm
	$(AS) $(ASFLAGS) $< -o $@

clean:
	rm -f *.o boot.bin loader.bin disk.img

run: disk.img
	qemu-system-x86_64 -drive format=raw,file=disk.img

.PHONY: all clean run
