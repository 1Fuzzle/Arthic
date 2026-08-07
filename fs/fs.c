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
static uint32_t block_alloc(void)
{
	for (uint32_t b = 0; b < super.total_blocks; b++) {
		if (!block_used(b)) {
			block_mark(b, 1);
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

	if (!entry->indirect)
		return 0xFFFFFFFFu;

	if (!ata_read_sector(super.data_start + entry->indirect, indirect_buffer))
		return 0xFFFFFFFFu;

	uint32_t slot = index - FS_DIRECT_BLOCKS;
	if (slot >= FS_PER_INDIRECT)
		return 0xFFFFFFFFu;

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
		return 0;                        /* file has hit its ceiling */

	if (!entry->indirect) {
		/* The indirect block is itself a block, taken from the same pool.
		 * It costs one block of overhead, and only for files big enough to
		 * need it - small files never pay. */
		uint32_t b = block_alloc();
		if (b == 0xFFFFFFFFu)
			return 0;

		entry->indirect = b;

		kmemset(indirect_buffer, 0, SECTOR_SIZE);
		if (!ata_write_sector(super.data_start + b, indirect_buffer))
			return 0;
	}

	if (!ata_read_sector(super.data_start + entry->indirect, indirect_buffer))
		return 0;

	((uint32_t *) indirect_buffer)[slot] = block;

	return ata_write_sector(super.data_start + entry->indirect, indirect_buffer);
}

/* ---- mount and format ----------------------------------------------------- */

int fs_mount(void)
{
	mounted = 0;

	if (!ata_read_sector(0, &super))
		return 0;

	if (super.magic != FS_MAGIC)
		return 0;                       /* not ours, or an older format */

	if (!read_directory())
		return 0;

	if (!ata_read_sector(FS_BITMAP_SECTOR, bitmap))
		return 0;

	mounted = 1;
	return 1;
}

int fs_format(void)
{
	uint32_t sectors = ata_sector_count();

	if (sectors <= FS_DATA_START)
		return 0;

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

	if (!ata_write_sector(0, &super))
		return 0;
	if (!write_directory())
		return 0;
	if (!ata_write_sector(FS_BITMAP_SECTOR, bitmap))
		return 0;

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
			return 0;                    /* file is as big as it can get */

		uint32_t block = block_at(entry, index);
		int fresh = 0;

		/* A block index past the current end, or an unset slot, needs a new
		 * block. Zero is a legitimate block number, so "unset" is signalled
		 * by the index being beyond what the file has. */
		if (index >= (entry->size + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE ||
		    block == 0xFFFFFFFFu) {
			block = block_alloc();
			if (block == 0xFFFFFFFFu)
				return 0;                /* disk full */

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
			return 0;

		kmemcpy(sector_buffer + within, data + written, chunk);

		if (!ata_write_sector(super.data_start + block, sector_buffer))
			return 0;

		written += chunk;
	}

	return 1;
}

int fs_create(const char *name, const void *data, uint32_t size)
{
	if (!mounted || !name || name[0] == '\0')
		return 0;

	/* Bound the name before copying it. The entry has a fixed 32 bytes and the
	 * caller's string is whatever it is - this check is the only thing between
	 * a long filename and a corrupted directory. */
	uint32_t name_length = 0;
	while (name[name_length] && name_length < FS_NAME_MAX)
		name_length++;

	if (name_length >= FS_NAME_MAX)
		return 0;

	if (find_entry(name))
		return 0;                       /* already exists */

	struct fs_entry *entry = find_free_entry();
	if (!entry)
		return 0;                       /* directory full */

	kmemset(entry, 0, sizeof(*entry));
	for (uint32_t i = 0; i < name_length; i++)
		entry->name[i] = name[i];

	if (!write_at(entry, 0, (const uint8_t *) data, size)) {
		kmemset(entry, 0, sizeof(*entry));   /* leave no half-made file */
		return 0;
	}

	entry->size = size;
	entry->used = 1;

	/* Metadata last. If power fails midway, an unreferenced data block is
	 * wasted space; a directory entry pointing at data that was never written
	 * is corruption. Ordering the writes so the cheaper failure is the likely
	 * one is most of what journalling formalises. */
	if (!write_directory())
		return 0;

	return ata_write_sector(FS_BITMAP_SECTOR, bitmap);
}

int fs_append(const char *name, const void *data, uint32_t size)
{
	if (!mounted)
		return 0;

	struct fs_entry *entry = find_entry(name);
	if (!entry)
		return 0;

	/* The whole point of block mapping: this simply was not possible before,
	 * because a contiguous file had nowhere to grow into. */
	if (!write_at(entry, entry->size, (const uint8_t *) data, size))
		return 0;

	entry->size += size;

	if (!write_directory())
		return 0;

	return ata_write_sector(FS_BITMAP_SECTOR, bitmap);
}

int fs_read(const char *name, void *buffer, uint32_t max, uint32_t *size_out)
{
	if (!mounted)
		return 0;

	struct fs_entry *entry = find_entry(name);
	if (!entry)
		return 0;

	uint32_t size = entry->size;
	if (size > max)
		size = max;                     /* truncate rather than overflow */

	uint8_t *dest = (uint8_t *) buffer;
	uint32_t copied = 0;
	uint32_t index = 0;

	while (copied < size) {
		uint32_t block = block_at(entry, index);
		if (block == 0xFFFFFFFFu)
			return 0;

		if (!ata_read_sector(super.data_start + block, sector_buffer))
			return 0;

		uint32_t chunk = size - copied;
		if (chunk > FS_BLOCK_SIZE)
			chunk = FS_BLOCK_SIZE;

		kmemcpy(dest + copied, sector_buffer, chunk);
		copied += chunk;
		index++;
	}

	if (size_out)
		*size_out = size;

	return 1;
}

int fs_delete(const char *name)
{
	if (!mounted)
		return 0;

	struct fs_entry *entry = find_entry(name);
	if (!entry)
		return 0;

	uint32_t blocks = (entry->size + FS_BLOCK_SIZE - 1) / FS_BLOCK_SIZE;

	for (uint32_t i = 0; i < blocks; i++) {
		uint32_t block = block_at(entry, i);
		if (block != 0xFFFFFFFFu)
			block_mark(block, 0);
	}

	if (entry->indirect)
		block_mark(entry->indirect, 0);

	kmemset(entry, 0, sizeof(*entry));

	/* Note what this does NOT do: the data blocks still hold the file's
	 * contents. Deleting only removes the reference, which is why deleted
	 * files are recoverable and why securely erasing something means
	 * overwriting it deliberately. */
	if (!write_directory())
		return 0;

	return ata_write_sector(FS_BITMAP_SECTOR, bitmap);
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
