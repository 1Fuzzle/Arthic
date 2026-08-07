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
	/* Marks around the code section, so paging can make it read-only.
	 * The linker knows where things ended up; C cannot know without being
	 * told, and this is how it gets told. */
	kernel_text_start = .;
	.text BLOCK(4K) : ALIGN(4K)
	{
		*(.multiboot)
		*(.text)
	}
	kernel_text_end = .;

	/* Code ring 3 is allowed to execute. Its own section so it can be given
	 * its own page permissions — page flags apply per 4 KB page, so anything
	 * needing different permissions needs its own page. */
	.usertext BLOCK(4K) : ALIGN(4K)
	{
		user_text_start = .;
		*(.usertext)
		*(.userdata)
		. = ALIGN(4K);
		user_text_end = .;
	}

	/* Read-only data: string literals, const values. */
	.rodata BLOCK(4K) : ALIGN(4K)
	{
		*(.rodata)
		*(.rodata.*)
	}
	kernel_rodata_end = .;

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

	/* The address just past the end of the kernel. The memory manager needs
	 * to know where it is safe to start putting things — everything below
	 * this is us, and overwriting ourselves would be a short trip. The
	 * linker fills this in; C reads it via `extern`. */
	kernel_end = .;

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
