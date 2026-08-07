/* switch.s — the context switch.
 *
 * void task_switch(uint32_t *save_esp_here, uint32_t new_esp);
 *
 * This is the whole mechanism, and it is smaller than people expect.
 *
 * A thread IS its stack. Everything that makes it distinct — where it was, what
 * its local variables hold, what it will do next — lives on that stack. So
 * switching threads is: push the registers that must survive, write down the
 * stack pointer, load a different one, pop the registers back. The instructions
 * after `mov %edx, %esp` belong to a different thread than the ones before it.
 *
 * Only the callee-saved registers are pushed. The C calling convention already
 * promises that eax, ecx and edx may be destroyed by any function call, and to
 * the compiler this IS a function call — so anything it cared about is already
 * spilled. Saving them would be correct but wasteful, and this runs on every
 * single timer tick.
 *
 * Note there is no iret here and no privilege change. These are all kernel
 * threads sharing one address space. Switching between processes additionally
 * means loading a different page directory into CR3.
 */
.section .text
.global task_switch
task_switch:
	mov 4(%esp), %eax        /* where to save the old stack pointer */
	mov 8(%esp), %edx        /* the stack pointer to switch to      */

	pushfl
	pushl %ebx
	pushl %esi
	pushl %edi
	pushl %ebp

	mov %esp, (%eax)         /* the old thread is now fully on its stack */
	mov %edx, %esp           /* <- the actual switch */

	popl %ebp
	popl %edi
	popl %esi
	popl %ebx
	popfl

	ret                      /* returns into the NEW thread */

.section .note.GNU-stack,"",@progbits
