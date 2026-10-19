CC = gcc
AS = nasm
LD = ld
OBJCOPY = objcopy
PYTHON = python3

CFLAGS = -std=c11 -m32 -march=i386 -Os -ffreestanding -fno-builtin -fno-pie \
         -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
         -mno-sse -mno-mmx -msoft-float -Wall -Wextra -Werror -MMD -MP
LDFLAGS = -m elf_i386 -nostdlib -T loader_linker.ld
SOURCES = loader.c config.c menu.c runtime.c disk.c fs/fs.c fs/fat.c fs/ext.c fs/iso9660.c fs/udf.c
OBJECTS = $(SOURCES:%.c=build/%.o) build/loader_entry.o

all: boot.bin cdboot.bin loader.bin

build/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/loader_entry.o: loader_entry.asm
	@mkdir -p $(@D)
	$(AS) -f elf32 $< -o $@

build/loader.elf: $(OBJECTS) loader_linker.ld
	$(LD) $(LDFLAGS) -Map build/loader.map -o $@ $(OBJECTS)

loader.bin: build/loader.elf
	$(OBJCOPY) -O binary $< $@

boot.bin: boot.asm loader.bin
	$(AS) -f bin -D BLOCK_SIZE=512 -D CORE_BLOCKS=$$((($$(stat -c %s loader.bin)+511)/512)) $< -o $@

cdboot.bin: boot.asm loader.bin
	$(AS) -f bin -D CD_BOOT=1 -D BLOCK_SIZE=2048 -D CORE_BLOCKS=$$((($$(stat -c %s loader.bin)+2047)/2048)) $< -o $@

# ROOT is a directory containing boot/arkboot.yaml (or .cfg), kernel and optional initramfs.
# Images are generated files only; no target writes to physical devices.
FS ?= fat32
ROOT ?= root
SIZE_MIB ?= 128
disk.img: all
	$(PYTHON) scripts/mkimage.py disk --root "$(ROOT)" --fs "$(FS)" --size-mib "$(SIZE_MIB)" --output $@

arkboot.iso: all
	$(PYTHON) scripts/mkimage.py iso --root "$(ROOT)" --output $@

run: disk.img
	qemu-system-x86_64 -m 128 -drive format=raw,file=disk.img -serial stdio

check: all
	$(PYTHON) tests/check.py

clean:
	rm -rf build/*.o build/*.d build/fs build/loader.elf build/loader.map boot.bin cdboot.bin loader.bin disk.img arkboot.iso

-include $(OBJECTS:.o=.d)
.PHONY: all clean run check disk.img arkboot.iso
