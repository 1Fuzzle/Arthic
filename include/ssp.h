/* ssp.h - stack smashing protection, 32-bit.
 *
 * Identical idea to the 64-bit branch's version, and identical trap: on x86,
 * GCC's DEFAULT is to read the canary from a thread-local-storage slot -
 * %gs:0x14 on i386, %fs:0x28 on x86_64 - rather than a plain global, because
 * that is where glibc keeps it on a real system. This kernel has no TLS
 * segment at all, so without overriding that default, every canary check
 * would compare a fixed, meaningless offset against itself and always
 * "pass" regardless of what actually happened to the stack -
 * -mstack-protector-guard=global is what makes GCC read the plain symbol
 * this header declares instead.
 */
#ifndef ARTHIC_SSP_H
#define ARTHIC_SSP_H

void ssp_init(void);

#endif
