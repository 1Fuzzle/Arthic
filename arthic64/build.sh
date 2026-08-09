#!/bin/sh
# build.sh - Arthic 64.
#
# Usage:  ./build.sh          build only
#         ./build.sh run      build, then boot it in QEMU
#         ./build.sh clean    delete build products
#
# On Arch:  sudo pacman -S base-devel qemu-system-x86 qemu-ui-gtk
#
# Note it is qemu-system-x86_64 now, not i386 - the kernel is 64-bit.

set -e

BUILD=build
INCLUDE=include

# Flags that differ from the 32-bit build, and why:
#   -m64                  obviously
#   -mno-red-zone         REQUIRED. The 64-bit ABI lets a function use 128
#                         bytes below the stack pointer without reserving it.
#                         An interrupt pushes onto that same area, silently
#                         corrupting whatever was there. Every 64-bit kernel
#                         turns this off; forgetting produces bugs that appear
#                         only when an interrupt lands in the wrong instruction.
#   -mgeneral-regs-only   no SSE registers, which are not saved on interrupt
#   -fno-pie -no-pie      no position-independent code
CFLAGS="-m64 -std=gnu11 -ffreestanding -nostdlib -fno-builtin \
        -fno-stack-protector -mno-red-zone -mgeneral-regs-only \
        -fno-pie -I$INCLUDE -Wall -Wextra -O2"

if [ "$1" = "clean" ]; then
	rm -rf "$BUILD" arthic64.bin arthic64-boot.bin
	echo "cleaned"
	exit 0
fi

mkdir -p "$BUILD"

for src in boot/boot.s kernel/interrupts.s kernel/syscall_entry.s kernel/switch.s; do
	obj="$BUILD/$(basename "$src" .s).o"
	echo "assembling $src"
	gcc -m64 -c "$src" -o "$obj"
done

for src in kernel/main.c kernel/idt.c kernel/shell.c \
           mm/pmm.c mm/paging.c mm/kheap.c \
           kernel/gdt.c kernel/tss.c kernel/syscall.c kernel/usermode.c kernel/task.c \
           drivers/terminal.c drivers/keyboard.c drivers/timer.c \
           lib/string.c; do
	obj="$BUILD/$(basename "$src" .c).o"
	echo "compiling  $src"
	gcc $CFLAGS -c "$src" -o "$obj"
done

echo "linking    arthic64.bin"
gcc -m64 -no-pie -Wl,--build-id=none -T linker.ld -o arthic64.bin \
    -ffreestanding -nostdlib -O2 "$BUILD"/*.o

# QEMU's -kernel only accepts a 32-bit multiboot image, and refuses a 64-bit
# ELF outright. The instructions inside are unaffected by the container format,
# so objcopy rewrites the ELF headers to elf32-i386 and leaves the code alone.
# This works only because the kernel is linked low - every address fits in 32
# bits. A kernel in the upper half of the address space would need a real
# bootloader and an ISO instead.
objcopy -O elf32-i386 arthic64.bin arthic64-boot.bin

echo
echo "built: arthic64.bin (and arthic64-boot.bin for QEMU)"

if [ "$1" = "run" ]; then
	echo "starting qemu ..."
	qemu-system-x86_64 -no-reboot -m 128M -kernel arthic64-boot.bin
fi
