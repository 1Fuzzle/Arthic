/* test_string.c - lib/string.c.
 *
 * The four functions here are the least interesting code in the kernel and the
 * most dangerous to get wrong, because everything else is built on them. A
 * kmemcpy that copies one byte too few would show up as a corrupt directory
 * entry or a garbled screen, three subsystems away from the actual mistake.
 *
 * The tests therefore care mostly about edges: zero-length calls, the byte
 * immediately past the end, signedness, and the empty string.
 */
#include "arthictest.h"

#include "string.h"

TEST(memset_fills_exactly_the_requested_bytes)
{
	unsigned char buffer[8];

	memset(buffer, 0x11, sizeof(buffer));

	void *returned = kmemset(buffer + 2, 0xAB, 4);

	/* The return value is the destination, as with the real memset - callers
	 * chain on it. */
	CHECK(returned == buffer + 2);

	CHECK_EQ(buffer[0], 0x11);
	CHECK_EQ(buffer[1], 0x11);
	CHECK_EQ(buffer[2], 0xAB);
	CHECK_EQ(buffer[5], 0xAB);
	/* The byte after the region is the one that catches an off-by-one. */
	CHECK_EQ(buffer[6], 0x11);
	CHECK_EQ(buffer[7], 0x11);
}

TEST(memset_uses_only_the_low_byte_of_value)
{
	unsigned char buffer[4] = { 0, 0, 0, 0 };

	/* The parameter is an int purely to match the real memset. Anything above
	 * 0xFF must be truncated, not stored. */
	kmemset(buffer, 0x1234, sizeof(buffer));

	for (size_t i = 0; i < sizeof(buffer); i++)
		CHECK_EQ(buffer[i], 0x34);
}

TEST(memset_of_zero_bytes_writes_nothing)
{
	unsigned char buffer[2] = { 1, 2 };

	kmemset(buffer, 0xFF, 0);

	CHECK_EQ(buffer[0], 1);
	CHECK_EQ(buffer[1], 2);
}

TEST(memcpy_copies_forward_and_returns_dest)
{
	const char   source[] = "abcdef";
	unsigned char dest[8];

	memset(dest, '.', sizeof(dest));

	void *returned = kmemcpy(dest, source, 6);

	CHECK(returned == dest);
	CHECK_MEM_EQ(dest, "abcdef", 6);
	CHECK_EQ(dest[6], '.');
}

TEST(memcpy_of_zero_bytes_writes_nothing)
{
	unsigned char dest[2] = { 7, 8 };

	kmemcpy(dest, "xy", 0);

	CHECK_EQ(dest[0], 7);
	CHECK_EQ(dest[1], 8);
}

TEST(memcpy_handles_overlap_when_dest_is_below_src)
{
	/* Copying forward is safe in this direction and only this direction. The
	 * comment in string.c says so; this is the test that says so too. Shifting
	 * a buffer left by one is the case the terminal's scroll relies on. */
	char buffer[] = "abcdef";

	kmemcpy(buffer, buffer + 1, 5);

	CHECK_MEM_EQ(buffer, "bcdef", 5);
}

TEST(strcmp_returns_zero_only_for_identical_strings)
{
	CHECK_EQ(kstrcmp("", ""), 0);
	CHECK_EQ(kstrcmp("help", "help"), 0);
	CHECK(kstrcmp("help", "helo") != 0);
	CHECK(kstrcmp("help", "help ") != 0);
	CHECK(kstrcmp("", "h") != 0);
}

TEST(strcmp_orders_by_first_differing_byte)
{
	CHECK(kstrcmp("a", "b") < 0);
	CHECK(kstrcmp("b", "a") > 0);

	/* A shorter string sorts before a longer one it prefixes, because the
	 * terminating zero compares below any character. */
	CHECK(kstrcmp("ls", "lsx") < 0);
	CHECK(kstrcmp("lsx", "ls") > 0);
}

TEST(strcmp_treats_high_bytes_as_unsigned)
{
	/* plain `char` is signed on x86, so comparing without the cast in
	 * string.c would make 0x80 sort BELOW 'a' instead of above it. Any byte
	 * over 127 arriving from the keyboard or a filename would then compare
	 * backwards. */
	const char high[] = { (char) 0x80, '\0' };

	CHECK(kstrcmp(high, "a") > 0);
	CHECK(kstrcmp("a", high) < 0);
}

TEST(startswith_matches_prefixes_only_at_the_start)
{
	CHECK_EQ(kstartswith("echo hello", "echo "), 1);
	CHECK_EQ(kstartswith("echo", "echo"), 1);

	/* Every string starts with the empty prefix - the shell relies on the
	 * loop simply not running. */
	CHECK_EQ(kstartswith("anything", ""), 1);

	CHECK_EQ(kstartswith("ec", "echo"), 0);   /* prefix longer than string */
	CHECK_EQ(kstartswith("", "echo"), 0);
	CHECK_EQ(kstartswith("say echo", "echo"), 0);
}

int main(void)
{
	RUN(memset_fills_exactly_the_requested_bytes);
	RUN(memset_uses_only_the_low_byte_of_value);
	RUN(memset_of_zero_bytes_writes_nothing);
	RUN(memcpy_copies_forward_and_returns_dest);
	RUN(memcpy_of_zero_bytes_writes_nothing);
	RUN(memcpy_handles_overlap_when_dest_is_below_src);
	RUN(strcmp_returns_zero_only_for_identical_strings);
	RUN(strcmp_orders_by_first_differing_byte);
	RUN(strcmp_treats_high_bytes_as_unsigned);
	RUN(startswith_matches_prefixes_only_at_the_start);

	return test_report("string");
}
