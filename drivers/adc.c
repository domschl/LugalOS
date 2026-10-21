#include "drivers/adc.h"
#include "kernel/time.h"
#include "lugalos_config.h"
#include <stdbool.h>

#if defined(CONFIG_BOARD_RP2350)
#define CLOCKS_BASE             0x40010000UL
#define CLOCKS_CLK_ADC_CTRL     (CLOCKS_BASE + 0x60)
#define CLOCKS_CLK_ADC_ENABLE   (1u << 11)

#define RESETS_BASE             0x40020000UL
#define RESETS_RESET_CLR        (RESETS_BASE + 0x3000 + 4)
#define RESETS_RESET_DONE       (RESETS_BASE + 0x8)
#define ADC_RESET_BIT           (1u << 0)

#define ADC_BASE                0x400a0000UL
#define ADC_CS                  (ADC_BASE + 0x0)
#define ADC_RESULT              (ADC_BASE + 0x4)
#define ADC_CS_EN_BIT           (1u << 0)
#define ADC_CS_START_ONCE       (1u << 2)
#define ADC_CS_READY_BIT        (1u << 8)
#define ADC_CS_AINSEL_LSB       12

#define REG(addr)               (*(volatile uint32_t *)(uintptr_t)(addr))

/* accepted: a lazy init, idempotent. */
static bool s_adc_ready = false;

static void adc_ensure_init(void) {
    if (s_adc_ready) return;
    REG(CLOCKS_CLK_ADC_CTRL) = CLOCKS_CLK_ADC_ENABLE;
    if (!(REG(RESETS_RESET_DONE) & ADC_RESET_BIT)) {
        REG(RESETS_RESET_CLR) = ADC_RESET_BIT;
        int timeout = 10000;
        while (!(REG(RESETS_RESET_DONE) & ADC_RESET_BIT) && --timeout > 0);
    }
    REG(ADC_CS) = ADC_CS_EN_BIT;
    time_delay_us(10);
    s_adc_ready = true;
}

int adc_read_channel_raw(uint32_t channel) {
    if (channel > 4) return -1;
    adc_ensure_init();
    uint32_t cs = REG(ADC_CS);
    cs &= ~(0xFu << ADC_CS_AINSEL_LSB);
    cs |= (channel & 0xFu) << ADC_CS_AINSEL_LSB;
    cs |= ADC_CS_START_ONCE;
    REG(ADC_CS) = cs;
    int timeout = 10000;
    while (!(REG(ADC_CS) & ADC_CS_READY_BIT) && --timeout > 0);
    return (int)(REG(ADC_RESULT) & 0xFFFu);
}

int adc_read_channel_mv(uint32_t channel) {
    int raw = adc_read_channel_raw(channel);
    if (raw < 0) return -1;
    return (int)(((uint32_t)raw * 3300u) / 4095u);
}

#else
/* QEMU / mock fallback */
int adc_read_channel_raw(uint32_t channel) {
    if (channel > 4) return -1;
    return 2048; /* nominal mid-scale */
}

int adc_read_channel_mv(uint32_t channel) {
    if (channel > 4) return -1;
    return 1650; /* nominal 1.65 V */
}
#endif
