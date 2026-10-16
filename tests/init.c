/* A static PID 1 for the boot test. No libc, disk drivers or host files. */
static long syscall3(long n, long a, long b, long c) {
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return result;
}
_Noreturn void _start(void) {
    static const char ok[] = "ARKBOOT_USERSPACE_OK\n";
    static const char bad[] = "ARKBOOT_USERSPACE_FAILED\n";
    long pid = syscall3(39, 0, 0, 0);
    if (pid == 1) syscall3(1, 1, (long)ok, sizeof ok - 1);
    else syscall3(1, 1, (long)bad, sizeof bad - 1);
    for (;;) syscall3(34, 0, 0, 0);
}
