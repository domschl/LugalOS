#include "drivers/usb_cdc.h"
#include "kernel/irq.h"
#include "arch/rp2350_bootrom.h"
#include "kernel/time.h"
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
// NOTE: was (USB_BASE + 0x54) for a long time, which is actually
// USB_INT_EP_CTRL (an endpoint-interrupt config register we never write, so
// it always read back 0). That single wrong offset was the root cause of
// USB_BUFF_STATUS appearing to "never latch" during EP0 bring-up; verified
// against pico-sdk's hardware/regs/usb.h, BUFF_STATUS is at 0x58.
#define USB_BUFF_STATUS       (USB_BASE + 0x58)
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

// EP2 (Bulk, CDC ACM0 "Console" data interface -> /dev/ttyACM0). DPRAM
// layout per usb_dpram.h: endpoint-control regs for EP1..EP15 start at
// 0x08 (8 bytes/endpoint, IN then OUT); buffer-control regs for EP0..EP15
// start at 0x80 (8 bytes/endpoint); general-purpose endpoint data buffers
// are software-allocated starting at 0x180.
#define USB_EP2_IN_ENDP_CTRL  (USB_DPRAM_BASE + 0x10)
#define USB_EP2_OUT_ENDP_CTRL (USB_DPRAM_BASE + 0x14)
#define USB_EP2_IN_BUF_CTRL   (USB_DPRAM_BASE + 0x90)
#define USB_EP2_OUT_BUF_CTRL  (USB_DPRAM_BASE + 0x94)
#define USB_EP2_OUT_BUF       (USB_DPRAM_BASE + 0x180)
#define USB_EP2_IN_BUF        (USB_DPRAM_BASE + 0x1C0)

// EP4 (Bulk, CDC ACM1 "Network" data interface -> /dev/ttyACM1, A3b
// link_usb_cdc). Same DPRAM layout rules as EP2 above: endpoint-control at
// 0x08 + (n-1)*8 (n=4 -> 0x20/0x24), buffer-control at 0x80 + n*8 (n=4 ->
// 0xA0/0xA4), data buffers software-allocated past EP2's own (which end at
// 0x200) with no overlap.
#define USB_EP4_IN_ENDP_CTRL  (USB_DPRAM_BASE + 0x20)
#define USB_EP4_OUT_ENDP_CTRL (USB_DPRAM_BASE + 0x24)
#define USB_EP4_IN_BUF_CTRL   (USB_DPRAM_BASE + 0xA0)
#define USB_EP4_OUT_BUF_CTRL  (USB_DPRAM_BASE + 0xA4)
#define USB_EP4_OUT_BUF       (USB_DPRAM_BASE + 0x200)
#define USB_EP4_IN_BUF        (USB_DPRAM_BASE + 0x240)

#define EP_CTRL_ENABLE            (1u << 31)
#define EP_CTRL_INTERRUPT_PER_BUF (1u << 29)
#define EP_CTRL_TYPE_BULK         (2u << 26)

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
static const uint8_t g_usb_str_serial[] = { 26, 3, 'L',0,'U',0,'G',0,'A',0,'L',0,'O',0,'S',0,'-',0,'0',0,'0',0,'0',0,'1',0 }; // iSerialNumber = 3
static const uint8_t g_usb_line_coding[] = { 0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08 }; // 115200 8N1

static const uint8_t *g_ep0_tx_buf = NULL;
static volatile uint32_t g_ep0_tx_rem = 0;
static volatile uint32_t g_ep0_tx_off = 0;
static volatile bool g_ep0_tx_data_pid = 1; // DATA1 for 1st packet
static volatile bool g_ep0_awaiting_line_coding = false; // SET_LINE_CODING OUT data stage pending

/* --- "1200-baud touch" BOOTSEL reset ---
 *
 * The Arduino convention, also implemented by the Pico SDK: a host that opens
 * the CDC port at 1200 baud and then drops DTR is asking the *firmware* to
 * reboot into the bootloader. Nothing in USB or the silicon does this on its
 * own -- it only works because the device chooses to act on it, which is why
 * it did nothing here until now (this driver received SET_LINE_CODING and
 * discarded the rate).
 *
 * Gated on DTR going low rather than on the rate alone, so merely configuring
 * 1200 baud cannot reboot a board out from under a user; the reset is the
 * *close* of a 1200-baud connection. Deferred out of the control-transfer
 * handler so the STATUS stage is acknowledged first -- rebooting mid-transfer
 * leaves the host reporting a failed control request instead of a clean
 * detach. */
#define BOOTSEL_MAGIC_BAUD 1200
static volatile uint32_t g_line_coding_baud = 115200;
static volatile bool     g_bootsel_pending = false;
static volatile uint64_t g_bootsel_at_ms = 0;

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

// EP2 bulk console (interface 1 -> /dev/ttyACM0). Byte-ring buffers decouple
// usb_cdc_task()'s polling from usb_cdc_putc()/getc()'s call sites (the
// shell), which run independently and may call in between polls.
//
// TX is sized generously: shell command output (e.g. `ls`) is produced in
// one tight synchronous loop of uart_putc() calls with no usb_cdc_task()
// polls in between, so nothing drains the ring mid-burst — it all has to
// fit, or the tail of the output is silently dropped (usb_cdc_putc() drops
// on a full ring rather than blocking, to avoid a reentrancy deadlock:
// usb_cdc_task() guards against re-entry, and some of its own printk calls
// happen from inside it, so blocking there could spin forever waiting for
// a drain that a nested call can't perform).
#define USB_EP2_TX_RING_SIZE 4096
#define USB_EP2_RX_RING_SIZE 512

static volatile uint8_t g_ep2_tx_ring[USB_EP2_TX_RING_SIZE];
static volatile uint32_t g_ep2_tx_head = 0;
static volatile uint32_t g_ep2_tx_tail = 0;
static volatile bool g_ep2_tx_busy = false;
static volatile bool g_ep2_tx_pid = false; // DATA0 first, per USB 2.0 8.6

static volatile uint8_t g_ep2_rx_ring[USB_EP2_RX_RING_SIZE];
static volatile uint32_t g_ep2_rx_head = 0;
static volatile uint32_t g_ep2_rx_tail = 0;
static volatile bool g_ep2_rx_pid = false; // DATA0 first

static volatile bool g_ep2_configured = false;

// CDC ACM DTR (SET_CONTROL_LINE_STATE, wValue bit 0). This driver
// enumerates (and ep2_configure() runs) as soon as the RP2350 boots and the
// host's USB stack sees the device -- long before a user actually opens a
// terminal on /dev/ttyACM0. Without gating on DTR, every printk() during
// boot (FAT32/VFS/Lisp/shell init, all mirrored to EP2 via uart_putc())
// queues into g_ep2_tx_ring and gets transmitted regardless of whether
// anyone is listening; the Linux side just buffers it, and whenever a
// terminal finally does open the port, that whole backlog arrives in one
// burst -- exactly the "garbage on first connect, clean on reconnect"
// pattern seen in testing. Real terminal programs assert DTR on open, so
// gate queuing on it and flush any pre-DTR backlog the instant it's seen.
static volatile bool g_ep2_dtr = false;

static void ep2_configure(void) {
    // The connect handshake can involve more than one bus reset / repeated
    // SET_CONFIGURATION before the host settles (seen throughout this
    // session's logs). If a previous ep2_configure() left an EP2 IN
    // transfer in flight at the SIE level, blindly re-arming on top of it
    // here would clobber it mid-transaction and can desync the DATA0/DATA1
    // toggle from the host's own tracking for that endpoint -- which looks
    // exactly like "replies silently vanish forever" (the SIE still
    // completes transfers on our side, so our own busy/ring bookkeeping
    // looks fine, but the host discards packets it sees as out-of-sequence
    // duplicates). Force AVAILABLE off on both buffer-control registers
    // first so any stale in-flight transfer is cleanly abandoned.
    REG(USB_EP2_IN_BUF_CTRL)  = 0;
    REG(USB_EP2_OUT_BUF_CTRL) = 0;

    REG(USB_EP2_IN_ENDP_CTRL)  = EP_CTRL_ENABLE | EP_CTRL_INTERRUPT_PER_BUF | EP_CTRL_TYPE_BULK
                                | (uint32_t)(USB_EP2_IN_BUF - USB_DPRAM_BASE);
    REG(USB_EP2_OUT_ENDP_CTRL) = EP_CTRL_ENABLE | EP_CTRL_INTERRUPT_PER_BUF | EP_CTRL_TYPE_BULK
                                | (uint32_t)(USB_EP2_OUT_BUF - USB_DPRAM_BASE);

    g_ep2_tx_head = g_ep2_tx_tail = 0;
    g_ep2_rx_head = g_ep2_rx_tail = 0;
    g_ep2_tx_busy = false;
    g_ep2_tx_pid = false;
    g_ep2_rx_pid = false;
    g_ep2_dtr = false; // wait for the host to actually assert DTR before queuing anything

    // Arm EP2 OUT to receive the host's first packet (AVAIL | DATA0 | 64 max)
    ep_buf_ctrl_write((volatile uint32_t *)USB_EP2_OUT_BUF_CTRL, (1u << 10) | 64);

    g_ep2_configured = true;
    printk_debug("[USB] EP2 Bulk Console Endpoint Configured (Interface 1)\n");
}

// Copy any buffered TX bytes into an EP2 IN packet if the endpoint is free.
static void ep2_tx_pump(void) {
    if (!g_ep2_configured || g_ep2_tx_busy || g_ep2_tx_head == g_ep2_tx_tail) {
        return;
    }

    volatile uint8_t *txbuf = (volatile uint8_t *)USB_EP2_IN_BUF;
    uint32_t n = 0;
    while (n < 64 && g_ep2_tx_tail != g_ep2_tx_head) {
        txbuf[n++] = g_ep2_tx_ring[g_ep2_tx_tail];
        g_ep2_tx_tail = (g_ep2_tx_tail + 1) % USB_EP2_TX_RING_SIZE;
    }

    uint32_t ctrl = (1u << 15) | (1u << 14) | (1u << 10) | n; // FULL | LAST_BUFF | AVAIL | len
    if (g_ep2_tx_pid) ctrl |= (1u << 13); // DATA1_PID
    g_ep2_tx_pid = !g_ep2_tx_pid;
    g_ep2_tx_busy = true;

    ep_buf_ctrl_write((volatile uint32_t *)USB_EP2_IN_BUF_CTRL, ctrl);
}

// EP4 (ACM1 "Network" bulk data, A3b link_usb_cdc): same byte-ring
// mirroring EP2 uses to decouple usb_cdc_task()'s polling from producer/
// consumer call sites -- here that's usb_cdc_get_net_link()'s p9_link_t
// glue, further down, rather than the shell. No DTR gating (unlike EP2's
// g_ep2_dtr): DTR-gating exists there to avoid replaying boot-time printk()
// backlog to a terminal that opens the port late; this link never
// proactively sends anything until a 9P request arrives, so there's no
// backlog to avoid, and RX bytes buffered before a peer "opens" the port
// are harmless (the peer just hasn't asked for them yet).
#define USB_EP4_TX_RING_SIZE 4096
#define USB_EP4_RX_RING_SIZE 8192 // must exceed P9_MAX_MSIZE (4096) with headroom for an in-flight partial frame

static volatile uint8_t g_ep4_tx_ring[USB_EP4_TX_RING_SIZE];
static volatile uint32_t g_ep4_tx_head = 0;
static volatile uint32_t g_ep4_tx_tail = 0;
static volatile bool g_ep4_tx_busy = false;
static volatile bool g_ep4_tx_pid = false; // DATA0 first, per USB 2.0 8.6

static volatile uint8_t g_ep4_rx_ring[USB_EP4_RX_RING_SIZE];
static volatile uint32_t g_ep4_rx_head = 0;
static volatile uint32_t g_ep4_rx_tail = 0;
static volatile uint32_t g_ep4_rx_count = 0;
static volatile bool g_ep4_rx_pid = false; // DATA0 first

static volatile bool g_ep4_configured = false;

static void ep4_configure(void) {
    // Same in-flight-transfer-abandonment rationale as ep2_configure() above.
    REG(USB_EP4_IN_BUF_CTRL)  = 0;
    REG(USB_EP4_OUT_BUF_CTRL) = 0;

    REG(USB_EP4_IN_ENDP_CTRL)  = EP_CTRL_ENABLE | EP_CTRL_INTERRUPT_PER_BUF | EP_CTRL_TYPE_BULK
                                | (uint32_t)(USB_EP4_IN_BUF - USB_DPRAM_BASE);
    REG(USB_EP4_OUT_ENDP_CTRL) = EP_CTRL_ENABLE | EP_CTRL_INTERRUPT_PER_BUF | EP_CTRL_TYPE_BULK
                                | (uint32_t)(USB_EP4_OUT_BUF - USB_DPRAM_BASE);

    g_ep4_tx_head = g_ep4_tx_tail = 0;
    g_ep4_rx_head = g_ep4_rx_tail = g_ep4_rx_count = 0;
    g_ep4_tx_busy = false;
    g_ep4_tx_pid = false;
    g_ep4_rx_pid = false;

    // Arm EP4 OUT to receive the host's first packet (AVAIL | DATA0 | 64 max)
    ep_buf_ctrl_write((volatile uint32_t *)USB_EP4_OUT_BUF_CTRL, (1u << 10) | 64);

    g_ep4_configured = true;
    printk_debug("[USB] EP4 Bulk Network Endpoint Configured (Interface 3)\n");
}

// Copy any buffered TX bytes into an EP4 IN packet if the endpoint is free.
static void ep4_tx_pump(void) {
    if (!g_ep4_configured || g_ep4_tx_busy || g_ep4_tx_head == g_ep4_tx_tail) {
        return;
    }

    volatile uint8_t *txbuf = (volatile uint8_t *)USB_EP4_IN_BUF;
    uint32_t n = 0;
    while (n < 64 && g_ep4_tx_tail != g_ep4_tx_head) {
        txbuf[n++] = g_ep4_tx_ring[g_ep4_tx_tail];
        g_ep4_tx_tail = (g_ep4_tx_tail + 1) % USB_EP4_TX_RING_SIZE;
    }

    uint32_t ctrl = (1u << 15) | (1u << 14) | (1u << 10) | n; // FULL | LAST_BUFF | AVAIL | len
    if (g_ep4_tx_pid) ctrl |= (1u << 13); // DATA1_PID
    g_ep4_tx_pid = !g_ep4_tx_pid;
    g_ep4_tx_busy = true;

    ep_buf_ctrl_write((volatile uint32_t *)USB_EP4_IN_BUF_CTRL, ctrl);
}

void usb_cdc_task(void) {
    if (!g_usb_cdc_connected) return;

    /* Deferred BOOTSEL reset (see BOOTSEL_MAGIC_BAUD above). Executed here
     * rather than inside the control-transfer handler so the STATUS stage has
     * already been acknowledged and the host sees a clean detach instead of a
     * failed request. Checked before the re-entrancy guard because it never
     * returns -- there is nothing to guard. */
    if (g_bootsel_pending && time_get_ms() >= g_bootsel_at_ms) {
        g_bootsel_pending = false;
        printk("[USB] 1200-baud touch: rebooting into BOOTSEL for flashing\n");
        if (!rp2350_reboot_to_bootsel()) {
            /* Only reachable if the bootrom lookup failed, which would mean
             * the ROM table is not where the SDK says it is. Say so instead
             * of silently continuing as though the request never happened. */
            printk("[USB] BOOTSEL reset failed: bootrom reset_usb_boot not found\n");
        }
    }

    static volatile bool in_task = false;
    if (in_task) return;
    in_task = true;

    uint32_t intr = REG(USB_INTR);
    uint32_t sie  = REG(USB_SIE_STATUS);

    // NOTE: this used to also printk_debug() an "[USB EVT]" line on every
    // INTR/SIE_STATUS change, which was essential for diagnosing EP0
    // bring-up but became a self-feeding loop once EP2 console mirroring
    // was added: EP2 traffic changes SIE_STATUS (ACK_REC etc.), which
    // logged a line, which (via uart_putc's USB mirror) became more EP2
    // traffic, forever. Removed now that enumeration and EP2 both work.

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
            g_ep0_awaiting_line_coding = false;
            // Drop EP2 immediately rather than waiting for the next
            // SET_CONFIGURATION to call ep2_configure(): a stray poll in
            // between could otherwise act on now-stale busy/pid/ring state
            // left over from before this reset.
            g_ep2_configured = false;
            REG(USB_EP2_IN_BUF_CTRL)  = 0;
            REG(USB_EP2_OUT_BUF_CTRL) = 0;
            g_ep2_tx_head = g_ep2_tx_tail = 0;
            g_ep2_rx_head = g_ep2_rx_tail = 0;
            g_ep2_tx_busy = false;
            g_ep2_tx_pid = false;
            g_ep2_rx_pid = false;
            g_ep2_dtr = false;
            // Same reasoning for EP4 (ACM1 network endpoint, A3b).
            g_ep4_configured = false;
            REG(USB_EP4_IN_BUF_CTRL)  = 0;
            REG(USB_EP4_OUT_BUF_CTRL) = 0;
            g_ep4_tx_head = g_ep4_tx_tail = 0;
            g_ep4_rx_head = g_ep4_rx_tail = g_ep4_rx_count = 0;
            g_ep4_tx_busy = false;
            g_ep4_tx_pid = false;
            g_ep4_rx_pid = false;
            printk_debug("[USB] Bus Reset Detected\n");
            bus_reset_handled = true;
        }
    } else {
        bus_reset_handled = false;
    }

    // EP0's own state machine still runs off SIE_STATUS (see below) rather
    // than being switched back to BUFF_STATUS now that the offset is fixed,
    // since that path is proven working and there's no reason to risk
    // regressing it. BUFF_STATUS is used here for EP2 (bulk console),
    // which SIE_STATUS's global ACK_REC/TRANS_COMPLETE bits can't drive
    // since they don't say *which* endpoint completed.
    uint32_t buf_status = REG(USB_BUFF_STATUS);
    if (buf_status) {
        REG(USB_BUFF_STATUS) = buf_status; // Write 1 to clear all completed buffer bits

        if (buf_status & (1u << 1)) { // EP0 OUT complete
            if (g_ep0_awaiting_line_coding) {
                // The 7 line-coding bytes just landed in USB_EP0_BUF. Only
                // dwDTERate (the first 4, little-endian) is acted on, and only
                // to recognize the 1200-baud BOOTSEL touch -- the framing
                // fields still have no meaning for a USB CDC endpoint.
                const volatile uint8_t *lc = (const volatile uint8_t *)USB_EP0_BUF;
                g_line_coding_baud = (uint32_t)lc[0] | ((uint32_t)lc[1] << 8) |
                                     ((uint32_t)lc[2] << 16) | ((uint32_t)lc[3] << 24);
                g_ep0_awaiting_line_coding = false;
                ep0_send_ack();
            }
        }
        if (buf_status & (1u << 4)) { // EP2 IN complete
            g_ep2_tx_busy = false;
        }
        if (buf_status & (1u << 5)) { // EP2 OUT complete: data arrived from host
            uint32_t ctrl = REG(USB_EP2_OUT_BUF_CTRL);
            uint32_t len = ctrl & 0x3FFu;
            volatile uint8_t *rxbuf = (volatile uint8_t *)USB_EP2_OUT_BUF;
            for (uint32_t i = 0; i < len; i++) {
                uint32_t next = (g_ep2_rx_head + 1) % USB_EP2_RX_RING_SIZE;
                if (next != g_ep2_rx_tail) { // drop byte if the ring is full
                    g_ep2_rx_ring[g_ep2_rx_head] = rxbuf[i];
                    g_ep2_rx_head = next;
                }
            }
            g_ep2_rx_pid = !g_ep2_rx_pid;
            uint32_t rearm = (1u << 10) | 64; // AVAIL | 64 max
            if (g_ep2_rx_pid) rearm |= (1u << 13); // DATA1_PID
            ep_buf_ctrl_write((volatile uint32_t *)USB_EP2_OUT_BUF_CTRL, rearm);
        }
        if (buf_status & (1u << 8)) { // EP4 IN complete
            g_ep4_tx_busy = false;
        }
        if (buf_status & (1u << 9)) { // EP4 OUT complete: data arrived from host
            uint32_t ctrl = REG(USB_EP4_OUT_BUF_CTRL);
            uint32_t len = ctrl & 0x3FFu;
            volatile uint8_t *rxbuf = (volatile uint8_t *)USB_EP4_OUT_BUF;
            for (uint32_t i = 0; i < len; i++) {
                uint32_t next = (g_ep4_rx_head + 1) % USB_EP4_RX_RING_SIZE;
                if (next != g_ep4_rx_tail) { // drop byte if the ring is full
                    g_ep4_rx_ring[g_ep4_rx_head] = rxbuf[i];
                    g_ep4_rx_head = next;
                    g_ep4_rx_count++;
                }
            }
            g_ep4_rx_pid = !g_ep4_rx_pid;
            uint32_t rearm4 = (1u << 10) | 64; // AVAIL | 64 max
            if (g_ep4_rx_pid) rearm4 |= (1u << 13); // DATA1_PID
            ep_buf_ctrl_write((volatile uint32_t *)USB_EP4_OUT_BUF_CTRL, rearm4);
        }
    }

    ep2_tx_pump();
    ep4_tx_pump();

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
        printk_debug("[USB] Assigned Device Address: %d\n", g_usb_pending_addr);
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
                        } else if (desc_idx == 3) {
                            ep0_send(g_usb_str_serial, sizeof(g_usb_str_serial));
                        } else {
                            // Unsupported string index: reuse ep0_send()'s
                            // 0-length path rather than ep0_send_ack(). This
                            // is a GET_DESCRIPTOR (IN data stage) request;
                            // ep0_send_ack() sends a 0-byte IN packet but
                            // never arms EP0 OUT for the STATUS stage that
                            // must follow, so the host waits out its ~5s
                            // control-transfer timeout and retries (this was
                            // exactly what happened for iSerialNumber before
                            // it had a real string above). ep0_send(ptr, 0)
                            // sends the same 0-byte IN packet but correctly
                            // arms EP0 OUT afterward via ep0_send_next_chunk().
                            ep0_send(g_usb_str_lang, 0);
                        }
                    } else {
                        ep0_send(g_usb_dev_desc, 0); // unsupported descriptor type; see note above
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
                    if (value != 0) {
                        ep2_configure();
                        ep4_configure();
                    }
                    break;
                default:
                    ep0_send_ack();
                    break;
            }
        } else if ((req_type & 0x60) == 0x20) { // Class Request (CDC)
            if (req == 0x21) { // GET_LINE_CODING
                ep0_send(g_usb_line_coding, sizeof(g_usb_line_coding));
            } else if (req == 0x20 && length > 0) {
                // SET_LINE_CODING has a host->device DATA stage (7 bytes):
                // arm EP0 OUT to actually receive it instead of ACKing a
                // transfer whose data was never read. The STATUS ack is
                // sent from the BUFF_STATUS EP0-OUT-complete handler above
                // once that data genuinely arrives.
                g_ep0_awaiting_line_coding = true;
                ep_buf_ctrl_write((volatile uint32_t *)USB_EP0_OUT_CTRL, (1u << 10) | (1u << 13) | 64);
            } else if (req == 0x22) { // SET_CONTROL_LINE_STATE (wValue bit 0 = DTR)
                bool dtr_now = (value & 0x1) != 0;
                if (dtr_now && !g_ep2_dtr) {
                    // Terminal just opened the port: drop anything queued
                    // before it was listening (boot-time printk output that
                    // accumulated with nowhere to go) so it starts clean
                    // instead of receiving that backlog as one burst.
                    g_ep2_tx_head = g_ep2_tx_tail = 0;
                    g_ep2_rx_head = g_ep2_rx_tail = 0;
                    printk_debug("[USB] DTR asserted, EP2 console active\n");
                }
                if (!dtr_now && g_line_coding_baud == BOOTSEL_MAGIC_BAUD) {
                    // Host closed a 1200-baud connection: reboot to BOOTSEL.
                    // Deferred ~64 ms so the STATUS ack for this very request
                    // reaches the host first.
                    g_bootsel_pending = true;
                    g_bootsel_at_ms = time_get_ms() + 64;
                    printk_debug("[USB] 1200-baud touch: BOOTSEL reset armed\n");
                }
                if (!dtr_now && g_ep2_dtr) {
                    printk_debug("[USB] DTR dropped, EP2 console inactive\n");
                }
                g_ep2_dtr = dtr_now;
                ep0_send_ack();
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

    printk_debug("[USB CDC Init] PLL_USB CS: 0x%08x (Lock: %d), CLK_USB_CTRL: 0x%08x, CLK_SELECTED: 0x%08x\n",
           REG(0x40058000UL), (REG(0x40058000UL) & (1u << 31)) ? 1 : 0,
           REG(CLK_USB_CTRL), REG(CLOCKS_BASE + 0x68));
    printk_debug("[USB CDC Init] MUXING: 0x%08x, MAIN_CTRL: 0x%08x, SIE_CTRL: 0x%08x, SIE_STATUS: 0x%08x, INTE: 0x%08x\n",
           REG(USB_MUXING), REG(USB_MAIN_CTRL), REG(USB_SIE_CTRL), REG(USB_SIE_STATUS), REG(USB_INTE));

    // Process initial USB enumeration setup packets during boot
    for (volatile int i = 0; i < 500000; i++) {
        usb_cdc_task();
    }

    printk_debug("[USB CDC] Native RP2350 Dual CDC ACM Controller Initialized (/dev/ttyACM0, /dev/ttyACM1).\n");
}

bool usb_cdc_is_connected(void) {
    return g_usb_cdc_connected;
}

// Queues a byte for EP2 bulk IN (/dev/ttyACM0); ep2_tx_pump() in
// usb_cdc_task() drains the ring into actual USB packets. Silently drops
// on a full ring, before the host has configured the endpoint, or before a
// terminal has actually asserted DTR (see g_ep2_dtr) -- otherwise every
// printk() between enumeration and someone opening a terminal would queue
// up with nowhere to go and arrive as one stale burst when they finally do.
void usb_cdc_putc(char c) {
    if (!g_usb_cdc_connected || !g_ep2_configured || !g_ep2_dtr) return;
    // Read-modify-write on the ring head, so it has to be atomic against
    // preemption (B6). Without this, a task preempted between computing
    // `next` and storing it writes at a stale head and then rewinds the head
    // over everything the other task appended -- silently losing whole
    // messages rather than interleaving them. Observed on RP2350 hardware as
    // a user program whose output simply never appeared while the kernel's
    // did. printk() now masks around a whole message (kernel/printk.c), but
    // this is the lower-level guarantee and other callers reach it directly.
    uintptr_t flags = irq_save();
    uint32_t next = (g_ep2_tx_head + 1) % USB_EP2_TX_RING_SIZE;
    if (next != g_ep2_tx_tail) { // ring full; drop
        g_ep2_tx_ring[g_ep2_tx_head] = (uint8_t)c;
        g_ep2_tx_head = next;
    }
    irq_restore(flags);
}

bool usb_cdc_has_char(void) {
    return g_ep2_configured && g_ep2_rx_head != g_ep2_rx_tail;
}

char usb_cdc_getc(void) {
    if (g_ep2_rx_head == g_ep2_rx_tail) return 0;
    char c = (char)g_ep2_rx_ring[g_ep2_rx_tail];
    g_ep2_rx_tail = (g_ep2_rx_tail + 1) % USB_EP2_RX_RING_SIZE;
    return c;
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

/* --- A3b link_usb_cdc: p9_link_t glue over EP4's byte rings ---
 * Peek/pop helpers mirror drivers/virtio_console.c's own byte_ring_t
 * exactly (that file's rationale for the peek-before-pop split applies
 * unchanged here: the 4-byte length prefix has to be inspected before
 * committing to consuming a whole frame). g_ep4_rx_head/tail/count are already
 * the ring's state; these just wrap them for read-only inspection. */
static uint32_t ep4_rx_peek(uint8_t *out, uint32_t max) {
    uint32_t n = max < g_ep4_rx_count ? max : g_ep4_rx_count;
    uint32_t idx = g_ep4_rx_tail;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = g_ep4_rx_ring[idx];
        idx = (idx + 1) % USB_EP4_RX_RING_SIZE;
    }
    return n;
}

static uint32_t ep4_rx_pop(uint8_t *out, uint32_t max) {
    uint32_t n = ep4_rx_peek(out, max);
    g_ep4_rx_tail = (g_ep4_rx_tail + n) % USB_EP4_RX_RING_SIZE;
    g_ep4_rx_count -= n;
    return n;
}

static int ep4_link_poll(p9_link_t *link) {
    (void)link;
    if (g_ep4_rx_count < 4) return 0;
    uint8_t hdr[4];
    ep4_rx_peek(hdr, 4);
    uint32_t declared = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (declared < 7 || declared > USB_EP4_RX_RING_SIZE) return -1; // corrupt stream
    return (g_ep4_rx_count >= declared) ? 1 : 0;
}

static int ep4_link_recv_frame(p9_link_t *link, uint8_t *buf, uint32_t max_len) {
    (void)link;
    if (g_ep4_rx_count < 4) return -1;
    uint8_t hdr[4];
    ep4_rx_peek(hdr, 4);
    uint32_t declared = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (declared < 7 || declared > max_len || g_ep4_rx_count < declared) return -1;
    return (int)ep4_rx_pop(buf, declared);
}

/* Blocking, like drivers/virtio_console.c's virtio_console_send(): queues
 * the whole frame into the TX ring (pumping usb_cdc_task() if it's briefly
 * full) and then waits for the ring to fully drain out over the wire before
 * returning, so the caller can reuse/free `buf` immediately afterward. */
static int ep4_link_send_frame(p9_link_t *link, const uint8_t *buf, uint32_t len) {
    (void)link;
    if (!g_ep4_configured || !buf || len == 0) return -1;

    uint32_t sent = 0;
    while (sent < len) {
        uint32_t next = (g_ep4_tx_head + 1) % USB_EP4_TX_RING_SIZE;
        if (next == g_ep4_tx_tail) {
            usb_cdc_task();
            continue;
        }
        g_ep4_tx_ring[g_ep4_tx_head] = buf[sent++];
        g_ep4_tx_head = next;
    }
    while (g_ep4_tx_head != g_ep4_tx_tail || g_ep4_tx_busy) {
        usb_cdc_task();
    }
    return (int)len;
}

static p9_link_t g_ep4_link = {
    .name = "usb-cdc-net",
    .poll = ep4_link_poll,
    .send_frame = ep4_link_send_frame,
    .recv_frame = ep4_link_recv_frame,
    .ctx = NULL,
};

p9_link_t *usb_cdc_get_net_link(void) {
    return g_usb_cdc_connected ? &g_ep4_link : NULL;
}

void usb_cdc_debug_dump(void) {
#if defined(CONFIG_BOARD_RP2350)
    printk_debug("\n=== RP2350 USB Hardware Controller Status ===\n");
    printk_debug("CLK_USB_CTRL     : 0x%08x\n", REG(CLK_USB_CTRL));
    printk_debug("CLK_USB_SELECTED : 0x%08x\n", REG(CLOCKS_BASE + 0x68));
    printk_debug("PLL_USB CS       : 0x%08x\n", REG(0x40058000UL));
    printk_debug("USB_MAIN_CTRL    : 0x%08x\n", REG(USB_MAIN_CTRL));
    printk_debug("USB_SIE_CTRL     : 0x%08x\n", REG(USB_SIE_CTRL));
    printk_debug("USB_SIE_STATUS   : 0x%08x\n", REG(USB_SIE_STATUS));
    printk_debug("USB_BUFF_STATUS  : 0x%08x\n", REG(USB_BUFF_STATUS));
    printk_debug("USB_INTR (0x8C)  : 0x%08x\n", REG(USB_INTR));
    printk_debug("USB_INTE (0x90)  : 0x%08x\n", REG(USB_INTE));
    printk_debug("USB_INTS (0x98)  : 0x%08x\n", REG(USB_INTS));
    printk_debug("USB_MUXING       : 0x%08x\n", REG(USB_MUXING));
    printk_debug("USB_EP0_IN_CTRL  : 0x%08x\n", REG(USB_EP0_IN_CTRL));

    volatile uint8_t *s = (volatile uint8_t *)USB_EP0_SETUP;
    printk_debug("EP0 Setup Bytes  : [%02x %02x %02x %02x %02x %02x %02x %02x]\n",
           s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
    printk_debug("=============================================\n\n");
#else
    printk_debug("[USB Debug] Host Pass-Through Mode (No hardware registers).\n");
#endif
}

#else

void usb_cdc_task(void) {}

void usb_cdc_init(void) {
    g_usb_cdc_connected = false;
    printk_debug("[USB CDC] Host Pass-Through Gateway Online (/dev/ttyUSB0 / /dev/ttyACM1).\n");
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

bool usb_cdc_has_char(void) {
    return false;
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

// No second CDC interface exists off RP2350 hardware.
p9_link_t *usb_cdc_get_net_link(void) {
    return NULL;
}

void usb_cdc_debug_dump(void) {
    printk_debug("[USB Debug] Host Pass-Through Mode (No hardware registers).\n");
}

#endif
