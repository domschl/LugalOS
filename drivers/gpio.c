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

#elif defined(CONFIG_BOARD_K210)
/* K210 GPIO, 65.6. The block at 0x50200000 is SiFive-shaped (vendor
 * lib/drivers/include/gpio.h spells every offset): DATA is the OUTPUT
 * latch, DIRECTION 1 = output, SOURCE 1 = output driven by the PWMP
 * block instead of the latch, and DATA_INPUT at +0x50 is the actual
 * pad state -- reading DATA back on an input reads the latch, not
 * the pin, so reads here go to +0x50 unconditionally.
 *
 * The pad mux is the FPIOA at 0x502B0000: function 56+n is GPIO_n,
 * one config word per pad. The flag word below is the one proven on
 * this silicon by the loader and the writer blob (it drove the NOR's
 * HOLD#/WP# lines and left MISO readable with its pull enabled):
 * bit16 powers the pad on, bits[6:5]=3 are the drive selector, and
 * bit12 enables the pad's pull. The arduino-k210 copy of the SDK
 * documents this struct's bit positions differently -- the empirical
 * word works on C0 silicon and that is what counts; do not "fix" it
 * toward that header. Pulls here are pull-up only: the same flag the
 * MISO bring-up used, and all this board needs (the KEY input).
 * Pin numbers are IO_n from the schematic, 0..31. */
#define K210_GPIO_BASE      0x50200000UL
#define GPIO_DATA           (K210_GPIO_BASE + 0x00u)
#define GPIO_DIRECTION      (K210_GPIO_BASE + 0x04u)
#define GPIO_SOURCE         (K210_GPIO_BASE + 0x08u)
#define GPIO_DATA_INPUT     (K210_GPIO_BASE + 0x50u)

#define K210_FPIOA_BASE     0x502B0000UL
#define FPIOA_PAD_CFG(p)    (K210_FPIOA_BASE + 4u * (uint32_t)(p))
#define FPIOA_FN_GPIO(n)    (56u + (uint32_t)(n))
#define FPIOA_PAD_FLAGS     ((1u << 16) | (3u << 5) | (1u << 12))

#define REG(addr)           (*(volatile uint32_t *)(uintptr_t)(addr))

static void fpioa_take_gpio(uint32_t pin) {
    REG(FPIOA_PAD_CFG(pin)) = FPIOA_FN_GPIO(pin) | FPIOA_PAD_FLAGS;
}

int gpio_set_mode(uint32_t pin, int mode) {
    if (pin > 31) return -1;
    fpioa_take_gpio(pin);
    REG(GPIO_SOURCE) &= ~(1u << pin);          /* output side drives from the latch */
    if (mode == GPIO_MODE_OUT) REG(GPIO_DIRECTION) |= (1u << pin);
    else                       REG(GPIO_DIRECTION) &= ~(1u << pin);
    return 0;
}

int gpio_write_pin(uint32_t pin, int val) {
    if (pin > 31) return -1;
    if (val) REG(GPIO_DATA) |= (1u << pin);
    else     REG(GPIO_DATA) &= ~(1u << pin);
    return 0;
}

int gpio_read_pin(uint32_t pin) {
    if (pin > 31) return -1;
    return (REG(GPIO_DATA_INPUT) & (1u << pin)) ? 1 : 0;
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
