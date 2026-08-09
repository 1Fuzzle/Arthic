/* prog.ld - layout for a 64-bit user program.
 *
 * Linked at USER_LOAD_ADDR, well clear of the kernel's identity-mapped low
 * gigabyte. Text and data get separate PT_LOAD segments, aligned apart, so
 * the loader can give one r-x and the other rw- - the actual, unconditional
 * half of W^X that PAE only approximated on the 32-bit branch.
 */
ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);   /* read + execute */
	data PT_LOAD FLAGS(6);   /* read + write   */
}

SECTIONS
{
	. = 0x40000000;

	.text : ALIGN(4096) {
		*(.text._start)
		*(.text*)
		*(.rodata*)
	} :text

	. = ALIGN(4096);

	.data : ALIGN(4096) {
		*(.data*)
	} :data

	.bss : ALIGN(16) {
		*(.bss*)
		*(COMMON)
	} :data

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }
}
