/* ata.c - a PIO disk driver.
 *
 * ADDRESSING
 *
 * We use LBA28: the disk is a flat array of 512-byte sectors numbered from
 * zero, and 28 bits of number gives 2^28 sectors, or 128 GB. Older schemes
 * addressed by cylinder, head and sector, which mapped to actual spinning
 * platters and became a fiction the moment drives stopped being shaped like
 * that. LBA is the sane abstraction that replaced it.
 *
 * THE PROTOCOL
 *
 * Every operation is the same shape: write the sector number across four
 * registers, write how many sectors, write a command byte, then wait for the
 * drive to say it is ready and move the data through a single port.
 *
 * WAITING
 *
 * Two status bits matter. BSY means the drive is thinking and every other bit
 * is meaningless. DRQ means it has data ready, or wants some. The correct
 * sequence is always: wait for BSY to clear, THEN look at DRQ. Checking DRQ
 * while BSY is set reads a bit that has no meaning yet, and it works right up
 * until it does not.
 *
 * We poll rather than using the disk's interrupt. Polling blocks the CPU for
 * the duration of the transfer, which for a real driver would be unacceptable -
 * you would sleep the calling thread and wake it on IRQ 14. Now that blocking
 * exists that is a genuinely small change, and a good next step.
 */

#include "ata.h"
#include "io.h"
#include "terminal.h"

/* Primary ATA bus. The secondary lives at 0x170. */
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECCOUNT    0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE       0x1F6
#define ATA_STATUS      0x1F7   /* reading  */
#define ATA_COMMAND     0x1F7   /* writing  */
#define ATA_CONTROL     0x3F6

#define STATUS_ERR  0x01
#define STATUS_DRQ  0x08
#define STATUS_DF   0x20
#define STATUS_BSY  0x80

#define CMD_READ    0x20
#define CMD_WRITE   0x30
#define CMD_FLUSH   0xE7
#define CMD_IDENTIFY 0xEC

static uint32_t total_sectors = 0;
static int      present       = 0;

/* Reading the status port has a side effect on some controllers, so the
 * convention is to read it four times and discard, giving the drive 400ns to
 * settle. One of those pieces of hardware lore that looks superstitious and is
 * not. */
static void ata_delay(void)
{
	for (int i = 0; i < 4; i++)
		(void) inb(ATA_STATUS);
}

/* Wait for BSY to clear. Bounded, because a missing or wedged drive would
 * otherwise hang the kernel forever - and "wait indefinitely for hardware that
 * may not exist" is never the right behaviour. */
static int wait_not_busy(void)
{
	for (uint32_t i = 0; i < 2000000; i++) {
		uint8_t status = inb(ATA_STATUS);

		if (!(status & STATUS_BSY))
			return 1;
	}
	return 0;
}

static int wait_ready(void)
{
	if (!wait_not_busy())
		return 0;

	for (uint32_t i = 0; i < 2000000; i++) {
		uint8_t status = inb(ATA_STATUS);

		if (status & (STATUS_ERR | STATUS_DF))
			return 0;

		if (status & STATUS_DRQ)
			return 1;
	}
	return 0;
}

/* Select the drive and load the sector number. Shared by read and write
 * because the only difference between them is the command byte. */
static void select_sector(uint32_t lba, uint8_t count)
{
	/* 0xE0 means: master drive, LBA mode. The low nibble carries bits 24-27
	 * of the sector number, which is where LBA28's odd size comes from. */
	outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
	outb(ATA_ERROR, 0x00);
	outb(ATA_SECCOUNT, count);
	outb(ATA_LBA_LOW,  (uint8_t)(lba & 0xFF));
	outb(ATA_LBA_MID,  (uint8_t)((lba >> 8) & 0xFF));
	outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
}

int ata_init(void)
{
	outb(ATA_DRIVE, 0xA0);          /* select master */
	ata_delay();

	/* A status of 0 means nothing is attached at all - the bus floats. */
	if (inb(ATA_STATUS) == 0)
		return 0;

	select_sector(0, 0);
	outb(ATA_COMMAND, CMD_IDENTIFY);
	ata_delay();

	if (inb(ATA_STATUS) == 0)
		return 0;

	if (!wait_ready())
		return 0;

	/* IDENTIFY returns 256 words describing the drive. We want words 60-61,
	 * which hold the LBA28 sector count as a 32-bit value. */
	uint16_t identify[256];
	for (int i = 0; i < 256; i++)
		identify[i] = inw(ATA_DATA);

	total_sectors = (uint32_t) identify[60] | ((uint32_t) identify[61] << 16);
	present = 1;

	return 1;
}

int ata_read_sector(uint32_t lba, void *buffer)
{
	if (!present || !wait_not_busy())
		return 0;

	select_sector(lba, 1);
	outb(ATA_COMMAND, CMD_READ);
	ata_delay();

	if (!wait_ready())
		return 0;

	uint16_t *out = (uint16_t *) buffer;
	for (int i = 0; i < SECTOR_SIZE / 2; i++)
		out[i] = inw(ATA_DATA);

	return 1;
}

int ata_write_sector(uint32_t lba, const void *buffer)
{
	if (!present || !wait_not_busy())
		return 0;

	select_sector(lba, 1);
	outb(ATA_COMMAND, CMD_WRITE);
	ata_delay();

	if (!wait_ready())
		return 0;

	const uint16_t *in = (const uint16_t *) buffer;
	for (int i = 0; i < SECTOR_SIZE / 2; i++)
		outw(ATA_DATA, in[i]);

	/* Without this the drive may hold the data in its own cache and report
	 * success before anything reaches the platter. On a real machine that is
	 * the difference between a file surviving a power cut and not. */
	outb(ATA_COMMAND, CMD_FLUSH);
	wait_not_busy();

	return 1;
}

uint32_t ata_sector_count(void)
{
	return total_sectors;
}
