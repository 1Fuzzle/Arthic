/* keyboard.c — turning key presses into characters.
 *
 * The keyboard does not send letters. It sends SCANCODES: a number identifying
 * a physical key position, plus a bit saying whether it went down or came up.
 * It has no idea what is printed on the keycap, and no idea what a capital
 * letter is. Everything above that is our job.
 *
 * Two consequences worth absorbing:
 *
 *   - Shift is not a modifier as far as the hardware is concerned. It is just
 *     another key that sends a press and a release. If you want Shift+A to
 *     mean 'A', you have to remember that Shift is currently held down. State
 *     tracking in software is the whole trick.
 *
 *   - Every key sends TWO events. Ignore the release events and every letter
 *     you type appears twice.
 *
 * This is scancode set 1, the default the PS/2 controller emulates, for a US
 * layout. Other layouts are the same mechanism with a different table.
 */

#include "keyboard.h"
#include "idt.h"
#include "io.h"
#include "terminal.h"

#define KEYBOARD_DATA_PORT 0x60

/* Scancodes we care about by name rather than by number. */
#define SC_LSHIFT    0x2A
#define SC_RSHIFT    0x36
#define SC_CAPSLOCK  0x3A
#define SC_RELEASED  0x80   /* this bit set means the key came UP */

/* Unshifted characters, indexed by scancode. 0 means "no printable
 * character" — function keys, Ctrl, Alt and so on land here and get dropped.
 *
 * Reading a table like this is easier than it looks: position N holds whatever
 * key number N produces. The gaps are real keys we simply have no character
 * for yet. */
static const char scancode_to_ascii[128] = {
	0,    0,   '1',  '2',  '3',  '4',  '5',  '6',    /* 0x00 - 0x07 */
	'7',  '8', '9',  '0',  '-',  '=',  '\b', '\t',   /* 0x08 - 0x0F */
	'q',  'w', 'e',  'r',  't',  'y',  'u',  'i',    /* 0x10 - 0x17 */
	'o',  'p', '[',  ']',  '\n', 0,    'a',  's',    /* 0x18 - 0x1F */
	'd',  'f', 'g',  'h',  'j',  'k',  'l',  ';',    /* 0x20 - 0x27 */
	'\'', '`', 0,    '\\', 'z',  'x',  'c',  'v',    /* 0x28 - 0x2F */
	'b',  'n', 'm',  ',',  '.',  '/',  0,    '*',    /* 0x30 - 0x37 */
	0,    ' ', 0,    0,    0,    0,    0,    0,      /* 0x38 - 0x3F */
	/* the rest are function keys, keypad and so on — all zero */
};

/* The same keys with Shift held. Note this is not a simple uppercase
 * transformation: '1' becomes '!', not '1'. Which is exactly why it needs its
 * own table rather than a clever function. */
static const char scancode_to_ascii_shift[128] = {
	0,    0,   '!',  '@',  '#',  '$',  '%',  '^',
	'&',  '*', '(',  ')',  '_',  '+',  '\b', '\t',
	'Q',  'W', 'E',  'R',  'T',  'Y',  'U',  'I',
	'O',  'P', '{',  '}',  '\n', 0,    'A',  'S',
	'D',  'F', 'G',  'H',  'J',  'K',  'L',  ':',
	'"',  '~', 0,    '|',  'Z',  'X',  'C',  'V',
	'B',  'N', 'M',  '<',  '>',  '?',  0,    '*',
	0,    ' ', 0,    0,    0,    0,    0,    0,
};

static int shift_held = 0;
static int caps_lock  = 0;

static key_callback_t on_key = 0;

static void keyboard_irq(struct registers *regs)
{
	(void) regs;

	uint8_t scancode = inb(KEYBOARD_DATA_PORT);

	/* Key released. The only releases that matter to us are the shift keys —
	 * everything else we simply ignore, or every keystroke would register
	 * twice. */
	if (scancode & SC_RELEASED) {
		uint8_t key = scancode & ~SC_RELEASED;
		if (key == SC_LSHIFT || key == SC_RSHIFT)
			shift_held = 0;
		return;
	}

	if (scancode == SC_LSHIFT || scancode == SC_RSHIFT) {
		shift_held = 1;
		return;
	}

	if (scancode == SC_CAPSLOCK) {
		caps_lock = !caps_lock;   /* a toggle, not a hold */
		return;
	}

	char c = shift_held ? scancode_to_ascii_shift[scancode]
	                    : scancode_to_ascii[scancode];

	if (c == 0)
		return;   /* a key we have no character for */

	/* Caps Lock affects letters only — it must not turn '1' into '!'.
	 * Hence the explicit range check rather than a blanket case flip. */
	if (caps_lock) {
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 'a' + 'A');
		else if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
	}

	if (on_key)
		on_key(c);
}

void keyboard_install(key_callback_t callback)
{
	on_key = callback;
	/* Also unmasks IRQ 1. Failing here means a machine with no keyboard input
	 * at all - unusable, and impossible to work out from the inside, so say it
	 * rather than leaving a dead keyboard to be guessed at. */
	if (!irq_install_handler(1, keyboard_irq))
		kprintf("keyboard: could not install the IRQ 1 handler - no input\n");
}
