#ifndef DRIVERS_GPIO_H
#define DRIVERS_GPIO_H

#include <stdint.h>
#include <stdbool.h>

#define GPIO_MODE_IN       0
#define GPIO_MODE_OUT      1
#define GPIO_MODE_PULLUP   2
#define GPIO_MODE_PULLDOWN 3

int gpio_set_mode(uint32_t pin, int mode);
int gpio_write_pin(uint32_t pin, int val);
int gpio_read_pin(uint32_t pin);

#endif /* DRIVERS_GPIO_H */
