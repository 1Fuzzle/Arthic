/* prog.c - a 64-bit program, loaded from disk and run in its own process.
 *
 * Not compiled into the kernel. Built separately into an ELF64 executable,
 * stored on ArthicFS, loaded at run time. Its entire world is the pages
 * loader.c mapped for it, and its only way out is `syscall`.
 */

#define SYS_WRITE 0
#define SYS_EXIT  1

/* Three separate lessons live in this one function, and all three came from
 * actually hitting the bug rather than reasoning about it in advance.
 *
 * "=a"(ret) is load-bearing: without it, the compiler has no idea RAX changes
 * at all, and reuses a stale value across calls. `noinline` is needed on top
 * of it once this is called more than once, because inlining lets the
 * optimiser fold repeated calls together and defeats even a correct output
 * constraint. Both of those carried straight over from the ring 3 demo in
 * usermode.c on this branch.
 *
 * The THIRD one is new, and it is the reason a 4096-byte checksum loop kept
 * coming back wrong even after the first two fixes were in place. The
 * clobber list below originally read only "rcx", "r11" - the two registers
 * SYSCALL itself uses for the return address and flags. That is correct as
 * far as it goes, but incomplete: the kernel on the other side of this call
 * is running ordinary C, free to use RDX, RSI, R8, R9 and R10 as scratch for
 * anything it likes, and nothing in a two-register clobber list told the
 * compiler that. So GCC's dataflow model kept believing EDX still held the
 * value it was set to BEFORE this call - in the caller, `checksum = 0`, hoisted
 * ahead of the print() calls that preceded the loop - and skipped actually
 * re-zeroing it afterward, because as far as the compiler could see, nothing
 * had touched it. The kernel HAD touched it, using RDX as ordinary scratch
 * space while answering the syscall, and that leftover value became the
 * checksum loop's starting point instead of zero.
 *
 * The fix is naming every register a real syscall boundary can actually
 * disturb, not just the two the instruction itself uses. This is exactly the
 * "undefined after a syscall" register set documented in kernel/syscall.c's
 * SYS_EXIT case and pushed/popped in syscall_entry.s - the two files agree
 * with each other, which is what makes this list correct rather than another
 * guess. */
__attribute__((noinline))
static long syscall1(long number, long arg)
{
	long ret;
	__asm__ volatile (
		"syscall"
		: "=a"(ret)
		: "a"(number), "D"(arg)
		: "rcx", "r11", "rdx", "rsi", "r8", "r9", "r10", "memory"
	);
	return ret;
}

static void print(const char *s)
{
	syscall1(SYS_WRITE, (long) s);
}

static char scratch[4096];   /* lives in .bss - occupies no space in the file */

__attribute__((section(".text._start")))
void _start(void)
{
	print("  [prog64] loaded from disk, running as its own process\n");

	unsigned int checksum = 0;
	for (int i = 0; i < 4096; i++)
		checksum += (unsigned char) scratch[i];

	if (checksum == 0)
		print("  [prog64] .bss arrived zeroed, as it should\n");
	else
		print("  [prog64] .bss was NOT zero - something is wrong\n");

	scratch[0] = 'A';
	print("  [prog64] wrote to my writable data segment\n");

	print("  [prog64] now trying to write to my own code segment\n");
	*(volatile char *) _start = 0;   /* should fault - code is r-x */

	print("  [prog64] STILL HERE - the code segment is writable, which is wrong\n");
	syscall1(SYS_EXIT, 0);

	for (;;) { }
}
