/* interrupts.s - the 64-bit stubs.
 *
 * Same shape as the 32-bit version, with three real differences.
 *
 * NO PUSHA. AMD removed it, so all fifteen general-purpose registers are pushed
 * individually. Tedious, and arguably clearer: the order is visible rather than
 * implied by a mnemonic.
 *
 * ARGUMENTS GO IN REGISTERS. The 64-bit calling convention passes the first
 * argument in rdi rather than on the stack, so handing the register block to C
 * is `mov %rsp, %rdi` instead of a push.
 *
 * THE STACK MUST BE 16-BYTE ALIGNED AT A CALL. The ABI requires it, and code
 * compiled for 64-bit genuinely relies on it. An interrupt can arrive with the
 * stack at any alignment, so we align it deliberately and put it back
 * afterwards. Skip this and things work until the first function that touches
 * an aligned type, at which point the fault is a long way from the cause.
 */

.macro ISR_NOERRCODE num
	.global isr\num
	isr\num:
		cli
		pushq $0
		pushq $\num
		jmp isr_common_stub
.endm

.macro ISR_ERRCODE num
	.global isr\num
	isr\num:
		cli
		pushq $\num
		jmp isr_common_stub
.endm

.macro IRQ num, vector
	.global irq\num
	irq\num:
		cli
		pushq $0
		pushq $\vector
		jmp irq_common_stub
.endm

.section .text
.code64

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

.macro PUSH_ALL
	push %rax
	push %rbx
	push %rcx
	push %rdx
	push %rsi
	push %rdi
	push %rbp
	push %r8
	push %r9
	push %r10
	push %r11
	push %r12
	push %r13
	push %r14
	push %r15
.endm

.macro POP_ALL
	pop %r15
	pop %r14
	pop %r13
	pop %r12
	pop %r11
	pop %r10
	pop %r9
	pop %r8
	pop %rbp
	pop %rdi
	pop %rsi
	pop %rdx
	pop %rcx
	pop %rbx
	pop %rax
.endm

.extern isr_handler
.extern irq_handler

isr_common_stub:
	PUSH_ALL

	mov %rsp, %rdi           /* first argument: the register block */

	/* Align the stack for the call, remembering where it was. rbx is
	 * callee-saved, and already on the stack, so borrowing it is safe. */
	mov %rsp, %rbx
	and $-16, %rsp
	call isr_handler
	mov %rbx, %rsp

	POP_ALL
	add $16, %rsp            /* discard int_no and err_code */
	sti
	iretq                    /* not iret - the 64-bit form */

irq_common_stub:
	PUSH_ALL

	mov %rsp, %rdi
	mov %rsp, %rbx
	and $-16, %rsp
	call irq_handler
	mov %rbx, %rsp

	POP_ALL
	add $16, %rsp
	sti
	iretq

.global idt_flush
idt_flush:
	lidt (%rdi)              /* the pointer arrives in rdi, not on the stack */
	ret

.section .note.GNU-stack,"",@progbits

/* --- surviving an expected fault -------------------------------------------
 * The page fault handler reads fault_resume_rip. If it is non-zero, it rewrites
 * the saved RIP so `iretq` returns there instead of retrying the instruction
 * that faulted.
 *
 * In assembly because the resume point must be a specific instruction address,
 * and at -O2 the compiler will happily move a C label relative to the code
 * around it.
 */
.section .bss
.align 8
.global fault_resume_rip
fault_resume_rip: .skip 8

.section .text

/* int probe_write(volatile uint64_t *addr, uint64_t value);
 * Arguments arrive in rdi and rsi - registers, not the stack. */
.global probe_write
probe_write:
	leaq probe_write_fault(%rip), %rax
	mov %rax, fault_resume_rip(%rip)

	xor %eax, %eax                     /* assume failure */
	mov %rsi, (%rdi)                   /* <- the risky store */
	mov $1, %eax                       /* reached only if it worked */

probe_write_fault:
	movq $0, fault_resume_rip(%rip)
	ret
