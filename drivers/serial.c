/* serial.c — COM1 serial port driver (16550 UART).
 *
 * WHAT THIS IS FOR
 *
 * The 16550 UART is a standard serial device found on nearly all x86 machines.
 * Kernel output traditionally went to a serial console on a separate terminal
 * machine, and still does in many systems. QEMU can capture this and write it
 * to a file on the host, making debugging far easier than parsing screenshots.
 *
 * HARDWARE REGISTERS
 *
 * COM1 is at I/O port 0x3F8. The 16550 has eight registers, addressable by
 * offset from this base:
 *
 *   0x3F8+0: Data Register (read = RBR, write = THR)
 *   0x3F8+1: Interrupt Enable Register (IER)
 *   0x3F8+2: Interrupt ID / FIFO Control (IIR / FCR)
 *   0x3F8+3: Line Control Register (LCR)
 *   0x3F8+4: Modem Control Register (MCR)
 *   0x3F8+5: Line Status Register (LSR)
 *   0x3F8+6: Modem Status Register (MSR)
 *   0x3F8+7: Scratch Register (SCR)
 *
 * and also, when DLAB is set in LCR:
 *   0x3F8+0: Divisor Latch Low (DLL)
 *   0x3F8+1: Divisor Latch High (DLH)
 *
 * The divisor sets the baud rate: baud = 115200 / divisor, so divisor = 1
 * gives 115200 baud. Bits 0-7 go in DLL, 8-15 in DLH; 1 fits in DLL alone.
 *
 * SERIAL OUTPUT: SIMPLE VERSION
 *
 * To send a character:
 *   1. Poll LSR bit 5 (Transmitter Holding Register Empty): wait until it's 1
 *   2. Write the character to THR (port 0x3F8)
 *   3. The UART transmits it asynchronously — we do not wait for completion
 *
 * This is called "polled" I/O: the CPU checks status before sending, not the
 * UART interrupting when it is ready. Simpler, but slower. On a serial line
 * running at 115200 baud, output is nearly free; the 8 bits + start + stop
 * take ~87 microseconds per character, and the CPU is so much faster that
 * spinning on LSR is negligible. For interactive debugging it is fine.
 *
 * WHAT WE ACTUALLY DO
 *
 * We skip the full initialisation and baud rate setup, because QEMU defaults
 * already match what we want. Some real machines may need it, but every modern
 * emulator starts you at 115200 8N1 (8 data bits, no parity, 1 stop bit), so
 * we can just write to the port and watch output appear on the host.
 */

#include <stdint.h>
#include <stddef.h>
#include "io.h"
#include "serial.h"

/* Base address of COM1. All register addresses are offsets from this. */
#define SERIAL_PORT 0x3F8

/* Register offsets. */
#define SERIAL_DATA 0   /* THR on write, RBR on read */
#define SERIAL_IER  1   /* Interrupt Enable Register */
#define SERIAL_FCR  2   /* FIFO Control Register */
#define SERIAL_LCR  3   /* Line Control Register */
#define SERIAL_MCR  4   /* Modem Control Register */
#define SERIAL_LSR  5   /* Line Status Register */
#define SERIAL_MSR  6   /* Modem Status Register */
#define SERIAL_SCR  7   /* Scratch Register */

/* Line Status Register bits. */
#define SERIAL_LSR_THRE 0x20  /* Transmitter Holding Register Empty — ready to send */

/* Wait until the transmitter is ready, then send the character.
 *
 * LSR bit 5 is 1 when THR (the transmitter holding register) is empty and
 * ready for a new character. We poll that bit, then write to port 0x3F8.
 *
 * inb / outb are defined in io.h — they compile to x86 I/O instructions.
 * The syntax takes getting used to: outb(value, port) sends value to the port.
 * That is backwards from C function conventions, but it matches the x86 `outb`
 * instruction where the destination port is the second argument.
 */
void serial_putchar(char ch) {
	/* Spin until the transmitter holding register is empty.
	 *
	 * `inb(SERIAL_PORT + SERIAL_LSR)` reads the Line Status Register at
	 * port 0x3F8+5 = 0x3FD. The result is a byte. Bit 5 of that byte is
	 * the THRE flag. We mask it with & SERIAL_LSR_THRE = & 0x20, so the
	 * condition is true only when the bit is set — the UART is ready.
	 *
	 * Once the UART is ready, `outb(ch, SERIAL_PORT + SERIAL_DATA)` writes
	 * the character to port 0x3F8, which is the Transmitter Holding Register.
	 * The UART then handles transmission asynchronously — we do not wait.
	 */
	while (!(inb(SERIAL_PORT + SERIAL_LSR) & SERIAL_LSR_THRE))
		;  /* spin */

	outb(SERIAL_PORT + SERIAL_DATA, (uint8_t) ch);
}

/* Write a string to serial, one character at a time.
 *
 * Nothing special: just iterate and call serial_putchar for each byte.
 * The NUL terminator is not sent — the string ends when we see it, not on
 * the wire.
 */
void serial_write(const char *str) {
	for (size_t i = 0; str[i]; i++)
		serial_putchar(str[i]);
}

/* Initialise the serial port.
 *
 * We do minimal setup, assuming the BIOS or bootloader left COM1 in a usable
 * state at 115200 8N1. On real hardware we might need to set baud rate,
 * parity, and stop bits; QEMU always defaults to the right settings, so we
 * just enable the port and call it done.
 *
 * This is a good example of how much you can do by reading one register and
 * doing nothing else. The UART stays configured by whatever firmware set it up.
 */
void serial_initialise(void) {
	/* Writing to the Modem Control Register bit 0 (DTR, Data Terminal Ready)
	 * and bit 1 (RTS, Request To Send) tells the device that a terminal is
	 * connected and ready to receive. Both are typically set by bootloaders;
	 * we set them again to be sure.
	 *
	 * 0x03 is binary 00000011: DTR and RTS both set.
	 */
	outb(SERIAL_PORT + SERIAL_MCR, 0x03);

	/* That's it. Reading LSR to check THRE will work now. */
}
