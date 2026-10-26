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

#elif defined(CONFIG_BOARD_ESP32P4)
/* The P4's GPIO matrix (HP peripheral bus, 0x500E0000) and its IO_MUX; the pins are 0-54 and
 * the registers come in a low (0-31) and a high (32-54) word. */
#define GPIO_BASE               0x500E0000UL
#define GPIO_OUT_W1TS(hi)       (GPIO_BASE + ((hi) ? 0x14u : 0x08u))
#define GPIO_OUT_W1TC(hi)       (GPIO_BASE + ((hi) ? 0x18u : 0x0Cu))
#define GPIO_ENABLE_W1TS(hi)    (GPIO_BASE + ((hi) ? 0x30u : 0x24u))
#define GPIO_ENABLE_W1TC(hi)    (GPIO_BASE + ((hi) ? 0x34u : 0x28u))
#define GPIO_IN(hi)             (GPIO_BASE + ((hi) ? 0x40u : 0x3Cu))
#define IO_MUX_GPIO(pin)        (0x500E1000UL + 4u + 4u * (pin))
#define IO_MUX_FUN_WPD          (1u << 7)
#define IO_MUX_FUN_WPU          (1u << 8)
#define IO_MUX_FUN_IE           (1u << 9)
#define IO_MUX_FUN_DRV_2        (2u << 10)
#define IO_MUX_MCU_SEL_GPIO     (1u << 12)
#define GPIO_PINS               55u
#define REG(addr)               (*(volatile uint32_t *)(uintptr_t)(addr))

/* An input is the pad switched to the GPIO function with its input buffer on: after reset
 * many pads (GPIO35, a strapping pin, is one) are muxed to another function with the buffer
 * off, and read as a constant 0. */
int gpio_set_mode(uint32_t pin, int mode) {
    if (pin >= GPIO_PINS) return -1;
    uint32_t hi = pin >> 5, bit = 1u << (pin & 31u);
    if (mode == GPIO_MODE_OUT) {
        REG(IO_MUX_GPIO(pin)) = IO_MUX_MCU_SEL_GPIO | IO_MUX_FUN_DRV_2;
        REG(GPIO_ENABLE_W1TS(hi)) = bit;
        return 0;
    }
    REG(GPIO_ENABLE_W1TC(hi)) = bit;
    REG(IO_MUX_GPIO(pin)) = IO_MUX_MCU_SEL_GPIO | IO_MUX_FUN_DRV_2 | IO_MUX_FUN_IE |
                            (mode == GPIO_MODE_PULLUP ? IO_MUX_FUN_WPU :
                             mode == GPIO_MODE_PULLDOWN ? IO_MUX_FUN_WPD : 0u);
    return 0;
}

int gpio_write_pin(uint32_t pin, int val) {
    if (pin >= GPIO_PINS) return -1;
    REG(val ? GPIO_OUT_W1TS(pin >> 5) : GPIO_OUT_W1TC(pin >> 5)) = 1u << (pin & 31u);
    return 0;
}

int gpio_read_pin(uint32_t pin) {
    if (pin >= GPIO_PINS) return -1;
    return (REG(GPIO_IN(pin >> 5)) & (1u << (pin & 31u))) ? 1 : 0;
}

#elif defined(CONFIG_BOARD_ESP32C6)
#define GPIO_OUT_W1TS_REG       0x60091008UL
#define GPIO_OUT_W1TC_REG       0x6009100cUL
#define GPIO_ENABLE_W1TS_REG    0x60091024UL
#define GPIO_ENABLE_W1TC_REG    0x60091028UL
#define GPIO_IN_REG             0x6009103cUL
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
/* accepted: the `gpio` command's shadow of what it set; two terminals at once can leave
 * it out of date. */
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
