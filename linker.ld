/* linker.ld — where each piece of Arthic lands in memory.
 *
 * Normally the linker uses a default script and you never think about it.
 * We cannot: the default assumes a Linux process, and we are not one. We have
 * to state exactly where things go, because the multiboot header must be near
 * the very front of the file or GRUB will not recognise us.
 */

ENTRY(_start)   /* execution begins at the _start label in boot.s */

SECTIONS
{
	/* 1 MiB. The first megabyte of memory is littered with BIOS structures
	 * and memory-mapped hardware, so by convention kernels load above it. */
	. = 1M;

	/* .text is executable code. The multiboot header is placed first, ahead
	 * of everything, so GRUB finds it while scanning the start of the file. */
	.text BLOCK(4K) : ALIGN(4K)
	{
		*(.multiboot)
		*(.text)
	}

	/* Read-only data: string literals, const values. */
	.rodata BLOCK(4K) : ALIGN(4K)
	{
		*(.rodata)
	}

	/* Initialised writable data. */
	.data BLOCK(4K) : ALIGN(4K)
	{
		*(.data)
	}

	/* .bss is data that starts as zero — our stack lives here. It takes up
	 * no space in the binary on disk; it is only reserved once loaded. */
	.bss BLOCK(4K) : ALIGN(4K)
	{
		*(COMMON)
		*(.bss)
	}

	/* Metadata the toolchain adds that a kernel has no use for. If we do not
	 * discard it, the linker may place it BEFORE our multiboot header, and
	 * GRUB only scans the first 8 KiB of the file. */
	/DISCARD/ :
	{
		*(.eh_frame)
		*(.comment)
		*(.note*)
	}
}
