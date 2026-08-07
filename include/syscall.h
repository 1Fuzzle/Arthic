/* syscall.h — the kernel/user boundary.
 *
 * A syscall is a deliberate, controlled way for ring 3 to ask the kernel for
 * something. It is the ONLY way — everything else is walled off by page
 * permissions and the I/O bitmap.
 *
 * That makes this interface the entire attack surface. Every argument arriving
 * here comes from code the kernel does not trust, and every one must be
 * validated before use. A pointer is not a pointer until it has been checked.
 */
#ifndef ARTHIC_SYSCALL_H
#define ARTHIC_SYSCALL_H

#include <stdint.h>

#define SYSCALL_VECTOR 0x80

#define SYS_WRITE 0    /* ebx = pointer to string */
#define SYS_TICKS 1    /* returns tick count in eax */
#define SYS_EXIT  2    /* never returns */

void syscall_install(void);

/* Tell the syscall layer which address range ring 3 may reference. Anything
 * outside it is rejected. */
void syscall_set_user_range(uint32_t start, uint32_t end);

#endif
