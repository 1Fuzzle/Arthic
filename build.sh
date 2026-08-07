#!/bin/sh
# build.sh — compile Arthic and boot it in QEMU.
#
# Usage:  ./build.sh          build only
#         ./build.sh run      build, then boot it in QEMU
#         ./build.sh clean    delete build products
#
# On Arch you need:  sudo pacman -S base-devel qemu-system-x86 qemu-ui-gtk
#
# Object files go in build/ rather than sitting next to the source. Keeps the
# tree clean and makes "delete everything generated" a one-liner.

set -e

BUILD=build
INCLUDE=include

# Compiler flags — these are what make this a kernel rather than a program:
#   -m32                      32-bit; the CPU is in 32-bit mode after GRUB
#   -ffreestanding            do not assume a standard library exists
#   -nostdlib                 do not link libc or the C startup files
#   -fno-builtin              do not silently swap our code for library calls
#   -mno-mmx -mno-sse ...     REQUIRED. Without these gcc vectorises loops into
#                             SSE instructions, which the CPU refuses to run
#                             before SSE is enabled in CR4 — triple fault.
#   -fno-stack-protector      needs __stack_chk_fail, which we have not written
#   -I$INCLUDE                where our headers live
#   -Wall -Wextra             warnings in kernel code are usually real bugs
CFLAGS="-m32 -std=gnu11 -ffreestanding -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
        -fno-builtin -fno-stack-protector -fno-pie -nostdlib \
        -I$INCLUDE -Wall -Wextra -O2"

ASFLAGS="-m32"

if [ "$1" = "clean" ]; then
	rm -rf "$BUILD" arthic.bin
	echo "cleaned"
	exit 0
fi

mkdir -p "$BUILD"

# Assembly sources
for src in boot/boot.s kernel/interrupts.s; do
	obj="$BUILD/$(basename "$src" .s).o"
	echo "assembling $src"
	gcc $ASFLAGS -c "$src" -o "$obj"
done

# C sources
for src in kernel/main.c kernel/gdt.c kernel/idt.c kernel/shell.c \
           drivers/terminal.c drivers/keyboard.c drivers/timer.c \
           mm/pmm.c lib/string.c; do
	obj="$BUILD/$(basename "$src" .c).o"
	echo "compiling  $src"
	gcc $CFLAGS -c "$src" -o "$obj"
done

echo "linking    arthic.bin"
gcc -m32 -no-pie -Wl,--build-id=none -T linker.ld -o arthic.bin \
    -ffreestanding -nostdlib -O2 "$BUILD"/*.o

echo
echo "built: arthic.bin"

if [ "$1" = "run" ]; then
	echo "starting qemu ... (close the window or press Ctrl-C to stop)"
	qemu-system-i386 -no-reboot -m 128M -kernel arthic.bin
fi
