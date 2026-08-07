/* interrupts.s — the assembly stubs every interrupt lands on first.
 *
 * WHY THIS FILE HAS TO BE ASSEMBLY
 *
 * When an interrupt fires, the CPU jumps straight to an address from the IDT.
 * It does not call a function. There is no argument passing, no return value,
 * and you must finish with `iret` rather than `ret` because the CPU pushed
 * extra state on the way in. A C function cannot do any of that.
 *
 * So each of the 48 entry points is a tiny assembly stub that normalises the
 * situation and then calls into C, where the real work happens.
 *
 * THE ERROR CODE PROBLEM
 *
 * Some exceptions push an extra error code onto the stack; most do not. That
 * inconsistency would make the stack layout differ between handlers, so for the
 * ones that push nothing we push a dummy zero. After that every handler sees an
 * identical stack, which is what lets one shared C function handle all of them.
 *
 * This is a good example of a general technique: when hardware hands you two
 * slightly different shapes, normalise them at the boundary rather than
 * complicating everything downstream.
 */

/* --- macros ---------------------------------------------------------------
 * \num is macro-argument substitution. isr\num with num=13 assembles as isr13.
 * Writing 48 near-identical stubs by hand would be miserable and error-prone.
 */

.macro ISR_NOERRCODE num
	.global isr\num
	isr\num:
		cli
		push $0          /* dummy error code, so the layout matches */
		push $\num       /* which interrupt this was */
		jmp isr_common_stub
.endm

.macro ISR_ERRCODE num
	.global isr\num
	isr\num:
		cli
		/* the CPU already pushed a real error code here */
		push $\num
		jmp isr_common_stub
.endm

.macro IRQ num, vector
	.global irq\num
	irq\num:
		cli
		push $0
		push $\vector
		jmp irq_common_stub
.endm

/* --- CPU exceptions, vectors 0-31 -----------------------------------------
 * These are reserved by Intel. 0 is divide-by-zero, 13 is general protection
 * fault, 14 is page fault — the three you will meet most.
 *
 * Vectors 8, 10-14 and 17 push a real error code. The rest do not.
 */
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_NOERRCODE 30
ISR_NOERRCODE 31

/* --- hardware IRQs, remapped to vectors 32-47 ------------------------------
 * IRQ 0 is the timer, IRQ 1 the keyboard. See pic_remap() in idt.c for why
 * these do not sit at 0-15 where the hardware originally put them.
 */
IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

/* --- the syscall vector ---------------------------------------------------
 * Ring 3 reaches the kernel through this one entry point and no other.
 */
.global isr128
isr128:
	cli
	push $0
	push $128
	jmp isr_common_stub

/* --- the shared tail -------------------------------------------------------
 * Every stub above jumps here. This saves the CPU state, switches to kernel
 * data segments, calls C, then puts everything back exactly as it was.
 *
 * `pusha` pushes all eight general-purpose registers in one instruction. The
 * order it uses is fixed, and `struct registers` in idt.h mirrors it exactly.
 * If you ever reorder one you must reorder the other, or the C code will read
 * the wrong values with no warning of any kind.
 *
 * Reloading DS matters for security as well as correctness: an interrupt can
 * arrive while ring 3 code is running with its own segment registers loaded,
 * and the kernel must not trust them.
 */
.extern isr_handler
.extern irq_handler

isr_common_stub:
	pusha                    /* edi esi ebp esp ebx edx ecx eax */

	mov %ds, %ax
	push %eax                /* remember the old data segment */

	mov $0x10, %ax           /* kernel data selector from our GDT */
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs

	push %esp                /* pass a pointer to all of the above to C */
	call isr_handler
	add $4, %esp             /* drop the argument */

	pop %eax                 /* restore the caller's data segment */
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs

	popa
	add $8, %esp             /* discard int_no and err_code */
	sti
	iret                     /* not ret — restores eip, cs and eflags at once */

irq_common_stub:
	pusha

	mov %ds, %ax
	push %eax

	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs

	push %esp
	call irq_handler
	add $4, %esp

	pop %eax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs

	popa
	add $8, %esp
	sti
	iret

/* --- loading the IDT ------------------------------------------------------- */
.global idt_flush
idt_flush:
	mov 4(%esp), %eax        /* first argument: address of the idt_ptr */
	lidt (%eax)
	ret

.section .note.GNU-stack,"",@progbits

/* --- entering and leaving ring 3 -------------------------------------------
 * This lives in assembly rather than inline asm because it deliberately
 * abandons and restores the stack, and the compiler cannot be told that.
 *
 * `current_uctx` points at the running task's two-word save area, and the
 * scheduler updates it on every switch. It has to be per-task rather than one
 * fixed location: two tasks can each be in ring 3 with only one of them
 * actually executing, and "where do I resume" belongs to the task.
 *
 * A pointer kept current by the scheduler is simpler than threading the
 * address through every call site, and it cannot go stale - the only thing
 * that changes which task is running is the scheduler itself.
 */
.section .bss
.align 4
.global current_uctx
current_uctx: .skip 4

.section .text

/* void usermode_jump(uint32_t entry, uint32_t user_stack_top);
 *
 * Does not return. Control comes back through usermode_return, which resumes
 * as though this function had returned normally.
 */
.global usermode_jump
usermode_jump:
	/* esp still points at our return address - no pushes yet. Saving it now
	 * means usermode_return can `ret` straight to our caller. */
	mov current_uctx, %eax
	mov %esp, 0(%eax)
	mov %ebp, 4(%eax)

	mov 4(%esp), %ecx        /* entry point */
	mov 8(%esp), %edx        /* user stack  */

	cli

	mov $0x23, %ax           /* user data selector, RPL 3 */
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs

	pushl $0x23              /* SS     */
	pushl %edx               /* ESP    */
	pushfl
	popl %eax
	orl $0x200, %eax         /* set IF, or ring 3 runs uninterruptible */
	pushl %eax               /* EFLAGS */
	pushl $0x1B              /* CS, user code selector, RPL 3 */
	pushl %ecx               /* EIP    */
	iret

/* void usermode_return(void);
 *
 * Called on the kernel stack the TSS supplied. Throws that stack away and
 * continues on the one usermode_jump was using.
 */
.global usermode_return
usermode_return:
	cli
	mov current_uctx, %eax
	mov 0(%eax), %esp
	mov 4(%eax), %ebp

	mov $0x10, %ax           /* back to kernel data selectors */
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs
	mov %ax, %gs

	sti
	ret                      /* returns to whoever called usermode_jump */

/* --- surviving an expected fault -------------------------------------------
 * The page fault handler reads fault_resume_eip. If it is non-zero, it rewrites
 * the saved EIP so `iret` returns there instead of retrying the instruction
 * that faulted.
 *
 * This is in assembly because the resume point must be a specific instruction
 * address, and at -O2 the compiler will happily move a C label relative to the
 * code around it. An earlier inline version looped forever for exactly that
 * reason.
 *
 * The same mechanism is what a real kernel uses for copy_from_user: touch a
 * pointer that ring 3 supplied, and if it turns out to be garbage, recover
 * instead of dying.
 */
.global fault_resume_eip
.section .bss
.align 4
fault_resume_eip: .skip 4

.section .text

/* int probe_write(volatile uint32_t *addr, uint32_t value);
 * Returns 1 if the write succeeded, 0 if it faulted. */
.global probe_write
probe_write:
	mov 4(%esp), %ecx                  /* address */
	mov 8(%esp), %edx                  /* value   */

	movl $probe_write_fault, fault_resume_eip

	xor %eax, %eax                     /* assume failure */
	mov %edx, (%ecx)                   /* <- the risky store */
	mov $1, %eax                       /* reached only if it worked */

probe_write_fault:
	movl $0, fault_resume_eip          /* disarm either way */
	ret
