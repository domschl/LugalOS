#ifndef LUGALOS_DRIVERS_USB_CRC_H
#define LUGALOS_DRIVERS_USB_CRC_H

#include <stdint.h>

/* USB's two CRCs -- 36.7, plan/phase36_rp2350_lcd7_terminal.md.
 *
 * Portable, with no hardware in it, so `usbselftest` checks them on QEMU
 * against known packets before the PIO-USB engine (drivers/piousb_rp2350.c)
 * puts them on a wire.
 *
 * CRC5 protects a token's 11 bits (address in [6:0], endpoint in [10:7]) or a
 * SOF's frame number: usb_crc5() returns the 5 bits to place in the top of
 * the token's last byte. CRC16 protects a data packet's payload; the two bytes
 * on the wire are its complement, low byte first. A receiver that runs the
 * CRC over payload *and* the received CRC ends at USB_CRC16_RESIDUAL. */

#define USB_CRC16_RESIDUAL 0xB001u

uint8_t usb_crc5(uint16_t v11);

/* Bitwise, 8 steps a byte: for building packets, not for the receive path. */
uint16_t usb_crc16_update(uint16_t crc, uint8_t byte);

/* The CRC16 two wire bytes of `data`: returns (hi << 8) | lo, sent lo first. */
uint16_t usb_crc16(const uint8_t *data, uint32_t len);

/* A 256-entry table for the receive path, where a byte arrives every 96
 * cycles and the ACK is due ~190 cycles after the last one. The caller owns
 * the storage, so it can live wherever the caller's code runs from. */
void usb_crc16_table(uint16_t table[256]);

/* `usbselftest`: the functions above against packets from the USB spec's
 * examples and common bus captures. Prints USB_SELFTEST_OK/_FAIL; returns the
 * number of failed cases. */
int usb_crc_selftest(void);

#endif /* LUGALOS_DRIVERS_USB_CRC_H */
