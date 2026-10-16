#!/bin/sh
# Pin the test source and keep it on disk: /tmp may be a RAM-backed filesystem.
set -eu
cd "$(dirname "$0")/.."
work="$PWD/build/validation"
mkdir -p "$work/tmp"
export TMPDIR="$work/tmp"
available=$(awk '/MemAvailable:/ { print $2 }' /proc/meminfo)
if [ "$available" -lt 1572864 ]; then
    echo 'Need at least 1.5 GiB available RAM before building the test kernel.' >&2
    exit 1
fi
jobs=${JOBS:-2}
case "$jobs" in 1|2) ;; *) echo 'JOBS must be 1 or 2 to bound RAM use.' >&2; exit 1;; esac
if [ ! -d "$work/linux-6.12" ]; then
    if [ ! -f "$work/linux-6.12.tar.xz" ]; then
        curl -fL --retry 2 https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.tar.xz -o "$work/linux-6.12.tar.xz.part"
        mv "$work/linux-6.12.tar.xz.part" "$work/linux-6.12.tar.xz"
    fi
    tar -xJf "$work/linux-6.12.tar.xz" -C "$work"
fi
# Bound any individual compiler/linker process to 1 GiB of address space.
ulimit -v 1048576
make -C "$work/linux-6.12" ARCH=x86_64 CC="gcc -std=gnu11" HOSTCC="gcc -std=gnu11" \
    KCONFIG_ALLCONFIG="$PWD/tests/linux.config" allnoconfig > "$work/linux-config.log" 2>&1
make -C "$work/linux-6.12" ARCH=x86_64 CC="gcc -std=gnu11" HOSTCC="gcc -std=gnu11" -j"$jobs" bzImage > "$work/linux-build.log" 2>&1
mkdir -p "$work/root/boot"
cp "$work/linux-6.12/arch/x86/boot/bzImage" "$work/root/boot/vmlinuz"
python3 tests/mkinitramfs.py "$work/root/boot/initrd.img"
cat > "$work/root/boot/arkboot.cfg" <<'CONFIG'
kernel=/boot/vmlinuz
initrd=/boot/initrd.img
cmdline=console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 panic=-1
CONFIG
printf 'Kernel, userspace and boot configuration are in %s/root/boot\n' "$work"
