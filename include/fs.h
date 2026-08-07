/* fs.h - ArthicFS, a deliberately small filesystem.
 *
 * LAYOUT
 *
 *   sector 0        superblock - magic number, sizes, where things are
 *   sectors 1-8     directory - 64 entries of 64 bytes
 *   sector 9        block bitmap - one bit per data block
 *   sectors 10+     file data
 *
 * DESIGN CHOICES, AND WHAT THEY COST
 *
 * Files are stored CONTIGUOUSLY: a start block and a length, nothing else.
 * That makes reading trivial and appending impossible - growing a file means
 * finding a larger run and copying. It also fragments: you can have plenty of
 * free blocks and still fail to create a file, because none of the free runs is
 * long enough.
 *
 * Real filesystems solve that by breaking files into blocks that need not be
 * adjacent, and keeping a map. FAT chains each block to the next; ext2 keeps a
 * list of block numbers in the inode. Both are strictly better than this and
 * both are considerably more code. Contiguous allocation is the version you can
 * hold in your head, and understanding why it is not good enough is the point.
 *
 * The directory is a single fixed-size table, so there are no subdirectories
 * and a hard limit of 64 files.
 */
#ifndef ARTHIC_FS_H
#define ARTHIC_FS_H

#include <stdint.h>

#define FS_MAGIC          0x41525448u   /* "ARTH" */
#define FS_NAME_MAX       32
#define FS_MAX_FILES      64
#define FS_DIR_START      1
#define FS_DIR_SECTORS    8
#define FS_BITMAP_SECTOR  9
#define FS_DATA_START     10

struct fs_superblock {
	uint32_t magic;
	uint32_t total_blocks;
	uint32_t data_start;
	uint32_t max_files;
	uint8_t  padding[512 - 16];
} __attribute__((packed));

struct fs_entry {
	char     name[FS_NAME_MAX];
	uint32_t start_block;
	uint32_t size;          /* bytes */
	uint32_t used;
	uint8_t  padding[64 - FS_NAME_MAX - 12];
} __attribute__((packed));

int  fs_mount(void);        /* 1 if a valid filesystem was found */
int  fs_format(void);
int  fs_is_mounted(void);

void fs_list(void);
int  fs_create(const char *name, const void *data, uint32_t size);
int  fs_read(const char *name, void *buffer, uint32_t max, uint32_t *size_out);
int  fs_delete(const char *name);
void fs_stats(uint32_t *total, uint32_t *used, uint32_t *files);

#endif
