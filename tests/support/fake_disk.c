/* fake_disk.c - a disk made of RAM.
 *
 * fs.c never touches hardware directly. It reads and writes numbered 512-byte
 * sectors through four functions, and has no way of knowing whether those
 * sectors live on a spinning platter or in an array. So the array is a
 * complete substitute, and a far better one for testing: it starts in a known
 * state, a test can inspect any sector afterwards, and it can be told to start
 * failing on demand.
 *
 * That last part matters more than it sounds. fs.c is full of `if (!write())
 * return 0;` paths that a working disk never reaches, and those are exactly
 * the paths where a filesystem corrupts itself. Making the disk fail on cue is
 * the only way to find out whether they behave.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "support.h"
#include "ata.h"

static uint8_t *disk;
static uint32_t disk_sectors;

static uint32_t reads;
static uint32_t writes;

/* 0xFFFFFFFF means "never fail", which keeps the common case free of special
 * cases - the counters simply never reach it. */
static uint32_t fail_writes_after = 0xFFFFFFFFu;
static uint32_t fail_reads_after  = 0xFFFFFFFFu;

void fake_disk_reset(uint32_t sectors)
{
	free(disk);

	disk_sectors = sectors;
	disk = calloc(sectors, SECTOR_SIZE);
	if (!disk)
		abort();

	reads  = 0;
	writes = 0;
	fail_writes_after = 0xFFFFFFFFu;
	fail_reads_after  = 0xFFFFFFFFu;
}

void fake_disk_release(void)
{
	free(disk);
	disk = NULL;
	disk_sectors = 0;
}

void fake_disk_fail_writes_after(uint32_t successful_writes)
{
	fail_writes_after = successful_writes;
}

void fake_disk_fail_reads_after(uint32_t successful_reads)
{
	fail_reads_after = successful_reads;
}

uint32_t fake_disk_write_count(void) { return writes; }
uint32_t fake_disk_read_count(void)  { return reads; }

uint8_t *fake_disk_sector(uint32_t lba)
{
	if (lba >= disk_sectors)
		return NULL;

	return disk + (size_t) lba * SECTOR_SIZE;
}

int ata_init(void)
{
	return disk != NULL;
}

uint32_t ata_sector_count(void)
{
	return disk_sectors;
}

int ata_read_sector(uint32_t lba, void *buffer)
{
	if (!disk || lba >= disk_sectors)
		return 0;

	if (reads >= fail_reads_after)
		return 0;

	reads++;
	memcpy(buffer, disk + (size_t) lba * SECTOR_SIZE, SECTOR_SIZE);
	return 1;
}

int ata_write_sector(uint32_t lba, const void *buffer)
{
	if (!disk || lba >= disk_sectors)
		return 0;

	if (writes >= fail_writes_after)
		return 0;

	writes++;
	memcpy(disk + (size_t) lba * SECTOR_SIZE, buffer, SECTOR_SIZE);
	return 1;
}
