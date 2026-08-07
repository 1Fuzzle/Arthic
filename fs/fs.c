/* fs.c - ArthicFS.
 *
 * Everything here reduces to reading and writing 512-byte sectors, which is the
 * only thing the disk understands. A filesystem is nothing more than an agreed
 * convention about what those sectors mean.
 *
 * ON-DISK STRUCTURES ARE A CONTRACT
 *
 * Once something is written to a disk it outlives the code that wrote it. The
 * structures in fs.h are `packed` for the same reason the GDT entry was: the
 * layout must be exactly what we say it is, with no compiler padding, or a
 * different build could read the same disk and find garbage.
 *
 * This is also why the superblock carries a magic number. Reading a disk that
 * was never formatted, or formatted by something else, and interpreting it as
 * ours would destroy whatever is there. Checking four bytes first is cheap.
 */

#include "fs.h"
#include "ata.h"
#include "string.h"
#include "terminal.h"

static struct fs_superblock super;
static int mounted = 0;

/* One sector's worth of scratch. Static rather than on the stack because a
 * kernel stack is 8 KB and 512-byte locals add up faster than you expect. */
static uint8_t sector_buffer[SECTOR_SIZE];
static uint8_t bitmap[SECTOR_SIZE];

static struct fs_entry directory[FS_MAX_FILES];

/* ---- directory and bitmap ------------------------------------------------- */

static int read_directory(void)
{
	uint8_t *dest = (uint8_t *) directory;

	for (uint32_t i = 0; i < FS_DIR_SECTORS; i++) {
		if (!ata_read_sector(FS_DIR_START + i, dest + i * SECTOR_SIZE))
			return 0;
	}
	return 1;
}

static int write_directory(void)
{
	const uint8_t *src = (const uint8_t *) directory;

	for (uint32_t i = 0; i < FS_DIR_SECTORS; i++) {
		if (!ata_write_sector(FS_DIR_START + i, src + i * SECTOR_SIZE))
			return 0;
	}
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

/* Find `count` consecutive free blocks.
 *
 * The same sliding-window scan as the physical frame allocator, and it has the
 * same weakness: it can fail while plenty of space is free, because the free
 * space is scattered. That is external fragmentation, and here it is a direct
 * consequence of storing files contiguously.
 */
static uint32_t find_run(uint32_t count)
{
	uint32_t run_start = 0;
	uint32_t run = 0;
	uint32_t blocks = super.total_blocks;

	for (uint32_t b = 0; b < blocks; b++) {
		if (block_used(b)) {
			run = 0;
			run_start = b + 1;
			continue;
		}

		if (++run == count)
			return run_start;
	}
	return 0xFFFFFFFFu;   /* no run that long */
}

/* ---- mount and format ----------------------------------------------------- */

int fs_mount(void)
{
	mounted = 0;

	if (!ata_read_sector(0, &super))
		return 0;

	if (super.magic != FS_MAGIC)
		return 0;                       /* not ours - leave it alone */

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
	 * was larger than expected is exactly the kind of bug that only shows up
	 * on somebody else's hardware. */
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
	for (uint32_t i = 0; i < FS_MAX_FILES; i++) {
		if (directory[i].used && kstrcmp(directory[i].name, name) == 0)
			return &directory[i];
	}
	return 0;
}

static struct fs_entry *find_free_entry(void)
{
	for (uint32_t i = 0; i < FS_MAX_FILES; i++) {
		if (!directory[i].used)
			return &directory[i];
	}
	return 0;
}

int fs_create(const char *name, const void *data, uint32_t size)
{
	if (!mounted || !name || name[0] == '\0')
		return 0;

	/* Bound the name before copying it. The directory entry is a fixed 32
	 * bytes and the caller's string is whatever it is - this check is the only
	 * thing standing between a long filename and a corrupted directory. */
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

	uint32_t blocks = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
	if (blocks == 0)
		blocks = 1;

	uint32_t start = find_run(blocks);
	if (start == 0xFFFFFFFFu)
		return 0;                       /* no contiguous space */

	/* Write the data out one block at a time, padding the final partial block
	 * with zeroes rather than whatever happened to be in the buffer. Leaking
	 * old memory contents into a file is a real information-disclosure bug,
	 * and this is where it would happen. */
	const uint8_t *src = (const uint8_t *) data;
	uint32_t remaining = size;

	for (uint32_t b = 0; b < blocks; b++) {
		kmemset(sector_buffer, 0, SECTOR_SIZE);

		uint32_t chunk = remaining > SECTOR_SIZE ? SECTOR_SIZE : remaining;
		if (chunk)
			kmemcpy(sector_buffer, src + b * SECTOR_SIZE, chunk);
		remaining -= chunk;

		if (!ata_write_sector(super.data_start + start + b, sector_buffer))
			return 0;
	}

	for (uint32_t b = 0; b < blocks; b++)
		block_mark(start + b, 1);

	kmemset(entry, 0, sizeof(*entry));
	for (uint32_t i = 0; i < name_length; i++)
		entry->name[i] = name[i];
	entry->start_block = start;
	entry->size        = size;
	entry->used        = 1;

	/* Metadata last. If power fails midway, an unreferenced data block is
	 * wasted space; a directory entry pointing at data that was never written
	 * is corruption. Order the writes so the cheaper failure is the likely
	 * one - that principle is most of what journalling formalises. */
	if (!write_directory())
		return 0;
	if (!ata_write_sector(FS_BITMAP_SECTOR, bitmap))
		return 0;

	return 1;
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
	uint32_t block = 0;

	while (copied < size) {
		if (!ata_read_sector(super.data_start + entry->start_block + block,
		                     sector_buffer))
			return 0;

		uint32_t chunk = size - copied;
		if (chunk > SECTOR_SIZE)
			chunk = SECTOR_SIZE;

		kmemcpy(dest + copied, sector_buffer, chunk);
		copied += chunk;
		block++;
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

	uint32_t blocks = (entry->size + SECTOR_SIZE - 1) / SECTOR_SIZE;
	if (blocks == 0)
		blocks = 1;

	for (uint32_t b = 0; b < blocks; b++)
		block_mark(entry->start_block + b, 0);

	kmemset(entry, 0, sizeof(*entry));

	/* Note what this does NOT do: the data blocks still hold the file's
	 * contents. Deleting only removes the reference, which is why deleted
	 * files are recoverable and why securely erasing something means
	 * overwriting it deliberately. */
	if (!write_directory())
		return 0;
	if (!ata_write_sector(FS_BITMAP_SECTOR, bitmap))
		return 0;

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

		/* Crude column alignment - we have no width specifiers in kprintf. */
		uint32_t n = 0;
		while (directory[i].name[n])
			n++;
		while (n++ < 20)
			kprintf(" ");

		kprintf("%u bytes\n", directory[i].size);
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
