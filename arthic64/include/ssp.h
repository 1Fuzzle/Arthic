/* ssp.h - stack smashing protection.
 *
 * GCC can emit a guard value (a "canary") on the stack of any function with a
 * local array, and a check before that function returns that the canary is
 * still intact. Overwrite the array past its end - the single most common
 * memory-safety bug in C - and the canary is what gets clobbered first, before
 * the return address itself, because the compiler places it between the two.
 * The check on the way out catches that corruption and stops the function
 * from returning at all, rather than letting a corrupted return address send
 * execution somewhere the attacker chose.
 *
 * -fstack-protector emits code that reads __stack_chk_guard at function entry
 * and calls __stack_chk_fail if the value has changed on exit. Neither exists
 * in a freestanding kernel unless we write them, which is the entire reason
 * -fno-stack-protector has been in every version of this build script since
 * the very first one - the flag was a placeholder for work not done yet, not
 * a decision that stack smashing didn't matter.
 */
#ifndef ARTHIC_SSP_H
#define ARTHIC_SSP_H

void ssp_init(void);

#endif
