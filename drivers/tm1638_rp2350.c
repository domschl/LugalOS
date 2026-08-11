/*
 * LugalOS Hardware Driver: QYF-TM1638 8-Digit 7-Segment Display + 4x4 Keypad
 * Vendored and adapted from ~/gith/domschl/LugalChess (firmware/tm1638.c, H2,
 * plan/phase9_chess_computer.md).
 *
 * Hardware Mapping (cmake/board-rp2350.cmake is the source of truth):
 *   GP6 : STB (Strobe, Function 5 - SIO GPIO)
 *   GP7 : CLK (Clock,  Function 5 - SIO GPIO)
 *   GP8 : DIO (Data,   Function 5 - SIO GPIO, bidirectional)
 *
 * A simple bit-banged 3-wire protocol, not a real SPI/I2C peripheral -- no
 * hardware controller dependency beyond the raw SIO register access already
 * used for the LED pins (drivers/uart_rp2350.c).
 */

#include "drivers/tm1638.h"
#include "kernel/time.h"
#include "lugalos_config.h"
#include <string.h>

#define IO_BANK0_BASE           0x40028000UL
#define IO_BANK0_CTRL(n)        (IO_BANK0_BASE + 0x004 + (n) * 8)

#define PADS_BANK0_BASE         0x40038000UL
#define PADS_BANK0_PAD(n)       (PADS_BANK0_BASE + 0x004 + (n) * 4)

#define SIO_BASE                0xD0000000UL
#define SIO_GPIO_IN             (SIO_BASE + 0x004)
#define SIO_GPIO_OUT_SET        (SIO_BASE + 0x018)
#define SIO_GPIO_OUT_CLR        (SIO_BASE + 0x020)
#define SIO_GPIO_OE_SET         (SIO_BASE + 0x038)
#define SIO_GPIO_OE_CLR         (SIO_BASE + 0x040)

#define REG(addr) (*(volatile uint32_t *)(addr))

#define STB_PIN CONFIG_TM1638_STB_GPIO
#define CLK_PIN CONFIG_TM1638_CLK_GPIO
#define DIO_PIN CONFIG_TM1638_DIO_GPIO
#define STB_MASK (1u << STB_PIN)
#define CLK_MASK (1u << CLK_PIN)
#define DIO_MASK (1u << DIO_PIN)

/* 7-segment font mapping for printable ASCII characters */
static const uint8_t font7seg[128] = {
    [' '] = 0x00, ['-'] = 0x40, ['_'] = 0x08, ['='] = 0x48,
    ['0'] = 0x3F, ['1'] = 0x06, ['2'] = 0x5B, ['3'] = 0x4F,
    ['4'] = 0x66, ['5'] = 0x6D, ['6'] = 0x7D, ['7'] = 0x07,
    ['8'] = 0x7F, ['9'] = 0x6F,
    ['A'] = 0x77, ['B'] = 0x7C, ['C'] = 0x39, ['D'] = 0x5E,
    ['E'] = 0x79, ['F'] = 0x71, ['G'] = 0x3D, ['H'] = 0x76,
    ['I'] = 0x06, ['J'] = 0x1E, ['L'] = 0x38, ['O'] = 0x3F,
    ['P'] = 0x73, ['S'] = 0x6D, ['U'] = 0x3E, ['Y'] = 0x6E,
    ['a'] = 0x5F, ['b'] = 0x7C, ['c'] = 0x58, ['d'] = 0x5E,
    ['e'] = 0x79, ['f'] = 0x71, ['g'] = 0x6F, ['h'] = 0x74,
    ['i'] = 0x04, ['j'] = 0x0E, ['l'] = 0x06, ['n'] = 0x54,
    ['o'] = 0x5C, ['p'] = 0x73, ['r'] = 0x50, ['u'] = 0x1C,
    ['t'] = 0x78, ['y'] = 0x6E,

    /* Additional approximations for letters that are otherwise missing */
    ['N'] = 0x54, ['R'] = 0x50, ['Q'] = 0x67, ['q'] = 0x67,
    ['K'] = 0x76, ['k'] = 0x76, ['M'] = 0x37, ['m'] = 0x54,
    ['W'] = 0x3E, ['w'] = 0x1C, ['V'] = 0x3E, ['v'] = 0x1C,
    ['T'] = 0x78
};

/* Internal 16-byte RAM cache corresponding to display & LED addresses */
static uint8_t tm1638_ram[16];

static inline void stb_low(void)  { REG(SIO_GPIO_OUT_CLR) = STB_MASK; }
static inline void stb_high(void) { REG(SIO_GPIO_OUT_SET) = STB_MASK; }

/* Write a byte to DIO with stable timing delays. Caller must ensure DIO is
 * in output mode. */
static void tm1638_write_byte(uint8_t byte) {
    for (int i = 0; i < 8; i++) {
        REG(SIO_GPIO_OUT_CLR) = CLK_MASK;
        if ((byte >> i) & 1) {
            REG(SIO_GPIO_OUT_SET) = DIO_MASK;
        } else {
            REG(SIO_GPIO_OUT_CLR) = DIO_MASK;
        }
        time_delay_us(3); /* Clock low pulse width */
        REG(SIO_GPIO_OUT_SET) = CLK_MASK;
        time_delay_us(3); /* Clock high pulse width */
    }
}

/* Read a byte from DIO. Caller must ensure DIO is in input mode. */
static uint8_t tm1638_read_byte(void) {
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        REG(SIO_GPIO_OUT_CLR) = CLK_MASK;
        time_delay_us(3);
        REG(SIO_GPIO_OUT_SET) = CLK_MASK;
        time_delay_us(3); /* TM1638 shifts data out on rising edge */
        if (REG(SIO_GPIO_IN) & DIO_MASK) {
            byte |= (1u << i);
        }
    }
    return byte;
}

static void tm1638_send_command(uint8_t cmd) {
    stb_low();
    tm1638_write_byte(cmd);
    stb_high();
    time_delay_us(2); /* Strobe recovery delay */
}

/* Flush local RAM cache to TM1638 display registers using auto-increment
 * address mode (0x40) */
static void tm1638_flush(void) {
    tm1638_send_command(0x40);

    stb_low();
    tm1638_write_byte(0xC0); /* Start address */
    for (int i = 0; i < 16; i++) {
        tm1638_write_byte(tm1638_ram[i]);
    }
    stb_high();
    time_delay_us(2);
}

void tm1638_init(void) {
    REG(IO_BANK0_CTRL(STB_PIN)) = 5;
    REG(IO_BANK0_CTRL(CLK_PIN)) = 5;
    REG(IO_BANK0_CTRL(DIO_PIN)) = 5;
    REG(PADS_BANK0_PAD(STB_PIN)) = 0x5A;
    REG(PADS_BANK0_PAD(CLK_PIN)) = 0x5A;
    /* DIO keeps its internal pull-up (PUE=1, part of 0x5A) for when it
     * switches to input during key reads. */
    REG(PADS_BANK0_PAD(DIO_PIN)) = 0x5A;

    REG(SIO_GPIO_OE_SET) = STB_MASK | CLK_MASK | DIO_MASK;

    stb_high();
    REG(SIO_GPIO_OUT_SET) = CLK_MASK;
    REG(SIO_GPIO_OUT_CLR) = DIO_MASK;

    time_delay_us(10000);

    tm1638_send_command(0x8F); /* Activate display, max brightness */

    memset(tm1638_ram, 0, sizeof(tm1638_ram));
    tm1638_flush();
}

void tm1638_display_string(const char *str) {
    uint8_t buffer[8];
    memset(buffer, 0, sizeof(buffer));

    int len = (int)strlen(str);
    int buf_idx = 0;

    for (int i = 0; i < len && buf_idx < 8; i++) {
        char c = str[i];

        if (c == '.' && buf_idx > 0) {
            buffer[buf_idx - 1] |= 0x80;
        } else {
            uint8_t pattern = 0x00;
            if ((uint8_t)c < 128) {
                pattern = font7seg[(uint8_t)c];
            }
            buffer[buf_idx++] = pattern;
        }
    }

    for (int seg = 0; seg < 8; seg++) {
        tm1638_ram[seg * 2] = 0x00;
    }

    /* CRITICAL: Transpose the 8x8 matrix (QYF-TM1638 is Common Anode).
     * Digit i segment s is bit s of buffer[i]. Digit index 0 (left-most
     * character) maps to bit 7 (Digit 1 on module); digit index 7
     * (right-most character) maps to bit 0 (Digit 8 on module). */
    for (int seg = 0; seg < 8; seg++) {
        uint8_t val = 0;
        for (int digit = 0; digit < 8; digit++) {
            if ((buffer[digit] >> seg) & 1) {
                val |= (1u << (7 - digit));
            }
        }
        tm1638_ram[seg * 2] = val;
    }

    tm1638_flush();
}

void tm1638_set_leds(uint8_t mask) {
    for (int i = 0; i < 8; i++) {
        tm1638_ram[(i * 2) + 1] = (mask >> i) & 1;
    }
    tm1638_flush();
}

int tm1638_get_key(void) {
    uint8_t keys[4];

    stb_low();
    tm1638_write_byte(0x42); /* Read keys command */

    REG(SIO_GPIO_OE_CLR) = DIO_MASK; /* DIO -> input for the reply */
    time_delay_us(5); /* Wait for DIO turnaround */

    for (int i = 0; i < 4; i++) {
        keys[i] = tm1638_read_byte();
    }

    REG(SIO_GPIO_OE_SET) = DIO_MASK; /* DIO -> output for subsequent writes */

    stb_high();
    time_delay_us(2); /* Strobe recovery delay */

    /* Phantom key rejection: count total pressed keys across all bytes. If
     * more than 1 key appears pressed, it's noise from the bus. Key-relevant
     * bits per byte: 0x04 (K3/KS_odd), 0x40 (K3/KS_even), 0x02 (K2/KS_odd),
     * 0x20 (K2/KS_even). */
    int total_pressed = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b = keys[i] & 0x66;
        keys[i] = b;
        while (b) {
            total_pressed += (b & 1);
            b >>= 1;
        }
    }
    if (total_pressed != 1) {
        return -1; /* No key or phantom (multiple keys / noise) */
    }

    /* Matrix key decoding: K3 line -> S1..S8 -> File keys (indices 0..7);
     * K2 line -> S9..S16 -> Rank keys (indices 8..15). */
    for (int i = 0; i < 4; i++) {
        uint8_t b = keys[i];
        if (b & 0x04) return i * 2 + 0;
        if (b & 0x40) return i * 2 + 1;
        if (b & 0x02) return i * 2 + 8;
        if (b & 0x20) return i * 2 + 9;
    }

    return -1;
}
