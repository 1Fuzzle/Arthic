/* prog.c - a program, in the real sense.
 *
 * This is not compiled into the kernel. It is built separately into a flat
 * binary, stored on the disk as a file, and loaded into memory at run time.
 * Arthic knows nothing about it beyond its bytes.
 *
 * Everything it can do is in this file. There is no libc, no startup code, and
 * no way to reach the kernel except `int $0x80`. Its entire world is the pages
 * the loader mapped for it.
 */

#define SYS_WRITE 0
#define SYS_TICKS 1
#define SYS_EXIT  2

/* The one door out. eax carries which call, ebx the argument, and the result
 * comes back in eax. That convention is ours - Linux uses the same registers
 * for the same reasons, since they are the ones not clobbered by the entry
 * sequence. */
static int syscall(int number, int arg)
{
	int result;
	__asm__ volatile ("int $0x80"
	                  : "=a" (result)
	                  : "a" (number), "b" (arg)
	                  : "memory");
	return result;
}

static void print(const char *s)
{
	syscall(SYS_WRITE, (int) s);
}

/* Turn a number into text. We have no printf out here - the kernel's kprintf
 * lives on the other side of a privilege boundary and might as well be on
 * another machine. */
static void print_number(unsigned int value)
{
	char buffer[12];
	int i = 0;

	if (value == 0) {
		print("0");
		return;
	}

	while (value > 0) {
		buffer[i++] = (char)('0' + (value % 10));
		value /= 10;
	}

	char out[13];
	int j = 0;
	while (i-- > 0)
		out[j++] = buffer[i];
	out[j] = '\0';

	print(out);
}

/* Uninitialised globals - these live in .bss, which occupies NO space in the
 * file. The ELF program header says "give me this much memory, but only this
 * much of it comes from the file", and the loader zeroes the rest.
 *
 * Prove it: this array is 4096 bytes and the binary is well under that. */
static char scratch[4096];
static unsigned int counter;

/* The entry point is now DECLARED in the ELF header rather than assumed to be
 * the first byte, so its position in the file no longer matters. Kept in its
 * own section anyway, because it costs nothing and keeps the layout readable. */
__attribute__((section(".text._start")))
void _start(void)
{
	print("  [prog] loaded from disk, running in ring 3\n");

	/* Check .bss really did arrive zeroed. If the loader forgot, this prints
	 * whatever the previous occupant of that memory left behind - which would
	 * be both a bug and an information leak. */
	counter = 0;
	for (int i = 0; i < 4096; i++)
		counter += (unsigned int) scratch[i];

	print("  [prog] .bss checksum (0 means properly zeroed): ");
	print_number(counter);
	print("\n");

	/* Writable data lives in a segment the loader mapped read-write, so this
	 * is allowed. Writing to the code segment would fault instead. */
	scratch[0] = 'A';
	print("  [prog] wrote to .bss without faulting\n");

	print("  [prog] ticks since boot: ");
	print_number((unsigned int) syscall(SYS_TICKS, 0));
	print("\n");

	print("  [prog] asking the kernel to read its own memory\n");
	syscall(SYS_WRITE, 0x100000);

	/* Last thing: try to modify our own code. The ELF header marked that
	 * segment read-only and the loader honoured it, so this should fault - and
	 * the fault should kill this program without disturbing the kernel.
	 *
	 * Nothing after this line runs. */
	print("  [prog] now trying to write to my own code segment\n");

	*(volatile char *) _start = 0;

	print("  [prog] STILL HERE - the code segment is writable, which is wrong\n");
	syscall(SYS_EXIT, 0);

	for (;;) { }
}
