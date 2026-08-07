/* prog.ld - layout for a user program.
 *
 * Linked at 0x20000000, which is deliberately ABOVE any physical RAM the
 * kernel identity-maps. That means the loader can map fresh pages there
 * without colliding with the kernel's own view of memory - the address is
 * meaningful only inside the page tables we build for this program.
 *
 * That is the first genuinely interesting thing virtual memory buys you: an
 * address that exists for one program and nothing else.
 */
ENTRY(_start)

SECTIONS
{
	. = 0x20000000;

	.text : { *(.text._start) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }
}
