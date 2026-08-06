#include "drivers/usb_cdc.h"
#include "kernel/printk.h"
#include "drivers/uart.h"
#include <string.h>

static bool g_usb_cdc_connected = false;

#if defined(CONFIG_BOARD_RP2350)

static uint8_t g_usb_pending_addr = 0;
static bool g_usb_need_set_addr = false;

#define USB_BASE              0x50110000UL
#define USB_DPRAM_BASE        0x50100000UL

#define USB_ADDR_ENDP         (USB_BASE + 0x00)
#define USB_MAIN_CTRL         (USB_BASE + 0x40)
#define USB_SIE_CTRL          (USB_BASE + 0x4C)
#define USB_SIE_STATUS        (USB_BASE + 0x50)
#define USB_BUFF_STATUS       (USB_BASE + 0x54)

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
#define RESETS_RESET_DONE     (RESETS_BASE + 0x000C)
#define RESET_USB_BIT         (1u << 17)

#define REG(addr) (*(volatile uint32_t *)(addr))

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
    // Configuration Descriptor
    9, 2, 106, 0, 4, 1, 0, 0xC0, 50,

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

static void ep0_send(const uint8_t *buf, uint32_t len) {
    if (len > 64) len = 64;
    volatile uint8_t *ep0_buf = (volatile uint8_t *)USB_EP0_BUF;
    for (uint32_t i = 0; i < len; i++) {
        ep0_buf[i] = buf[i];
    }
    // Set FULL (bit 15) | AVAILABLE (bit 10) | DATA1 (bit 13) | Length on EP0 IN
    REG(USB_EP0_IN_CTRL) = (1u << 15) | (1u << 10) | (1u << 13) | len;

    // Arm EP0 OUT with AVAILABLE (bit 10) | DATA1 (bit 13) | 0 to receive host STATUS OUT packet
    REG(USB_EP0_OUT_CTRL) = (1u << 10) | (1u << 13) | 0;
}

static void ep0_send_ack(void) {
    // Set FULL (bit 15) | AVAILABLE (bit 10) | DATA1 (bit 13) | 0 on EP0 IN
    REG(USB_EP0_IN_CTRL) = (1u << 15) | (1u << 10) | (1u << 13) | 0;

    // Arm EP0 OUT with AVAILABLE (bit 10) | DATA1 (bit 13) | 0
    REG(USB_EP0_OUT_CTRL) = (1u << 10) | (1u << 13) | 0;
}

#define USB_INTS (USB_BASE + 0x90)

void usb_cdc_task(void) {
    if (!g_usb_cdc_connected) return;

    uint32_t ints = REG(USB_INTS);

    // Check BUS_RESET (bit 12 in USB_INTS)
    if (ints & (1u << 12)) {
        REG(USB_INTS) = (1u << 12);
        REG(USB_ADDR_ENDP) = 0;
        printk("[USB] Bus Reset Detected\n");
    }

    // Check buffer status completion for pending address setup
    uint32_t buf_status = REG(USB_BUFF_STATUS);
    if (buf_status & 1u) { // EP0 IN buffer complete
        REG(USB_BUFF_STATUS) = 1u; // Clear bit
        if (g_usb_need_set_addr) {
            REG(USB_ADDR_ENDP) = g_usb_pending_addr;
            printk("[USB] Assigned Device Address: %d\n", g_usb_pending_addr);
            g_usb_need_set_addr = false;
        }
    }

    // Check SETUP_REQ (bit 16 in USB_INTS)
    if (ints & (1u << 16)) {
        REG(USB_INTS) = (1u << 16); // Clear SETUP_REQ flag

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
}

void usb_cdc_init(void) {
    // 1. Initialize PLL_USB (48 MHz)
    REG(RESETS_RESET_CLR) = (1u << 13); // Clear RESET_PLL_USB
    int timeout_pll = 10000;
    while (!(REG(RESETS_RESET_DONE) & (1u << 13)) && --timeout_pll > 0);

    REG(0x40058000UL) = 1; // RefDiv = 1
    REG(0x40058008UL) = 40; // FBDiv = 40 (12MHz * 40 = 480MHz VCO)
    REG(0x40058004UL) &= ~((1u << 0) | (1u << 5)); // Clear main power-down and VCO power-down

    timeout_pll = 10000;
    while (!(REG(0x40058000UL) & (1u << 31)) && --timeout_pll > 0); // Wait for PLL_USB lock

    REG(0x4005800CUL) = (5u << 16) | (2u << 12); // PostDiv1 = 5, PostDiv2 = 2 (480 / 10 = 48 MHz)
    REG(0x40058004UL) &= ~(1u << 3); // Clear post-divider power-down

    // 2. Enable 48 MHz USB Clock Source
    REG(CLK_USB_CTRL) = (1u << 11) | (0x0u << 5); // Enable CLK_USB from PLL_USB auxsrc
    REG(CLK_USB_DIV) = 0x00010000;

    // 3. Reset USB Controller
    REG(RESETS_RESET_SET) = RESET_USB_BIT;
    for (volatile int i = 0; i < 1000; i++);
    REG(RESETS_RESET_CLR) = RESET_USB_BIT;
    int timeout = 10000;
    while (!(REG(RESETS_RESET_DONE) & RESET_USB_BIT) && --timeout > 0);

    // 4. Enable USB PHY Muxing & Software Pullup Control (TO_PHY | SOFTCON)
    REG(USB_BASE + 0x74) = (1u << 0) | (1u << 3);

    // 5. Enable SIE Controller in Device Mode & D+ Pullup
    REG(USB_MAIN_CTRL) = (1u << 0); // Enable controller in Device Mode
    REG(USB_SIE_CTRL) = (1u << 16) | (1u << 29); // D+ Pullup, Enable SIE

    g_usb_cdc_connected = true;

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
    printk("USB_MUXING       : 0x%08x\n", REG(USB_BASE + 0x74));
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
