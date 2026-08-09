/* switch.s - the 64-bit context switch.
 *
 * void task_switch(uint64_t *save_esp_here, uint64_t new_esp);
 *
 * Same mechanism as the 32-bit branch, wider: a thread is its stack, so
 * switching is push what must survive, remember where the stack pointer ends
 * up, load a different one, pop the other thread's values back.
 *
 * Fewer registers to save than you might expect. The SysV ABI already treats
 * rax, rcx, rdx, rsi, rdi, r8-r11 as caller-saved - any ordinary function call
 * is entitled to clobber them, and to the compiler this switch genuinely IS a
 * function call. Only the callee-saved set needs pushing: rbx, rbp, r12-r15,
 * plus rflags. Six general-purpose registers, not fifteen - the interrupt
 * path needs more because an interrupt is NOT a function call and has no such
 * agreement with whatever it interrupted.
 */
.section .text
.code64

.global task_switch
task_switch:
	/* Arguments arrive in registers under this ABI, not on the stack - rdi
	 * holds the address to save into, rsi the stack to switch to. */
	pushfq
	push %rbx
	push %rbp
	push %r12
	push %r13
	push %r14
	push %r15

	mov %rsp, (%rdi)     /* the old thread is now fully on its own stack */
	mov %rsi, %rsp       /* <- the actual switch */

	pop %r15
	pop %r14
	pop %r13
	pop %r12
	pop %rbp
	pop %rbx
	popfq

	ret                  /* returns into the NEW thread */

.section .note.GNU-stack,"",@progbits

/* --- starting a brand new thread -------------------------------------------
 * `ret` needs an address to jump to but cannot hand it an argument the way a
 * `call` would - so the fake stack frame in task_create points here, not
 * straight at the thread's entry function.
 *
 * task_trampoline_target is read exactly once, right after task_switch's
 * pops hand control here, before this thread has any stack frame of its own.
 * That is why it can be a plain global rather than something passed more
 * carefully: nothing else touches it in the brief window between a new
 * thread being spliced onto the run ring and its first switch-in.
 */
.section .text
.code64

.global task_entry_trampoline
task_entry_trampoline:
	movq task_trampoline_target(%rip), %rax
	call *%rax

	/* A thread whose function returns normally lands here rather than
	 * running into whatever bytes happen to follow. */
	call task_exit

.section .note.GNU-stack,"",@progbits
