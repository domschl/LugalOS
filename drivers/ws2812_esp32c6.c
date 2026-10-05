/* The ESP32-C6's WS2812 output: RMT transmit channel 0 to GPIO8 (45.5,
 * plan/phase45_esp32c6.md). The Waveshare ESP32-C6-Zero's one LED is on GPIO8.
 *
 * The RMT is a pulse generator: 48 words of (level, duration) x2 in its own RAM,
 * clocked from the 40 MHz crystal divided to 10 MHz, played out on a pad through
 * the GPIO matrix. The encoder (drivers/ws2812.c) fills the RAM; this file
 * sets the peripheral up once and starts it per colour.
 *
 * Registers: IDF components/soc/esp32c6/register/soc/{rmt,pcr,gpio,io_mux}_reg.h
 * and hal/rmt_ll.h for the order of the steps.
 *
 * GPIO8 is also a strapping pin (it selects the ROM's download-mode message
 * level); it is only read at reset, so driving it afterwards is harmless.
 */

#include "drivers/ws2812.h"
#include "lugalos_config.h"

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define RMT_BASE        0x60006000u
#define RMT_RAM         (RMT_BASE + 0x400u)
#define RMT_CH0CONF0    (RMT_BASE + 0x10u)
#define RMT_INT_RAW     (RMT_BASE + 0x38u)
#define RMT_INT_CLR     (RMT_BASE + 0x44u)
#define RMT_SYS_CONF    (RMT_BASE + 0x68u)
#define RMT_CH0_STATUS  (RMT_BASE + 0x28u)

#define C0_TX_START     (1u << 0)
#define C0_MEM_RD_RST   (1u << 1)
#define C0_APB_MEM_RST  (1u << 2)
#define C0_IDLE_OUT_EN  (1u << 6)       /* idle level (low) is driven, not left floating */
#define C0_DIV_SHIFT    8
#define C0_MEM_SIZE_SHIFT 16
#define C0_CONF_UPDATE  (1u << 24)
#define TX_END_CH0      (1u << 0)

#define PCR_BASE        0x60096000u
#define PCR_RMT_CONF    (PCR_BASE + 0x2cu)
#define PCR_RMT_SCLK    (PCR_BASE + 0x30u)

#define GPIO_BASE       0x60091000u
#define GPIO_ENABLE_W1TS (GPIO_BASE + 0x24u)
#define GPIO_FUNC_OUT_SEL(n) (GPIO_BASE + 0x554u + 4u * (n))
#define IO_MUX_GPIO(n)  (0x60090000u + 4u + 4u * (n))
#define RMT_SIG_OUT0    71u

#ifndef CONFIG_WS2812_GPIO
#define CONFIG_WS2812_GPIO 8
#endif
/* The Zero's LED takes R,G,B (its vendor demo says so); a bare WS2812 is G,R,B. */
#ifndef CONFIG_WS2812_RGB_ORDER
#define CONFIG_WS2812_RGB_ORDER 1
#endif

#define SPIN_LIMIT 200000u

static bool g_ready;

bool ws2812_init(void) {
    if (g_ready) return true;
    /* Peripheral bus clock on, pulse out of reset, then the pulse clock: XTAL
     * (source 3), 40 MHz / (1 + 3) = 10 MHz, running. */
    REG(PCR_RMT_CONF) = 1u;
    REG(PCR_RMT_CONF) = 1u | 2u;
    REG(PCR_RMT_CONF) = 1u;
    REG(PCR_RMT_SCLK) = (3u << 20) | (1u << 22) | (3u << 12) | (1u << 6) | 0u;
    /* Plain APB access to the RMT RAM (not the FIFO window), RAM clock forced on,
     * register clock on. */
    REG(RMT_SYS_CONF) = (1u << 0) | (1u << 1) | (1u << 31);
    /* GPIO8 -> RMT output 0: pad function GPIO (MCU_SEL = 1), source = signal 71,
     * output enable from the register (the pulse generator has none). */
    REG(IO_MUX_GPIO(CONFIG_WS2812_GPIO)) = (1u << 12);
    REG(GPIO_FUNC_OUT_SEL(CONFIG_WS2812_GPIO)) = RMT_SIG_OUT0 | (1u << 9);
    REG(GPIO_ENABLE_W1TS) = 1u << CONFIG_WS2812_GPIO;
    /* Channel 0: pulse clock undivided, one 48-word RAM block, idle low. */
    REG(RMT_CH0CONF0) = (1u << C0_DIV_SHIFT) | (1u << C0_MEM_SIZE_SHIFT) | C0_IDLE_OUT_EN;
    g_ready = true;
    return true;
}

bool ws2812_set(uint8_t r, uint8_t g, uint8_t b) {
    if (!ws2812_init()) return false;
    /* A frame still on the wire (~31 us) would be cut short: wait for its end. */
    for (unsigned i = 0; i < SPIN_LIMIT; i++) {
        if (((REG(RMT_CH0_STATUS) >> 9) & 7u) == 0) break;           /* STATE_CH0: idle */
    }
    uint32_t words[WS2812_WORDS];
    ws2812_encode(words, r, g, b, CONFIG_WS2812_RGB_ORDER);
    for (unsigned i = 0; i < WS2812_WORDS; i++) REG(RMT_RAM + 4u * i) = words[i];

    uint32_t c0 = REG(RMT_CH0CONF0) & ~(C0_TX_START | C0_MEM_RD_RST | C0_APB_MEM_RST | C0_CONF_UPDATE);
    REG(RMT_INT_CLR) = TX_END_CH0;
    REG(RMT_CH0CONF0) = c0 | C0_MEM_RD_RST | C0_APB_MEM_RST;      /* read pointer to word 0 */
    REG(RMT_CH0CONF0) = c0;
    REG(RMT_CH0CONF0) = c0 | C0_CONF_UPDATE | C0_TX_START;
    for (unsigned i = 0; i < SPIN_LIMIT; i++) {
        if (REG(RMT_INT_RAW) & TX_END_CH0) { REG(RMT_INT_CLR) = TX_END_CH0; return true; }
    }
    return false;                                                  /* it never finished: the clock or the pad is not right */
}
