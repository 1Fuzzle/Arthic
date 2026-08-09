/* fs.h - ArthicFS, version 2.
 *
 * LAYOUT
 *
 *   sector 0         superblock - magic number, sizes, where things are
 *   sectors 1-16     directory - 64 entries of 128 bytes
 *   sector 17        block bitmap - one bit per data block
 *   sectors 18+      file data
 *
 * WHAT CHANGED, AND WHY
 *
 * Version 1 stored each file as a start block and a length: contiguous, and
 * therefore unable to grow and prone to failing while plenty of space was free
 * but scattered. That is external fragmentation, and it is the reason nothing
 * real stores files that way.
 *
 * Now a file is a LIST of block numbers. The blocks can be anywhere, in any
 * order, and adding one to the end is trivial - so files can grow, and creation
 * only fails when the disk is genuinely full rather than merely untidy.
 *
 * DIRECT AND INDIRECT
 *
 * The first 8 blocks are named in the directory entry itself. Beyond that, one
 * more field points at a whole BLOCK full of block numbers - 128 of them - and
 * the file continues through those.
 *
 * That two-level shape is not arbitrary. Most files are small, and for them the
 * entry alone is enough: one read gets both the metadata and the whole layout.
 * Large files pay one extra read for the indirect block, which is a small price
 * for something already reading dozens of blocks. ext2 uses exactly this idea,
 * with double and triple indirect blocks beyond it.
 *
 *   8 direct + 128 indirect = 136 blocks = 68 KB per file.
 *
 * The on-disk format is INCOMPATIBLE with version 1. The superblock magic
 * changed accordingly, so an old disk is rejected rather than misread.
 */
#ifndef ARTHIC_FS_H
#define ARTHIC_FS_H

#include <stdint.h>

#define FS_MAGIC          0x41525432u   /* "ART2" */
#define FS_NAME_MAX       32
#define FS_MAX_FILES      64
#define FS_DIR_START      1
#define FS_DIR_SECTORS    16
#define FS_BITMAP_SECTOR  17
#define FS_DATA_START     18

#define FS_DIRECT_BLOCKS  8
#define FS_BLOCK_SIZE     512
#define FS_PER_INDIRECT   (FS_BLOCK_SIZE / 4)          /* 128 */
#define FS_MAX_BLOCKS     (FS_DIRECT_BLOCKS + FS_PER_INDIRECT)

struct fs_superblock {
	uint32_t magic;
	uint32_t total_blocks;
	uint32_t data_start;
	uint32_t max_files;
	uint8_t  padding[512 - 16];
} __attribute__((packed));

struct fs_entry {
	char     name[FS_NAME_MAX];
	uint32_t size;                          /* bytes */
	uint32_t used;
	uint32_t direct[FS_DIRECT_BLOCKS];      /* first 8 blocks */
	uint32_t indirect;                      /* block holding 128 more, or 0 */
	uint8_t  padding[128 - FS_NAME_MAX - 8 - FS_DIRECT_BLOCKS * 4 - 4];
} __attribute__((packed));

int  fs_mount(void);        /* 1 if a valid filesystem was found */
int  fs_format(void);
int  fs_is_mounted(void);

void fs_list(void);
int  fs_create(const char *name, const void *data, uint32_t size);
int  fs_append(const char *name, const void *data, uint32_t size);
int  fs_read(const char *name, void *buffer, uint32_t max, uint32_t *size_out);
int  fs_delete(const char *name);
void fs_stats(uint32_t *total, uint32_t *used, uint32_t *files);

#endif
