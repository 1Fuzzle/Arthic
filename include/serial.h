/* serial.h — serial output via COM1, for other files to use. */
#ifndef ARTHIC_SERIAL_H
#define ARTHIC_SERIAL_H

void serial_initialise(void);
void serial_putchar(char ch);
void serial_write(const char *str);

#endif
