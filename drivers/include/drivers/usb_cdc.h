#ifndef DRIVERS_USB_CDC_H
#define DRIVERS_USB_CDC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void usb_cdc_init(void);
void usb_cdc_task(void);
void usb_cdc_debug_dump(void);
bool usb_cdc_is_connected(void);

// Console CDC Port (/dev/ttyACM0)
void usb_cdc_putc(char c);
char usb_cdc_getc(void);
bool usb_cdc_has_char(void);

// 9P Network Interconnect CDC Port (/dev/ttyACM1)
int usb_cdc_write_net(const uint8_t *buf, size_t len);
int usb_cdc_read_net(uint8_t *buf, size_t max_len);

#endif // DRIVERS_USB_CDC_H
