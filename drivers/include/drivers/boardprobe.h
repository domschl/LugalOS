#ifndef LUGALOS_DRIVERS_BOARDPROBE_H
#define LUGALOS_DRIVERS_BOARDPROBE_H

/* 36.0, plan/phase36_rp2350_lcd7_terminal.md: the questions the board answers
 * before any driver depends on the answer -- the die stepping, what is on the
 * PIO-USB port and at what speed, and whether GP0 (the PSRAM chip select on
 * the RP2350-LCD-7) is still untouched. Read-only apart from the two USB pad
 * configurations it needs to sample the bus. Built on boards whose file
 * declares a PIO-USB port (CONFIG_PIOUSB_DP_GPIO). */
void boardprobe(void);

#endif /* LUGALOS_DRIVERS_BOARDPROBE_H */
