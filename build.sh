#!/bin/sh
# build.sh — compile Arthic and boot it in QEMU.
#
# Usage:  ./build.sh          build only
#         ./build.sh run      build, then boot it in QEMU
#
# On Arch you need:  sudo pacman -S base-devel qemu-system-x86 grub xorriso
#
# Note on compiler flags — these are what make this "freestanding":
#   -m32           build 32-bit code (the CPU is in 32-bit mode at boot)
#   -ffreestanding do not assume a standard library exists
#   -nostdlib      do not link the standard library or C startup files
#   -fno-builtin   do not silently swap our code for library calls
#   -Wall -Wextra  turn on warnings; in kernel code, listen to all of them

set -e

CFLAGS="-m32 -std=gnu11 -ffreestanding -mno-mmx -mno-sse -mno-sse2 -mno-80387 -fno-builtin -fno-stack-protector \
        -fno-pie -nostdlib -Wall -Wextra -O2"

echo "assembling boot.s ..."
gcc -m32 -c boot.s -o boot.o

echo "compiling kernel.c ..."
gcc $CFLAGS -c kernel.c -o kernel.o

echo "compiling gdt.c ..."
gcc $CFLAGS -c gdt.c -o gdt.o

echo "linking arthic.bin ..."
gcc -m32 -no-pie -Wl,--build-id=none -T linker.ld -o arthic.bin -ffreestanding -nostdlib -O2 \
    boot.o kernel.o gdt.o

echo
echo "built: arthic.bin"

if [ "$1" = "run" ]; then
	echo "starting qemu ... (close the window or press Ctrl-C to stop)"
	qemu-system-i386 -kernel arthic.bin
fi
