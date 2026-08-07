/* boot.s — the very first code that runs in Arthic.
 *
 * This is x86 assembly, not C. It exists because there are a few things C
 * simply cannot do: declare a magic header at a fixed spot in the file, and
 * set up a stack. C assumes a working stack already exists — it needs one for
 * local variables and function calls. So somebody has to make one first.
 * That somebody is this file. It is the only assembly in the whole project.
 */

/* ---- The Multiboot header -------------------------------------------------
 * We are not writing our own bootloader. GRUB already exists, it is very good,
 * and writing one is a separate project. GRUB agrees to load any kernel that
 * puts a specific magic number near the start of its file — that is the
 * "Multiboot standard". These four numbers are our side of that contract.
 */
.set ALIGN,    1<<0              /* ask GRUB to page-align loaded modules   */
.set MEMINFO,  1<<1              /* ask GRUB to give us a memory map        */
.set FLAGS,    ALIGN | MEMINFO   /* the two requests, OR'd into one field   */
.set MAGIC,    0x1BADB002        /* the number GRUB scans the file for      */
.set CHECKSUM, -(MAGIC + FLAGS)  /* must make the three sum to zero         */

.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

/* ---- The stack ------------------------------------------------------------
 * There is no operating system underneath us to allocate memory, so we cannot
 * ask for a stack — we reserve 16 KiB of blank space right here in our own
 * binary and declare that it is the stack.
 *
 * x86 stacks grow DOWNWARD (toward lower addresses), which is why below we
 * point the stack pointer at stack_top, the END of this block.
 */
.section .bss
.align 16
stack_bottom:
.skip 16384        /* 16 KiB */
stack_top:

/* ---- Entry point ---------------------------------------------------------- */
.section .text
.global _start
.type _start, @function

_start:
	/* At this instant: the CPU is in 32-bit protected mode, interrupts are
	 * off, paging is off, and we have total control of the machine. No other
	 * code is running anywhere. There is no printf, no malloc, no files.     */

	mov $stack_top, %esp   /* esp = the stack pointer. C now works.          */

	call kernel_main       /* jump into kernel.c — the last assembly we do   */

	/* kernel_main should never return, but if it somehow does, park the CPU
	 * forever rather than letting it wander into whatever bytes come next.  */
	cli                    /* disable interrupts                             */
1:	hlt                    /* halt until an interrupt (there are none now)   */
	jmp 1b                 /* if we ever wake, halt again                    */

.size _start, . - _start

/* Tell the linker we do not need an executable stack. */
.section .note.GNU-stack,"",@progbits
