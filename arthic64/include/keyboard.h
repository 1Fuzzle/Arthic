/* keyboard.h — PS/2 keyboard driver. */
#ifndef ARTHIC_KEYBOARD_H
#define ARTHIC_KEYBOARD_H

/* Called with each character the user types. '\n' for Enter, '\b' for
 * Backspace. Keys with no printable meaning are dropped by the driver. */
typedef void (*key_callback_t)(char c);

void keyboard_install(key_callback_t callback);

#endif
