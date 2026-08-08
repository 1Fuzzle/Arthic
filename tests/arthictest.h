/* arthictest.h - the whole test framework, such as it is.
 *
 * WHY THESE TESTS RUN ON LINUX AND NOT IN THE KERNEL
 *
 * Most of what Arthic does can only be observed by booting it. But a fair
 * amount of it is ordinary logic that happens to live in a kernel: a first-fit
 * allocator, a ring buffer, a bitmap, a block list. None of that needs a CPU in
 * protected mode to be true or false, so we compile those files - the real
 * ones, unmodified - into a normal Linux program and call them directly.
 *
 * The parts they would normally lean on (the disk, the scheduler, the frame
 * allocator, the screen) are replaced by fakes in tests/support. That is the
 * only reason this works, and it is worth noticing WHY it works: those modules
 * talk to the rest of the kernel through narrow, declared interfaces, so a
 * different implementation can be substituted at link time. Code that reaches
 * directly into hardware - terminal.c writing to 0xB8000, ata.c running `in`
 * and `out`, anything executing `cli` - cannot be tested this way, because
 * those instructions fault in a user-mode process.
 *
 * The test programs are HOSTED: they may use printf and exit, because they are
 * not part of the kernel. The kernel files they link against are still built
 * freestanding, with the same flags build.sh uses. Nothing under test is
 * compiled differently from the real thing.
 *
 * WHY -m32
 *
 * Not a detail. Arthic stores addresses in uint32_t all over the place -
 * kheap.c casts the value pmm_alloc_frames returns straight to a pointer. On a
 * 64-bit build that cast would truncate the address and the tests would fail
 * for a reason that has nothing to do with the kernel. Testing a 32-bit kernel
 * means building the tests 32-bit too.
 *
 * USAGE
 *
 *   TEST(name) { ... CHECK(...); }        define a case
 *   RUN(name);                            run it from main
 *   return test_report();                 print the tally, return an exit code
 */
#ifndef ARTHIC_TEST_H
#define ARTHIC_TEST_H

#include <stdio.h>
#include <string.h>

static int tests_run;
static int tests_failed;
static int current_failed;   /* did the case being run fail a check yet? */

/* A test case is just a function taking nothing and returning nothing. The
 * macro exists so the declaration reads as a sentence and so RUN() can find
 * the function by name. */
#define TEST(name) static void test_##name(void)

/* `do { ... } while (0)` is the standard way to make a multi-statement macro
 * behave like one statement, so it is safe after an `if` without braces. */
#define RUN(name)                                                              \
	do {                                                                       \
		current_failed = 0;                                                    \
		tests_run++;                                                           \
		test_##name();                                                         \
		if (current_failed) {                                                  \
			tests_failed++;                                                    \
			printf("  FAIL  %s\n", #name);                                     \
		} else {                                                               \
			printf("  ok    %s\n", #name);                                     \
		}                                                                      \
	} while (0)

/* #cond stringifies the condition, so a failure prints the source text of what
 * was expected rather than just a line number. */
#define CHECK(cond)                                                            \
	do {                                                                       \
		if (!(cond)) {                                                         \
			current_failed = 1;                                                \
			printf("        %s:%d: expected %s\n", __FILE__, __LINE__, #cond); \
		}                                                                      \
	} while (0)

/* Comparisons get their own macros purely so the failure message can show the
 * values involved, which is most of what you want to know. Both sides are cast
 * to long so one macro covers ints, uint32_t and sizes alike. */
#define CHECK_EQ(actual, expected)                                             \
	do {                                                                       \
		long a_ = (long)(actual), e_ = (long)(expected);                       \
		if (a_ != e_) {                                                        \
			current_failed = 1;                                                \
			printf("        %s:%d: %s == %ld, expected %ld\n",                 \
			       __FILE__, __LINE__, #actual, a_, e_);                       \
		}                                                                      \
	} while (0)

#define CHECK_STR_EQ(actual, expected)                                         \
	do {                                                                       \
		const char *a_ = (actual), *e_ = (expected);                           \
		if (strcmp(a_, e_) != 0) {                                             \
			current_failed = 1;                                                \
			printf("        %s:%d: %s == \"%s\", expected \"%s\"\n",           \
			       __FILE__, __LINE__, #actual, a_, e_);                       \
		}                                                                      \
	} while (0)

#define CHECK_MEM_EQ(actual, expected, length)                                 \
	do {                                                                       \
		if (memcmp((actual), (expected), (length)) != 0) {                     \
			current_failed = 1;                                                \
			printf("        %s:%d: %s differs from %s over %u bytes\n",        \
			       __FILE__, __LINE__, #actual, #expected,                     \
			       (unsigned)(length));                                        \
		}                                                                      \
	} while (0)

static inline int test_report(const char *suite)
{
	printf("%s: %d run, %d failed\n", suite, tests_run, tests_failed);
	/* A non-zero exit status is how build.sh knows the suite failed, and it
	 * is the only reason `set -e` can be trusted to stop on a bad test. */
	return tests_failed == 0 ? 0 : 1;
}

#endif
