/* fs.c - ArthicFS, block-mapped.
 *
 * Everything reduces to reading and writing 512-byte sectors, which is the only
 * thing the disk understands. A filesystem is an agreed convention about what
 * those sectors mean.
 *
 * ON-DISK STRUCTURES ARE A CONTRACT
 *
 * Once something is written to a disk it outlives the code that wrote it. The
 * structures in fs.h are `packed` so the layout is exactly what we say it is,
 * with no compiler padding, and the superblock carries a magic number so a disk
 * written by a different version is refused rather than misread. Changing the
 * layout means changing the magic - which is what happened between version 1
 * and this one.
 */

#include "fs.h"
#include "ata.h"
#include "string.h"
#include "terminal.h"

static struct fs_superblock super;
static int mounted = 0;

/* Scratch space. Static rather than on the stack because a kernel stack is
 * 8 KB and 512-byte locals add up faster than you expect. */
static uint8_t sector_buffer[SECTOR_SIZE];
static uint8_t indirect_buffer[SECTOR_SIZE];
static uint8_t bitmap[SECTOR_SIZE];

static struct fs_entry directory[FS_MAX_FILES];

/* ---- reporting why something failed ---------------------------------------
 *
 * One variable holding the reason for the most recent failure, in the style of
 * errno. `fail` sets it and returns 0 in one expression, so every failure path
 * below reads `return fail(FS_ERR_IO);` - short enough that there is no
 * temptation to leave the reason out.
 */
static enum fs_error last_error = FS_OK;

static int fail(enum fs_error error)
{
	last_error = error;
	return 0;
}

enum fs_error fs_last_error(void)
{
	return last_error;
}

const char *fs_error_string(enum fs_error error)
{
	switch (error) {
	case FS_OK:              return "no error";
	case FS_ERR_NOT_MOUNTED: return "no filesystem mounted";
	case FS_ERR_NO_DISK:     return "no disk, or it is too small";
	case FS_ERR_IO:          return "disk read or write failed";
	case FS_ERR_BAD_MAGIC:   return "not an ArthicFS disk";
	case FS_ERR_NOT_FOUND:   return "no such file";
	case FS_ERR_EXISTS:      return "already exists";
	case FS_ERR_NAME:        return "bad or too long a name";
	case FS_ERR_DIR_FULL:    return "directory full";
	case FS_ERR_DISK_FULL:   return "disk full";
	case FS_ERR_TOO_BIG:     return "file at its maximum size";
	case FS_ERR_CORRUPT:     return "corrupt block list";
	case FS_ERR_TRUNCATED:   return "truncated - buffer too small";
	}

	return "unknown error";
}

/* ---- directory and bitmap ------------------------------------------------- */

static int read_directory(void)
{
	uint8_t *dest = (uint8_t *) directory;

	for (uint32_t i = 0; i < FS_DIR_SECTORS; i++)
		if (!ata_read_sector(FS_DIR_START + i, dest + i * SECTOR_SIZE))
			return 0;

	return 1;
}

static int write_directory(void)
{
	const uint8_t *src = (const uint8_t *) directory;

	for (uint32_t i = 0; i < FS_DIR_SECTORS; i++)
		if (!ata_write_sector(FS_DIR_START + i, src + i * SECTOR_SIZE))
			return 0;

	return 1;
}

static int block_used(uint32_t block)
{
	return (bitmap[block / 8] >> (block % 8)) & 1;
}

static void block_mark(uint32_t block, int used)
{
	if (used)
		bitmap[block / 8] |= (uint8_t)(1 << (block % 8));
	else
		bitmap[block / 8] &= (uint8_t) ~(1 << (block % 8));
}

/* Take any free block.
 *
 * Note what is NOT here: no search for a run of adjacent blocks. That
 * requirement is what made the old version fail on a fragmented disk, and
 * dropping it is the whole improvement. A file's blocks can be scattered
 * anywhere because the file now records where they are.
 *
 * The cost is real but subtle: scattered blocks mean a spinning disk seeks
 * between them. That is why defragmenting was once a thing people did, and why
 * filesystems still try to keep a file's blocks near each other even though
 * they no longer have to.
 */
/* Blocks claimed by the operation currently in progress.
 *
 * The bitmap on disk is only rewritten once an operation has finished, so
 * until then block_alloc has changed nothing but memory. That is what makes
 * undoing possible - and undoing is necessary, because a write that fails half
 * way used to leave its blocks marked used in memory with nothing referring to
 * them. They were lost until the next `format`: a leak the caller was never
 * told about, and one that made the in-memory bitmap disagree with the disk.
 *
 * One file can occupy at most FS_MAX_BLOCKS blocks plus its indirect block,
 * which bounds this list. */
static uint32_t claimed[FS_MAX_BLOCKS + 1];
static uint32_t claimed_count = 0;

static void claim_begin(void)
{
	claimed_count = 0;
}

static void claim_rollback(void)
{
	for (uint32_t i = 0; i < claimed_count; i++)
		block_mark(claimed[i], 0);

	claimed_count = 0;
}

static uint32_t block_alloc(void)
{
	for (uint32_t b = 0; b < super.total_blocks; b++) {
		if (!block_used(b)) {
			block_mark(b, 1);

			/* The bound cannot be exceeded by a correct caller; refusing
			 * rather than writing past the array is the only safe answer if
			 * one ever is. */
			if (claimed_count < FS_MAX_BLOCKS + 1)
				claimed[claimed_count++] = b;
			else {
				block_mark(b, 0);
				return 0xFFFFFFFFu;
			}

			return b;
		}
	}
	return 0xFFFFFFFFu;
}

/* ---- walking a file's blocks ----------------------------------------------
 * Block N of a file is either named directly in the entry, or found in the
 * indirect block. One function hides that split from everything else, which is
 * the point of having it - callers just ask for block N.
 */
static uint32_t block_at(const struct fs_entry *entry, uint32_t index)
{
	if (index < FS_DIRECT_BLOCKS)
		return entry->direct[index];

	uint32_t slot = index - FS_DIRECT_BLOCKS;

	/* Every path out of here returns the same 0xFFFFFFFF, but the reasons are
	 * not the same thing at all: an index past the end of the file is the
	 * caller asking a silly question, while a failed read of the indirect block
	 * is the disk breaking underneath a perfectly good one. The sentinel cannot
	 * carry that difference, so it is recorded separately - otherwise a dying
	 * disk reports itself as "no such block". */
	if (slot >= FS_PER_INDIRECT || !entry->indirect) {
		last_error = FS_ERR_CORRUPT;
		return 0xFFFFFFFFu;
	}

	if (!ata_read_sector(super.data_start + entry->indirect, indirect_buffer)) {
		last_error = FS_ERR_IO;
		return 0xFFFFFFFFu;
	}

	return ((uint32_t *) indirect_buffer)[slot];
}

/* Attach a newly allocated block as block `index` of the file. */
static int block_set(struct fs_entry *entry, uint32_t index, uint32_t block)
{
	if (index < FS_DIRECT_BLOCKS) {
		entry->direct[index] = block;
		return 1;
	}

	uint32_t slot = index - FS_DIRECT_BLOCKS;
	if (slot >= FS_PER_INDIRECT)
		return fail(FS_ERR_TOO_BIG);     /* file has hit its ceiling */

	if (!entry->indirect) {
		/* The indirect block is itself a block, taken from the same pool.
		 * It costs one block of overhead, and only for files big enough to
		 * need it - small files never pay. */
		uint32_t b = block_alloc();
		if (b == 0xFFFFFFFFu)
			return fail(FS_ERR_DISK_FULL);

		kmemset(indirect_buffer, 0, SECTOR_SIZE);
		if (!ata_write_sector(super.data_start + b, indirect_buffer))
			return fail(FS_ERR_IO);

		/* Only recorded in the entry once the block behind it exists and is
		 * zeroed. Setting it first and then failing left the file pointing at
		 * an indirect block full of whatever was there before - a list of
		 * garbage block numbers presented as the file's own. */
		entry->indirect = b;
	}

	if (!ata_read_sector(super.data_start + entry->indirect, indirect_buffer))
		return fail(FS_ERR_IO);

	((uint32_t *) indirect_buffer)[slot] = block;

	if (!ata_write_sector(super.data_start + entry->indirect, indirect_buffer))
		return fail(FS_ERR_IO);

	return 1;
}

/* ---- mount and format ----------------------------------------------------- */

int fs_mount(void)
{
	mounted    = 0;
	last_error = FS_OK;

	if (!ata_read_sector(0, &super))
		return fail(FS_ERR_IO);

	if (super.magic != FS_MAGIC)
		return fail(FS_ERR_BAD_MAGIC);  /* not ours, or an older format */

	if (!read_directory())
		return fail(FS_ERR_IO);

	if (!ata_read_sector(FS_BITMAP_SECTOR, bitmap))
		return fail(FS_ERR_IO);

	mounted = 1;
	return 1;
}

int fs_format(void)
{
	uint32_t sectors = ata_sector_count();

	last_error = FS_OK;

	if (sectors <= FS_DATA_START)
		return fail(FS_ERR_NO_DISK);

	uint32_t data_blocks = sectors - FS_DATA_START;

	/* The bitmap is one sector, so 512 * 8 blocks is all we can track. Cap
	 * rather than overrun it - silently writing past a buffer because the disk
	 * was larger than expected is the kind of bug that only shows up on
	 * somebody else's hardware. */
	if (data_blocks > SECTOR_SIZE * 8)
		data_blocks = SECTOR_SIZE * 8;

	kmemset(&super, 0, sizeof(super));
	super.magic        = FS_MAGIC;
	super.total_blocks = data_blocks;
	super.data_start   = FS_DATA_START;
	super.max_files    = FS_MAX_FILES;

	kmemset(directory, 0, sizeof(directory));
	kmemset(bitmap, 0, sizeof(bitmap));

	/* The superblock goes last on purpose. It is what fs_mount looks at, so a
	 * format that dies part way through leaves a disk that still fails to
	 * mount rather than one that mounts and presents a directory that was never
	 * written. */
	if (!write_directory())
		return fail(FS_ERR_IO);
	if (!ata_write_sector(FS_BITMAP_SECTOR, bitmap))
		return fail(FS_ERR_IO);
	if (!ata_write_sector(0, &super))
		return fail(FS_ERR_IO);

	mounted = 1;
	return 1;
}

int fs_is_mounted(void)
{
	return mounted;
}

/* ---- files ---------------------------------------------------------------- */

static struct fs_entry *find_entry(const char *name)
{
	for (uint32_t i = 0; i < FS_MAX_FILES; i++)
		if (directory[i].used && kstrcmp(directory[i].name, name) == 0)
			return &directory[i];

	return 0;
}

static struct fs_entry *find_free_entry(void)
{
	for (uint32_t i = 0; i < FS_MAX_FILES; i++)
		if (!directory[i].used)
			return &directory[i];

	return 0;
}

/* Write `size` bytes starting at byte offset `offset` within the file,
 * allocating blocks as needed. Shared by create and append, because appending
 * is just writing at the current end.
 */
static int write_at(struct fs_entry *entry, uint32_t offset,
                    const uint8_t *data, uint32_t size)
{
	uint32_t written = 0;

	while (written < size) {
		uint32_t position = offset + written;
		uint32_t index    = position / FS_BLOCK_SIZE;
		uint32_t within   = position % FS_BLOCK_SIZE;
		uint32_t chunk    = FS_BLOCK_SIZE - within;

		if (chunk > size - written)
			chunk = size - written;

		if (index >= FS_MAX_BLOCKS)
			return fail(FS_ERR_TOO_BIG); /* file is as big as it can get */

		uint32_t block = block_at(entry, index);
		int fresh = 0;

		/* A block index past the current end, or an unset slot, needs a new
		 * block. Zero is a legitimate block number, so "unset" is signalled
		 * by the index being beyond what the file has. */
		if (index >= (entry->size + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE ||
		    block == 0xFFFFFFFFu) {
			block = block_alloc();
			if (block == 0xFFFFFFFFu)
				return fail(FS_ERR_DISK_FULL);

			/* block_set records its own reason for failing, so pass that
			 * along rather than flattening every cause into one. */
			if (!block_set(entry, index, block))
				return 0;

			fresh = 1;
		}

		/* Partial writes need the existing contents first, or the rest of the
		 * block would be replaced with whatever is in the buffer. A brand new
		 * block is zeroed instead - never left holding a previous owner's
		 * data, which would be an information leak. */
		if (fresh)
			kmemset(sector_buffer, 0, SECTOR_SIZE);
		else if (!ata_read_sector(super.data_start + block, sector_buffer))
			return fail(FS_ERR_IO);

		kmemcpy(sector_buffer + within, data + written, chunk);

		if (!ata_write_sector(super.data_start + block, sector_buffer))
			return fail(FS_ERR_IO);

		written += chunk;
	}

	return 1;
}

/* Push the bitmap and the directory to disk after a change.
 *
 * The ORDER is the interesting part, and it is not the same in both
 * directions:
 *
 *   growing a file - bitmap first, then the directory. If the second write
 *     fails, the disk holds blocks marked used that no file claims: wasted
 *     space, and nothing worse.
 *
 *   deleting a file - directory first, then the bitmap. The survivable failure
 *     is again leaked blocks.
 *
 * Do it the other way round and the failure leaves a directory entry naming
 * blocks the bitmap believes are free - which the next allocation hands to
 * another file, and then two files share sectors. Both writes are metadata, so
 * the rule cannot be "metadata last"; it is that a reference must never outlive
 * the record of what it points at. */
static int write_metadata(int freeing)
{
	if (freeing) {
		if (!write_directory())
			return fail(FS_ERR_IO);
		if (!ata_write_sector(FS_BITMAP_SECTOR, bitmap))
			return fail(FS_ERR_IO);
		return 1;
	}

	if (!ata_write_sector(FS_BITMAP_SECTOR, bitmap))
		return fail(FS_ERR_IO);
	if (!write_directory())
		return fail(FS_ERR_IO);

	return 1;
}

int fs_create(const char *name, const void *data, uint32_t size)
{
	last_error = FS_OK;
	claim_begin();

	if (!mounted)
		return fail(FS_ERR_NOT_MOUNTED);

	if (!name || name[0] == '\0')
		return fail(FS_ERR_NAME);

	/* Bound the name before copying it. The entry has a fixed 32 bytes and the
	 * caller's string is whatever it is - this check is the only thing between
	 * a long filename and a corrupted directory. */
	uint32_t name_length = 0;
	while (name[name_length] && name_length < FS_NAME_MAX)
		name_length++;

	if (name_length >= FS_NAME_MAX)
		return fail(FS_ERR_NAME);

	if (find_entry(name))
		return fail(FS_ERR_EXISTS);

	struct fs_entry *entry = find_free_entry();
	if (!entry)
		return fail(FS_ERR_DIR_FULL);

	kmemset(entry, 0, sizeof(*entry));
	for (uint32_t i = 0; i < name_length; i++)
		entry->name[i] = name[i];

	if (!write_at(entry, 0, (const uint8_t *) data, size)) {
		kmemset(entry, 0, sizeof(*entry));   /* leave no half-made file */
		claim_rollback();
		return 0;                            /* write_at recorded why */
	}

	entry->size = size;
	entry->used = 1;

	if (!write_metadata(0)) {
		/* Nothing on disk mentions this file, so memory must not either.
		 * Leaving the entry behind would have the directory in RAM disagree
		 * with the one on the disk, and the next successful write would
		 * commit that disagreement. */
		kmemset(entry, 0, sizeof(*entry));
		claim_rollback();
		return 0;
	}

	return 1;
}

int fs_append(const char *name, const void *data, uint32_t size)
{
	last_error = FS_OK;
	claim_begin();

	if (!mounted)
		return fail(FS_ERR_NOT_MOUNTED);

	struct fs_entry *entry = find_entry(name);
	if (!entry)
		return fail(FS_ERR_NOT_FOUND);

	/* Kept so a failure can put it back. The recorded length is the only thing
	 * that says which of the file's blocks hold real data, and a block that was
	 * allocated but never written must not end up inside it. */
	uint32_t previous_size = entry->size;

	/* The whole point of block mapping: this simply was not possible before,
	 * because a contiguous file had nowhere to grow into. */
	if (!write_at(entry, entry->size, (const uint8_t *) data, size)) {
		entry->size = previous_size;
		claim_rollback();
		return 0;
	}

	entry->size += size;

	if (!write_metadata(0)) {
		entry->size = previous_size;
		claim_rollback();
		return 0;
	}

	return 1;
}

int fs_read(const char *name, void *buffer, uint32_t max, uint32_t *size_out)
{
	last_error = FS_OK;

	if (!mounted)
		return fail(FS_ERR_NOT_MOUNTED);

	struct fs_entry *entry = find_entry(name);
	if (!entry)
		return fail(FS_ERR_NOT_FOUND);

	uint32_t size = entry->size;
	int truncated = 0;

	if (size > max) {
		size = max;                     /* truncate rather than overflow */
		truncated = 1;
	}

	uint8_t *dest = (uint8_t *) buffer;
	uint32_t copied = 0;
	uint32_t index = 0;

	while (copied < size) {
		uint32_t block = block_at(entry, index);
		if (block == 0xFFFFFFFFu)
			return 0;                   /* block_at recorded why */

		if (!ata_read_sector(super.data_start + block, sector_buffer))
			return fail(FS_ERR_IO);

		uint32_t chunk = size - copied;
		if (chunk > FS_BLOCK_SIZE)
			chunk = FS_BLOCK_SIZE;

		kmemcpy(dest + copied, sector_buffer, chunk);
		copied += chunk;
		index++;
	}

	if (size_out)
		*size_out = size;

	/* Success, but not the whole file. Reported through the error code rather
	 * than a failed return, because the bytes in the buffer are real and the
	 * caller may well want them - it just must not be allowed to believe it has
	 * everything. */
	if (truncated)
		last_error = FS_ERR_TRUNCATED;

	return 1;
}

int fs_delete(const char *name)
{
	last_error = FS_OK;

	if (!mounted)
		return fail(FS_ERR_NOT_MOUNTED);

	struct fs_entry *entry = find_entry(name);
	if (!entry)
		return fail(FS_ERR_NOT_FOUND);

	uint32_t blocks = (entry->size + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;
	uint32_t leaked = 0;

	for (uint32_t i = 0; i < blocks; i++) {
		uint32_t block = block_at(entry, i);

		if (block == 0xFFFFFFFFu) {
			/* The block list could not be followed - a failing indirect read,
			 * or a number that was never valid. The delete carries on, because
			 * refusing would leave the user with a file they cannot remove, but
			 * this block cannot be reclaimed and pretending otherwise would
			 * hand it out twice later. Count it and say so. */
			leaked++;
			continue;
		}

		block_mark(block, 0);
	}

	if (entry->indirect)
		block_mark(entry->indirect, 0);

	kmemset(entry, 0, sizeof(*entry));

	/* Note what this does NOT do: the data blocks still hold the file's
	 * contents. Deleting only removes the reference, which is why deleted
	 * files are recoverable and why securely erasing something means
	 * overwriting it deliberately. */
	if (!write_metadata(1))
		return 0;

	/* Deliberately after write_metadata, which sets its own reason on failure.
	 * The delete itself succeeded, so the return is 1 - but a leak is a fact
	 * about the disk worth printing rather than filing away in a code the
	 * caller has no reason to look at on success. */
	if (leaked)
		kprintf("fs: %s deleted, but %u block(s) could not be reclaimed\n",
		        name, leaked);
	else
		last_error = FS_OK;   /* block_at set nothing worth keeping */

	return 1;
}

void fs_list(void)
{
	if (!mounted) {
		kprintf("no filesystem mounted - try 'format'\n");
		return;
	}

	uint32_t count = 0;

	for (uint32_t i = 0; i < FS_MAX_FILES; i++) {
		if (!directory[i].used)
			continue;

		kprintf("  %s", directory[i].name);

		uint32_t n = 0;
		while (directory[i].name[n])
			n++;
		while (n++ < 20)
			kprintf(" ");

		uint32_t blocks = (directory[i].size + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;

		kprintf("%u bytes  %u block%s%s\n", directory[i].size, blocks,
		        blocks == 1 ? "" : "s",
		        directory[i].indirect ? " + indirect" : "");
		count++;
	}

	if (count == 0)
		kprintf("  (empty)\n");
}

void fs_stats(uint32_t *total, uint32_t *used, uint32_t *files)
{
	uint32_t u = 0, f = 0;

	if (mounted) {
		for (uint32_t b = 0; b < super.total_blocks; b++)
			if (block_used(b))
				u++;

		for (uint32_t i = 0; i < FS_MAX_FILES; i++)
			if (directory[i].used)
				f++;
	}

	if (total) *total = mounted ? super.total_blocks : 0;
	if (used)  *used  = u;
	if (files) *files = f;
}
