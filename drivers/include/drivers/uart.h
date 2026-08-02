#ifndef LUGALOS_DRIVERS_UART_H
#define LUGALOS_DRIVERS_UART_H

#include <stdint.h>
#include <stdbool.h>

void uart_init(uintptr_t base_addr);
void uart_putc(char c);
char uart_getc(void);
bool uart_has_char(void);
void uart_puts(const char *s);

#endif /* LUGALOS_DRIVERS_UART_H */
