/* drivers/audiotest_esp32p4.c — Audio Hardware Diagnostic Tool for ESP32-P4
 *
 * Implements hardware verification for audio codecs (ES8311, ES8388), speaker
 * power amplifiers (NS4150B via PI4IOE1 on Tab5 or GPIO 53 on LCD-7B/NANO),
 * and the ESP32-P4 I2S0 peripheral registers and clock gates.
 *
 * Concurrency: Stateless shell command execution; all register updates to
 * shared clock gates are serialized via sysreg_set/sysreg_clear().
 */

#include "drivers/audiotest_esp32p4.h"
#include "lugalos_config.h"
#include "kernel/console.h"
#include "kernel/printk.h"
#include "kernel/sysreg.h"
#include "kernel/time.h"
#include "drivers/gpio.h"
#include "drivers/i2c_bus.h"
#include "drivers/i2c_reg.h"

#include <string.h>

#if defined(CONFIG_BOARD_ESP32P4)

/* ESP32-P4 Peripheral Addresses */
#define P4_I2S0_BASE                    0x500C6000UL
#define P4_HP_SYS_CLKRST_BASE           0x500E6000UL
#define P4_HP_SYS_CLKRST_SOC_CLK_CTRL2  (P4_HP_SYS_CLKRST_BASE + 0x1CUL)
#define P4_HP_SYS_CLKRST_HP_RST_EN1     (P4_HP_SYS_CLKRST_BASE + 0xC4UL)

#define I2S0_APB_CLK_EN_BIT             (1u << 14)
#define I2S0_APB_RST_EN_BIT             (1u << 4)

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* --- Tab5 ES8388 Codec (0x10) Configuration --- */
#if defined(CONFIG_BOARD_TAB5)

#define CODEC_NAME "ES8388"
#define CODEC_ADDR 0x10u

/* M5Unified verified DAC initialization sequence for M5Stack Tab5 */
static const uint8_t s_es8388_init_seq[][2] = {
    { 0,  0x80 }, /* RESET / CSM power on */
    { 0,  0x00 },
    { 0,  0x00 },
    { 0,  0x0E },
    { 1,  0x00 },
    { 2,  0x0A }, /* CHIP POWER: power up all */
    { 3,  0xFF }, /* ADC POWER: power down all */
    { 4,  0x3C }, /* DAC POWER: power up and LOUT1/ROUT1/LOUT2/ROUT2 enable */
    { 5,  0x00 }, /* ChipLowPower1 */
    { 6,  0x00 }, /* ChipLowPower2 */
    { 7,  0x7C }, /* VSEL */
    { 8,  0x00 }, /* Set I2S slave mode */
    { 23, 0x18 }, /* I2S format: 16-bit */
    { 24, 0x00 }, /* I2S MCLK ratio: 128 */
    { 25, 0x20 }, /* DAC unmute */
    { 26, 0x00 }, /* LDACVOL: 0dB */
    { 27, 0x00 }, /* RDACVOL: 0dB */
    { 28, 0x08 }, /* Enable digital click-free power up/down */
    { 29, 0x00 },
    { 38, 0x00 }, /* DAC CTRL16 */
    { 39, 0xB8 }, /* LEFT Ch MIX */
    { 42, 0xB8 }, /* RIGHT Ch MIX */
    { 43, 0x08 }, /* ADC and DAC separate */
    { 45, 0x00 }, /* 1.5k VREF analog output */
    { 46, 0x21 },
    { 47, 0x21 },
    { 48, 0x21 },
    { 49, 0x21 },
};

static bool p4_audio_amp_set(bool on) {
    /* PI4IOE1 at 0x43, P1 = SPK_EN (bit 1) */
    return i2c_tab5_expander_update(0x43u, 0x02u, on ? 0x02u : 0x00u);
}

static int p4_audio_amp_get(void) {
    int val = i2c_tab5_expander_read(0x43u, 0x05u); /* Reg 0x05: OUT_SET */
    if (val < 0) return -1;
    return (val & 0x02u) ? 1 : 0;
}

static bool p4_codec_init(void) {
    size_t count = sizeof(s_es8388_init_seq) / sizeof(s_es8388_init_seq[0]);
    for (size_t i = 0; i < count; i++) {
        uint8_t reg = s_es8388_init_seq[i][0];
        uint8_t val = s_es8388_init_seq[i][1];
        if (!i2c_reg_write_u8(CODEC_ADDR, reg, val)) {
            cprintf("  failed writing ES8388 reg 0x%02x\n", reg);
            return false;
        }
    }
    return true;
}

static void p4_codec_dump(void) {
    cprintf("ES8388 Register Dump (0x10):\n");
    for (uint8_t reg = 0; reg <= 50; reg++) {
        uint8_t val = 0;
        if (i2c_reg_read_u8(CODEC_ADDR, reg, &val)) {
            cprintf("  [0x%02x] = 0x%02x\n", reg, val);
        } else {
            cprintf("  [0x%02x] = <NACK>\n", reg);
        }
    }
}

#else /* Waveshare ESP32-P4-WIFI6-Touch-LCD-7B and ESP32-P4-NANO */

#define CODEC_NAME "ES8311"
#define CODEC_ADDR 0x18u

/* Waveshare verified DAC initialization sequence for ES8311 */
static const uint8_t s_es8311_init_seq[][2] = {
    { 0x00, 0x1F }, /* Reset */
    { 0x00, 0x00 },
    { 0x00, 0x80 }, /* Power-on command */
    { 0x01, 0x3F }, /* Enable all clocks */
    { 0x06, 0x03 }, /* SCLK divider: BCLK div */
    { 0x07, 0x00 }, /* LRCK divider high */
    { 0x08, 0xFF }, /* LRCK divider low */
    { 0x09, 0x0C }, /* SDP In: 16-bit I2S slave mode */
    { 0x0A, 0x0C }, /* SDP Out: 16-bit */
    { 0x0D, 0x01 }, /* Power up analog circuitry */
    { 0x0E, 0x02 }, /* Enable analog PGA & ADC modulator */
    { 0x12, 0x00 }, /* Power up DAC */
    { 0x13, 0x10 }, /* Enable output to HP drive */
    { 0x1C, 0x6A }, /* ADC Equalizer bypass, cancel DC offset */
    { 0x37, 0x08 }, /* Bypass DAC equalizer */
    { 0x31, 0x00 }, /* DAC unmute */
    { 0x32, 0xBF }, /* Voice volume (~75%, 0xBF / 255) */
};

static bool p4_audio_amp_set(bool on) {
    /* NS4150B PA enable on GPIO 53 (active high) */
    gpio_set_mode(53, GPIO_MODE_OUT);
    gpio_write_pin(53, on ? 1 : 0);
    return true;
}

static int p4_audio_amp_get(void) {
    return gpio_read_pin(53);
}

static bool p4_codec_init(void) {
    /* 1. Reset command */
    if (!i2c_reg_write_u8(CODEC_ADDR, 0x00, 0x1F)) return false;
    time_delay_us(20000);
    if (!i2c_reg_write_u8(CODEC_ADDR, 0x00, 0x00)) return false;
    if (!i2c_reg_write_u8(CODEC_ADDR, 0x00, 0x80)) return false;

    /* 2. Configure remaining registers */
    size_t count = sizeof(s_es8311_init_seq) / sizeof(s_es8311_init_seq[0]);
    for (size_t i = 3; i < count; i++) {
        uint8_t reg = s_es8311_init_seq[i][0];
        uint8_t val = s_es8311_init_seq[i][1];
        if (!i2c_reg_write_u8(CODEC_ADDR, reg, val)) {
            cprintf("  failed writing ES8311 reg 0x%02x\n", reg);
            return false;
        }
    }
    return true;
}

static void p4_codec_dump(void) {
    cprintf("ES8311 Register Dump (0x18):\n");
    for (uint8_t reg = 0; reg <= 0x37; reg++) {
        uint8_t val = 0;
        if (i2c_reg_read_u8(CODEC_ADDR, reg, &val)) {
            cprintf("  [0x%02x] = 0x%02x\n", reg, val);
        } else {
            cprintf("  [0x%02x] = <NACK>\n", reg);
        }
    }
    /* Chip ID registers */
    for (uint32_t reg = 0xFD; reg <= 0xFF; reg++) {
        uint8_t val = 0;
        if (i2c_reg_read_u8(CODEC_ADDR, (uint8_t)reg, &val)) {
            cprintf("  [0x%02x] = 0x%02x\n", reg, val);
        }
    }
}

#endif /* CONFIG_BOARD_TAB5 */

static void p4_i2s_enable_clocks(void) {
    /* 1. Gate I2S0 APB clock ON in HP_SYS_CLKRST */
    sysreg_set(P4_HP_SYS_CLKRST_SOC_CLK_CTRL2, I2S0_APB_CLK_EN_BIT);

    /* 2. Deassert I2S0 APB Reset */
    sysreg_clear(P4_HP_SYS_CLKRST_HP_RST_EN1, I2S0_APB_RST_EN_BIT);
}

void audiotest_cmd(const char *arg) {
    while (arg && *arg == ' ') arg++;

    if (arg && strcmp(arg, "help") == 0) {
        cprintf("Usage: audiotest [status | init | amp on | amp off | dump]\n");
        cprintf("  status   - Probe codec, report amplifier & I2S0 peripheral state (default)\n");
        cprintf("  init     - Initialize codec DAC path and enable speaker power amplifier\n");
        cprintf("  amp on   - Turn on NS4150B speaker amplifier\n");
        cprintf("  amp off  - Turn off NS4150B speaker amplifier\n");
        cprintf("  dump     - Dump all audio codec I2C registers\n");
        return;
    }

    if (arg && strcmp(arg, "amp on") == 0) {
        if (p4_audio_amp_set(true)) {
            cprintf("audiotest: speaker power amplifier ENABLED.\n");
        } else {
            cprintf("audiotest: failed to enable speaker power amplifier.\n");
        }
        return;
    }

    if (arg && strcmp(arg, "amp off") == 0) {
        if (p4_audio_amp_set(false)) {
            cprintf("audiotest: speaker power amplifier MUTED (disabled).\n");
        } else {
            cprintf("audiotest: failed to mute speaker power amplifier.\n");
        }
        return;
    }

    if (arg && strcmp(arg, "dump") == 0) {
        p4_codec_dump();
        return;
    }

    if (arg && strcmp(arg, "init") == 0) {
        cprintf("audiotest: initializing %s at 0x%02x...\n", CODEC_NAME, CODEC_ADDR);
        p4_i2s_enable_clocks();

        if (p4_codec_init()) {
            p4_audio_amp_set(true);
            cprintf("[PASS] %s codec initialized for DAC output; speaker amplifier ENABLED.\n", CODEC_NAME);
        } else {
            cprintf("[FAIL] Failed to initialize %s codec over I2C.\n", CODEC_NAME);
        }
        return;
    }

    /* Default: audiotest status */
    cprintf("=== ESP32-P4 Audio Hardware Diagnostics ===\n");
#if defined(CONFIG_BOARD_DESC)
    cprintf("Board Target:      %s\n", CONFIG_BOARD_DESC);
#endif
#if defined(CONFIG_BOARD_TAB5)
    cprintf("Audio Codec:       ES8388 at 0x10 on SYS_I2C (GPIO 31/32)\n");
    cprintf("Power Amplifier:   NS4150B controlled by PI4IOE1 (0x43) Pin 1 (SPK_EN)\n");
    cprintf("I2S0 Pin Routing:  MCLK=GPIO30, BCLK=GPIO27, WS=GPIO29, DOUT=GPIO26, DIN=GPIO28\n");
#else
    cprintf("Audio Codec:       ES8311 at 0x18 on I2C0 (GPIO 7/8)\n");
    cprintf("Power Amplifier:   NS4150B controlled by GPIO 53\n");
    cprintf("I2S0 Pin Routing:  MCLK=GPIO13, BCLK=GPIO12, WS=GPIO10, DOUT=GPIO9, DIN=GPIO11\n");
#endif

    /* 1. Probe Codec on I2C */
    cprintf("\n--- 1. Codec I2C Probe ---\n");
#if defined(CONFIG_BOARD_TAB5)
    uint8_t ctl0 = 0, ctl1 = 0, ctl2 = 0, dacp = 0;
    bool ok0 = i2c_reg_read_u8(CODEC_ADDR, 0x00, &ctl0);
    bool ok1 = i2c_reg_read_u8(CODEC_ADDR, 0x01, &ctl1);
    bool ok2 = i2c_reg_read_u8(CODEC_ADDR, 0x02, &ctl2);
    bool ok4 = i2c_reg_read_u8(CODEC_ADDR, 0x04, &dacp);
    if (ok0 && ok1 && ok2 && ok4) {
        cprintf("  [PASS] ES8388 responded at 0x10\n");
        cprintf("  Control0=0x%02x Control1=0x%02x ChipPower=0x%02x DACPower=0x%02x\n",
                ctl0, ctl1, ctl2, dacp);
    } else {
        cprintf("  [FAIL] ES8388 at 0x10 did not answer I2C read!\n");
    }
#else
    uint8_t id1 = 0, id2 = 0, rev = 0, ctl0 = 0;
    bool ok1 = i2c_reg_read_u8(CODEC_ADDR, 0xFD, &id1);
    bool ok2 = i2c_reg_read_u8(CODEC_ADDR, 0xFE, &id2);
    bool ok3 = i2c_reg_read_u8(CODEC_ADDR, 0xFF, &rev);
    bool ok0 = i2c_reg_read_u8(CODEC_ADDR, 0x00, &ctl0);
    if (ok1 && ok2 && ok3 && ok0 && id1 == 0x83 && id2 == 0x11) {
        cprintf("  [PASS] ES8311 responded at 0x18 (Silicon Signature: 0x%02x%02x, Rev: 0x%02x)\n",
                id1, id2, rev);
        cprintf("  Reset/Power Register [0x00] = 0x%02x\n", ctl0);
    } else {
        cprintf("  [FAIL] ES8311 at 0x18 did not match chip ID (read: 0x%02x 0x%02x)!\n", id1, id2);
    }
#endif

    /* 2. Speaker Power Amplifier */
    cprintf("\n--- 2. Speaker Power Amplifier ---\n");
    int amp_state = p4_audio_amp_get();
    if (amp_state >= 0) {
        cprintf("  Speaker Amplifier (NS4150B): %s\n",
                amp_state ? "ENABLED (active)" : "MUTED (standby/low-power)");
    } else {
        cprintf("  [FAIL] Unable to read amplifier control status.\n");
    }

    /* 3. I2S0 Subsystem Clock & Registers */
    cprintf("\n--- 3. ESP32-P4 I2S0 Peripheral Subsystem ---\n");
    uint32_t clk_ctrl2 = REG32(P4_HP_SYS_CLKRST_SOC_CLK_CTRL2);
    uint32_t rst_en1   = REG32(P4_HP_SYS_CLKRST_HP_RST_EN1);
    bool clk_on = (clk_ctrl2 & I2S0_APB_CLK_EN_BIT) != 0;
    bool in_rst = (rst_en1 & I2S0_APB_RST_EN_BIT) != 0;

    cprintf("  I2S0 APB Clock Gate: %s (HP_SYS_CLKRST bit 14 = %d)\n",
            clk_on ? "ENABLED" : "GATED OFF", clk_on ? 1 : 0);
    cprintf("  I2S0 Reset Status:   %s (HP_SYS_CLKRST bit 4 = %d)\n",
            in_rst ? "ASSERTED (in reset)" : "RELEASED (running)", in_rst ? 1 : 0);

    if (!clk_on || in_rst) {
        cprintf("  (Tip: run 'audiotest init' to gate clock on, deassert reset, and initialize codec)\n");
    } else {
        uint32_t i2s_date  = REG32(P4_I2S0_BASE + 0x00);
        uint32_t i2s_state = REG32(P4_I2S0_BASE + 0x04);
        uint32_t i2s_conf1 = REG32(P4_I2S0_BASE + 0x08);
        uint32_t i2s_tx    = REG32(P4_I2S0_BASE + 0x18);
        cprintf("  I2S0 HW Registers:   DATE=0x%08x STATE=0x%08x CONF1=0x%08x TX_CONF=0x%08x\n",
                i2s_date, i2s_state, i2s_conf1, i2s_tx);
    }
}

#else /* Non-ESP32-P4 targets */

void audiotest_cmd(const char *arg) {
    (void)arg;
    cprintf("audiotest: requires ESP32-P4 target with audio hardware.\n");
}

#endif