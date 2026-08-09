/* syscall.h - the 64-bit kernel/user boundary.
 *
 * A separate mechanism from the interrupt-based one on the 32-bit branch.
 * SYSCALL is a dedicated instruction, faster than int because it skips the
 * IDT and the privilege checks a general interrupt gate requires - and that
 * speed is bought by leaving more of the transition to the kernel to do by
 * hand. See kernel/syscall.c and kernel/syscall_entry.s for what that costs.
 */
#ifndef ARTHIC_SYSCALL_H
#define ARTHIC_SYSCALL_H

#include <stdint.h>

/* The frame the assembly stub builds on the kernel's syscall stack before
 * calling into C. Field order matches the push order in syscall_entry.s
 * exactly - change one without the other and this reads garbage. */
struct syscall_frame {
	uint64_t rax;   /* syscall number in; return value out */
	uint64_t rdi, rsi, rdx, r10, r8, r9;   /* arguments, Linux's register set */
};

#define SYS_WRITE 0
#define SYS_EXIT  1

void syscall_install(void);
void syscall_set_user_range(uint64_t start, uint64_t end);

#endif
