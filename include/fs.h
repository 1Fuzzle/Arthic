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

/* WHY THESE EXIST
 *
 * Every call below used to answer only "did it work?", and the shell had to
 * invent a reason: `cat` printed "no such file" whether the file was missing,
 * the disk had failed, or the block list pointed somewhere impossible. Those
 * need different responses from whoever is reading the message, so the reason
 * has to survive the return journey.
 *
 * The 1/0 return is kept - callers that only care whether it worked stay
 * unchanged - and the reason is recorded alongside it, which is the same shape
 * as C's own errno. The cost of that design is that the code is a single
 * global: it describes the LAST call, so read it immediately, before making
 * another. */
enum fs_error {
	FS_OK = 0,
	FS_ERR_NOT_MOUNTED,   /* no filesystem in memory to work with     */
	FS_ERR_NO_DISK,       /* the disk itself is absent or too small    */
	FS_ERR_IO,            /* the disk refused a read or a write        */
	FS_ERR_BAD_MAGIC,     /* readable, but not an ArthicFS v2 disk     */
	FS_ERR_NOT_FOUND,
	FS_ERR_EXISTS,
	FS_ERR_NAME,          /* empty, or longer than FS_NAME_MAX - 1     */
	FS_ERR_DIR_FULL,      /* all FS_MAX_FILES entries are taken        */
	FS_ERR_DISK_FULL,     /* no free block left                        */
	FS_ERR_TOO_BIG,       /* would exceed FS_MAX_BLOCKS for one file   */
	FS_ERR_CORRUPT,       /* the file's block list does not make sense */
	FS_ERR_TRUNCATED      /* the read worked but did not all fit       */
};

/* Why the last call failed, or FS_OK. Also set to FS_ERR_TRUNCATED by a read
 * that succeeded but could not return the whole file. */
enum fs_error fs_last_error(void);

/* A short description, suitable for printing straight to the user. */
const char *fs_error_string(enum fs_error error);

int  fs_mount(void);        /* 1 if a valid filesystem was found */
int  fs_format(void);
int  fs_is_mounted(void);

void fs_list(void);
int  fs_create(const char *name, const void *data, uint32_t size);
int  fs_append(const char *name, const void *data, uint32_t size);

/* Copy up to `max` bytes of `name` into `buffer`. A file larger than `max` is
 * truncated: the call still returns 1, but fs_last_error becomes
 * FS_ERR_TRUNCATED so the caller can say so rather than quietly presenting a
 * fragment as the whole file. */
int  fs_read(const char *name, void *buffer, uint32_t max, uint32_t *size_out);
int  fs_delete(const char *name);
void fs_stats(uint32_t *total, uint32_t *used, uint32_t *files);

#endif
