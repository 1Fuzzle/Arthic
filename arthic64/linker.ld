/* linker.ld - a 64-bit kernel at 1 MB.
 *
 * The multiboot header must still land within the first 8 KB of the file, so
 * .text comes first and .multiboot comes first inside it.
 *
 * Linked at 1 MB, low enough that the "small" code model works - a 64-bit
 * kernel placed in the top of the address space would need -mcmodel=kernel and
 * a different set of relocations. Identity mapping the low gigabyte keeps this
 * simple.
 */
ENTRY(_start)

SECTIONS
{
	. = 1M;

	kernel_text_start = .;
	.text BLOCK(4K) : ALIGN(4K)
	{
		*(.multiboot)
		*(.text)
	}
	kernel_text_end = .;

	.rodata BLOCK(4K) : ALIGN(4K)
	{
		*(.rodata)
		*(.rodata.*)
	}
	kernel_rodata_end = .;

	.data BLOCK(4K) : ALIGN(4K)
	{
		*(.data)
	}

	.bss BLOCK(4K) : ALIGN(4K)
	{
		*(COMMON)
		*(.bss)
	}

	kernel_end = .;

	/DISCARD/ :
	{
		*(.eh_frame)
		*(.comment)
		*(.note*)
	}
}
