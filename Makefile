CC = gcc
AS = nasm
LD = ld
OBJCOPY = objcopy
PYTHON = python3

CFLAGS = -std=c11 -m32 -march=i386 -Os -ffreestanding -fno-builtin -fno-pie \
         -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
         -mno-sse -mno-mmx -msoft-float -Wall -Wextra -Werror -MMD -MP
LDFLAGS = -m elf_i386 -nostdlib -T loader_linker.ld
SHARED = common.c config.c boot_config.c menu.c disk.c
FILESYSTEM_SOURCES = fs/fs.c fs/fat.c fs/ext.c fs/iso9660.c fs/udf.c
SOURCES = loader.c runtime.c bios_disk.c $(SHARED) $(FILESYSTEM_SOURCES)
OBJECTS = $(SOURCES:%.c=build/%.o) build/loader_entry.o

all: bios uefi

bios: boot.bin cdboot.bin loader.bin
uefi: BOOTX64.EFI

# GNU-EFI supplies the x86-64 ABI definitions, startup and ELF relocation code.
EFI_INCLUDE ?= /usr/include/efi
EFI_LIB ?= /usr/lib
EFI_CFLAGS = -std=c11 -m64 -march=x86-64 -Os -ffreestanding -fno-builtin \
             -fpic -fshort-wchar -mno-red-zone -maccumulate-outgoing-args -fno-stack-protector \
             -fno-asynchronous-unwind-tables -fno-unwind-tables \
             -Wall -Wextra -Werror -MMD -MP -DGNU_EFI_USE_MS_ABI \
             -I$(EFI_INCLUDE) -I$(EFI_INCLUDE)/x86_64
EFI_SOURCES = efi/main.c efi/console.c efi/disk.c efi/linux.c $(SHARED) $(FILESYSTEM_SOURCES)
EFI_OBJECTS = $(EFI_SOURCES:%.c=build/uefi/%.o)

build/uefi/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(EFI_CFLAGS) -c $< -o $@

build/uefi/arkboot.so: $(EFI_OBJECTS)
	$(LD) -m elf_x86_64 -nostdlib -shared -Bsymbolic -z nocombreloc --no-undefined \
	    -T $(EFI_LIB)/elf_x86_64_efi.lds -Map build/uefi/arkboot.map \
	    $(EFI_LIB)/crt0-efi-x86_64.o $(EFI_OBJECTS) -L$(EFI_LIB) -lgnuefi -o $@

BOOTX64.EFI: build/uefi/arkboot.so
	$(OBJCOPY) -j .text -j .data -j .rodata -j .dynamic -j .dynsym \
	    -j .rel -j .rela -j .reloc --output-target=pei-x86-64 --subsystem=10 $< $@

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
UEFI ?=
IMAGE_FLAGS = $(if $(filter 1,$(UEFI)),--uefi,)
disk.img: all
	$(PYTHON) scripts/mkimage.py disk $(IMAGE_FLAGS) --root "$(ROOT)" --fs "$(FS)" --size-mib "$(SIZE_MIB)" --output $@

arkboot.iso: all
	$(PYTHON) scripts/mkimage.py iso $(IMAGE_FLAGS) --root "$(ROOT)" --output $@

run: disk.img
	qemu-system-x86_64 -m 128 -drive format=raw,file=disk.img -serial stdio

check: all
	$(PYTHON) tests/check.py

clean:
	rm -rf build/uefi BOOTX64.EFI build/*.o build/*.d build/fs build/loader.elf build/loader.map boot.bin cdboot.bin loader.bin disk.img arkboot.iso

-include $(OBJECTS:.o=.d) $(EFI_OBJECTS:.o=.d)
.PHONY: all bios uefi clean run check disk.img arkboot.iso
