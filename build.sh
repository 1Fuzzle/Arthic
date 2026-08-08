#!/bin/sh
# build.sh — compile Arthic and boot it in QEMU.
#
# Usage:  ./build.sh          build only
#         ./build.sh run      build, then boot it in QEMU
#         ./build.sh test     build and run the unit tests on this machine
#         ./build.sh coverage run the tests and report line coverage
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
#   -I$INCLUDE                where our headers live
#   -Wall -Wextra             warnings in kernel code are usually real bugs
BASE_CFLAGS="-m32 -std=gnu11 -ffreestanding -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
        -fno-builtin -fno-pie -nostdlib \
        -I$INCLUDE -I$BUILD -Wall -Wextra -O2"

# Kernel-specific flags: enable stack canary protection
KERNEL_CFLAGS="$BASE_CFLAGS -fstack-protector"

# User program flags: no stack protection (user programs handle their own security)
USER_CFLAGS="$BASE_CFLAGS -fno-stack-protector"

ASFLAGS="-m32"

if [ "$1" = "clean" ]; then
	rm -rf "$BUILD" arthic.bin
	echo "cleaned (arthic.img kept - delete it by hand to wipe the disk)"
	exit 0
fi

mkdir -p "$BUILD"

# ---- the unit tests ---------------------------------------------------------
# Some of the kernel is ordinary logic - a first-fit allocator, a ring buffer, a
# block bitmap - and none of it needs a CPU in protected mode to be right or
# wrong. Those files are compiled here into normal Linux programs, with fakes
# standing in for the disk, the scheduler and the frame allocator, and run
# directly. See tests/arthictest.h for what this can and cannot reach.
#
# The kernel sources are compiled with the SAME flags as the real build, so
# nothing under test differs from what boots. Only the test files themselves
# are hosted, and only they may use libc.
#
# Requires 32-bit libc headers: gcc-multilib on Debian/Ubuntu, lib32-glibc on
# Arch. -m32 is not optional - Arthic stores addresses in uint32_t, and a
# 64-bit build would truncate every one of them.

# -iquote rather than -I for the kernel headers: include/string.h has the same
# name as libc's, and with -I it would shadow the real one for the test files,
# which unlike the kernel do get to use libc. -iquote is only searched for the
# "quoted" form, so #include "string.h" finds ours and <string.h> finds libc's.
TEST_CFLAGS="-m32 -std=gnu11 -fno-pie -iquote $INCLUDE -iquote tests -Wall -Wextra -g"
TEST_LDFLAGS="-m32 -no-pie"

# Each suite is "name : kernel sources : test support sources". They are built
# as separate programs rather than one, because a test of mm/pmm.c wants the
# real frame allocator and a test of mm/kheap.c wants a fake one - and the two
# cannot both define pmm_alloc_frames in the same binary.
TEST_SUITES="
string : lib/string.c :
kheap  : mm/kheap.c lib/string.c : tests/support/console.c tests/support/fake_pmm.c
pmm    : mm/pmm.c lib/string.c   : tests/support/console.c tests/support/kernel_end.c
pipe   : kernel/pipe.c lib/string.c : tests/support/console.c tests/support/fake_task.c
lock   : kernel/lock.c : tests/support/console.c tests/support/fake_task.c
fs     : fs/fs.c lib/string.c : tests/support/console.c tests/support/fake_disk.c
"

# $1 extra flags for the kernel sources (used by the coverage build)
build_and_run_tests() {
	extra="$1"
	outdir="$BUILD/tests"

	rm -rf "$outdir"
	mkdir -p "$outdir"

	failed=""

	# Split on newlines only, and iterate in THIS shell rather than piping
	# into a while loop: the right-hand side of a pipe is a subshell, and a
	# variable set there - like the list of failures - vanishes when it ends.
	old_ifs=$IFS
	IFS='
'
	for suite in $TEST_SUITES; do
		IFS=$old_ifs

		[ -z "$(echo "$suite" | tr -d ' ')" ] && continue

		name=$(echo "$suite" | cut -d: -f1 | tr -d ' ')
		kernel_srcs=$(echo "$suite" | cut -d: -f2)
		support_srcs=$(echo "$suite" | cut -d: -f3)

		objs=""

		# Kernel sources: freestanding, exactly as the kernel builds them.
		for src in $kernel_srcs; do
			obj="$outdir/$name-$(basename "$src" .c).o"
			# shellcheck disable=SC2086
			gcc $CFLAGS $extra -c "$src" -o "$obj"
			objs="$objs $obj"
		done

		# Test and support sources: hosted, libc allowed.
		for src in $support_srcs tests/test_$name.c; do
			obj="$outdir/$name-$(basename "$src" .c)-test.o"
			# shellcheck disable=SC2086
			gcc $TEST_CFLAGS -iquote tests/support -c "$src" -o "$obj"
			objs="$objs $obj"
		done

		# shellcheck disable=SC2086
		gcc $TEST_LDFLAGS $extra -o "$outdir/$name" $objs

		if ! "./$outdir/$name"; then
			failed="$failed $name"
		fi
		echo
	done
	IFS=$old_ifs

	if [ -n "$failed" ]; then
		echo "FAILED:$failed"
		return 1
	fi

	echo "all suites passed"
	return 0
}

if [ "$1" = "test" ]; then
	build_and_run_tests ""
	exit $?
fi

if [ "$1" = "coverage" ]; then
	# --coverage makes gcc emit counters alongside the object files; running
	# the program writes them out, and gcov turns them back into per-line
	# figures. -O0 because optimised code merges and reorders lines until the
	# report describes something other than the source you wrote.
	build_and_run_tests "--coverage -O0" || true

	echo "coverage (unit tests only - the rest of the kernel is untested):"
	echo

	for src in lib/string.c mm/kheap.c mm/pmm.c kernel/pipe.c kernel/lock.c fs/fs.c; do
		base=$(basename "$src" .c)

		# One .gcda per suite that compiled the file, so a file used by
		# several suites is reported once per suite. Take the best.
		best=""
		for gcda in "$BUILD"/tests/*-"$base".gcda; do
			[ -f "$gcda" ] || continue

			line=$(gcov -n -o "$BUILD/tests" "$gcda" 2>/dev/null \
			       | grep -A1 "File .*$src" | grep "Lines executed")
			[ -z "$line" ] && continue

			percent=${line#Lines executed:}
			percent=${percent%%%*}

			if [ -z "$best" ] || [ "${percent%.*}" -gt "${best%.*}" ]; then
				best=$percent
			fi
		done

		if [ -n "$best" ]; then
			printf '  %-16s %s%%\n' "$src" "$best"
		else
			printf '  %-16s no data\n' "$src"
		fi
	done

	echo
	echo "not covered at all: everything that touches hardware directly -"
	echo "  drivers/, kernel/idt.c, kernel/gdt.c, mm/paging.c, kernel/task.c,"
	echo "  kernel/syscall.c, kernel/loader.c, kernel/shell.c"
	exit 0
fi

# ---- the user program -------------------------------------------------------
# Built completely separately from the kernel: its own linker script, its own
# load address, no shared headers. Then flattened to a raw image and turned
# into a C array so the kernel can carry a copy and write it to the disk.
#
# This is the honest version of "a program is just a file". Nothing about
# prog.c knows it will be run by Arthic.
echo "compiling  user/prog.c"
gcc $USER_CFLAGS -c user/prog.c -o "$BUILD/prog.o"

echo "linking    user program"
ld -m elf_i386 -T user/prog.ld -o "$BUILD/prog.elf" "$BUILD/prog.o"

# The ELF file itself is what gets stored on the disk now - no objcopy to a
# flat image. The whole point is that the program describes itself.
cp "$BUILD/prog.elf" "$BUILD/prog.bin"

# Turn the raw bytes into a C array using only coreutils, so the build does not
# depend on xxd being installed.
{
	printf '/* Generated by build.sh from user/prog.c - do not edit. */\n'
	printf '#ifndef ARTHIC_PROG_BLOB_H\n#define ARTHIC_PROG_BLOB_H\n\n'
	printf '#include <stdint.h>\n\n'
	printf 'static const uint8_t prog_blob[] = {\n'
	od -An -v -tx1 "$BUILD/prog.bin" | awk '{for(i=1;i<=NF;i++) printf "0x%s,", $i; print ""}'
	printf '};\n\n'
	printf 'static const uint32_t prog_blob_length = sizeof(prog_blob);\n\n'
	printf '#endif\n'
} > "$BUILD/prog_blob.h"


# Assembly sources
for src in boot/boot.s kernel/interrupts.s kernel/switch.s; do
	obj="$BUILD/$(basename "$src" .s).o"
	echo "assembling $src"
	gcc $ASFLAGS -c "$src" -o "$obj"
done

# C sources
for src in kernel/main.c kernel/gdt.c kernel/idt.c kernel/shell.c kernel/tss.c kernel/syscall.c kernel/usermode.c kernel/task.c kernel/lock.c kernel/loader.c kernel/pipe.c \
           drivers/terminal.c drivers/keyboard.c drivers/timer.c drivers/serial.c \
           mm/pmm.c mm/paging.c mm/kheap.c lib/string.c lib/canary.c \
           drivers/ata.c fs/fs.c; do
	obj="$BUILD/$(basename "$src" .c).o"
	echo "compiling  $src"
	gcc $KERNEL_CFLAGS -c "$src" -o "$obj"
done

echo "linking    arthic.bin"
# Note the explicit exclusion: prog.o belongs to the user program, not the
# kernel, and it has its own _start. Linking "$BUILD"/*.o blindly pulls it in
# and collides with the kernel entry point.
KERNEL_OBJS=$(ls "$BUILD"/*.o | grep -v '/prog\.o$')

gcc -m32 -no-pie -Wl,--build-id=none -T linker.ld -o arthic.bin \
    -ffreestanding -nostdlib -O2 $KERNEL_OBJS

echo
echo "built: arthic.bin"

# A disk image for the filesystem to live on. Created once and then kept, so
# files survive between runs - which is rather the point of a filesystem.
DISK=arthic.img

if [ "$1" = "run" ]; then
	if [ ! -f "$DISK" ]; then
		echo "creating $DISK (16 MB)"
		dd if=/dev/zero of="$DISK" bs=1M count=16 2>/dev/null
	fi

	echo "starting qemu ... (close the window or press Ctrl-C to stop)"
	echo "Serial output will be written to serial.log"
	qemu-system-i386 -no-reboot -m 128M -kernel arthic.bin \
	    -drive file="$DISK",format=raw,if=ide \
	    -serial file:serial.log
fi
