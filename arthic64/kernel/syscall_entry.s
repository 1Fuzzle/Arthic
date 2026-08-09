/* syscall_entry.s - the far side of the SYSCALL instruction.
 *
 * Everything kernel/syscall.c's comments describe happens here, in the order
 * it has to happen in. Read top to bottom; the order is the whole point.
 */

.section .text
.code64

.global syscall_entry
syscall_entry:
	/* CS/SS are already kernel selectors, courtesy of STAR - the CPU did
	 * that before we got here. RSP is still whatever the user program was
	 * using. RCX holds the user's RIP, R11 the user's RFLAGS - nowhere else,
	 * and nothing but us is going to protect them. */

	swapgs                    /* GS now reaches cpu_data, not the user's GS */

	movq %rsp, %gs:8          /* park the user's stack pointer              */
	movq %gs:0, %rsp          /* stand on our own small syscall stack       */

	/* Save the two registers a plain C call is entitled to destroy, and
	 * which SYSRET absolutely needs back afterwards. This is the save that
	 * has no equivalent on the interrupt path - IRET reads its return address
	 * off the stack, not out of a register the compiler might reuse. */
	push %r11
	push %rcx

	/* Build the syscall_frame struct in exactly the order include/syscall.h
	 * declares it, so the pointer we are about to hand to C reads correctly.
	 * rax ends up at the lowest address because it is pushed last. */
	push %r9
	push %r8
	push %r10
	push %rdx
	push %rsi
	push %rdi
	push %rax

	mov %rsp, %rdi            /* first argument to syscall_dispatch: &frame */
	call syscall_dispatch

	pop %rax                  /* frame.rax - possibly changed by dispatch   */
	add $48, %rsp              /* drop rdi,rsi,rdx,r10,r8,r9 - Linux's ABI
	                            * leaves these undefined after a syscall, so
	                            * there is nothing to restore them TO */

	pop %rcx                  /* the return address SYSRET will use         */
	pop %r11                  /* the flags SYSRET will restore               */

	mov %gs:8, %rsp           /* back onto the caller's own stack            */
	swapgs                    /* GS returns to whatever the user had         */

	sysretq                   /* CS/SS come from STAR again; RIP from RCX,
	                           * RFLAGS from R11 - the two we just took such
	                           * care to keep intact */

.section .note.GNU-stack,"",@progbits

/* --- entering and leaving ring 3 for the first time -------------------------
 * Distinct from syscall_entry above. This is the IRETQ-based transition, used
 * exactly once per program run - to get INTO ring 3 the first time, and to
 * come back out when the program exits or faults. Everything AFTER the first
 * entry that goes ring3 -> ring0 -> ring3 again goes through SYSCALL/SYSRET
 * instead; this pair only handles the very first hop and the final return.
 */
.section .bss
.align 8
kernel_saved_rsp: .skip 8
kernel_saved_rbp: .skip 8

.section .text

/* void usermode_jump(uint64_t entry, uint64_t user_stack_top); */
.global usermode_jump
usermode_jump:
	/* rsp still points at our return address - nothing pushed yet. Saving it
	 * now means usermode_return can `ret` straight back to whoever called us,
	 * as though this function had returned normally. */
	movq %rsp, kernel_saved_rsp(%rip)
	movq %rbp, kernel_saved_rbp(%rip)

	/* Arguments arrived in rdi (entry) and rsi (user stack) per the calling
	 * convention - already in registers, nothing to pop off a stack the way
	 * the 32-bit version had to. */
	cli

	mov $0x1B, %ax           /* GDT_USER_DATA */
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs

	pushq $0x1B              /* SS                                  */
	pushq %rsi               /* RSP - the user stack top             */
	pushfq
	orq $0x200, (%rsp)       /* set IF in the flags just pushed, or ring 3
	                          * runs with interrupts permanently off  */
	pushq $0x23              /* CS, GDT_USER_CODE, RPL 3             */
	pushq %rdi               /* RIP - the entry point                */
	iretq

/* void usermode_return(void); */
.global usermode_return
usermode_return:
	cli
	movq kernel_saved_rsp(%rip), %rsp
	movq kernel_saved_rbp(%rip), %rbp

	mov $0x10, %ax           /* back to kernel selectors */
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %fs

	sti
	ret

.section .note.GNU-stack,"",@progbits
