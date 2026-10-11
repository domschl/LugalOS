#include "drivers/gpio.h"
#include "kernel/printk.h"
#include "lugalos_config.h"

#if defined(CONFIG_BOARD_RP2350)
#define SIO_BASE                0xd0000000UL
#define SIO_GPIO_IN             (SIO_BASE + 0x004)
#define SIO_GPIO_HI_IN          (SIO_BASE + 0x008)
#define SIO_GPIO_OUT_SET        (SIO_BASE + 0x018)
#define SIO_GPIO_HI_OUT_SET     (SIO_BASE + 0x01c)
#define SIO_GPIO_OUT_CLR        (SIO_BASE + 0x020)
#define SIO_GPIO_HI_OUT_CLR     (SIO_BASE + 0x024)
#define SIO_GPIO_OE_SET         (SIO_BASE + 0x038)
#define SIO_GPIO_HI_OE_SET      (SIO_BASE + 0x03c)
#define SIO_GPIO_OE_CLR         (SIO_BASE + 0x040)
#define SIO_GPIO_HI_OE_CLR      (SIO_BASE + 0x044)

#define IO_BANK0_BASE           0x40028000UL
#define IO_BANK0_CTRL(p)        (IO_BANK0_BASE + ((p) * 8u) + 4u)
#define PADS_BANK0_BASE         0x40038000UL
#define PADS_BANK0_PAD(p)       (PADS_BANK0_BASE + 4u + ((p) * 4u))
#define REG(addr)               (*(volatile uint32_t *)(uintptr_t)(addr))

int gpio_set_mode(uint32_t pin, int mode) {
    if (pin > 47) return -1;
    REG(IO_BANK0_CTRL(pin)) = 5; /* GPIO_FUNC_SIO */
    if (mode == GPIO_MODE_OUT) {
        REG(PADS_BANK0_PAD(pin)) = 0x52; /* 4mA, IE=1 */
        if (pin < 32) REG(SIO_GPIO_OE_SET) = (1u << pin);
        else          REG(SIO_GPIO_HI_OE_SET) = (1u << (pin - 32));
    } else {
        uint32_t pad = 0x40; /* IE=1 */
        if (mode == GPIO_MODE_PULLUP) pad |= 0x08;   /* PUE=1 */
        else if (mode == GPIO_MODE_PULLDOWN) pad |= 0x04; /* PDE=1 */
        REG(PADS_BANK0_PAD(pin)) = pad;
        if (pin < 32) REG(SIO_GPIO_OE_CLR) = (1u << pin);
        else          REG(SIO_GPIO_HI_OE_CLR) = (1u << (pin - 32));
    }
    return 0;
}

int gpio_write_pin(uint32_t pin, int val) {
    if (pin > 47) return -1;
    if (val) {
        if (pin < 32) REG(SIO_GPIO_OUT_SET) = (1u << pin);
        else          REG(SIO_GPIO_HI_OUT_SET) = (1u << (pin - 32));
    } else {
        if (pin < 32) REG(SIO_GPIO_OUT_CLR) = (1u << pin);
        else          REG(SIO_GPIO_HI_OUT_CLR) = (1u << (pin - 32));
    }
    return 0;
}

int gpio_read_pin(uint32_t pin) {
    if (pin > 47) return -1;
    uint32_t in = (pin < 32) ? REG(SIO_GPIO_IN) : REG(SIO_GPIO_HI_IN);
    uint32_t bit = (pin < 32) ? pin : (pin - 32);
    return (in & (1u << bit)) ? 1 : 0;
}

#elif defined(CONFIG_BOARD_ESP32P4) || defined(CONFIG_BOARD_ESP32C6)
#define GPIO_OUT_W1TS_REG       0x60091008UL
#define GPIO_OUT_W1TC_REG       0x6009100cUL
#define GPIO_ENABLE_W1TS_REG    0x60091024UL
#define GPIO_ENABLE_W1TC_REG    0x60091028UL
#define GPIO_IN_REG             0x60091040UL
#define REG(addr)               (*(volatile uint32_t *)(uintptr_t)(addr))

int gpio_set_mode(uint32_t pin, int mode) {
    if (pin > 31) return -1;
    if (mode == GPIO_MODE_OUT) {
        REG(GPIO_ENABLE_W1TS_REG) = (1u << pin);
    } else {
        REG(GPIO_ENABLE_W1TC_REG) = (1u << pin);
    }
    return 0;
}

int gpio_write_pin(uint32_t pin, int val) {
    if (pin > 31) return -1;
    if (val) REG(GPIO_OUT_W1TS_REG) = (1u << pin);
    else     REG(GPIO_OUT_W1TC_REG) = (1u << pin);
    return 0;
}

int gpio_read_pin(uint32_t pin) {
    if (pin > 31) return -1;
    return (REG(GPIO_IN_REG) & (1u << pin)) ? 1 : 0;
}

#else
/* QEMU / Mock fallback */
static uint8_t s_gpio_dirs[64];
static uint8_t s_gpio_vals[64];

int gpio_set_mode(uint32_t pin, int mode) {
    if (pin >= 64) return -1;
    s_gpio_dirs[pin] = (uint8_t)mode;
    return 0;
}

int gpio_write_pin(uint32_t pin, int val) {
    if (pin >= 64) return -1;
    s_gpio_vals[pin] = val ? 1 : 0;
    return 0;
}

int gpio_read_pin(uint32_t pin) {
    if (pin >= 64) return -1;
    return s_gpio_vals[pin] ? 1 : 0;
}
#endif
