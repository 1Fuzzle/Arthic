/* test_fs.c - fs/fs.c.
 *
 * ArthicFS is the largest module in the kernel and the one whose mistakes last
 * longest: a filesystem writes a format to a disk, and the disk outlives the
 * code. Checking it by hand in QEMU means typing `write`, `cat`, `ls` and
 * believing what comes back, which exercises the happy path of a nearly empty
 * disk and nothing else.
 *
 * The cases that matter are the other ones - a file crossing from its direct
 * blocks into the indirect block, a disk full, a name at the length limit, a
 * write failing halfway. The RAM disk in tests/support/fake_disk.c makes all
 * of them reachable, and lets a test look at the sectors afterwards to see
 * what was actually written rather than what was reported.
 */
#include <string.h>

#include "arthictest.h"

#include "support/support.h"

#include "ata.h"
#include "fs.h"

#define DISK_SECTORS 512                 /* 256 KB: enough to fill, small enough to */
                                         /* fill quickly */

static void disk_fresh(void)
{
	console_reset();
	fake_disk_reset(DISK_SECTORS);
	CHECK_EQ(fs_format(), 1);
}

/* ---- mount and format ----------------------------------------------------- */

TEST(format_writes_a_superblock_the_next_mount_accepts)
{
	console_reset();
	fake_disk_reset(DISK_SECTORS);

	CHECK_EQ(fs_format(), 1);
	CHECK_EQ(fs_is_mounted(), 1);

	/* Read the superblock off the disk rather than asking fs.c what it
	 * thinks: the on-disk bytes are the contract, and they are what a future
	 * version will have to keep reading. */
	const struct fs_superblock *super =
		(const struct fs_superblock *) fake_disk_sector(0);

	CHECK_EQ(super->magic, FS_MAGIC);
	CHECK_EQ(super->data_start, FS_DATA_START);
	CHECK_EQ(super->max_files, FS_MAX_FILES);
	CHECK_EQ(super->total_blocks, DISK_SECTORS - FS_DATA_START);

	CHECK_EQ(fs_mount(), 1);
}

TEST(mount_refuses_a_disk_that_is_not_ours)
{
	fake_disk_reset(DISK_SECTORS);

	/* An unformatted disk is all zeroes, so the magic does not match. Refusing
	 * is the whole point - the alternative is reading a stranger's data as if
	 * it were a directory. */
	CHECK_EQ(fs_mount(), 0);
	CHECK_EQ(fs_is_mounted(), 0);
}

TEST(mount_refuses_an_older_format)
{
	disk_fresh();

	struct fs_superblock *super = (struct fs_superblock *) fake_disk_sector(0);
	super->magic = 0x41525431u;          /* "ART1", the contiguous version */

	CHECK_EQ(fs_mount(), 0);
	CHECK_EQ(fs_is_mounted(), 0);
}

TEST(format_refuses_a_disk_with_no_room_for_data)
{
	fake_disk_reset(FS_DATA_START);      /* metadata and nothing else */

	CHECK_EQ(fs_format(), 0);
}

TEST(format_caps_the_block_count_at_what_the_bitmap_can_track)
{
	/* One sector of bitmap is 4096 bits. A larger disk must be capped, not
	 * trusted - tracking blocks the bitmap has no room for would write past
	 * the end of it. */
	fake_disk_reset(6000);
	CHECK_EQ(fs_format(), 1);

	const struct fs_superblock *super =
		(const struct fs_superblock *) fake_disk_sector(0);

	CHECK_EQ(super->total_blocks, FS_BLOCK_SIZE * 8);

	uint32_t total = 0;
	fs_stats(&total, NULL, NULL);
	CHECK_EQ(total, FS_BLOCK_SIZE * 8);
}

TEST(operations_are_refused_when_nothing_is_mounted)
{
	fake_disk_reset(DISK_SECTORS);
	fs_mount();                          /* fails, leaving mounted == 0 */

	CHECK_EQ(fs_is_mounted(), 0);
	CHECK_EQ(fs_create("a", "x", 1), 0);
	CHECK_EQ(fs_append("a", "x", 1), 0);
	CHECK_EQ(fs_read("a", NULL, 0, NULL), 0);
	CHECK_EQ(fs_delete("a"), 0);

	uint32_t total = 1, used = 1, files = 1;
	fs_stats(&total, &used, &files);
	CHECK_EQ(total, 0);
	CHECK_EQ(used, 0);
	CHECK_EQ(files, 0);

	console_reset();
	fs_list();
	CHECK(console_contains("no filesystem mounted"));
}

/* ---- creating and reading -------------------------------------------------- */

TEST(a_file_reads_back_exactly_what_was_written)
{
	disk_fresh();

	const char *text = "the quick brown fox";

	CHECK_EQ(fs_create("fox.txt", text, 19), 1);

	char     buffer[64];
	uint32_t size = 0;

	memset(buffer, 0, sizeof(buffer));
	CHECK_EQ(fs_read("fox.txt", buffer, sizeof(buffer), &size), 1);
	CHECK_EQ(size, 19);
	CHECK_MEM_EQ(buffer, text, 19);
}

TEST(a_file_survives_being_unmounted_and_mounted_again)
{
	disk_fresh();

	CHECK_EQ(fs_create("keep", "persistent", 10), 1);

	/* Everything fs.c holds in memory is thrown away and re-read from the
	 * sectors. If the directory or the bitmap was only ever updated in RAM,
	 * this is where it shows. */
	CHECK_EQ(fs_mount(), 1);

	char     buffer[16];
	uint32_t size = 0;

	memset(buffer, 0, sizeof(buffer));
	CHECK_EQ(fs_read("keep", buffer, sizeof(buffer), &size), 1);
	CHECK_EQ(size, 10);
	CHECK_MEM_EQ(buffer, "persistent", 10);

	uint32_t used = 0, files = 0;
	fs_stats(NULL, &used, &files);
	CHECK_EQ(files, 1);
	CHECK_EQ(used, 1);                   /* ten bytes still costs a whole block */
}

TEST(an_empty_file_is_allowed_and_uses_no_blocks)
{
	disk_fresh();

	CHECK_EQ(fs_create("empty", "", 0), 1);

	uint32_t used = 0, files = 0;
	fs_stats(NULL, &used, &files);
	CHECK_EQ(files, 1);
	CHECK_EQ(used, 0);

	uint32_t size = 99;
	char buffer[4];
	CHECK_EQ(fs_read("empty", buffer, sizeof(buffer), &size), 1);
	CHECK_EQ(size, 0);
}

TEST(reading_more_than_the_buffer_holds_truncates)
{
	disk_fresh();

	fs_create("long", "abcdefghij", 10);

	char     buffer[5] = { 0 };
	uint32_t size = 0;

	/* Truncating rather than overflowing is the only safe choice, and the
	 * caller is told how much it actually got. */
	CHECK_EQ(fs_read("long", buffer, 4, &size), 1);
	CHECK_EQ(size, 4);
	CHECK_MEM_EQ(buffer, "abcd", 4);
	CHECK_EQ(buffer[4], 0);              /* nothing written past the limit */
}

TEST(reading_a_file_that_is_not_there_fails)
{
	disk_fresh();

	char buffer[8];
	CHECK_EQ(fs_read("absent", buffer, sizeof(buffer), NULL), 0);
}

TEST(read_tolerates_a_null_size_pointer)
{
	disk_fresh();

	fs_create("x", "hi", 2);

	char buffer[4] = { 0 };
	CHECK_EQ(fs_read("x", buffer, sizeof(buffer), NULL), 1);
	CHECK_MEM_EQ(buffer, "hi", 2);
}

/* ---- names ----------------------------------------------------------------- */

TEST(a_duplicate_name_is_refused)
{
	disk_fresh();

	CHECK_EQ(fs_create("twice", "a", 1), 1);
	CHECK_EQ(fs_create("twice", "b", 1), 0);

	/* And the original is untouched. */
	char buffer[4] = { 0 };
	fs_read("twice", buffer, sizeof(buffer), NULL);
	CHECK_EQ(buffer[0], 'a');
}

TEST(an_empty_or_missing_name_is_refused)
{
	disk_fresh();

	CHECK_EQ(fs_create("", "x", 1), 0);
	CHECK_EQ(fs_create(NULL, "x", 1), 0);
}

TEST(a_name_at_the_limit_is_allowed_and_one_over_is_not)
{
	disk_fresh();

	char longest[FS_NAME_MAX];
	memset(longest, 'n', sizeof(longest));
	longest[FS_NAME_MAX - 1] = '\0';     /* 31 characters plus a terminator */

	CHECK_EQ(fs_create(longest, "x", 1), 1);

	char too_long[FS_NAME_MAX + 8];
	memset(too_long, 'm', sizeof(too_long));
	too_long[FS_NAME_MAX + 7] = '\0';

	/* The bound is the only thing standing between a long filename and a
	 * directory entry overwriting the one after it. */
	CHECK_EQ(fs_create(too_long, "x", 1), 0);

	uint32_t files = 0;
	fs_stats(NULL, NULL, &files);
	CHECK_EQ(files, 1);
}

TEST(the_directory_fills_up_and_says_so)
{
	disk_fresh();

	char name[8];

	for (uint32_t i = 0; i < FS_MAX_FILES; i++) {
		snprintf(name, sizeof(name), "f%u", i);
		CHECK_EQ(fs_create(name, "", 0), 1);
	}

	CHECK_EQ(fs_create("one-too-many", "", 0), 0);

	uint32_t files = 0;
	fs_stats(NULL, NULL, &files);
	CHECK_EQ(files, FS_MAX_FILES);
}

/* ---- growing ---------------------------------------------------------------- */

TEST(append_extends_a_file_within_one_block)
{
	disk_fresh();

	fs_create("notes", "one ", 4);
	CHECK_EQ(fs_append("notes", "two", 3), 1);

	char     buffer[16] = { 0 };
	uint32_t size = 0;

	CHECK_EQ(fs_read("notes", buffer, sizeof(buffer), &size), 1);
	CHECK_EQ(size, 7);
	CHECK_MEM_EQ(buffer, "one two", 7);

	/* Appending inside an existing block must not allocate another one - and
	 * must not lose what was already in it, which means reading the block
	 * before rewriting it. */
	uint32_t used = 0;
	fs_stats(NULL, &used, NULL);
	CHECK_EQ(used, 1);
}

TEST(append_to_a_file_that_is_not_there_fails)
{
	disk_fresh();

	CHECK_EQ(fs_append("nowhere", "x", 1), 0);
}

TEST(a_file_spanning_several_blocks_reads_back_whole)
{
	disk_fresh();

	/* Not a multiple of the block size, so the last block is partial - the
	 * case where a length calculation that rounds the wrong way loses the
	 * tail or returns rubbish after it. */
	static char written[FS_BLOCK_SIZE * 3 + 37];
	for (size_t i = 0; i < sizeof(written); i++)
		written[i] = (char)(i * 7);

	CHECK_EQ(fs_create("big", written, sizeof(written)), 1);

	static char read_back[sizeof(written)];
	uint32_t size = 0;

	CHECK_EQ(fs_read("big", read_back, sizeof(read_back), &size), 1);
	CHECK_EQ(size, sizeof(written));
	CHECK_MEM_EQ(read_back, written, sizeof(written));

	uint32_t used = 0;
	fs_stats(NULL, &used, NULL);
	CHECK_EQ(used, 4);
}

TEST(a_file_grows_past_its_direct_blocks_into_the_indirect_one)
{
	disk_fresh();

	/* Nine blocks: one more than fits in the entry itself, so the ninth has
	 * to go through the indirect block. This is the boundary the whole
	 * two-level design exists for, and nothing in the shell reaches it. */
	static char written[FS_BLOCK_SIZE * (FS_DIRECT_BLOCKS + 1)];
	for (size_t i = 0; i < sizeof(written); i++)
		written[i] = (char)(i % 251);

	CHECK_EQ(fs_create("indirect", written, sizeof(written)), 1);

	static char read_back[sizeof(written)];
	uint32_t size = 0;

	CHECK_EQ(fs_read("indirect", read_back, sizeof(read_back), &size), 1);
	CHECK_EQ(size, sizeof(written));
	CHECK_MEM_EQ(read_back, written, sizeof(written));

	/* Nine data blocks plus one block holding the block numbers. That extra
	 * block is the overhead the design accepts, and only large files pay it. */
	uint32_t used = 0;
	fs_stats(NULL, &used, NULL);
	CHECK_EQ(used, FS_DIRECT_BLOCKS + 2);
}

TEST(appending_across_the_indirect_boundary_keeps_the_earlier_data)
{
	disk_fresh();

	static char first[FS_BLOCK_SIZE * FS_DIRECT_BLOCKS];
	memset(first, 'A', sizeof(first));

	CHECK_EQ(fs_create("grow", first, sizeof(first)), 1);

	static char second[FS_BLOCK_SIZE];
	memset(second, 'B', sizeof(second));

	CHECK_EQ(fs_append("grow", second, sizeof(second)), 1);

	static char read_back[sizeof(first) + sizeof(second)];
	uint32_t size = 0;

	CHECK_EQ(fs_read("grow", read_back, sizeof(read_back), &size), 1);
	CHECK_EQ(size, sizeof(read_back));
	CHECK_MEM_EQ(read_back, first, sizeof(first));
	CHECK_MEM_EQ(read_back + sizeof(first), second, sizeof(second));
}

TEST(a_file_cannot_grow_past_the_maximum_block_count)
{
	fake_disk_reset(4096);               /* room to spare, so the limit is the */
	CHECK_EQ(fs_format(), 1);            /* file format rather than the disk   */

	static char full[FS_BLOCK_SIZE * FS_MAX_BLOCKS];
	memset(full, 'F', sizeof(full));

	CHECK_EQ(fs_create("max", full, sizeof(full)), 1);

	/* One byte more needs block 137, and there is nowhere to record it. The
	 * ceiling is 8 direct + 128 indirect = 68 KB per file. */
	CHECK_EQ(fs_append("max", "x", 1), 0);

	uint32_t size = 0;
	static char read_back[sizeof(full)];
	CHECK_EQ(fs_read("max", read_back, sizeof(read_back), &size), 1);
	CHECK_EQ(size, sizeof(full));

	fake_disk_reset(DISK_SECTORS);
}

TEST(a_file_larger_than_the_format_allows_is_refused_outright)
{
	fake_disk_reset(4096);
	CHECK_EQ(fs_format(), 1);

	static char over[FS_BLOCK_SIZE * (FS_MAX_BLOCKS + 1)];
	memset(over, 'o', sizeof(over));

	CHECK_EQ(fs_create("toobig", over, sizeof(over)), 0);

	/* And no half-made file is left in the directory. */
	uint32_t files = 1;
	fs_stats(NULL, NULL, &files);
	CHECK_EQ(files, 0);

	fake_disk_reset(DISK_SECTORS);
}

TEST(a_full_disk_is_reported_rather_than_overrun)
{
	/* A small disk, deliberately: 24 sectors leaves six data blocks. */
	fake_disk_reset(FS_DATA_START + 6);
	CHECK_EQ(fs_format(), 1);

	static char block[FS_BLOCK_SIZE];
	memset(block, 'd', sizeof(block));

	for (int i = 0; i < 6; i++) {
		char name[8];
		snprintf(name, sizeof(name), "b%d", i);
		CHECK_EQ(fs_create(name, block, sizeof(block)), 1);
	}

	uint32_t total = 0, used = 0;
	fs_stats(&total, &used, NULL);
	CHECK_EQ(used, total);

	CHECK_EQ(fs_create("nomore", block, sizeof(block)), 0);

	uint32_t files = 0;
	fs_stats(NULL, NULL, &files);
	CHECK_EQ(files, 6);                  /* the failed one left nothing behind */

	fake_disk_reset(DISK_SECTORS);
}

/* ---- deleting ---------------------------------------------------------------- */

TEST(delete_removes_the_entry_and_returns_the_blocks)
{
	disk_fresh();

	static char data[FS_BLOCK_SIZE * 2];
	memset(data, 'z', sizeof(data));

	fs_create("gone", data, sizeof(data));

	uint32_t used_before = 0;
	fs_stats(NULL, &used_before, NULL);
	CHECK_EQ(used_before, 2);

	CHECK_EQ(fs_delete("gone"), 1);

	uint32_t used = 0, files = 0;
	fs_stats(NULL, &used, &files);
	CHECK_EQ(used, 0);
	CHECK_EQ(files, 0);

	char buffer[8];
	CHECK_EQ(fs_read("gone", buffer, sizeof(buffer), NULL), 0);
}

TEST(deleting_a_large_file_also_frees_its_indirect_block)
{
	disk_fresh();

	static char data[FS_BLOCK_SIZE * (FS_DIRECT_BLOCKS + 2)];
	memset(data, 'q', sizeof(data));

	fs_create("wide", data, sizeof(data));
	CHECK_EQ(fs_delete("wide"), 1);

	/* Forgetting the indirect block would leak one block per large file -
	 * space that nothing can ever reclaim, and invisible until the disk is
	 * mysteriously full. */
	uint32_t used = 1;
	fs_stats(NULL, &used, NULL);
	CHECK_EQ(used, 0);
}

TEST(the_space_a_deleted_file_used_can_be_taken_again)
{
	disk_fresh();

	static char block[FS_BLOCK_SIZE];
	memset(block, 'r', sizeof(block));

	fs_create("first", block, sizeof(block));
	fs_delete("first");

	CHECK_EQ(fs_create("second", block, sizeof(block)), 1);

	uint32_t used = 0;
	fs_stats(NULL, &used, NULL);
	CHECK_EQ(used, 1);
}

TEST(deleting_a_file_that_is_not_there_fails)
{
	disk_fresh();

	CHECK_EQ(fs_delete("imaginary"), 0);
}

/* ---- listing ------------------------------------------------------------------ */

TEST(an_empty_filesystem_lists_as_empty)
{
	disk_fresh();

	console_reset();
	fs_list();

	CHECK(console_contains("(empty)"));
}

TEST(the_listing_names_each_file_and_its_size)
{
	disk_fresh();

	fs_create("alpha", "12345", 5);

	static char big[FS_BLOCK_SIZE * (FS_DIRECT_BLOCKS + 1)];
	memset(big, 'b', sizeof(big));
	fs_create("beta", big, sizeof(big));

	console_reset();
	fs_list();

	CHECK(console_contains("alpha"));
	CHECK(console_contains("5 bytes"));
	CHECK(console_contains("1 block\n"));      /* singular for one block */
	CHECK(console_contains("beta"));
	CHECK(console_contains("9 blocks + indirect"));
}

/* ---- when the disk misbehaves --------------------------------------------------- */

TEST(a_disk_that_fails_mid_format_reports_failure)
{
	fake_disk_reset(DISK_SECTORS);
	fake_disk_fail_writes_after(3);      /* superblock, then two directory sectors */

	CHECK_EQ(fs_format(), 0);
}

TEST(a_write_failure_leaves_no_half_made_file)
{
	disk_fresh();

	static char data[FS_BLOCK_SIZE * 2];
	memset(data, 'w', sizeof(data));

	/* Fail after the first data block is written, so creation dies partway
	 * through - the moment where a filesystem is most likely to leave a
	 * directory entry pointing at data that was never stored. */
	fake_disk_fail_writes_after(fake_disk_write_count() + 1);

	CHECK_EQ(fs_create("doomed", data, sizeof(data)), 0);

	fake_disk_fail_writes_after(0xFFFFFFFFu);

	uint32_t files = 1;
	fs_stats(NULL, NULL, &files);
	CHECK_EQ(files, 0);

	char buffer[8];
	CHECK_EQ(fs_read("doomed", buffer, sizeof(buffer), NULL), 0);
}

TEST(a_disk_that_cannot_be_read_fails_to_mount)
{
	disk_fresh();

	fake_disk_reset(DISK_SECTORS);
	fs_format();

	fake_disk_fail_reads_after(0);       /* even the superblock is unreachable */

	CHECK_EQ(fs_mount(), 0);
	CHECK_EQ(fs_is_mounted(), 0);
}

int main(void)
{
	RUN(format_writes_a_superblock_the_next_mount_accepts);
	RUN(mount_refuses_a_disk_that_is_not_ours);
	RUN(mount_refuses_an_older_format);
	RUN(format_refuses_a_disk_with_no_room_for_data);
	RUN(format_caps_the_block_count_at_what_the_bitmap_can_track);
	RUN(operations_are_refused_when_nothing_is_mounted);

	RUN(a_file_reads_back_exactly_what_was_written);
	RUN(a_file_survives_being_unmounted_and_mounted_again);
	RUN(an_empty_file_is_allowed_and_uses_no_blocks);
	RUN(reading_more_than_the_buffer_holds_truncates);
	RUN(reading_a_file_that_is_not_there_fails);
	RUN(read_tolerates_a_null_size_pointer);

	RUN(a_duplicate_name_is_refused);
	RUN(an_empty_or_missing_name_is_refused);
	RUN(a_name_at_the_limit_is_allowed_and_one_over_is_not);
	RUN(the_directory_fills_up_and_says_so);

	RUN(append_extends_a_file_within_one_block);
	RUN(append_to_a_file_that_is_not_there_fails);
	RUN(a_file_spanning_several_blocks_reads_back_whole);
	RUN(a_file_grows_past_its_direct_blocks_into_the_indirect_one);
	RUN(appending_across_the_indirect_boundary_keeps_the_earlier_data);
	RUN(a_file_cannot_grow_past_the_maximum_block_count);
	RUN(a_file_larger_than_the_format_allows_is_refused_outright);
	RUN(a_full_disk_is_reported_rather_than_overrun);

	RUN(delete_removes_the_entry_and_returns_the_blocks);
	RUN(deleting_a_large_file_also_frees_its_indirect_block);
	RUN(the_space_a_deleted_file_used_can_be_taken_again);
	RUN(deleting_a_file_that_is_not_there_fails);

	RUN(an_empty_filesystem_lists_as_empty);
	RUN(the_listing_names_each_file_and_its_size);

	RUN(a_disk_that_fails_mid_format_reports_failure);
	RUN(a_write_failure_leaves_no_half_made_file);
	RUN(a_disk_that_cannot_be_read_fails_to_mount);

	fake_disk_release();

	return test_report("fs");
}
