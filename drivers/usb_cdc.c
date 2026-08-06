#include "drivers/usb_cdc.h"
#include "kernel/printk.h"
#include "drivers/uart.h"
#include <string.h>

static bool g_usb_cdc_connected = false;

#if defined(CONFIG_BOARD_RP2350)

static volatile uint8_t g_usb_pending_addr = 0;
static volatile bool g_usb_need_set_addr = false;

#define USB_BASE              0x50110000UL
#define USB_DPRAM_BASE        0x50100000UL

#define USB_ADDR_ENDP         (USB_BASE + 0x00)
#define USB_MAIN_CTRL         (USB_BASE + 0x40)
#define USB_SIE_CTRL          (USB_BASE + 0x4C)
#define USB_SIE_STATUS        (USB_BASE + 0x50)
#define USB_BUFF_STATUS       (USB_BASE + 0x54)
#define USB_MUXING            (USB_BASE + 0x74)
#define USB_PWR               (USB_BASE + 0x78)
#define USB_INTR              (USB_BASE + 0x8C)
#define USB_INTE              (USB_BASE + 0x90)
#define USB_INTF              (USB_BASE + 0x94)
#define USB_INTS              (USB_BASE + 0x98)

#define USB_SIE_STATUS_ACK_REC        (1u << 30)
#define USB_SIE_STATUS_BUS_RESET      (1u << 19)
#define USB_SIE_STATUS_TRANS_COMPLETE (1u << 18)
#define USB_SIE_STATUS_SETUP_REC      (1u << 17)

// Latched (write-1-to-clear) event/error bits in SIE_STATUS. Left uncleared,
// each one sticks at 1 forever after its first occurrence and makes every
// later [USB EVT] log line look like a fresh completion/error when it may
// just be stale. Cleared every poll so the diagnostic log reflects reality.
#define USB_SIE_STATUS_EVENT_BITS ( (1u << 31) /* DATA_SEQ_ERROR */ \
                                   | (1u << 30) /* ACK_REC */ \
                                   | (1u << 29) /* STALL_REC */ \
                                   | (1u << 28) /* NAK_REC */ \
                                   | (1u << 27) /* RX_TIMEOUT */ \
                                   | (1u << 26) /* RX_OVERFLOW */ \
                                   | (1u << 25) /* BIT_STUFF_ERROR */ \
                                   | (1u << 24) /* CRC_ERROR */ \
                                   | (1u << 19) /* BUS_RESET */ \
                                   | (1u << 18) /* TRANS_COMPLETE */ \
                                   | (1u << 17) /* SETUP_REC */ \
                                   | (1u << 13) /* RESUME */ )

#define USB_INTR_SETUP_REQ       (1u << 16)
#define USB_INTR_BUS_RESET       (1u << 12)
#define USB_INTR_BUFF_STATUS     (1u << 4)

#define USB_EP0_SETUP         (USB_DPRAM_BASE + 0x00)
#define USB_EP0_IN_CTRL       (USB_DPRAM_BASE + 0x80)
#define USB_EP0_OUT_CTRL      (USB_DPRAM_BASE + 0x84)
#define USB_EP0_BUF           (USB_DPRAM_BASE + 0x100)

#define CLOCKS_BASE           0x40010000UL
#define CLK_USB_CTRL          (CLOCKS_BASE + 0x60)
#define CLK_USB_DIV           (CLOCKS_BASE + 0x64)

#define RESETS_BASE           0x40020000UL
#define RESETS_RESET_SET      (RESETS_BASE + 0x2000)
#define RESETS_RESET_CLR      (RESETS_BASE + 0x3000)
#define RESETS_RESET_DONE     (RESETS_BASE + 0x0008)
#define RESET_PLL_USB_BIT     (1u << 15)
#define RESET_USB_BIT         (1u << 28)

#define REG(addr) (*(volatile uint32_t *)(addr))
#define USB_BUF_CTRL_AVAIL    (1u << 10)

static inline void usb_busy_wait_cycles(uint32_t n) {
    for (volatile uint32_t i = 0; i < n; i++) {
        __asm__ volatile("nop");
    }
}

// RP2350 datasheet 12.7.3.7.1 "Concurrent access": the AVAILABLE bit of an
// endpoint buffer control register must reach the SIE in a write separate
// from (and after) the rest of the fields, with a few core clock cycles in
// between. Setting AVAILABLE together with FULL/LEN/PID races the hardware
// and the SIE can miss or corrupt the transfer.
static void ep_buf_ctrl_write(volatile uint32_t *reg, uint32_t value) {
    if (value & USB_BUF_CTRL_AVAIL) {
        *reg = value & ~USB_BUF_CTRL_AVAIL;
        usb_busy_wait_cycles(12);
    }
    *reg = value;
}

// USB Descriptors
static const uint8_t g_usb_dev_desc[] = {
    18,         // bLength
    1,          // bDescriptorType = Device
    0x00, 0x02, // bcdUSB = 2.00
    0xEF,       // bDeviceClass = Miscellaneous / IAD
    0x02,       // bDeviceSubClass = Common Class
    0x01,       // bDeviceProtocol = Interface Association Descriptor
    64,         // bMaxPacketSize0
    0x8A, 0x2E, // idVendor = 0x2E8A (Raspberry Pi)
    0x0A, 0x00, // idProduct = 0x000A (Pico CDC)
    0x00, 0x01, // bcdDevice
    1,          // iManufacturer
    2,          // iProduct
    3,          // iSerialNumber
    1           // bNumConfigurations
};

static const uint8_t g_usb_cfg_desc[] = {
    // Configuration Descriptor (Total Length: 141 bytes)
    9, 2, 141, 0, 4, 1, 0, 0xC0, 50,

    // IAD 0 (CDC ACM 0 - Console /dev/ttyACM0)
    8, 11, 0, 2, 2, 2, 1, 0,
    // Interface 0 (CDC Control)
    9, 4, 0, 0, 1, 2, 2, 1, 0,
    // Header Functional Desc
    5, 36, 0, 0x10, 0x01,
    // ACM Functional Desc
    4, 36, 2, 2,
    // Union Functional Desc
    5, 36, 6, 0, 1,
    // Call Management Desc
    5, 36, 1, 0, 1,
    // Endpoint 1 IN (Interrupt)
    7, 5, 0x81, 3, 16, 0, 16,

    // Interface 1 (CDC Data)
    9, 4, 1, 0, 2, 10, 0, 0, 0,
    // Endpoint 2 OUT (Bulk)
    7, 5, 0x02, 2, 64, 0, 0,
    // Endpoint 2 IN (Bulk)
    7, 5, 0x82, 2, 64, 0, 0,

    // IAD 1 (CDC ACM 1 - Network /dev/ttyACM1)
    8, 11, 2, 2, 2, 2, 1, 0,
    // Interface 2 (CDC Control)
    9, 4, 2, 0, 1, 2, 2, 1, 0,
    // Header Functional Desc
    5, 36, 0, 0x10, 0x01,
    // ACM Functional Desc
    4, 36, 2, 2,
    // Union Functional Desc
    5, 36, 6, 2, 3,
    // Call Management Desc
    5, 36, 1, 0, 3,
    // Endpoint 3 IN (Interrupt)
    7, 5, 0x83, 3, 16, 0, 16,

    // Interface 3 (CDC Data)
    9, 4, 3, 0, 2, 10, 0, 0, 0,
    // Endpoint 4 OUT (Bulk)
    7, 5, 0x04, 2, 64, 0, 0,
    // Endpoint 4 IN (Bulk)
    7, 5, 0x84, 2, 64, 0, 0
};

static const uint8_t g_usb_str_lang[] = { 4, 3, 0x09, 0x04 };
static const uint8_t g_usb_str_mfg[]  = { 16, 3, 'L',0,'u',0,'g',0,'a',0,'l',0,'O',0,'S',0 };
static const uint8_t g_usb_str_prod[] = { 44, 3, 'L',0,'u',0,'g',0,'a',0,'l',0,'O',0,'S',0,' ',0,'D',0,'u',0,'a',0,'l',0,' ',0,'C',0,'D',0,'C',0,' ',0,'A',0,'C',0,'M',0 };
static const uint8_t g_usb_line_coding[] = { 0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08 }; // 115200 8N1

static const uint8_t *g_ep0_tx_buf = NULL;
static volatile uint32_t g_ep0_tx_rem = 0;
static volatile uint32_t g_ep0_tx_off = 0;
static volatile bool g_ep0_tx_data_pid = 1; // DATA1 for 1st packet

static void ep0_send_next_chunk(void) {
    uint32_t chunk = g_ep0_tx_rem;
    if (chunk > 64) chunk = 64;

    volatile uint8_t *ep0_buf = (volatile uint8_t *)USB_EP0_BUF;
    for (uint32_t i = 0; i < chunk; i++) {
        ep0_buf[i] = g_ep0_tx_buf[g_ep0_tx_off + i];
    }

    g_ep0_tx_off += chunk;
    g_ep0_tx_rem -= chunk;

    uint32_t ctrl = (1u << 15) | (1u << 10) | chunk; // FULL | AVAIL | chunk
    if (g_ep0_tx_rem == 0) {
        ctrl |= (1u << 14); // LAST_BUFF on final chunk
    }
    if (g_ep0_tx_data_pid) {
        ctrl |= (1u << 13); // DATA1_PID
    }
    g_ep0_tx_data_pid = !g_ep0_tx_data_pid; // Toggle DATA1/DATA0 for next chunk

    ep_buf_ctrl_write((volatile uint32_t *)USB_EP0_IN_CTRL, ctrl);

    if (g_ep0_tx_rem == 0) {
        // Arm EP0 OUT immediately for the host's zero-length STATUS ack.
        // Deferring this until BUFF_STATUS confirms the IN completed was
        // tried before (see commit 8e0ca37) and reintroduced here (then
        // reverted again): under cooperative polling the host can time out
        // waiting ~5s for the status phase before we get back around to
        // arming it. Arming both together is what actually works.
        ep_buf_ctrl_write((volatile uint32_t *)USB_EP0_OUT_CTRL, (1u << 10) | (1u << 13) | 64);
    }
}

static void ep0_send(const uint8_t *buf, uint32_t len) {
    g_ep0_tx_buf = buf;
    g_ep0_tx_rem = len;
    g_ep0_tx_off = 0;
    g_ep0_tx_data_pid = 1; // 1st packet is always DATA1
    ep0_send_next_chunk();
}

static void ep0_send_ack(void) {
    g_ep0_tx_rem = 0;
    // Zero-length IN packet for Control transfer Status phase
    // FULL (bit 15) | LAST_BUFF (bit 14) | DATA1 (bit 13) | AVAIL (bit 10) | 0
    ep_buf_ctrl_write((volatile uint32_t *)USB_EP0_IN_CTRL, (1u << 15) | (1u << 14) | (1u << 13) | (1u << 10) | 0);
}

void usb_cdc_task(void) {
    if (!g_usb_cdc_connected) return;

    static volatile bool in_task = false;
    if (in_task) return;
    in_task = true;

    static uint32_t last_intr = 0;
    static uint32_t last_sie  = 0;

    uint32_t intr = REG(USB_INTR);
    uint32_t sie  = REG(USB_SIE_STATUS);
    uint32_t sie_filtered = sie & ~0x0000000Cu; // Filter LINE_STATE bits 2..3 oscillation

    if (intr != last_intr || sie_filtered != last_sie) {
        printk("[USB EVT] INTR=0x%08x SIE=0x%08x\n", intr, sie);
        last_intr = intr;
        last_sie  = sie_filtered;
    }

    // Clear latched event/error bits now that we've captured `sie` above,
    // so a bit set once doesn't make every future poll look like a fresh
    // event. Bus-reset/setup-rec detection below still uses the captured
    // `sie` value, not a re-read, so this is safe to do unconditionally.
    if (sie & USB_SIE_STATUS_EVENT_BITS) {
        REG(USB_SIE_STATUS) = sie & USB_SIE_STATUS_EVENT_BITS;
    }

    // Check BUS_RESET (bit 12 in USB_INTR or bit 19 in USB_SIE_STATUS)
    static bool bus_reset_handled = false;
    if ((intr & USB_INTR_BUS_RESET) || (sie & USB_SIE_STATUS_BUS_RESET)) {
        REG(USB_SIE_STATUS) = USB_SIE_STATUS_BUS_RESET; // Clear bit 19 in SIE_STATUS
        if (!bus_reset_handled) {
            REG(USB_ADDR_ENDP) = 0;
            g_usb_need_set_addr = false;
            g_ep0_tx_rem = 0;
            printk("[USB] Bus Reset Detected\n");
            bus_reset_handled = true;
        }
    } else {
        bus_reset_handled = false;
    }

    // USB_BUFF_STATUS has been observed to never latch nonzero on this
    // hardware/poll cadence even when SIE_STATUS proves a transfer
    // completed (TRANS_COMPLETE/ACK_REC), so it is only logged here, not
    // relied upon; SIE_STATUS bits drive all the real state transitions.
    uint32_t buf_status = REG(USB_BUFF_STATUS);
    if (buf_status) {
        printk("[USB BUFF] status=0x%08x\n", buf_status);
        REG(USB_BUFF_STATUS) = buf_status; // Write 1 to clear all completed buffer bits
    }

    // Continue a multi-packet EP0 IN transfer (e.g. the 141-byte config
    // descriptor) once the host has ACKed the chunk just sent. ACK_REC
    // fires per packet (unlike TRANS_COMPLETE, which only fires once the
    // whole control transfer's STATUS phase completes), so it's the right
    // per-chunk continuation signal here.
    if (g_ep0_tx_rem > 0 && (sie & USB_SIE_STATUS_ACK_REC)) {
        ep0_send_next_chunk();
    }

    // Apply a pending SET_ADDRESS once the STATUS-stage ack has genuinely
    // gone out, per USB 2.0 9.4.6 (must not switch address before the ack
    // is sent). SIE_STATUS TRANS_COMPLETE is used as that signal instead of
    // USB_BUFF_STATUS bit 0, which does not reliably reflect completion here.
    if (g_usb_need_set_addr && g_ep0_tx_rem == 0 && (sie & USB_SIE_STATUS_TRANS_COMPLETE)) {
        REG(USB_ADDR_ENDP) = g_usb_pending_addr;
        printk("[USB] Assigned Device Address: %d\n", g_usb_pending_addr);
        g_usb_need_set_addr = false;
    }

    // Check SETUP_REQ (bit 16 in USB_INTR or bit 17 in USB_SIE_STATUS)
    if ((intr & USB_INTR_SETUP_REQ) || (sie & USB_SIE_STATUS_SETUP_REC)) {
        REG(USB_SIE_STATUS) = USB_SIE_STATUS_SETUP_REC; // Clear bit 17 in SIE_STATUS

        volatile uint8_t *setup = (volatile uint8_t *)USB_EP0_SETUP;
        uint8_t req_type = setup[0];
        uint8_t req      = setup[1];
        uint16_t value   = (uint16_t)setup[2] | ((uint16_t)setup[3] << 8);
        uint16_t length  = (uint16_t)setup[6] | ((uint16_t)setup[7] << 8);

        printk("[USB SETUP] Type=0x%02x Req=0x%02x Val=0x%04x Len=%d\n", req_type, req, value, length);

        if ((req_type & 0x60) == 0x00) { // Standard Device Request
            switch (req) {
                case 0x06: { // GET_DESCRIPTOR
                    uint8_t desc_type = value >> 8;
                    uint8_t desc_idx  = value & 0xFF;
                    if (desc_type == 1) { // Device Desc
                        uint32_t send_len = sizeof(g_usb_dev_desc);
                        if (send_len > length) send_len = length;
                        ep0_send(g_usb_dev_desc, send_len);
                    } else if (desc_type == 2) { // Config Desc
                        uint32_t send_len = sizeof(g_usb_cfg_desc);
                        if (send_len > length) send_len = length;
                        ep0_send(g_usb_cfg_desc, send_len);
                    } else if (desc_type == 3) { // String Desc
                        if (desc_idx == 0) {
                            ep0_send(g_usb_str_lang, sizeof(g_usb_str_lang));
                        } else if (desc_idx == 1) {
                            ep0_send(g_usb_str_mfg, sizeof(g_usb_str_mfg));
                        } else if (desc_idx == 2) {
                            ep0_send(g_usb_str_prod, sizeof(g_usb_str_prod));
                        } else {
                            ep0_send_ack();
                        }
                    } else {
                        ep0_send_ack();
                    }
                    break;
                }
                case 0x05: // SET_ADDRESS
                    g_usb_pending_addr = value & 0x7F;
                    g_usb_need_set_addr = true;
                    ep0_send_ack();
                    break;
                case 0x09: // SET_CONFIGURATION
                    ep0_send_ack();
                    break;
                default:
                    ep0_send_ack();
                    break;
            }
        } else if ((req_type & 0x60) == 0x20) { // Class Request (CDC)
            if (req == 0x21) { // GET_LINE_CODING
                ep0_send(g_usb_line_coding, sizeof(g_usb_line_coding));
            } else {
                ep0_send_ack();
            }
        } else {
            ep0_send_ack();
        }
    }

    in_task = false;
}

void usb_cdc_init(void) {
    g_usb_cdc_connected = false;

    // 1. Initialize PLL_USB (48 MHz) according to Pico SDK pll_init
    REG(RESETS_RESET_SET) = RESET_PLL_USB_BIT;
    for (volatile int i = 0; i < 1000; i++);
    REG(RESETS_RESET_CLR) = RESET_PLL_USB_BIT;
    while (!(REG(RESETS_RESET_DONE) & RESET_PLL_USB_BIT));

    REG(0x40058000UL) = 1;   // RefDiv = 1
    REG(0x40058008UL) = 100; // FBDiv = 100
    REG(0x40058004UL + 0x3000) = (1u << 0) | (1u << 5); // Power up main PLL & VCO

    for (volatile int i = 0; i < 500000; i++) {
        if (REG(0x40058000UL) & (1u << 31)) break; // Wait for PLL_USB lock
    }

    REG(0x4005800CUL) = (5u << 16) | (5u << 12); // PostDiv1 = 5, PostDiv2 = 5 (48 MHz)
    REG(0x40058004UL + 0x3000) = (1u << 3); // Power up post dividers

    // 2. Select PLL_USB as CLK_USB source
    REG(CLK_USB_CTRL) = (1u << 11) | (0x0u << 5); // Enable CLK_USB from PLL_USB auxsrc
    REG(CLK_USB_DIV) = 0x00010000;

    // 3. Reset USB Controller
    REG(RESETS_RESET_SET) = RESET_USB_BIT;
    for (volatile int i = 0; i < 1000; i++);
    REG(RESETS_RESET_CLR) = RESET_USB_BIT;
    int timeout = 10000;
    while (!(REG(RESETS_RESET_DONE) & RESET_USB_BIT) && --timeout > 0);

    // 4. Enable USB PHY Muxing & Software Pullup Control (TO_PHY | SOFTCON)
    REG(USB_MUXING) = (1u << 0) | (1u << 3);

    // Force VBUS_DETECT in USB_PWR (Override bit 3 = 1, VBUS_DETECT bit 2 = 1)
    REG(USB_PWR) = (1u << 3) | (1u << 2);

    // 5. Enable Interrupt Flags in USB_INTE (0x90) (SETUP_REQ bit 16, BUS_RESET bit 12, BUFF_STATUS bit 4)
    REG(USB_INTE) = USB_INTR_SETUP_REQ | USB_INTR_BUS_RESET | USB_INTR_BUFF_STATUS;

    // 6. Enable SIE Controller in Device Mode with PHY_ISO active during SIE setup
    REG(USB_MAIN_CTRL) = (1u << 0) | (1u << 2); // Controller EN (bit 0) | PHY_ISO (bit 2)
    REG(USB_SIE_CTRL)  = (1u << 29) | (1u << 16); // EP0_INT_1BUF (bit 29) | PULLUP_EN (bit 16)

    // 7. Remove PHY Isolation (Clear PHY_ISO bit 2) now that SIE & D+ PULLUP are ready
    REG(USB_MAIN_CTRL) = (1u << 0);

    g_usb_cdc_connected = true;

    printk("[USB CDC Init] PLL_USB CS: 0x%08x (Lock: %d), CLK_USB_CTRL: 0x%08x, CLK_SELECTED: 0x%08x\n",
           REG(0x40058000UL), (REG(0x40058000UL) & (1u << 31)) ? 1 : 0,
           REG(CLK_USB_CTRL), REG(CLOCKS_BASE + 0x68));
    printk("[USB CDC Init] MUXING: 0x%08x, MAIN_CTRL: 0x%08x, SIE_CTRL: 0x%08x, SIE_STATUS: 0x%08x, INTE: 0x%08x\n",
           REG(USB_MUXING), REG(USB_MAIN_CTRL), REG(USB_SIE_CTRL), REG(USB_SIE_STATUS), REG(USB_INTE));

    // Process initial USB enumeration setup packets during boot
    for (volatile int i = 0; i < 500000; i++) {
        usb_cdc_task();
    }

    printk("[USB CDC] Native RP2350 Dual CDC ACM Controller Initialized (/dev/ttyACM0, /dev/ttyACM1).\n");
}

bool usb_cdc_is_connected(void) {
    return g_usb_cdc_connected;
}

void usb_cdc_putc(char c) {
    uart_putc(c);
}

char usb_cdc_getc(void) {
    return uart_getc();
}

int usb_cdc_write_net(const uint8_t *buf, size_t len) {
    if (!buf || len == 0) return 0;
    for (size_t i = 0; i < len; i++) {
        uart_putc((char)buf[i]);
    }
    return (int)len;
}

int usb_cdc_read_net(uint8_t *buf, size_t max_len) {
    if (!buf || max_len == 0) return 0;
    buf[0] = (uint8_t)uart_getc();
    return 1;
}

void usb_cdc_debug_dump(void) {
#if defined(CONFIG_BOARD_RP2350)
    printk("\n=== RP2350 USB Hardware Controller Status ===\n");
    printk("CLK_USB_CTRL     : 0x%08x\n", REG(CLK_USB_CTRL));
    printk("CLK_USB_SELECTED : 0x%08x\n", REG(CLOCKS_BASE + 0x68));
    printk("PLL_USB CS       : 0x%08x\n", REG(0x40058000UL));
    printk("USB_MAIN_CTRL    : 0x%08x\n", REG(USB_MAIN_CTRL));
    printk("USB_SIE_CTRL     : 0x%08x\n", REG(USB_SIE_CTRL));
    printk("USB_SIE_STATUS   : 0x%08x\n", REG(USB_SIE_STATUS));
    printk("USB_BUFF_STATUS  : 0x%08x\n", REG(USB_BUFF_STATUS));
    printk("USB_INTR (0x8C)  : 0x%08x\n", REG(USB_INTR));
    printk("USB_INTE (0x90)  : 0x%08x\n", REG(USB_INTE));
    printk("USB_INTS (0x98)  : 0x%08x\n", REG(USB_INTS));
    printk("USB_MUXING       : 0x%08x\n", REG(USB_MUXING));
    printk("USB_EP0_IN_CTRL  : 0x%08x\n", REG(USB_EP0_IN_CTRL));

    volatile uint8_t *s = (volatile uint8_t *)USB_EP0_SETUP;
    printk("EP0 Setup Bytes  : [%02x %02x %02x %02x %02x %02x %02x %02x]\n",
           s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
    printk("=============================================\n\n");
#else
    printk("[USB Debug] Host Pass-Through Mode (No hardware registers).\n");
#endif
}

#else

void usb_cdc_task(void) {}

void usb_cdc_init(void) {
    g_usb_cdc_connected = false;
    printk("[USB CDC] Host Pass-Through Gateway Online (/dev/ttyUSB0 / /dev/ttyACM1).\n");
}

bool usb_cdc_is_connected(void) {
    return false;
}

void usb_cdc_putc(char c) {
    uart_putc(c);
}

char usb_cdc_getc(void) {
    return uart_getc();
}

int usb_cdc_write_net(const uint8_t *buf, size_t len) {
    if (!buf || len == 0) return 0;
    for (size_t i = 0; i < len; i++) {
        uart_putc((char)buf[i]);
    }
    return (int)len;
}

int usb_cdc_read_net(uint8_t *buf, size_t max_len) {
    if (!buf || max_len == 0) return 0;
    buf[0] = (uint8_t)uart_getc();
    return 1;
}

void usb_cdc_debug_dump(void) {
    printk("[USB Debug] Host Pass-Through Mode (No hardware registers).\n");
}

#endif
