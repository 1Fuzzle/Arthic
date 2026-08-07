/* prog.ld - layout for a user program.
 *
 * Linked at 0x20000000, deliberately above any physical RAM the kernel
 * identity-maps, so the loader can map fresh pages there without colliding
 * with the kernel's view of memory.
 *
 * The ALIGN(4096) between sections is what makes per-segment permissions
 * possible at all. Page flags apply to a whole 4 KB page, so if code and
 * writable data shared one page, the loader would have to make that page
 * writable - and the code on it would become modifiable. Alignment costs a
 * little space and buys the boundary.
 */
ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);   /* 4 = read, 1 = execute -> read + execute */
	data PT_LOAD FLAGS(6);   /* 4 = read, 2 = write   -> read + write   */
}

SECTIONS
{
	. = 0x20000000;

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
