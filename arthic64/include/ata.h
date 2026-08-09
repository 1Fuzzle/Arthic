/* ata.h - talking to a hard disk.
 *
 * ATA (also called IDE) is the interface almost every PC disk has spoken for
 * decades. We use the simplest mode it offers: PIO, where the CPU itself moves
 * every byte through an I/O port, 16 bits at a time.
 *
 * That is slow and wasteful - a real driver sets up DMA and lets the disk write
 * straight into memory while the CPU does something useful. But PIO needs no
 * bus mastering, no interrupt handling, and no scatter-gather lists, which
 * makes it about eighty lines instead of eight hundred.
 */
#ifndef ARTHIC_ATA_H
#define ARTHIC_ATA_H

#include <stdint.h>

#define SECTOR_SIZE 512

/* Returns 1 if a disk responded, 0 if there is nothing there. */
int ata_init(void);

/* Both return 1 on success, 0 on failure. `buffer` must hold SECTOR_SIZE
 * bytes; LBA is a plain sector number counting from zero. */
int ata_read_sector(uint32_t lba, void *buffer);
int ata_write_sector(uint32_t lba, const void *buffer);

uint32_t ata_sector_count(void);

#endif
