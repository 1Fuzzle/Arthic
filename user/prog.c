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

/* Placed in its own section so the linker script can force it to the very
 * front of the binary. The loader jumps to the first byte and nothing else -
 * a flat binary has no header to say where the entry point is. */
__attribute__((section(".text._start")))
void _start(void)
{
	print("  [prog] loaded from disk, running in ring 3\n");

	print("  [prog] ticks since boot: ");
	print_number((unsigned int) syscall(SYS_TICKS, 0));
	print("\n");

	print("  [prog] asking the kernel to read its own memory\n");
	syscall(SYS_WRITE, 0x100000);

	print("  [prog] done, exiting\n");
	syscall(SYS_EXIT, 0);

	for (;;) { }
}
