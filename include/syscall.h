/* syscall.h — the kernel/user boundary.
 *
 * A syscall is a deliberate, controlled way for ring 3 to ask the kernel for
 * something. It is the ONLY way — everything else is walled off by page
 * permissions and the I/O bitmap.
 *
 * That makes this interface the entire attack surface. Every argument arriving
 * here comes from code the kernel does not trust, and every one must be
 * validated before use. A pointer is not a pointer until it has been checked.
 *
 * Nothing here registers which addresses are legitimate: the syscall layer asks
 * the page tables of the address space it is standing in, so the answer is
 * always the running task's own and always what the MMU would say.
 */
#ifndef ARTHIC_SYSCALL_H
#define ARTHIC_SYSCALL_H

#include <stdint.h>

#define SYSCALL_VECTOR 0x80

#define SYS_WRITE 0    /* ebx = pointer to string */
#define SYS_TICKS 1    /* returns tick count in eax */
#define SYS_EXIT  2    /* never returns */
#define SYS_SLEEP 3    /* ebx = ticks */
#define SYS_ID    4    /* returns this task's id */
#define SYS_PIPE_WRITE 5   /* ebx = buffer, ecx = length */
#define SYS_PIPE_READ  6   /* ebx = buffer, ecx = max, returns bytes read */

struct task;

/* Emit whatever the task has buffered but not yet terminated with a newline.
 * Called when a program exits or dies, so its last partial line is not lost. */
void syscall_flush_output(struct task *t);

void syscall_install(void);

#endif
