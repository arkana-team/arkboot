#include "config.h"
#include "bios.h"

#define PARAM_SIZE 4096
#define E820_MAX 128
struct e820_entry { u64 address, size; u32 type; } __attribute__((packed));
static u8 params[PARAM_SIZE] __attribute__((aligned(16)));
static struct e820_entry memory[E820_MAX];
static u32 memory_count;
static struct boot_entry config;

static void memory_map(void) {
    struct bios_regs low = {0};
    bios_call(0x12, &low);
    if ((low.eax & 0xffff) < 512) panic("at least 512 KiB conventional memory is required");
    u32 continuation = 0;
    for (u32 calls = 0; calls < 256; calls++) {
        u8 *b = (u8 *)0x68100;
        memset(b, 0, 24); wr32(b + 20, 1);
        struct bios_regs r = { .eax = 0xe820, .ebx = continuation,
            .ecx = 24, .edx = 0x534d4150, .es = 0x6810 };
        bios_call(0x15, &r);
        if (r.flags & 1) {
            if (!memory_count) panic("BIOS E820 memory map is unavailable");
            break;
        }
        if (r.eax != 0x534d4150 || r.ecx < 20 || r.ecx > 24) panic("invalid BIOS E820 response");
        if (rd64(b + 8) && (r.ecx < 24 || (rd32(b + 20) & 1))) {
            if (memory_count == E820_MAX) panic("BIOS memory map exceeds 128 entries");
            memory[memory_count++] = (struct e820_entry){rd64(b), rd64(b + 8), rd32(b + 16)};
            if (memory[memory_count - 1].size > UINT64_MAX - memory[memory_count - 1].address)
                panic("overflow in BIOS memory map");
        }
        continuation = r.ebx;
        if (!continuation) break;
        if (calls == 255) panic("BIOS E820 enumeration did not terminate");
    }
    if (!memory_count) panic("empty BIOS memory map");
    params[0x1e8] = memory_count;
    memcpy(params + 0x2d0, memory, memory_count * sizeof memory[0]);
}
static bool usable(u64 address, u64 size) {
    if (!size || address + size < address) return false;
    bool found = false;
    for (u32 i = 0; i < memory_count; i++) {
        u64 start = memory[i].address, end = start + memory[i].size;
        if (memory[i].type == 1 && start <= address && end >= address + size) found = true;
        if (memory[i].type != 1 && start < address + size && end > address) return false;
    }
    return found;
}
static void check_a20(void) {
    volatile u32 *low = (volatile u32 *)0x500, *high = (volatile u32 *)0x100500;
    /* This is physical scratch memory, outside the compiler's object model. */
    __asm__ volatile("" : "+r"(low));
    u32 saved_low = *low, saved_high = *high;
    *low = 0x12345678; *high = 0x87654321;
    bool enabled = *low != *high;
    *high = saved_high; *low = saved_low;
    if (!enabled) panic("could not enable A20");
}
static u32 kernel_address(const u8 *h, u32 size) {
    u64 preferred = rd64(h + 0x258);
    if (!h[0x234]) {
        u64 base = preferred ? preferred : rd32(h + 0x214);
        if (base < 0x100000 || base > UINT32_MAX || size > 0x100000000ull - base ||
            !usable(base, size)) panic("fixed kernel runtime address is unavailable");
        return base;
    }
    u32 alignment = rd32(h + 0x230);
    if (!power2(alignment) || alignment > 0x40000000) panic("invalid kernel alignment");
    if (preferred >= 0x100000 && preferred <= UINT32_MAX && !(preferred & (alignment - 1)) &&
        size <= 0x100000000ull - preferred && usable(preferred, size)) return preferred;
    for (u32 i = 0; i < memory_count; i++) {
        if (memory[i].type != 1) continue;
        u64 floor = preferred > 0x100000 ? preferred : 0x100000;
        u64 base = memory[i].address < floor ? floor : memory[i].address;
        if (base > UINT32_MAX) continue;
        base = (base + alignment - 1) & ~((u64)alignment - 1);
        if (base <= UINT32_MAX && size <= 0x100000000ull - base && usable(base, size)) return base;
    }
    panic("no suitable RAM for kernel");
}
static u32 initrd_address(u32 size, u32 limit, u32 kernel, u32 reserved) {
    u64 best = 0;
    for (u32 i = 0; i < memory_count; i++) {
        if (memory[i].type != 1) continue;
        u64 end = memory[i].address + memory[i].size, maximum = (u64)limit + 1;
        if (end > maximum) end = maximum;
        for (u32 attempt = 0; attempt < 2; attempt++) {
            if (end < size) break;
            u64 start = (end - size) & ~4095ull;
            if (start < (u64)kernel + reserved && start + size > kernel) { end = kernel; continue; }
            if (start >= 0x100000 && start <= UINT32_MAX && usable(start, size) && start > best) best = start;
            break;
        }
    }
    if (!best) panic("no suitable RAM for initramfs");
    return best;
}
static _Noreturn void boot_linux(struct volume *v) {
    struct node kernel, initrd;
    u8 header[1024];
    if (!fs_open(v, config.kernel, &kernel)) panic(fs_error);
    if (kernel.directory || kernel.size < sizeof header || kernel.size > UINT32_MAX ||
        !fs_read(v, &kernel, 0, header, sizeof header)) panic("invalid or unreadable kernel");
    if (rd16(header + 0x1fe) != 0xaa55 || rd32(header + 0x202) != 0x53726448 ||
        rd16(header + 0x206) < 0x20a || !(header[0x211] & 1))
        panic("requires an x86 bzImage with Linux boot protocol 2.10 or newer");
    u32 header_end = 0x202 + header[0x201];
    if (header_end < 0x264 || header_end > 0x290) panic("unsupported Linux setup header size");
    memcpy(params + 0x1f1, header + 0x1f1, header_end - 0x1f1);
    u32 setup = ((header[0x1f1] ? header[0x1f1] : 4) + 1) * 512;
    if (setup >= kernel.size) panic("truncated kernel setup");
    u32 payload = kernel.size - setup, reserved = rd32(header + 0x260);
    if (reserved < payload) panic("invalid kernel initialization size");
    u32 address = kernel_address(header, reserved);
    print("Loading kernel at "); print_hex(address); print("\n");
    if (!fs_read(v, &kernel, setup, (void *)(uintptr_t)address, payload)) panic(fs_error);
    params[0x210] = 0xff; /* unassigned bootloader ID */
    params[0x211] &= ~0x20; /* allow early kernel console output */
    wr16(params + 0x1fa, 0xffff); /* preserve text mode */
    wr32(params + 0x214, address);
    wr32(params + 0x218, 0); wr32(params + 0x21c, 0);
    wr32(params + 0x250, 0); wr32(params + 0x254, 0); /* no setup_data chain */
    wr32(params + 0x23c, 0); /* PC hardware subarchitecture */
    if (strlen(config.cmdline) > rd32(header + 0x238)) panic("kernel command line exceeds kernel limit");
    wr32(params + 0x228, (uintptr_t)config.cmdline);
    if (config.initrd[0]) {
        if (!fs_open(v, config.initrd, &initrd)) {
            if (!fs_error || config.initrd_explicit || strcmp(fs_error, "file not found")) panic(fs_error);
        } else {
            if (initrd.directory || !initrd.size || initrd.size > UINT32_MAX) panic("invalid initramfs");
            u32 initrd_base = initrd_address(initrd.size, rd32(header + 0x22c), address, reserved);
            print("Loading initramfs at "); print_hex(initrd_base); print("\n");
            if (!fs_read(v, &initrd, 0, (void *)(uintptr_t)initrd_base, initrd.size)) panic(fs_error);
            wr32(params + 0x218, initrd_base); wr32(params + 0x21c, initrd.size);
        }
    }
    /* Minimal VGA text screen_info for a direct 32-bit protocol entry. */
    params[0x06] = 3; params[0x07] = 80; params[0x0e] = 25; params[0x0f] = 0x22;
    wr16(params + 0x10, 16);
    print("Starting Linux\n");
    linux_enter(address, params);
}

void boot_main(u32 drive, u32 block_size) {
    console_init();
    print("ARKBOOT\n");
    check_a20(); memory_map();
    struct disk disk;
    if (!disk_init(&disk, drive, block_size)) panic(fs_error);
    struct partition candidates[32];
    u32 count = partitions(&disk, candidates, 32);
    if (!count) panic(fs_error ? fs_error : "invalid partition table");
    for (u32 i = 0; i < count; i++) {
        struct volume v;
        if (!fs_mount(&v, &candidates[i])) {
            if (fs_error) { print("Skipping volume: "); print(fs_error); print("\n"); }
            continue;
        }
        print("Found "); print(fs_name(&v)); print("\n");
        if (!config_load(&v, &config)) continue;
        boot_linux(&v);
    }
    panic("no boot configuration found on boot drive");
}
