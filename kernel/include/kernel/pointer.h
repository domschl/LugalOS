#ifndef LUGALOS_KERNEL_POINTER_H
#define LUGALOS_KERNEL_POINTER_H

#include <stdbool.h>
#include <stdint.h>

/* A relative pointer (a USB mouse's motion) made absolute for the console
 * (kernel/console.h: console_pointer(), console_wheel()): the motion scaled
 * by the `input.pointer_speed` setting (percent; the remainder kept, so slow
 * motion is not lost), the position clamped to the screen as it is now (a
 * rotation keeps the point on it). Called by one consumer at a time: the
 * console's input pump (under its lock), which reads the mouse's ring. */
void pointer_relative(int dx, int dy, uint8_t buttons, int wheel);

/* The mouse went away: its held buttons are let go. */
void pointer_release(void);

/* Where the pointer is, in screen pixels. */
void pointer_position(int *x, int *y);

#endif /* LUGALOS_KERNEL_POINTER_H */
