/* drivers/audio_esp32p4.c — Audio Subsystem Driver for ESP32-P4
 *
 * Implements hardware audio playback via I2S0 and AHB-GDMA circular descriptors
 * for ESP32-P4 hardware targets:
 *  - M5Stack Tab5: ES8388 codec on SYS_I2C (0x10) + NS4150B PA via PI4IOE1 (0x43)
 *  - Waveshare LCD-7B / NANO: ES8311 codec on I2C0 (0x18) + NS4150B PA on GPIO 53
 *
 * Concurrency: All register updates to shared clock gates are serialized via
 * sysreg_set/sysreg_clear/sysreg_update(). Static DMA buffers and streaming state
 * are accessed synchronously within the single-task execution domain of the
 * active shell command or game session (ceres / moonbase).
 *
 * Freestanding C, zero floating-point math (pure fixed-point integer arithmetic).
 */

#include "drivers/audio_esp32p4.h"
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

#include "arch/esp32p4_intr.h"

/* --- ESP32-P4 Peripheral Register Definitions --- */

/* AHB-GDMA Controller (0x50085000) */
#define AHB_DMA_BASE                    0x50085000UL
#define AHB_DMA_OUT_INT_RAW_CH0         (AHB_DMA_BASE + 0x0030UL)
#define AHB_DMA_OUT_INT_CLR_CH0         (AHB_DMA_BASE + 0x003CUL)
#define AHB_DMA_MISC_CONF               (AHB_DMA_BASE + 0x0064UL)
#define AHB_DMA_OUT_CONF0_CH0           (AHB_DMA_BASE + 0x00D0UL)
#define AHB_DMA_OUT_LINK_CH0            (AHB_DMA_BASE + 0x00E0UL)
#define AHB_DMA_OUT_PERI_SEL_CH0        (AHB_DMA_BASE + 0x0100UL)
#define AHB_DMA_OUT_LINK_ADDR_CH0       (AHB_DMA_BASE + 0x03B8UL)
#define AHB_DMA_INTR_MEM_START_ADDR     (AHB_DMA_BASE + 0x03C4UL)
#define AHB_DMA_INTR_MEM_END_ADDR       (AHB_DMA_BASE + 0x03C8UL)

/* I2S0 Controller (0x500C6000) */
#define P4_I2S0_BASE                    0x500C6000UL
#define I2S0_TX_CONF                    (P4_I2S0_BASE + 0x0024UL)
#define I2S0_TX_CONF1                   (P4_I2S0_BASE + 0x002CUL)
#define I2S0_TX_PCM2PDM_CONF            (P4_I2S0_BASE + 0x0040UL)
#define I2S0_TX_TDM_CTRL                (P4_I2S0_BASE + 0x0054UL)
#define I2S0_STATE                      (P4_I2S0_BASE + 0x006CUL)
#define I2S0_CLK_GATE                   (P4_I2S0_BASE + 0x007CUL)
#define I2S0_DATE                       (P4_I2S0_BASE + 0x0080UL)

/* HP_SYS_CLKRST Subsystem (0x500E6000) */
#define P4_HP_SYS_CLKRST_BASE           0x500E6000UL
#define P4_CLKRST_SOC_CLK_CTRL1         (P4_HP_SYS_CLKRST_BASE + 0x0018UL)
#define P4_CLKRST_SOC_CLK_CTRL2         (P4_HP_SYS_CLKRST_BASE + 0x001CUL)
#define P4_CLKRST_PERI_CLK_CTRL13       (P4_HP_SYS_CLKRST_BASE + 0x004CUL)
#define P4_CLKRST_PERI_CLK_CTRL14       (P4_HP_SYS_CLKRST_BASE + 0x0050UL)
#define P4_CLKRST_HP_RST_EN1            (P4_HP_SYS_CLKRST_BASE + 0x00C4UL)
#define P4_CLKRST_HP_RST_EN2            (P4_HP_SYS_CLKRST_BASE + 0x00C8UL)

/* GPIO Matrix and IO_MUX Registers */
#define P4_GPIO_BASE                    0x500E0000UL
#define P4_IOMUX_BASE                   0x500E1000UL
#define P4_IOMUX_PAD(n)                 (P4_IOMUX_BASE + 0x0004UL + 4u * (uint32_t)(n))
#define P4_GPIO_OUT_SEL(n)              (P4_GPIO_BASE + 0x0558UL + 4u * (uint32_t)(n))
#define P4_GPIO_ENABLE_W1TS_LO          (P4_GPIO_BASE + 0x0024UL)
#define P4_GPIO_ENABLE_W1TS_HI          (P4_GPIO_BASE + 0x0030UL)

/* I2S0 Matrix Signal IDs */
#define I2S0_SIG_BCK                    25u
#define I2S0_SIG_MCLK                   26u
#define I2S0_SIG_WS                     27u
#define I2S0_SIG_DOUT                   28u

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* --- Hardware DMA Descriptor Layout (padded to 64-byte L1 cache line) ---
 * Each descriptor must occupy exactly one cache line to prevent false-sharing
 * cache line evictions between the CPU and GDMA hardware controller. */
typedef struct {
    uint32_t dw0;
    uint32_t buffer;
    uint32_t next;
    uint8_t  pad[52]; /* 64 - 12 = 52 bytes */
} dma_desc_t;

_Static_assert(sizeof(dma_desc_t) == 64, "dma_desc_t must occupy exactly one 64-byte cache line");

#define DMA_DESC_DW0(size, length, suc_eof, owner) \
    (((uint32_t)(size) & 0xFFFu) | \
     (((uint32_t)(length) & 0xFFFu) << 12) | \
     (((uint32_t)(suc_eof) & 1u) << 30) | \
     (((uint32_t)(owner) & 1u) << 31))

/* Static DMA storage aligned to ESP32-P4 L1 cache line (64 bytes) */
/* single task: shell */
static _Alignas(64) dma_desc_t s_dma_desc[AUDIO_P4_NUM_DESC];
/* single task: shell */
static _Alignas(64) int16_t s_audio_buf[AUDIO_P4_NUM_DESC][AUDIO_P4_CHUNK_SAMPLES * 2];

/* Streaming state */
/* single task: shell */
static bool s_streaming = false;
/* single task: shell */
static int s_active_desc = 0;
/* single task: shell */
static audio_p4_render_fn s_stream_render_fn = NULL;
/* single task: shell */
static void *s_stream_render_user = NULL;
#if defined(CONFIG_BOARD_TAB5)
/* single task: shell */
static int s_audio_volume = 70;
#else
/* single task: shell */
static int s_audio_volume = 60;
#endif

/* --- Fixed-Point Trigonometry (Pure Fixed-Point, Zero FPU) --- */
static const int16_t s_sin_tab[256] = {
        0,   804,  1607,  2410,  3211,  4011,  4807,  5601,
     6392,  7179,  7961,  8739,  9511, 10278, 11038, 11792,
    12539, 13278, 14009, 14732, 15446, 16150, 16845, 17530,
    18204, 18867, 19519, 20159, 20787, 21402, 22004, 22594,
    23169, 23731, 24278, 24811, 25329, 25831, 26318, 26789,
    27244, 27683, 28105, 28510, 28897, 29268, 29621, 29955,
    30272, 30571, 30851, 31113, 31356, 31580, 31785, 31970,
    32137, 32284, 32412, 32520, 32609, 32678, 32727, 32757,
    32767, 32757, 32727, 32678, 32609, 32520, 32412, 32284,
    32137, 31970, 31785, 31580, 31356, 31113, 30851, 30571,
    30272, 29955, 29621, 29268, 28897, 28510, 28105, 27683,
    27244, 26789, 26318, 25831, 25329, 24811, 24278, 23731,
    23169, 22594, 22004, 21402, 20787, 20159, 19519, 18867,
    18204, 17530, 16845, 16150, 15446, 14732, 14009, 13278,
    12539, 11792, 11038, 10278,  9511,  8739,  7961,  7179,
     6392,  5601,  4807,  4011,  3211,  2410,  1607,   804,
        0,  -804, -1607, -2410, -3211, -4011, -4807, -5601,
    -6392, -7179, -7961, -8739, -9511,-10278,-11038,-11792,
   -12539,-13278,-14009,-14732,-15446,-16150,-16845,-17530,
   -18204,-18867,-19519,-20159,-20787,-21402,-22004,-22594,
   -23169,-23731,-24278,-24811,-25329,-25831,-26318,-26789,
   -27244,-27683,-28105,-28510,-28897,-29268,-29621,-29955,
   -30272,-30571,-30851,-31113,-31356,-31580,-31785,-31970,
   -32137,-32284,-32412,-32520,-32609,-32678,-32727,-32757,
   -32767,-32757,-32727,-32678,-32609,-32520,-32412,-32284,
   -32137,-31970,-31785,-31580,-31356,-31113,-30851,-30571,
   -30272,-29955,-29621,-29268,-28897,-28510,-28105,-27683,
   -27244,-26789,-26318,-25831,-25329,-24811,-24278,-23731,
   -23169,-22594,-22004,-21402,-20787,-20159,-19519,-18867,
   -18204,-17530,-16845,-16150,-15446,-14732,-14009,-13278,
   -12539,-11792,-11038,-10278, -9511, -8739, -7961, -7179,
    -6392, -5601, -4807, -4011, -3211, -2410, -1607,  -804
};

int16_t audio_p4_sine(uint16_t phase) {
    uint8_t idx = (uint8_t)(phase >> 8);
    uint8_t frac = (uint8_t)(phase & 0xFF);
    int32_t v0 = s_sin_tab[idx];
    int32_t v1 = s_sin_tab[(uint8_t)(idx + 1)];
    return (int16_t)(v0 + (((v1 - v0) * (int32_t)frac) >> 8));
}

/* --- Tab5 ES8388 Codec (0x10) Configuration --- */
#if defined(CONFIG_BOARD_TAB5)

#define CODEC_NAME "ES8388"
#define CODEC_ADDR 0x10u

static const uint8_t s_es8388_init_seq[][2] = {
    /* 1. Mute DAC during configuration */
    { 25, 0x04 }, /* DACCONTROL3: DAC mute */

    /* 2. Chip Control and Power Management */
    { 1,  0x00 }, /* CONTROL2: full analog bias & reference buffers powered */
    { 2,  0x00 }, /* CHIPPOWER: normal all and power up all */
    { 8,  0x00 }, /* MASTERMODE: I2S slave mode */

    /* 3. DAC Power and Routing */
    { 4,  0xC0 }, /* DACPOWER: disable DAC and disable Lout/Rout/1/2 during config */
    { 0,  0x12 }, /* CONTROL1: Enfr=0, Play & Record Mode, VMIDSEL=01 */

    /* 4. Audio Format & Clocks */
    { 12, 0x0C }, /* ADCCONTROL4: 16-bit I2S */
    { 13, 0x02 }, /* ADCCONTROL5: SINGLE SPEED, RATIO=256 */
    { 23, 0x18 }, /* DACCONTROL1: 16-bit standard I2S */
    { 24, 0x02 }, /* DACCONTROL2: SINGLE SPEED, RATIO=256 */
    { 38, 0x00 }, /* DACCONTROL16: RLIN1/LLIN1 not bypassed to mixers */
    { 39, 0x90 }, /* DACCONTROL17: Left DAC to Left Mixer enable (0dB) */
    { 42, 0x90 }, /* DACCONTROL20: Right DAC to Right Mixer enable (0dB) */
    { 43, 0x80 }, /* DACCONTROL21: DAC uses matching LRCK (ADCLRCK from GPIO 29), MCLK/DLL on */
    { 45, 0x00 }, /* DACCONTROL23: VROI=0 (1.5k VREF) */

    /* 5. Digital Volume: 0dB (unattenuated) */
    { 26, 0x00 }, /* DACCONTROL4: LDACVOL = 0dB */
    { 27, 0x00 }, /* DACCONTROL5: RDACVOL = 0dB */

    /* 6. Analog Output Volume: 0dB (0x1E) unity gain to minimize analog noise floor */
    { 46, 0x1E }, /* DACCONTROL24: LOUT1 Volume (0dB) */
    { 47, 0x1E }, /* DACCONTROL25: ROUT1 Volume (0dB) */
    { 48, 0x1E }, /* DACCONTROL26: LOUT2 Volume (0dB) */
    { 49, 0x1E }, /* DACCONTROL27: ROUT2 Volume (0dB) */

    /* 7. Power up DAC and outputs */
    { 4,  0x3C }, /* DACPOWER: power up DAC, LOUT1, ROUT1, LOUT2, ROUT2 */
    { 3,  0xFF }, /* ADCPOWER: power down ADC */
};

bool audio_p4_amp_set(bool on) {
    /* PI4IOE1 at 0x43:
     * P1 = SPK_EN (bit 1)
     * P2 = EXT5V_EN (bit 2) - 5V boost converter rail for NS4150B PA!
     * Ensure P1 and P2 are configured as outputs (reg 0x03) and not Hi-Z (reg 0x07). */
    i2c_tab5_expander_reg_update(0x43u, 0x03u, 0x06u, 0x06u); /* IO_DIR: output */
    i2c_tab5_expander_reg_update(0x43u, 0x07u, 0x06u, 0x00u); /* OUT_H_IM: push-pull */
    return i2c_tab5_expander_update(0x43u, 0x06u, on ? 0x06u : 0x04u);
}

int audio_p4_amp_get(void) {
    int val = i2c_tab5_expander_read(0x43u, 0x05u); /* Reg 0x05: OUT_SET */
    if (val < 0) return -1;
    return (val & 0x02u) ? 1 : 0;
}

static bool p4_codec_init(void) {
    /* 1. Soft reset */
    if (!i2c_reg_write_u8(CODEC_ADDR, 0x00, 0x80)) return false;
    time_delay_us(20000); /* 20ms settling time */
    if (!i2c_reg_write_u8(CODEC_ADDR, 0x00, 0x00)) return false;
    time_delay_us(5000);

    /* 2. Write register configuration table */
    size_t count = sizeof(s_es8388_init_seq) / sizeof(s_es8388_init_seq[0]);
    for (size_t i = 0; i < count; i++) {
        uint8_t reg = s_es8388_init_seq[i][0];
        uint8_t val = s_es8388_init_seq[i][1];
        if (!i2c_reg_write_u8(CODEC_ADDR, reg, val)) {
            cprintf("  failed writing ES8388 reg 0x%02x\n", reg);
            return false;
        }
    }

    /* 3. Start state machine while I2S clocks run */
    if (!i2c_reg_write_u8(CODEC_ADDR, 0x02, 0xF0)) return false;
    time_delay_us(2000);
    if (!i2c_reg_write_u8(CODEC_ADDR, 0x02, 0x00)) return false;
    time_delay_us(2000);

    /* 4. Unmute DAC */
    if (!i2c_reg_write_u8(CODEC_ADDR, 0x19, 0x00)) return false;

    audio_p4_set_volume(s_audio_volume);
    return true;
}

void audio_p4_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_audio_volume = percent;

    /* ES8388 DAC digital attenuation: reg 26/27 (LDACVOL / RDACVOL).
     * 0x00 is 0dB (maximum undistorted volume), 0xC0 (192) is -96dB (mute).
     * Resolution: 0.5dB per step.
     * Decibel scaling maps 100% -> 0dB (atten = 0), 10% -> -24.0dB (atten = 48),
     * providing smooth ~2.4dB steps between volume levels 1-9: */
    uint8_t atten = (percent <= 0) ? 0xC0 : (uint8_t)(((100 - percent) * 54) / 100);
    i2c_reg_write_u8(CODEC_ADDR, 26, atten);
    i2c_reg_write_u8(CODEC_ADDR, 27, atten);
}

void audio_p4_codec_dump(void) {
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

static const uint8_t s_es8311_init_seq[][2] = {
    { 0x00, 0x1F }, /* Reset */
    { 0x00, 0x00 },
    { 0x00, 0x80 }, /* Power-on command */
    { 0x01, 0x3F }, /* Enable all clocks, MCLK from MCLK pin */
    { 0x02, 0x00 }, /* Pre-divider=1, pre-multiplier=1 */
    { 0x03, 0x10 }, /* Normal single-speed mode, ADC OSR */
    { 0x04, 0x10 }, /* DAC OSR = 0x10 (for 256fs @ 16 kHz) */
    { 0x05, 0x00 }, /* ADC div = 1, DAC div = 1 */
    { 0x06, 0x03 }, /* SCLK divider: BCLK div = 4 (32 bits per frame) */
    { 0x07, 0x00 }, /* LRCK divider high */
    { 0x08, 0xFF }, /* LRCK divider low = 255 (256 BCLK cycles / frame) */
    { 0x09, 0x0C }, /* SDP In: 16-bit I2S slave mode */
    { 0x0A, 0x0C }, /* SDP Out: 16-bit */
    { 0x0D, 0x01 }, /* Power up analog circuitry */
    { 0x0E, 0x02 }, /* Enable analog PGA & ADC modulator */
    { 0x12, 0x00 }, /* Power up DAC */
    { 0x13, 0x10 }, /* Enable output to HP drive */
    { 0x1C, 0x6A }, /* ADC Equalizer bypass, cancel DC offset */
    { 0x37, 0x08 }, /* Bypass DAC equalizer */
    { 0x31, 0x00 }, /* DAC unmute */
    { 0x32, 0xBF }, /* Voice volume default 0dB unity gain (overwritten by audio_p4_set_volume) */
};

bool audio_p4_amp_set(bool on) {
    /* NS4150B PA enable on GPIO 53 (active high) */
    gpio_set_mode(53, GPIO_MODE_OUT);
    gpio_write_pin(53, on ? 1 : 0);
    return true;
}

int audio_p4_amp_get(void) {
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
    audio_p4_set_volume(s_audio_volume);
    return true;
}

void audio_p4_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_audio_volume = percent;

    /* ES8311 Voice volume: reg 0x32 (DAC_VOLUME).
     * 0x00 is mute (-95.5dB).
     * 0xBF (191) is 0dB (unity gain, full-scale clean digital output).
     * Values above 0xBF apply positive digital gain (up to +32dB @ 0xFF),
     * which causes severe digital clipping distortion.
     *
     * Resolution: 0.5dB per step.
     * Decibel scaling maps 100% -> 0dB (reg 0xBF), 10% -> -24.0dB (reg 0x8F),
     * providing smooth ~2.4dB steps between volume levels 1-9: */
    uint8_t vol;
    if (percent <= 0) {
        vol = 0x00;
    } else {
        uint8_t steps = (uint8_t)(((100 - percent) * 54) / 100);
        vol = (uint8_t)(0xBF - steps);
    }
    i2c_reg_write_u8(CODEC_ADDR, 0x32, vol);
}

void audio_p4_codec_dump(void) {
    cprintf("ES8311 Register Dump (0x18):\n");
    for (uint8_t reg = 0; reg <= 0x37; reg++) {
        uint8_t val = 0;
        if (i2c_reg_read_u8(CODEC_ADDR, reg, &val)) {
            cprintf("  [0x%02x] = 0x%02x\n", reg, val);
        } else {
            cprintf("  [0x%02x] = <NACK>\n", reg);
        }
    }
    for (uint32_t reg = 0xFD; reg <= 0xFF; reg++) {
        uint8_t val = 0;
        if (i2c_reg_read_u8(CODEC_ADDR, (uint8_t)reg, &val)) {
            cprintf("  [0x%02x] = 0x%02x\n", reg, val);
        }
    }
}

#endif /* CONFIG_BOARD_TAB5 */

int audio_p4_get_volume(void) {
    return s_audio_volume;
}

const char *audio_p4_codec_name(void) {
    return CODEC_NAME;
}

bool audio_p4_codec_write(uint8_t reg, uint8_t val) {
    return i2c_reg_write_u8(CODEC_ADDR, reg, val);
}

bool audio_p4_codec_read(uint8_t reg, uint8_t *val) {
    return i2c_reg_read_u8(CODEC_ADDR, reg, val);
}

static void p4_gpio_route_out(uint32_t pin, uint32_t signal_id) {
    REG32(P4_IOMUX_PAD(pin)) = (1u << 9) | (1u << 8); /* FUNC=0 (GPIO), IE=1 */
    REG32(P4_GPIO_OUT_SEL(pin)) = signal_id;
    if (pin < 32) {
        REG32(P4_GPIO_ENABLE_W1TS_LO) = (1u << pin);
    } else {
        REG32(P4_GPIO_ENABLE_W1TS_HI) = (1u << (pin - 32));
    }
}

static void p4_i2s_dma_init(void) {
    /* Pin Routing */
#if defined(CONFIG_BOARD_TAB5)
    /* M5Stack Tab5: MCLK=30, BCLK=27, WS=29, DOUT=26 */
    p4_gpio_route_out(30, I2S0_SIG_MCLK);
    p4_gpio_route_out(27, I2S0_SIG_BCK);
    p4_gpio_route_out(29, I2S0_SIG_WS);
    p4_gpio_route_out(26, I2S0_SIG_DOUT);
#else
    /* Waveshare LCD-7B / NANO: MCLK=13, BCLK=12, WS=10, DOUT=9 */
    p4_gpio_route_out(13, I2S0_SIG_MCLK);
    p4_gpio_route_out(12, I2S0_SIG_BCK);
    p4_gpio_route_out(10, I2S0_SIG_WS);
    p4_gpio_route_out(9,  I2S0_SIG_DOUT);
#endif

    /* 1. Gate AHB-GDMA clock ON and release reset */
    sysreg_set(P4_CLKRST_SOC_CLK_CTRL1, (1u << 3));
    sysreg_set(P4_CLKRST_HP_RST_EN1, (1u << 1));
    sysreg_clear(P4_CLKRST_HP_RST_EN1, (1u << 1));

    /* 2. Gate I2S0 APB clock ON and release reset */
    sysreg_set(P4_CLKRST_SOC_CLK_CTRL2, (1u << 14));
    sysreg_set(P4_CLKRST_HP_RST_EN2, (1u << 4));
    sysreg_clear(P4_CLKRST_HP_RST_EN2, (1u << 4));

    /* 3. Bind MCLK to TX module */
    sysreg_set(P4_CLKRST_PERI_CLK_CTRL14, (1u << 19));

    /* 4. MCLK Divider for 16,000 Hz audio (40 MHz XTAL / 9.765625 = 4,096,000 Hz):
     *    N=9, x=3, y=4, z=15, yn1=1.
     *    Hardware erratum workaround: set small division first (N=2, y=1, z=0, x=0, yn1=0) */
    sysreg_update(P4_CLKRST_PERI_CLK_CTRL13, (0xFFu << 13) | (0x1FFu << 21), (2u << 13));
    sysreg_update(P4_CLKRST_PERI_CLK_CTRL14, 0x7FFFFu, (1u << 0));

    /* Target fractional division: */
    sysreg_update(P4_CLKRST_PERI_CLK_CTRL14, 0x7FFFFu, (4u << 0) | (15u << 9) | (1u << 18));
    sysreg_update(P4_CLKRST_PERI_CLK_CTRL13, (0xFFu << 13) | (0x1FFu << 21) | (3u << 11) | (1u << 10),
                  (9u << 13) | (3u << 21) | (1u << 10)); /* tx_clk_en=1, clk_src=0 (XTAL) */

    /* 5. Force I2S internal clock gate ON */
    REG32(I2S0_CLK_GATE) = 1u;

    /* 6. Configure AHB-GDMA Channel 0 */
    REG32(AHB_DMA_INTR_MEM_START_ADDR) = 0x40000000u;
    REG32(AHB_DMA_INTR_MEM_END_ADDR)   = 0x4FFC0000u;
    REG32(AHB_DMA_MISC_CONF) |= (1u << 3); /* Force register clock on */

    /* Reset TX Channel 0 FSM & FIFO */
    REG32(AHB_DMA_OUT_CONF0_CH0) = 1u;
    REG32(AHB_DMA_OUT_CONF0_CH0) = (1u << 2) | (1u << 3); /* auto_wrback=1, eof_mode=1 */

    /* Connect Channel 0 TX to I2S0 (peripheral trigger ID 3) */
    REG32(AHB_DMA_OUT_PERI_SEL_CH0) = 3u;

    /* 7. Configure I2S0 Controller */
    REG32(I2S0_TX_CONF) = 3u; /* Reset transmitter & FIFO */
    REG32(I2S0_TX_CONF) = 0u;

    REG32(I2S0_TX_PCM2PDM_CONF) = 0u; /* Bypass PCM to PDM converter */

    /* TDM Control: 2 channels (left & right), both enabled */
    REG32(I2S0_TX_TDM_CTRL) = (1u << 16) | 3u;

    /* CONF1: 16-bit slot, 16-bit audio data, 16-bit half sample, 16 BCK WS width */
    REG32(I2S0_TX_CONF1) = (15u << 27) | (15u << 19) | (15u << 14) | 15u;

    /* CONF: Philips standard (msb_shift=1), master mode, TDM mode, bck_div=8 (bck_div_num=7),
     * stop_en=1, pcm_bypass=1 */
    REG32(I2S0_TX_CONF) = 0x00E87210u;

    /* Latch register changes into I2S clock domain */
    REG32(I2S0_TX_CONF) |= (1u << 8);
    while (REG32(I2S0_TX_CONF) & (1u << 8)) ;
}

bool audio_p4_init(void) {
    p4_i2s_dma_init();
    REG32(I2S0_TX_CONF) |= (1u << 2); /* Start TX clocks */
    time_delay_us(10000);
    return p4_codec_init();
}

bool audio_p4_is_playing(void) {
    return s_streaming;
}

/* Continuous circular streaming API (used by games) */
bool audio_p4_start(audio_p4_render_fn render_fn, void *user) {
    if (s_streaming) return true;

    p4_i2s_dma_init();

    /* 1. Start I2S TX clocks first */
    REG32(I2S0_TX_CONF) |= (1u << 2);
    time_delay_us(10000);

    /* 2. Initialize codec with active clocks */
    if (!p4_codec_init()) {
        return false;
    }

    /* 3. Enable amplifier and wait 150ms for NS4150B startup latency */
    audio_p4_amp_set(true);
    time_delay_us(150000);

    /* 4. Reset GDMA Channel 0 TX FSM & FIFO */
    REG32(AHB_DMA_OUT_CONF0_CH0) |= (1u << 0);
    REG32(AHB_DMA_OUT_CONF0_CH0) &= ~(1u << 0);

    /* 5. Reset I2S0 TX FIFO */
    REG32(I2S0_TX_CONF) |= (1u << 8);
    while (REG32(I2S0_TX_CONF) & (1u << 8)) ;

    s_stream_render_fn   = render_fn;
    s_stream_render_user = user;
    s_active_desc        = 0;

    /* Initialize circular descriptors */
    for (int i = 0; i < AUDIO_P4_NUM_DESC; i++) {
        s_dma_desc[i].buffer = (uint32_t)(uintptr_t)s_audio_buf[i];
        s_dma_desc[i].next   = (uint32_t)(uintptr_t)&s_dma_desc[(i + 1) % AUDIO_P4_NUM_DESC];
        if (s_stream_render_fn) {
            s_stream_render_fn(s_audio_buf[i], AUDIO_P4_CHUNK_SAMPLES, s_stream_render_user);
        } else {
            memset(s_audio_buf[i], 0, sizeof(s_audio_buf[i]));
        }
        s_dma_desc[i].dw0 = DMA_DESC_DW0(AUDIO_P4_CHUNK_BYTES, AUDIO_P4_CHUNK_BYTES, 0, 1);
        esp32p4_dcache_writeback((uintptr_t)s_audio_buf[i], sizeof(s_audio_buf[i]));
        esp32p4_dcache_writeback((uintptr_t)&s_dma_desc[i], sizeof(s_dma_desc[i]));
    }

    REG32(AHB_DMA_OUT_INT_CLR_CH0) = 0xFFFFFFFFu;
    REG32(AHB_DMA_OUT_LINK_ADDR_CH0) = (uint32_t)(uintptr_t)&s_dma_desc[0];
    REG32(AHB_DMA_OUT_LINK_CH0) = 2u; /* outlink_start_ch0 = bit 1 */

    s_streaming = true;
    return true;
}

void audio_p4_pump(void) {
    if (!s_streaming) return;

    bool refilled = false;
    while (1) {
        esp32p4_dcache_invalidate((uintptr_t)&s_dma_desc[s_active_desc], sizeof(dma_desc_t));
        if ((s_dma_desc[s_active_desc].dw0 & (1u << 31)) != 0) {
            /* Current descriptor is still owned by DMA hardware */
            break;
        }

        /* In-flight buffer finished playing; refill it without blocking */
        if (s_stream_render_fn) {
            s_stream_render_fn(s_audio_buf[s_active_desc], AUDIO_P4_CHUNK_SAMPLES, s_stream_render_user);
        } else {
            memset(s_audio_buf[s_active_desc], 0, sizeof(s_audio_buf[s_active_desc]));
        }
        s_dma_desc[s_active_desc].dw0 = DMA_DESC_DW0(AUDIO_P4_CHUNK_BYTES, AUDIO_P4_CHUNK_BYTES, 0, 1);
        esp32p4_dcache_writeback((uintptr_t)s_audio_buf[s_active_desc], sizeof(s_audio_buf[s_active_desc]));
        esp32p4_dcache_writeback((uintptr_t)&s_dma_desc[s_active_desc], sizeof(s_dma_desc[s_active_desc]));
        s_active_desc = (s_active_desc + 1) % AUDIO_P4_NUM_DESC;
        refilled = true;
    }

    if (refilled) {
        /* If DMA parked due to frame underrun, restart transmission cleanly */
        if (REG32(AHB_DMA_OUT_LINK_CH0) & (1u << 3)) {
            REG32(AHB_DMA_OUT_LINK_CH0) = 4u; /* bit 2: outlink_restart_ch0 */
        }
    }
}

void audio_p4_stop(void) {
    if (!s_streaming) return;
    s_streaming = false;

    /* Wait for in-flight descriptors to finish */
    for (int k = 0; k < AUDIO_P4_NUM_DESC; k++) {
        uint64_t t0 = time_get_us();
        while (1) {
            esp32p4_dcache_invalidate((uintptr_t)&s_dma_desc[s_active_desc], sizeof(dma_desc_t));
            if ((s_dma_desc[s_active_desc].dw0 & (1u << 31)) == 0) break;
            if (time_get_us() - t0 > 100000u) break;
        }
        s_active_desc = (s_active_desc + 1) % AUDIO_P4_NUM_DESC;
    }

    time_delay_us(10000);              /* 10ms ramp settle before muting */
    audio_p4_amp_set(false);           /* Standby mute */
#if defined(CONFIG_BOARD_TAB5)
    i2c_reg_write_u8(CODEC_ADDR, 0x19, 0x04); /* Mute DAC */
#endif
    REG32(AHB_DMA_OUT_LINK_CH0) = 1u;  /* outlink_stop_ch0 = bit 0 */
    REG32(I2S0_TX_CONF) &= ~(1u << 2); /* tx_start = 0 */
}

/* One-shot stream playback (blocking until done, used by audiotest) */
void audio_p4_play_stream(void (*synth_fn)(int16_t *buf, uint32_t samples, void *user),
                          void *user, uint32_t total_samples) {
    p4_i2s_dma_init();

    /* 1. Start I2S TX clocks first */
    REG32(I2S0_TX_CONF) |= (1u << 2);
    time_delay_us(10000);

    /* 2. Initialize codec with active clocks */
    if (!p4_codec_init()) {
        cprintf("audiotest: codec initialization failed!\n");
        goto done;
    }

    /* 3. Enable amplifier and wait 150ms for NS4150B startup latency */
    audio_p4_amp_set(true);
    time_delay_us(150000);

    /* 4. Reset GDMA Channel 0 TX FSM & FIFO */
    REG32(AHB_DMA_OUT_CONF0_CH0) |= (1u << 0);
    REG32(AHB_DMA_OUT_CONF0_CH0) &= ~(1u << 0);

    /* 5. Reset I2S0 TX FIFO */
    REG32(I2S0_TX_CONF) |= (1u << 8);
    while (REG32(I2S0_TX_CONF) & (1u << 8)) ;

    memset(s_audio_buf, 0, sizeof(s_audio_buf));

    for (int i = 0; i < AUDIO_P4_NUM_DESC; i++) {
        s_dma_desc[i].buffer = (uint32_t)(uintptr_t)s_audio_buf[i];
        s_dma_desc[i].next   = (uint32_t)(uintptr_t)&s_dma_desc[(i + 1) % AUDIO_P4_NUM_DESC];
        s_dma_desc[i].dw0    = DMA_DESC_DW0(AUDIO_P4_CHUNK_BYTES, AUDIO_P4_CHUNK_BYTES, 0, 0);
    }

    /* Pre-fill buffers */
    uint32_t samples_left = total_samples;
    for (int i = 0; i < AUDIO_P4_NUM_DESC; i++) {
        uint32_t chunk = samples_left < AUDIO_P4_CHUNK_SAMPLES ? samples_left : AUDIO_P4_CHUNK_SAMPLES;
        if (chunk > 0) {
            synth_fn(s_audio_buf[i], chunk, user);
            samples_left -= chunk;
            if (chunk < AUDIO_P4_CHUNK_SAMPLES) {
                memset(&s_audio_buf[i][chunk * 2], 0, (AUDIO_P4_CHUNK_SAMPLES - chunk) * 2 * sizeof(int16_t));
            }
        } else {
            memset(s_audio_buf[i], 0, sizeof(s_audio_buf[i]));
        }
        s_dma_desc[i].dw0 = DMA_DESC_DW0(AUDIO_P4_CHUNK_BYTES, AUDIO_P4_CHUNK_BYTES, 0, 1);
        esp32p4_dcache_writeback((uintptr_t)s_audio_buf[i], sizeof(s_audio_buf[i]));
        esp32p4_dcache_writeback((uintptr_t)&s_dma_desc[i], sizeof(s_dma_desc[i]));
    }

    REG32(AHB_DMA_OUT_INT_CLR_CH0) = 0xFFFFFFFFu;
    REG32(AHB_DMA_OUT_LINK_ADDR_CH0) = (uint32_t)(uintptr_t)&s_dma_desc[0];
    REG32(AHB_DMA_OUT_LINK_CH0) = 2u;

    int active = 0;
    while (samples_left > 0) {
        uint64_t t0 = time_get_us();
        while (1) {
            esp32p4_dcache_invalidate((uintptr_t)&s_dma_desc[active], sizeof(s_dma_desc[active]));
            if ((s_dma_desc[active].dw0 & (1u << 31)) == 0) {
                break;
            }
            if (time_get_us() - t0 > 250000u) {
                cprintf("audiotest: DMA wait timeout!\n");
                goto done;
            }
        }

        uint32_t chunk = samples_left < AUDIO_P4_CHUNK_SAMPLES ? samples_left : AUDIO_P4_CHUNK_SAMPLES;
        synth_fn(s_audio_buf[active], chunk, user);
        samples_left -= chunk;
        if (chunk < AUDIO_P4_CHUNK_SAMPLES) {
            memset(&s_audio_buf[active][chunk * 2], 0, (AUDIO_P4_CHUNK_SAMPLES - chunk) * 2 * sizeof(int16_t));
        }

        s_dma_desc[active].dw0 = DMA_DESC_DW0(AUDIO_P4_CHUNK_BYTES, AUDIO_P4_CHUNK_BYTES, 0, 1);
        esp32p4_dcache_writeback((uintptr_t)s_audio_buf[active], sizeof(s_audio_buf[active]));
        esp32p4_dcache_writeback((uintptr_t)&s_dma_desc[active], sizeof(s_dma_desc[active]));

        active = (active + 1) % AUDIO_P4_NUM_DESC;
    }

    for (int k = 0; k < AUDIO_P4_NUM_DESC; k++) {
        uint64_t t0 = time_get_us();
        while (1) {
            esp32p4_dcache_invalidate((uintptr_t)&s_dma_desc[active], sizeof(dma_desc_t));
            if ((s_dma_desc[active].dw0 & (1u << 31)) == 0) break;
            if (time_get_us() - t0 > 250000u) break;
        }
        active = (active + 1) % AUDIO_P4_NUM_DESC;
    }

done:
    time_delay_us(10000);
    audio_p4_amp_set(false);
#if defined(CONFIG_BOARD_TAB5)
    i2c_reg_write_u8(CODEC_ADDR, 0x19, 0x04);
#endif
    REG32(AHB_DMA_OUT_LINK_CH0) = 1u;
    REG32(I2S0_TX_CONF) &= ~(1u << 2);
}

#endif /* CONFIG_BOARD_ESP32P4 */
