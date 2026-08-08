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
#define SYS_SLEEP 3
#define SYS_ID    4
#define SYS_PIPE_WRITE 5
#define SYS_PIPE_READ  6

/* Where the loader leaves whatever `run prog <this>` was given. */
#define ARGS ((const char *) 0x20100000)

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

/* Two arguments: a buffer and a length. The kernel checks both, and it has to -
 * either one being wrong is a way to make it read or write memory on our
 * behalf. */
static int syscall2(int number, int arg1, int arg2)
{
	int result;
	__asm__ volatile ("int $0x80"
	                  : "=a" (result)
	                  : "a" (number), "b" (arg1), "c" (arg2)
	                  : "memory");
	return result;
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
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
/* Try to execute data.
 *
 * Copies a `ret` instruction (0xC3) into a stack array and calls it. On a
 * system without NX this returns harmlessly - the data was executable and
 * nothing objected, which is precisely the problem. With NX the CPU refuses on
 * the instruction fetch and the program is killed.
 *
 * That difference is the whole of W^X in one line. Every buffer overflow that
 * ends in "and then jump to the shellcode the attacker wrote into the buffer"
 * depends on data being executable.
 */
static void run_nx_test(void)
{
	volatile char code[16];
	code[0] = (char) 0xC3;               /* ret */

	print("  [nx] wrote a ret instruction onto my own stack\n");
	print("  [nx] now calling it - NX should stop this\n");

	void (*f)(void) = (void (*)(void)) code;
	f();

	print("  [nx] SURVIVED - the stack is executable, W^X is NOT enforced\n");
}

/* ---- the two pipe modes ----------------------------------------------------
 *
 * Same binary, different behaviour depending on the argument. Two separate
 * processes, in separate address spaces, sharing nothing except a channel the
 * kernel owns - which is exactly what a pipe is for.
 */
static void run_writer(void)
{
	char line[] = "line 00 through the pipe\n";

	print("  [writer] sending 20 lines\n");

	for (int i = 1; i <= 20; i++) {
		line[5] = (char)('0' + (i / 10) % 10);
		line[6] = (char)('0' + i % 10);

		int length = 0;
		while (line[length])
			length++;

		syscall2(SYS_PIPE_WRITE, (int) line, length);
	}

	syscall2(SYS_PIPE_WRITE, (int) "DONE\n", 5);
	print("  [writer] finished\n");
}

static void run_reader(void)
{
	char chunk[65];

	print("  [reader] waiting on the pipe\n");

	for (;;) {
		int got = syscall2(SYS_PIPE_READ, (int) chunk, 64);

		if (got <= 0)
			break;

		chunk[got] = '\0';
		print(chunk);

		for (int i = 0; i + 3 < got; i++) {
			if (chunk[i] == 'D' && chunk[i+1] == 'O' &&
			    chunk[i+2] == 'N' && chunk[i+3] == 'E') {
				print("  [reader] finished\n");
				return;
			}
		}
	}
}

__attribute__((section(".text._start")))
void _start(void)
{
	/* Behave differently depending on what we were told. */
	if (streq(ARGS, "write")) {
		run_writer();
		syscall(SYS_EXIT, 0);
	}

	if (streq(ARGS, "nx")) {
		run_nx_test();
		syscall(SYS_EXIT, 0);
	}

	if (streq(ARGS, "read")) {
		run_reader();
		syscall(SYS_EXIT, 0);
	}

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

	/* Every copy of this program is loaded at the same address, in its own
	 * address space. Printing the task id alongside a fixed address makes
	 * that visible: two programs, same 0x20000000, different memory. */
	unsigned int id = (unsigned int) syscall(SYS_ID, 0);

	for (int round = 1; round <= 4; round++) {
		print("  [prog id ");
		print_number(id);
		print("] round ");
		print_number((unsigned int) round);
		print(" of 4, my code is at 0x20000000\n");
		syscall(SYS_SLEEP, 18);
	}

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
