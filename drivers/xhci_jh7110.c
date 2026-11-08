/*
 * LugalOS Hardware Driver: VIA Labs VL805 USB 3.0 xHCI Host Controller
 *
 * Drives the onboard VIA VL805-Q6 xHCI 1.0 controller on the StarFive
 * VisionFive 2 v1.3B, connected via PCIe Port 0 (Bus 01:00.0, 1106:3483).
 *
 * Implements Host Controller reset, BIOS/OS handoff, DCBAA and Command/Event
 * ring initialization, Root Hub port power and status monitoring for the
 * four USB Type-A ports (J11, J12), USB 2.0 Hub class support, device slot
 * allocation, USB address assignment, descriptor fetching, and USB HID
 * boot keyboard and mouse driver integration.
 */

#include "drivers/xhci.h"
#include "drivers/pcie_jh7110.h"
#include "drivers/usbkbd.h"
#include "kernel/printk.h"
#include "kernel/console.h"
#include "kernel/event.h"
#include "kernel/time.h"
#include "kernel/lock.h"
#include <string.h>

#define REG8(addr)  (*(volatile uint8_t  *)(uintptr_t)(addr))
#define REG16(addr) (*(volatile uint16_t *)(uintptr_t)(addr))
#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#define REG64(addr) (*(volatile uint64_t *)(uintptr_t)(addr))

/* SiFive L2 Cache Controller Flush64 register (JH7110 non-coherent DMA) */
#define SIFIVE_L2_BASE      0x02010000UL
#define SIFIVE_L2_FLUSH64   0x200UL

static inline void xhci_mb(void) {
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

static inline void xhci_flush_l2_range(uintptr_t pa_start, uintptr_t pa_end) {
    volatile uint64_t *flush64 = (volatile uint64_t *)(SIFIVE_L2_BASE + SIFIVE_L2_FLUSH64);
    xhci_mb();
    uintptr_t start = pa_start & ~(64UL - 1UL);
    for (uintptr_t line = start; line < pa_end; line += 64UL) {
        *flush64 = line;
        xhci_mb();
    }
}

#define XHCI_MAX_SLOTS      8u
#define XHCI_CMD_RING_SIZE  64u
#define XHCI_EVENT_RING_SZ  64u
#define XHCI_EP_RING_SIZE   32u
#define XHCI_MAX_SCRATCHPAD 32u
#define KBD_QUEUE_SIZE      64u

/* Static DMA Ring Buffers and Contexts (aligned in .bss) */
/* guarded by: g_xhci_lock for controller DMA and register access */
static uint64_t          g_dcbaa[XHCI_MAX_SLOTS + 1] __attribute__((aligned(64)));
/* guarded by: g_xhci_lock */
static xhci_trb_t        g_cmd_ring[XHCI_CMD_RING_SIZE] __attribute__((aligned(64)));
/* guarded by: g_xhci_lock */
static xhci_trb_t        g_event_ring[XHCI_EVENT_RING_SZ] __attribute__((aligned(64)));
/* guarded by: g_xhci_lock */
static xhci_erst_entry_t g_erst[1] __attribute__((aligned(64)));
/* guarded by: g_xhci_lock */
static uint64_t          g_scratchpad_ptrs[XHCI_MAX_SCRATCHPAD] __attribute__((aligned(64)));
/* guarded by: g_xhci_lock */
static uint8_t           g_scratchpad_pages[XHCI_MAX_SCRATCHPAD][4096] __attribute__((aligned(4096)));

/* EP0 Transfer Rings per slot */
/* guarded by: g_xhci_lock */
static xhci_trb_t        g_ep0_ring[XHCI_MAX_SLOTS + 1][XHCI_EP_RING_SIZE] __attribute__((aligned(64)));
/* guarded by: g_xhci_lock */
static uint8_t           g_ep0_cycle[XHCI_MAX_SLOTS + 1];
/* guarded by: g_xhci_lock */
static uint8_t           g_ep0_enqueue[XHCI_MAX_SLOTS + 1];

/* Dedicated Transfer Rings and buffers for HID Keyboard and Mouse */
/* guarded by: g_xhci_lock */
static xhci_trb_t        g_kbd_ep_ring[XHCI_EP_RING_SIZE] __attribute__((aligned(64)));
/* guarded by: g_xhci_lock */
static uint8_t           g_kbd_report_buf[16] __attribute__((aligned(64)));
/* guarded by: g_xhci_lock */
static uint8_t           g_kbd_cycle = 1;
/* guarded by: g_xhci_lock */
static uint8_t           g_kbd_enqueue = 0;
/* guarded by: g_xhci_lock */
static uint8_t           g_kbd_slot = 0;
/* guarded by: g_xhci_lock */
static uint8_t           g_kbd_dci = 0;
/* guarded by: g_xhci_lock */
static uint8_t           g_kbd_prev_report[8];
/* guarded by: g_xhci_lock */
static usbkbd_xlate_t    g_kbd_xl;
/* guarded by: g_xhci_lock */
static uint8_t           g_kbd_queue[KBD_QUEUE_SIZE];
/* guarded by: g_xhci_lock */
static uint32_t          g_kbd_head = 0;
/* guarded by: g_xhci_lock */
static uint32_t          g_kbd_tail = 0;
/* guarded by: g_xhci_lock */
static bool              g_kbd_online = false;

/* guarded by: g_xhci_lock */
static xhci_trb_t        g_mouse_ep_ring[XHCI_EP_RING_SIZE] __attribute__((aligned(64)));
/* guarded by: g_xhci_lock */
static uint8_t           g_mouse_report_buf[16] __attribute__((aligned(64)));
/* guarded by: g_xhci_lock */
static uint8_t           g_mouse_cycle = 1;
/* guarded by: g_xhci_lock */
static uint8_t           g_mouse_enqueue = 0;
/* guarded by: g_xhci_lock */
static uint8_t           g_mouse_slot = 0;
/* guarded by: g_xhci_lock */
static uint8_t           g_mouse_dci = 0;
/* guarded by: g_xhci_lock */
static int               g_mouse_x = 512;
/* guarded by: g_xhci_lock */
static int               g_mouse_y = 300;
/* guarded by: g_xhci_lock */
static uint8_t           g_mouse_buttons = 0;
/* guarded by: g_xhci_lock */
static int               g_mouse_wheel = 0;
/* guarded by: g_xhci_lock */
static uint32_t          g_mouse_last_ms = 0;
/* guarded by: g_xhci_lock */
static bool              g_mouse_online = false;

/* Device and Input Contexts (2048 bytes per slot, enough for 64-byte context size) */
/* guarded by: g_xhci_lock */
static uint8_t           g_dev_ctx[XHCI_MAX_SLOTS + 1][2048] __attribute__((aligned(64)));
/* guarded by: g_xhci_lock */
static uint8_t           g_input_ctx[2048] __attribute__((aligned(64)));

/* Scratch DMA buffer for Control transfers */
/* guarded by: g_xhci_lock */
static uint8_t           g_ctrl_buf[1024] __attribute__((aligned(64)));

/* Controller State */
/* guarded by: g_xhci_lock for hardware state and port updates */
static ylock_t           g_xhci_lock;
/* guarded by: g_xhci_lock */
static uintptr_t         g_xhci_bar0 = 0;
/* guarded by: g_xhci_lock */
static uintptr_t         g_xhci_op_base = 0;
/* guarded by: g_xhci_lock */
static uintptr_t         g_xhci_rt_base = 0;
/* guarded by: g_xhci_lock */
static uintptr_t         g_xhci_db_base = 0;
/* guarded by: g_xhci_lock */
static uint16_t          g_hci_version = 0;
/* guarded by: g_xhci_lock */
static uint8_t           g_max_slots = 0;
/* guarded by: g_xhci_lock */
static uint8_t           g_max_ports = 0;
/* guarded by: g_xhci_lock */
static uint16_t          g_max_scratchpad = 0;
/* guarded by: g_xhci_lock */
static uint8_t           g_context_size = 32; /* 32 or 64 bytes */
/* guarded by: g_xhci_lock */
static uint8_t           g_cmd_enqueue = 0;
/* guarded by: g_xhci_lock */
static uint8_t           g_cmd_cycle = 1;
/* guarded by: g_xhci_lock */
static uint8_t           g_event_dequeue = 0;
/* guarded by: g_xhci_lock */
static uint8_t           g_event_cycle = 1;
/* guarded by: g_xhci_lock */
static bool              g_xhci_present = false;

/* Enumerated Devices (1..XHCI_MAX_SLOTS) */
/* guarded by: g_xhci_lock */
static xhci_device_t     g_devices[XHCI_MAX_SLOTS + 1];

/* Forward declarations */
static int xhci_setup_device(uint8_t slot_id, uint8_t root_port, uint8_t parent_hub_slot,
                             uint8_t parent_hub_port, uint8_t speed);
static void xhci_enumerate_hub(uint8_t hub_slot, uint8_t root_port);
static int xhci_control_transfer(uint8_t slot_id, uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                 void *data, bool in_direction);

static inline void ring_doorbell(uint32_t target, uint32_t slot) {
    REG32(g_xhci_db_base + slot * 4u) = target;
}

static void xhci_process_transfer_event(uint8_t slot_id, uint8_t dci, uint8_t cc) {
    (void)cc;
    if (slot_id == 0 || slot_id > XHCI_MAX_SLOTS) return;
    xhci_device_t *dev = &g_devices[slot_id];
    if (!dev->present) return;

    if (slot_id == g_kbd_slot && dci == g_kbd_dci && g_kbd_online) {
        xhci_flush_l2_range((uintptr_t)g_kbd_report_buf,
                            (uintptr_t)g_kbd_report_buf + 16);
        uint32_t ev_list[16];
        int m = usbkbd_diff(g_kbd_prev_report, g_kbd_report_buf, ev_list, 16);
        if (m > 0) {
            uint32_t now = (uint32_t)time_get_ms();
            for (int i = 0; i < m; i++) {
                uint8_t out[16];
                uint32_t n = usbkbd_translate(&g_kbd_xl, ev_list[i], now, out);
                for (uint32_t j = 0; j < n; j++) {
                    if (g_kbd_head - g_kbd_tail < KBD_QUEUE_SIZE) {
                        g_kbd_queue[g_kbd_head % KBD_QUEUE_SIZE] = out[j];
                        g_kbd_head++;
                    }
                }
                if (g_kbd_xl.hotkey) {
                    console_hotkey(g_kbd_xl.hotkey);
                    g_kbd_xl.hotkey = 0;
                }
            }
            memcpy(g_kbd_prev_report, g_kbd_report_buf, 8);
        }

        /* Re-queue next Normal TRB on Keyboard ring */
        uint8_t qidx = g_kbd_enqueue;
        uint8_t cycle = g_kbd_cycle;
        memset(g_kbd_report_buf, 0, 16);
        xhci_flush_l2_range((uintptr_t)g_kbd_report_buf,
                            (uintptr_t)g_kbd_report_buf + 16);

        g_kbd_ep_ring[qidx].parameter = (uintptr_t)g_kbd_report_buf;
        g_kbd_ep_ring[qidx].status = dev->ep_in_mps ? dev->ep_in_mps : 8;
        g_kbd_ep_ring[qidx].control = (XHCI_TRB_NORMAL << 10) | (1u << 5) | cycle;
        xhci_flush_l2_range((uintptr_t)&g_kbd_ep_ring[qidx],
                            (uintptr_t)(&g_kbd_ep_ring[qidx] + 1));

        qidx++;
        if (qidx == XHCI_EP_RING_SIZE - 1u) {
            qidx = 0;
            g_kbd_cycle ^= 1u;
        }
        g_kbd_enqueue = qidx;

        ring_doorbell(dci, slot_id);
    } else if (slot_id == g_mouse_slot && dci == g_mouse_dci && g_mouse_online) {
        xhci_flush_l2_range((uintptr_t)g_mouse_report_buf,
                            (uintptr_t)g_mouse_report_buf + 16);
        uint8_t buttons = g_mouse_report_buf[0];
        int8_t dx = (int8_t)g_mouse_report_buf[1];
        int8_t dy = (int8_t)g_mouse_report_buf[2];
        int8_t wheel = (dev->ep_in_mps >= 4) ? (int8_t)g_mouse_report_buf[3] : 0;

        g_mouse_x += dx;
        g_mouse_y += dy;
        if (g_mouse_x < 0) g_mouse_x = 0;
        if (g_mouse_x > 1919) g_mouse_x = 1919;
        if (g_mouse_y < 0) g_mouse_y = 0;
        if (g_mouse_y > 1079) g_mouse_y = 1079;
        g_mouse_buttons = buttons;
        g_mouse_wheel += wheel;
        g_mouse_last_ms = (uint32_t)time_get_ms();

        /* Re-queue next Normal TRB on Mouse ring */
        uint8_t qidx = g_mouse_enqueue;
        uint8_t cycle = g_mouse_cycle;
        memset(g_mouse_report_buf, 0, 16);
        xhci_flush_l2_range((uintptr_t)g_mouse_report_buf,
                            (uintptr_t)g_mouse_report_buf + 16);

        g_mouse_ep_ring[qidx].parameter = (uintptr_t)g_mouse_report_buf;
        g_mouse_ep_ring[qidx].status = dev->ep_in_mps ? dev->ep_in_mps : 4;
        g_mouse_ep_ring[qidx].control = (XHCI_TRB_NORMAL << 10) | (1u << 5) | cycle;
        xhci_flush_l2_range((uintptr_t)&g_mouse_ep_ring[qidx],
                            (uintptr_t)(&g_mouse_ep_ring[qidx] + 1));

        qidx++;
        if (qidx == XHCI_EP_RING_SIZE - 1u) {
            qidx = 0;
            g_mouse_cycle ^= 1u;
        }
        g_mouse_enqueue = qidx;

        ring_doorbell(dci, slot_id);
    }
}

static int xhci_wait_event(uint8_t trb_type, xhci_trb_t *out_trb, uint32_t timeout_ms) {
    uint64_t deadline = time_get_us() + (uint64_t)timeout_ms * 1000ULL;
    do {
        volatile xhci_trb_t *trb = &g_event_ring[g_event_dequeue];
        xhci_flush_l2_range((uintptr_t)trb, (uintptr_t)(trb + 1));

        uint8_t cycle = trb->control & 1u;
        if (cycle == g_event_cycle) {
            uint8_t type = (trb->control >> 10) & 0x3Fu;
            uint8_t slot = (trb->control >> 24) & 0xFFu;
            uint8_t dci  = (trb->control >> 16) & 0x1Fu;
            uint8_t cc   = (trb->status >> 24) & 0xFFu;

            /* Advance dequeue pointer */
            g_event_dequeue++;
            if (g_event_dequeue == XHCI_EVENT_RING_SZ) {
                g_event_dequeue = 0;
                g_event_cycle ^= 1u;
            }

            /* Update ERDP in Runtime register */
            uintptr_t erdp = (uintptr_t)&g_event_ring[g_event_dequeue] | (1u << 3); /* EHB = 1 */
            REG32(g_xhci_rt_base + 0x20 + 0x18) = (uint32_t)erdp;
            REG32(g_xhci_rt_base + 0x20 + 0x1C) = (uint32_t)(erdp >> 32);

            /* If it's a Transfer Event on an Interrupt Endpoint (DCI > 1), handle it */
            if (type == XHCI_TRB_TRANSFER_EVENT && dci > 1) {
                xhci_process_transfer_event(slot, dci, cc);
                if (trb_type == XHCI_TRB_TRANSFER_EVENT && out_trb) {
                    /* Control transfer caller only wants DCI 1 */
                    continue;
                }
            }

            if (trb_type == 0 || type == trb_type) {
                if (out_trb) {
                    memcpy(out_trb, (const void *)trb, sizeof(xhci_trb_t));
                }
                return 0;
            }
        }
        if (timeout_ms > 0) {
            time_delay_us(10);
        }
    } while (time_get_us() < deadline);

    return -1;
}

static int xhci_submit_cmd(xhci_trb_t *cmd, xhci_trb_t *out_event) {
    uint8_t idx = g_cmd_enqueue;
    uint32_t ctrl = (cmd->control & ~1u) | g_cmd_cycle;

    g_cmd_ring[idx].parameter = cmd->parameter;
    g_cmd_ring[idx].status = cmd->status;
    g_cmd_ring[idx].control = ctrl;

    xhci_flush_l2_range((uintptr_t)&g_cmd_ring[idx], (uintptr_t)(&g_cmd_ring[idx] + 1));

    idx++;
    if (idx == XHCI_CMD_RING_SIZE - 1u) {
        /* Last entry is Link TRB */
        g_cmd_ring[idx].control = (XHCI_TRB_LINK << 10) | (1u << 1) | g_cmd_cycle; /* TC=1 */
        xhci_flush_l2_range((uintptr_t)&g_cmd_ring[idx], (uintptr_t)(&g_cmd_ring[idx] + 1));
        idx = 0;
        g_cmd_cycle ^= 1u;
    }
    g_cmd_enqueue = idx;

    /* Ring Host Controller Doorbell */
    ring_doorbell(0, 0);

    /* Wait for Command Completion Event */
    return xhci_wait_event(XHCI_TRB_CMD_COMPLETION, out_event, 1000);
}

/* Perform standard USB control transfer on Endpoint 0 of a given slot */
static void xhci_ep0_push_trb(uint8_t slot_id, uint64_t param, uint32_t status, uint32_t control) {
    uint8_t idx = g_ep0_enqueue[slot_id];
    uint8_t cycle = g_ep0_cycle[slot_id];

    g_ep0_ring[slot_id][idx].parameter = param;
    g_ep0_ring[slot_id][idx].status = status;
    g_ep0_ring[slot_id][idx].control = control | cycle;
    xhci_flush_l2_range((uintptr_t)&g_ep0_ring[slot_id][idx], (uintptr_t)(&g_ep0_ring[slot_id][idx] + 1));

    idx++;
    if (idx == XHCI_EP_RING_SIZE - 1u) {
        /* Link TRB to chain back to index 0 with Toggle Cycle */
        g_ep0_ring[slot_id][idx].parameter = (uintptr_t)&g_ep0_ring[slot_id][0];
        g_ep0_ring[slot_id][idx].status = 0;
        g_ep0_ring[slot_id][idx].control = (XHCI_TRB_LINK << 10) | (1u << 1) | cycle; /* TC=1 */
        xhci_flush_l2_range((uintptr_t)&g_ep0_ring[slot_id][idx], (uintptr_t)(&g_ep0_ring[slot_id][idx] + 1));
        idx = 0;
        cycle ^= 1u;
    }

    g_ep0_enqueue[slot_id] = idx;
    g_ep0_cycle[slot_id] = cycle;
}

static int xhci_control_transfer(uint8_t slot_id, uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                 void *data, bool in_direction) {
    if (slot_id == 0 || slot_id > XHCI_MAX_SLOTS) return -1;

    /* 1. Setup Stage TRB */
    uint64_t setup_param = (uint64_t)bmRequestType |
                           ((uint64_t)bRequest << 8) |
                           ((uint64_t)wValue << 16) |
                           ((uint64_t)wIndex << 32) |
                           ((uint64_t)wLength << 48);

    uint32_t trt = (wLength == 0) ? 0 : (in_direction ? 3u : 2u); /* TRT: 0=No Data, 2=Out, 3=In */
    xhci_ep0_push_trb(slot_id, setup_param, 8u, (XHCI_TRB_SETUP << 10) | (1u << 6) | (trt << 16)); /* IDT=1 */

    /* 2. Data Stage TRB (if wLength > 0) */
    if (wLength > 0 && data) {
        if (!in_direction) {
            memcpy(g_ctrl_buf, data, wLength);
        }
        xhci_flush_l2_range((uintptr_t)g_ctrl_buf, (uintptr_t)g_ctrl_buf + wLength);

        xhci_ep0_push_trb(slot_id, (uintptr_t)g_ctrl_buf, wLength,
                          (XHCI_TRB_DATA << 10) | (in_direction ? (1u << 16) : 0));
    }

    /* 3. Status Stage TRB */
    uint32_t status_dir = (wLength == 0 || !in_direction) ? (1u << 16) : 0;
    xhci_ep0_push_trb(slot_id, 0, 0, (XHCI_TRB_STATUS << 10) | (1u << 5) | status_dir); /* IOC=1 */

    /* Ring Doorbell for Slot ID, Endpoint 1 (DCI 1 = Control EP0) */
    ring_doorbell(1, slot_id);

    /* Wait for Transfer Event */
    xhci_trb_t ev;
    if (xhci_wait_event(XHCI_TRB_TRANSFER_EVENT, &ev, 150) != 0) {
        return -1;
    }

    uint8_t completion_code = (ev.status >> 24) & 0xFFu;
    if (completion_code != 1u) { /* 1 = Success */
        return -1;
    }

    if (wLength > 0 && data && in_direction) {
        xhci_flush_l2_range((uintptr_t)g_ctrl_buf, (uintptr_t)g_ctrl_buf + wLength);
        memcpy(data, g_ctrl_buf, wLength);
    }

    return 0;
}



/* Configure an Interrupt IN endpoint on an HID device slot */
static int xhci_configure_hid_endpoint(uint8_t slot_id) {
    if (slot_id == 0 || slot_id > XHCI_MAX_SLOTS) return -1;
    xhci_device_t *dev = &g_devices[slot_id];
    if (!dev->present || dev->ep_in_ctx_idx == 0) return -1;

    uint8_t dci = dev->ep_in_ctx_idx;
    uint32_t ctx_sz = g_context_size;

    /* 1. Build Input Context for Configure Endpoint */
    memset(g_input_ctx, 0, sizeof(g_input_ctx));
    uint32_t *input_ctrl = (uint32_t *)g_input_ctx;
    uint32_t *slot_ctx   = (uint32_t *)(g_input_ctx + ctx_sz);
    uint32_t *ep_ctx     = (uint32_t *)(g_input_ctx + (dci + 1u) * ctx_sz);

    input_ctrl[0] = 0; /* Drop flags = 0 */
    input_ctrl[1] = (1u << 0) | (1u << dci); /* Add Slot Context (A0) and Endpoint Context (Adci) */

    /* Slot Context: Context Entries must be at least dci */
    uint32_t route_string = (dev->parent_slot == 0) ? 0 : (dev->port_num & 0x0Fu);
    slot_ctx[0] = ((uint32_t)dci << 27) | (dev->speed << 20) | route_string;
    slot_ctx[1] = ((uint32_t)dev->root_port << 16);
    if (dev->parent_slot != 0) {
        slot_ctx[2] = ((uint32_t)dev->port_num << 8) | (uint32_t)dev->parent_slot;
    } else {
        slot_ctx[2] = 0;
    }

    /* Endpoint Context */
    uint32_t interval = 6; /* 8 ms default */
    if (dev->ep_in_interval <= 1) interval = 3;      /* 1 ms */
    else if (dev->ep_in_interval <= 2) interval = 4; /* 2 ms */
    else if (dev->ep_in_interval <= 4) interval = 5; /* 4 ms */
    else if (dev->ep_in_interval <= 8) interval = 6; /* 8 ms */
    else if (dev->ep_in_interval <= 16) interval = 7;/* 16 ms */
    else interval = 8;                               /* 32 ms */

    ep_ctx[0] = (interval << 16);
    uint32_t mps = dev->ep_in_mps ? dev->ep_in_mps : 8;
    ep_ctx[1] = (3u << 1) | (7u << 3) | (mps << 16); /* CErr=3, EP Type=7 (Interrupt IN), MPS */

    uintptr_t ep_ring_phys = 0;
    if (dev->has_hid_keyboard) {
        memset(g_kbd_ep_ring, 0, sizeof(g_kbd_ep_ring));
        g_kbd_ep_ring[XHCI_EP_RING_SIZE - 1u].parameter = (uintptr_t)&g_kbd_ep_ring[0];
        g_kbd_ep_ring[XHCI_EP_RING_SIZE - 1u].control = (XHCI_TRB_LINK << 10) | (1u << 1) | 1u;
        xhci_flush_l2_range((uintptr_t)&g_kbd_ep_ring[0],
                            (uintptr_t)&g_kbd_ep_ring[XHCI_EP_RING_SIZE]);
        g_kbd_cycle = 1;
        g_kbd_enqueue = 0;
        ep_ring_phys = (uintptr_t)&g_kbd_ep_ring[0] | 1u; /* DCS=1 */
    } else {
        memset(g_mouse_ep_ring, 0, sizeof(g_mouse_ep_ring));
        g_mouse_ep_ring[XHCI_EP_RING_SIZE - 1u].parameter = (uintptr_t)&g_mouse_ep_ring[0];
        g_mouse_ep_ring[XHCI_EP_RING_SIZE - 1u].control = (XHCI_TRB_LINK << 10) | (1u << 1) | 1u;
        xhci_flush_l2_range((uintptr_t)&g_mouse_ep_ring[0],
                            (uintptr_t)&g_mouse_ep_ring[XHCI_EP_RING_SIZE]);
        g_mouse_cycle = 1;
        g_mouse_enqueue = 0;
        ep_ring_phys = (uintptr_t)&g_mouse_ep_ring[0] | 1u; /* DCS=1 */
    }

    ep_ctx[2] = (uint32_t)ep_ring_phys;
    ep_ctx[3] = (uint32_t)(ep_ring_phys >> 32);
    ep_ctx[4] = mps; /* Average TRB Length */

    xhci_flush_l2_range((uintptr_t)g_input_ctx,
                        (uintptr_t)g_input_ctx + (dci + 2u) * ctx_sz);

    /* 2. Issue Configure Endpoint Command */
    xhci_trb_t cmd, ev;
    memset(&cmd, 0, sizeof(cmd));
    cmd.parameter = (uintptr_t)g_input_ctx;
    cmd.control = (XHCI_TRB_CONFIG_EP << 10) | ((uint32_t)slot_id << 24);

    if (xhci_submit_cmd(&cmd, &ev) != 0) {
        printk("[xHCI] Slot %u: Configure Endpoint timed out\n", slot_id);
        return -1;
    }

    uint8_t cc = (ev.status >> 24) & 0xFFu;
    if (cc != 1u) {
        printk("[xHCI] Slot %u: Configure Endpoint failed (cc=%u)\n", slot_id, cc);
        return -1;
    }

    /* 3. Send HID Class Requests on EP0: Set Protocol (0 = Boot), Set Idle (0 = Indefinite) */
    (void)xhci_control_transfer(slot_id, 0x21, 0x0B, 0, 0, 0, NULL, false);
    (void)xhci_control_transfer(slot_id, 0x21, 0x0A, 0, 0, 0, NULL, false);

    /* 4. Queue First Normal TRB and Ring Doorbell */
    if (dev->has_hid_keyboard) {
        g_kbd_slot = slot_id;
        g_kbd_dci = dci;
        g_kbd_online = true;
        memset(g_kbd_prev_report, 0, sizeof(g_kbd_prev_report));
        memset(&g_kbd_xl, 0, sizeof(g_kbd_xl));

        uint8_t qidx = g_kbd_enqueue;
        uint8_t cycle = g_kbd_cycle;
        memset(g_kbd_report_buf, 0, 16);
        xhci_flush_l2_range((uintptr_t)g_kbd_report_buf,
                            (uintptr_t)g_kbd_report_buf + 16);

        g_kbd_ep_ring[qidx].parameter = (uintptr_t)g_kbd_report_buf;
        g_kbd_ep_ring[qidx].status = mps;
        g_kbd_ep_ring[qidx].control = (XHCI_TRB_NORMAL << 10) | (1u << 5) | cycle;
        xhci_flush_l2_range((uintptr_t)&g_kbd_ep_ring[qidx],
                            (uintptr_t)(&g_kbd_ep_ring[qidx] + 1));

        qidx++;
        if (qidx == XHCI_EP_RING_SIZE - 1u) {
            qidx = 0;
            g_kbd_cycle ^= 1u;
        }
        g_kbd_enqueue = qidx;

        ring_doorbell(dci, slot_id);
        console_input_present(WIN_INPUT_KEYBOARD, true);
        printk("[xHCI] Slot %u: USB HID Keyboard Online (DCI %u, MPS %u)\n", slot_id, dci, mps);
    } else if (dev->has_hid_mouse) {
        g_mouse_slot = slot_id;
        g_mouse_dci = dci;
        g_mouse_online = true;
        if (g_mouse_x == 0 && g_mouse_y == 0) {
            g_mouse_x = 512;
            g_mouse_y = 300;
        }

        uint8_t qidx = g_mouse_enqueue;
        uint8_t cycle = g_mouse_cycle;
        memset(g_mouse_report_buf, 0, 16);
        xhci_flush_l2_range((uintptr_t)g_mouse_report_buf,
                            (uintptr_t)g_mouse_report_buf + 16);

        g_mouse_ep_ring[qidx].parameter = (uintptr_t)g_mouse_report_buf;
        g_mouse_ep_ring[qidx].status = mps;
        g_mouse_ep_ring[qidx].control = (XHCI_TRB_NORMAL << 10) | (1u << 5) | cycle;
        xhci_flush_l2_range((uintptr_t)&g_mouse_ep_ring[qidx],
                            (uintptr_t)(&g_mouse_ep_ring[qidx] + 1));

        qidx++;
        if (qidx == XHCI_EP_RING_SIZE - 1u) {
            qidx = 0;
            g_mouse_cycle ^= 1u;
        }
        g_mouse_enqueue = qidx;

        ring_doorbell(dci, slot_id);
        console_input_present(WIN_INPUT_POINTER, true);
        printk("[xHCI] Slot %u: USB HID Mouse Online (DCI %u, MPS %u)\n", slot_id, dci, mps);
    }

    return 0;
}

/* Enumerate downstream ports of a USB 2.0 Hub */
static void xhci_enumerate_hub(uint8_t hub_slot, uint8_t root_port) {
    usb_hub_desc_t hub_desc;
    memset(&hub_desc, 0, sizeof(hub_desc));
    if (xhci_control_transfer(hub_slot, 0xA0, 0x06, (0x29u << 8), 0, sizeof(hub_desc), &hub_desc, true) != 0) {
        printk("[xHCI] Hub Slot %u: Failed to get hub descriptor\n", hub_slot);
        return;
    }

    g_devices[hub_slot].is_hub = true;
    g_devices[hub_slot].hub_num_ports = hub_desc.bNbrPorts;

    printk("[xHCI] Hub Slot %u: %u downstream ports, pwr_good=%u ms\n",
           hub_slot, hub_desc.bNbrPorts, (unsigned)hub_desc.bPwrOn2PwrGood * 2u);

    /* Configure slot as a hub in xHCI using Evaluate Context */
    memset(g_input_ctx, 0, sizeof(g_input_ctx));
    uint32_t ctx_sz = g_context_size;
    uint32_t *input_ctrl = (uint32_t *)g_input_ctx;
    uint32_t *slot_ctx   = (uint32_t *)(g_input_ctx + ctx_sz);

    input_ctrl[0] = 0;
    input_ctrl[1] = (1u << 0); /* Add Slot Context (A0) */
    slot_ctx[0] = (1u << 26);  /* Hub = 1 (bit 26) */
    slot_ctx[1] = ((uint32_t)hub_desc.bNbrPorts << 24); /* Number of Ports */

    xhci_flush_l2_range((uintptr_t)g_input_ctx, (uintptr_t)g_input_ctx + 2u * ctx_sz);

    xhci_trb_t cmd, ev;
    memset(&cmd, 0, sizeof(cmd));
    cmd.parameter = (uintptr_t)g_input_ctx;
    cmd.control = (XHCI_TRB_EVAL_CTX << 10) | ((uint32_t)hub_slot << 24);

    if (xhci_submit_cmd(&cmd, &ev) != 0) {
        printk("[xHCI] Hub Slot %u: Evaluate Context timed out\n", hub_slot);
    } else {
        uint8_t cc = (ev.status >> 24) & 0xFFu;
        if (cc != 1u) {
            printk("[xHCI] Hub Slot %u: Evaluate Context cc=%u\n", hub_slot, cc);
        }
    }

    /* 1. Power on each hub port (staggered by 20 ms to prevent power rail inrush spikes) */
    for (uint8_t p = 1; p <= hub_desc.bNbrPorts; p++) {
        (void)xhci_control_transfer(hub_slot, 0x23, 0x03, 8 /* PORT_POWER */, p, 0, NULL, false);
        time_delay_us(20000);
    }

    /* Wait for decoupling capacitors to charge */
    time_delay_us(100000);

    /* 2. Clear any inrush C_PORT_OVER_CURRENT flags and re-assert PORT_POWER */
    for (uint8_t p = 1; p <= hub_desc.bNbrPorts; p++) {
        uint32_t port_status = 0;
        if (xhci_control_transfer(hub_slot, 0xA3, 0x00, 0, p, 4, &port_status, true) == 0) {
            uint16_t status = (uint16_t)(port_status & 0xFFFF);
            uint16_t change = (uint16_t)(port_status >> 16);
            if ((change & (1u << 3)) || !(status & (1u << 8))) {
                /* Clear C_PORT_OVER_CURRENT */
                (void)xhci_control_transfer(hub_slot, 0x23, 0x01, 19 /* C_PORT_OVER_CURRENT */, p, 0, NULL, false);
                time_delay_us(20000);
                /* Re-enable PORT_POWER now that decoupling capacitors are charged */
                (void)xhci_control_transfer(hub_slot, 0x23, 0x03, 8 /* PORT_POWER */, p, 0, NULL, false);
                time_delay_us(20000);
            }
        }
    }

    /* Wait for device pullup assertion (USB devices can take up to 800 ms to boot) */
    time_delay_us(400000);

    /* Scan hub downstream ports */
    for (uint8_t p = 1; p <= hub_desc.bNbrPorts; p++) {
        uint32_t port_status = 0;
        uint16_t wPortStatus = 0;
        uint64_t poll_deadline = time_get_us() + 800000ULL;

        /* Poll up to 800 ms for pullup attachment */
        while (time_get_us() < poll_deadline) {
            port_status = 0;
            int r = xhci_control_transfer(hub_slot, 0xA3, 0x00, 0, p, 4, &port_status, true);
            if (r != 0) {
                printk("[xHCI DBG] GetPortStatus p=%u failed: r=%d\n", p, r);
                break; /* Port query error (e.g. non-existent port) */
            }

            /* If over-current change is flagged, clear it to unlatch the hub */
            if (port_status & (1u << 19)) {
                (void)xhci_control_transfer(hub_slot, 0x23, 0x01, 19 /* C_PORT_OVER_CURRENT */, p, 0, NULL, false);
                (void)xhci_control_transfer(hub_slot, 0x23, 0x03, 8 /* PORT_POWER */, p, 0, NULL, false);
            }

            wPortStatus = (uint16_t)(port_status & 0xFFFF);
            if (wPortStatus & 1u) {
                break; /* Device connected */
            }
            time_delay_us(25000);
        }

        printk("[xHCI] Hub Slot %u Port %u: Status=0x%08x (wPortStatus=0x%04x)\n",
               hub_slot, p, port_status, wPortStatus);
        if (!(wPortStatus & 1u)) {
            continue; /* No device connected */
        }

        printk("[xHCI] Hub Slot %u Port %u: Device detected (status=0x%04x)\n", hub_slot, p, wPortStatus);

        /* Issue Port Reset */
        (void)xhci_control_transfer(hub_slot, 0x23, 0x03, 4 /* PORT_RESET */, p, 0, NULL, false);
        time_delay_us(50000); /* 50 ms reset pulse */

        uint64_t t0 = time_get_us();
        while (time_get_us() - t0 < 200000ULL) {
            port_status = 0;
            if (xhci_control_transfer(hub_slot, 0xA3, 0x00, 0, p, 4, &port_status, true) == 0) {
                wPortStatus = (uint16_t)(port_status & 0xFFFF);
                if (!(wPortStatus & (1u << 4))) {
                    break; /* Reset finished */
                }
            }
            time_delay_us(5000);
        }

        /* Clear C_PORT_RESET */
        (void)xhci_control_transfer(hub_slot, 0x23, 0x01, 20 /* C_PORT_RESET */, p, 0, NULL, false);
        time_delay_us(10000);

        /* Query status again to check negotiated speed */
        port_status = 0;
        (void)xhci_control_transfer(hub_slot, 0xA3, 0x00, 0, p, 4, &port_status, true);
        wPortStatus = (uint16_t)(port_status & 0xFFFF);

        uint8_t dev_speed;
        if (wPortStatus & (1u << 9)) dev_speed = XHCI_SPEED_LOW;
        else if (wPortStatus & (1u << 10)) dev_speed = XHCI_SPEED_HIGH;
        else dev_speed = XHCI_SPEED_FULL;

        /* Enable Slot */
        memset(&cmd, 0, sizeof(cmd));
        cmd.control = (XHCI_TRB_ENABLE_SLOT << 10);
        if (xhci_submit_cmd(&cmd, &ev) != 0) {
            printk("[xHCI] Hub Slot %u Port %u: Enable Slot timed out\n", hub_slot, p);
            continue;
        }

        uint8_t cc = (ev.status >> 24) & 0xFFu;
        if (cc != 1u) {
            printk("[xHCI] Hub Slot %u Port %u: Enable Slot cc=%u\n", hub_slot, p, cc);
            continue;
        }

        uint8_t child_slot = (ev.control >> 24) & 0xFFu;
        if (child_slot == 0 || child_slot > XHCI_MAX_SLOTS) continue;

        (void)xhci_setup_device(child_slot, root_port, hub_slot, p, dev_speed);
    }
}

/* Set up and configure an enumerated device slot */
static int xhci_setup_device(uint8_t slot_id, uint8_t root_port, uint8_t parent_hub_slot,
                             uint8_t parent_hub_port, uint8_t speed) {
    /* 1. Initialize EP0 Transfer Ring for this slot */
    memset(g_ep0_ring[slot_id], 0, sizeof(g_ep0_ring[slot_id]));
    g_ep0_ring[slot_id][XHCI_EP_RING_SIZE - 1u].parameter = (uintptr_t)&g_ep0_ring[slot_id][0];
    g_ep0_ring[slot_id][XHCI_EP_RING_SIZE - 1u].control = (XHCI_TRB_LINK << 10) | (1u << 1) | 1u; /* TC=1, Cycle=1 */
    xhci_flush_l2_range((uintptr_t)&g_ep0_ring[slot_id][0],
                        (uintptr_t)&g_ep0_ring[slot_id][XHCI_EP_RING_SIZE]);
    g_ep0_cycle[slot_id] = 1;
    g_ep0_enqueue[slot_id] = 0;

    /* 2. Build Input Context for Address Device */
    memset(g_input_ctx, 0, sizeof(g_input_ctx));
    uint32_t ctx_sz = g_context_size;
    uint32_t *input_ctrl = (uint32_t *)g_input_ctx;
    uint32_t *slot_ctx   = (uint32_t *)(g_input_ctx + ctx_sz);
    uint32_t *ep0_ctx    = (uint32_t *)(g_input_ctx + 2u * ctx_sz);

    input_ctrl[0] = 0;
    input_ctrl[1] = (1u << 0) | (1u << 1); /* Add Slot Context (A0) and EP0 (A1) */

    /* Slot Context: Context Entries = 1, Route String, Speed, Root Hub Port Num */
    uint32_t route_string = (parent_hub_slot == 0) ? 0 : (parent_hub_port & 0x0Fu);
    slot_ctx[0] = (1u << 27) | (speed << 20) | route_string; /* Context Entries = 1 */
    slot_ctx[1] = ((uint32_t)root_port << 16);               /* Root Hub Port */
    if (parent_hub_slot != 0) {
        slot_ctx[2] = ((uint32_t)parent_hub_port << 8) | (uint32_t)parent_hub_slot;
    } else {
        slot_ctx[2] = 0;
    }

    /* Endpoint 0 Context: EP Type = 4 (Control), Max Packet Size, Error Count = 3 */
    uint32_t mps = (speed == XHCI_SPEED_LOW) ? 8u : (speed == XHCI_SPEED_FULL) ? 8u : (speed == XHCI_SPEED_HIGH) ? 64u : 512u;
    ep0_ctx[1] = (3u << 1) | (4u << 3) | (mps << 16); /* CErr=3, EPType=4 (Control), MPS */
    uintptr_t ep0_ring_phys = (uintptr_t)&g_ep0_ring[slot_id][0] | 1u; /* DCS=1 */
    ep0_ctx[2] = (uint32_t)ep0_ring_phys;
    ep0_ctx[3] = (uint32_t)(ep0_ring_phys >> 32);
    ep0_ctx[4] = 8u; /* Average TRB Length */

    xhci_flush_l2_range((uintptr_t)g_input_ctx, (uintptr_t)g_input_ctx + 3u * ctx_sz);

    /* Point DCBAA[slot_id] to Output Device Context */
    memset(g_dev_ctx[slot_id], 0, sizeof(g_dev_ctx[slot_id]));
    xhci_flush_l2_range((uintptr_t)g_dev_ctx[slot_id], (uintptr_t)g_dev_ctx[slot_id] + sizeof(g_dev_ctx[slot_id]));
    g_dcbaa[slot_id] = (uintptr_t)&g_dev_ctx[slot_id][0];
    xhci_flush_l2_range((uintptr_t)&g_dcbaa[slot_id], (uintptr_t)(&g_dcbaa[slot_id] + 1));

    /* 3. Issue Address Device Command (BSR = 0) */
    xhci_trb_t cmd, ev;
    memset(&cmd, 0, sizeof(cmd));
    cmd.parameter = (uintptr_t)g_input_ctx;
    cmd.control = (XHCI_TRB_ADDRESS_DEVICE << 10) | ((uint32_t)slot_id << 24);

    if (xhci_submit_cmd(&cmd, &ev) != 0) {
        printk("[xHCI] Slot %u: Address Device timed out\n", slot_id);
        return -1;
    }

    uint8_t cc = (ev.status >> 24) & 0xFFu;
    if (cc != 1u) {
        printk("[xHCI] Slot %u: Address Device failed (cc=%u)\n", slot_id, cc);
        return -1;
    }

    /* 4. Fetch initial 8 bytes of Device Descriptor */
    usb_device_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    if (xhci_control_transfer(slot_id, 0x80, 0x06, (1u << 8), 0, 8, &desc, true) != 0) {
        printk("[xHCI] Slot %u: Failed to read initial Device Descriptor\n", slot_id);
        return -1;
    }

    /* If Full-Speed and MPS > 8, evaluate context to update MPS */
    if (speed == XHCI_SPEED_FULL && desc.bMaxPacketSize0 > 8) {
        memset(g_input_ctx, 0, sizeof(g_input_ctx));
        input_ctrl[0] = 0;
        input_ctrl[1] = (1u << 1); /* EP0 Context (A1) */
        ep0_ctx[1] = (3u << 1) | (4u << 3) | ((uint32_t)desc.bMaxPacketSize0 << 16);
        xhci_flush_l2_range((uintptr_t)g_input_ctx, (uintptr_t)g_input_ctx + 3u * ctx_sz);

        memset(&cmd, 0, sizeof(cmd));
        cmd.parameter = (uintptr_t)g_input_ctx;
        cmd.control = (XHCI_TRB_EVAL_CTX << 10) | ((uint32_t)slot_id << 24);
        (void)xhci_submit_cmd(&cmd, &ev);
    }

    /* 5. Fetch full 18-byte Device Descriptor */
    memset(&desc, 0, sizeof(desc));
    if (xhci_control_transfer(slot_id, 0x80, 0x06, (1u << 8), 0, sizeof(desc), &desc, true) != 0) {
        printk("[xHCI] Slot %u: Failed to get full Device Descriptor\n", slot_id);
        return -1;
    }

    printk("[xHCI] Slot %u: VID=%04x PID=%04x Class=%02x SubClass=%02x Proto=%02x Configs=%u\n",
           slot_id, desc.idVendor, desc.idProduct, desc.bDeviceClass, desc.bDeviceSubClass,
           desc.bDeviceProtocol, desc.bNumConfigurations);

    /* Record Device Details */
    xhci_device_t *dev = &g_devices[slot_id];
    dev->present = true;
    dev->slot_id = slot_id;
    dev->port_num = parent_hub_slot ? parent_hub_port : root_port;
    dev->root_port = root_port;
    dev->parent_slot = parent_hub_slot;
    dev->speed = speed;
    dev->vid = desc.idVendor;
    dev->pid = desc.idProduct;
    dev->class_code = desc.bDeviceClass;
    dev->subclass_code = desc.bDeviceSubClass;
    dev->protocol = desc.bDeviceProtocol;

    /* 6. Fetch Configuration Descriptor Tree to inspect interfaces */
    uint8_t cfg_buf[256];
    memset(cfg_buf, 0, sizeof(cfg_buf));
    if (xhci_control_transfer(slot_id, 0x80, 0x06, (2u << 8), 0, sizeof(cfg_buf), cfg_buf, true) == 0) {
        usb_config_desc_t *cfg = (usb_config_desc_t *)cfg_buf;
        uint16_t total_len = cfg->wTotalLength;
        if (total_len > sizeof(cfg_buf)) total_len = sizeof(cfg_buf);

        uint32_t off = sizeof(usb_config_desc_t);
        while (off + 2 <= total_len) {
            uint8_t len = cfg_buf[off];
            uint8_t type = cfg_buf[off + 1];
            if (len == 0 || off + len > total_len) break;

            if (type == 0x04) { /* Interface Descriptor */
                usb_interface_desc_t *iface = (usb_interface_desc_t *)&cfg_buf[off];
                if (iface->bInterfaceClass == 0x03) { /* HID */
                    if (iface->bInterfaceProtocol == 0x01) {
                        dev->has_hid_keyboard = true;
                    } else if (iface->bInterfaceProtocol == 0x02) {
                        dev->has_hid_mouse = true;
                    }
                }
            } else if (type == 0x05) { /* Endpoint Descriptor */
                usb_endpoint_desc_t *ep = (usb_endpoint_desc_t *)&cfg_buf[off];
                if ((ep->bEndpointAddress & 0x80u) && (ep->bmAttributes & 0x03u) == 0x03u) {
                    /* Interrupt IN endpoint */
                    dev->ep_in_addr = ep->bEndpointAddress;
                    dev->ep_in_mps = ep->wMaxPacketSize;
                    dev->ep_in_interval = ep->bInterval;
                    uint8_t ep_num = ep->bEndpointAddress & 0x0Fu;
                    dev->ep_in_ctx_idx = (ep_num * 2u) + 1u;
                }
            }
            off += len;
        }

        /* 7. Issue Set Configuration (Config 1) */
        (void)xhci_control_transfer(slot_id, 0x00, 0x09, 1, 0, 0, NULL, false);
    }

    const char *speed_str = (speed == XHCI_SPEED_SUPER) ? "SuperSpeed 5G" :
                            (speed == XHCI_SPEED_HIGH)  ? "High-Speed 480M" :
                            (speed == XHCI_SPEED_FULL)  ? "Full-Speed 12M" : "Low-Speed 1.5M";

    printk("[xHCI] Slot %u: Device attached (%s, %s): VID=%04x PID=%04x%s%s\n",
           slot_id, parent_hub_slot ? "Hub Downstream" : "Root Port",
           speed_str, dev->vid, dev->pid,
           dev->has_hid_keyboard ? " [HID Keyboard]" : "",
           dev->has_hid_mouse    ? " [HID Mouse]" : "");

    /* 8. If Hub, enumerate downstream ports */
    if (desc.bDeviceClass == 0x09) {
        xhci_enumerate_hub(slot_id, root_port);
    }

    /* 9. If HID Keyboard or Mouse, configure Interrupt IN endpoint */
    if (dev->has_hid_keyboard || dev->has_hid_mouse) {
        (void)xhci_configure_hid_endpoint(slot_id);
    }

    return 0;
}

static int xhci_enumerate_port(uint8_t port_num) {
    uintptr_t port_reg = g_xhci_op_base + XHCI_OP_PORTSC_BASE + (port_num - 1u) * 0x10u;
    uint32_t portsc = REG32(port_reg);

    if (!(portsc & XHCI_PORT_CCS)) {
        return 0; /* No device connected */
    }

    uint8_t speed = (portsc >> XHCI_PORT_SPEED_SHIFT) & 0x0Fu;

    /* If not SuperSpeed, issue Port Reset to transition to Enabled */
    if (speed != XHCI_SPEED_SUPER) {
        uint32_t pr_val = (portsc & 0x0E000E00u) | XHCI_PORT_PR;
        REG32(port_reg) = pr_val;

        time_delay_us(25000);

        uint64_t t0 = time_get_us();
        while (REG32(port_reg) & XHCI_PORT_PR) {
            if (time_get_us() - t0 > 100000ULL) break;
            time_delay_us(500);
        }

        portsc = REG32(port_reg);
        speed = (portsc >> XHCI_PORT_SPEED_SHIFT) & 0x0Fu;
    }

    if (!(portsc & XHCI_PORT_PED)) {
        printk("[xHCI] Port %u reset failed (PED not set)\n", port_num);
        return -1;
    }

    /* 1. Enable Slot Command */
    xhci_trb_t cmd, ev;
    memset(&cmd, 0, sizeof(cmd));
    cmd.control = (XHCI_TRB_ENABLE_SLOT << 10);

    if (xhci_submit_cmd(&cmd, &ev) != 0) {
        printk("[xHCI] Port %u: Enable Slot command timed out\n", port_num);
        return -1;
    }

    uint8_t cc = (ev.status >> 24) & 0xFFu;
    if (cc != 1u) {
        printk("[xHCI] Port %u: Enable Slot failed (cc=%u)\n", port_num, cc);
        return -1;
    }

    uint8_t slot_id = (ev.control >> 24) & 0xFFu;
    if (slot_id == 0 || slot_id > XHCI_MAX_SLOTS) {
        return -1;
    }

    return xhci_setup_device(slot_id, port_num, 0, 0, speed);
}

/* Console Input Integration for physical USB Keyboard */
static void xhci_kbd_pump(void) {
    xhci_poll();
    uint32_t now = (uint32_t)time_get_ms();
    if (g_kbd_online) {
        uint8_t rep[16];
        uint32_t n = usbkbd_repeat(&g_kbd_xl, now, rep);
        for (uint32_t i = 0; i < n; i++) {
            if (g_kbd_head - g_kbd_tail < KBD_QUEUE_SIZE) {
                g_kbd_queue[g_kbd_head % KBD_QUEUE_SIZE] = rep[i];
                g_kbd_head++;
            }
        }
    }
}

static bool xhci_kbd_has_char(void) {
    xhci_kbd_pump();
    return g_kbd_head != g_kbd_tail;
}

static int xhci_kbd_getc(void) {
    if (g_kbd_head == g_kbd_tail) return -1;
    uint8_t ch = g_kbd_queue[g_kbd_tail % KBD_QUEUE_SIZE];
    g_kbd_tail++;
    return ch;
}

static bool xhci_kbd_peek_interrupt(void) {
    xhci_kbd_pump();
    for (uint32_t i = g_kbd_tail; i != g_kbd_head; i++) {
        if (g_kbd_queue[i % KBD_QUEUE_SIZE] == 0x03) return true;
    }
    return false;
}

static const console_input_t g_in_xhci_kbd = {
    "xhci_kbd",
    xhci_kbd_has_char,
    xhci_kbd_getc,
    xhci_kbd_peek_interrupt,
    NULL,
    false
};

int xhci_init(void) {
    if (g_xhci_present) return 0;

    ylock_init(&g_xhci_lock);
    ylock_acquire(&g_xhci_lock);

    /* 1. Locate VIA VL805 on PCIe (Class 0x0C, Subclass 0x03) */
    const pci_device_t *pdev = jh7110_pcie_find_class(0x0C, 0x03);
    if (!pdev) {
        ylock_release(&g_xhci_lock);
        return -1;
    }

    if (pdev->bars[0].size == 0) {
        printk("[xHCI] Error: Controller BAR0 unallocated\n");
        ylock_release(&g_xhci_lock);
        return -1;
    }

    g_xhci_bar0 = (uintptr_t)pdev->bars[0].base;

    /* 2. Read Capability Registers */
    uint8_t caplength = REG8(g_xhci_bar0 + XHCI_CAP_CAPLENGTH);
    g_hci_version = REG16(g_xhci_bar0 + XHCI_CAP_HCIVERSION);

    uint32_t hcsparams1 = REG32(g_xhci_bar0 + XHCI_CAP_HCSPARAMS1);
    g_max_slots = hcsparams1 & 0xFFu;
    g_max_ports = (hcsparams1 >> 24) & 0xFFu;

    uint32_t hcsparams2 = REG32(g_xhci_bar0 + XHCI_CAP_HCSPARAMS2);
    g_max_scratchpad = (((hcsparams2 >> 21) & 0x1Fu) << 5) | ((hcsparams2 >> 27) & 0x1Fu);

    uint32_t hccparams1 = REG32(g_xhci_bar0 + XHCI_CAP_HCCPARAMS1);
    g_context_size = (hccparams1 & (1u << 2)) ? 64u : 32u;

    uint32_t dboff = REG32(g_xhci_bar0 + XHCI_CAP_DBOFF);
    uint32_t rtsoff = REG32(g_xhci_bar0 + XHCI_CAP_RTSOFF);

    g_xhci_op_base = g_xhci_bar0 + caplength;
    g_xhci_rt_base = g_xhci_bar0 + rtsoff;
    g_xhci_db_base = g_xhci_bar0 + dboff;

    /* 3. Halt and Reset Controller */
    REG32(g_xhci_op_base + XHCI_OP_USBCMD) &= ~XHCI_CMD_RS;
    uint64_t t0 = time_get_us();
    while ((REG32(g_xhci_op_base + XHCI_OP_USBSTS) & XHCI_STS_HCH) == 0) {
        if (time_get_us() - t0 > 100000ULL) break;
        time_delay_us(100);
    }

    REG32(g_xhci_op_base + XHCI_OP_USBCMD) |= XHCI_CMD_HCRST;
    t0 = time_get_us();
    while ((REG32(g_xhci_op_base + XHCI_OP_USBCMD) & XHCI_CMD_HCRST) ||
           (REG32(g_xhci_op_base + XHCI_OP_USBSTS) & XHCI_STS_CNR)) {
        if (time_get_us() - t0 > 200000ULL) {
            printk("[xHCI] Controller reset timeout\n");
            ylock_release(&g_xhci_lock);
            return -1;
        }
        time_delay_us(100);
    }

    /* 4. Configure MaxSlotsEn */
    uint32_t max_slots_en = (g_max_slots > XHCI_MAX_SLOTS) ? XHCI_MAX_SLOTS : g_max_slots;
    REG32(g_xhci_op_base + XHCI_OP_CONFIG) = max_slots_en;

    /* 6. Set up Scratchpads and DCBAA */
    memset(g_dcbaa, 0, sizeof(g_dcbaa));
    if (g_max_scratchpad > 0 && g_max_scratchpad <= XHCI_MAX_SCRATCHPAD) {
        for (uint32_t i = 0; i < g_max_scratchpad; i++) {
            g_scratchpad_ptrs[i] = (uintptr_t)&g_scratchpad_pages[i][0];
            xhci_flush_l2_range((uintptr_t)&g_scratchpad_pages[i][0],
                                (uintptr_t)&g_scratchpad_pages[i][0] + 4096);
        }
        xhci_flush_l2_range((uintptr_t)&g_scratchpad_ptrs[0],
                            (uintptr_t)&g_scratchpad_ptrs[g_max_scratchpad]);
        g_dcbaa[0] = (uintptr_t)&g_scratchpad_ptrs[0];
    }
    xhci_flush_l2_range((uintptr_t)&g_dcbaa[0], (uintptr_t)&g_dcbaa[max_slots_en + 1]);

    REG32(g_xhci_op_base + XHCI_OP_DCBAAP_LO) = (uint32_t)(uintptr_t)&g_dcbaa[0];
    REG32(g_xhci_op_base + XHCI_OP_DCBAAP_HI) = (uint32_t)((uintptr_t)&g_dcbaa[0] >> 32);

    /* 7. Initialize Command Ring */
    memset(g_cmd_ring, 0, sizeof(g_cmd_ring));
    g_cmd_ring[XHCI_CMD_RING_SIZE - 1u].parameter = (uintptr_t)&g_cmd_ring[0];
    g_cmd_ring[XHCI_CMD_RING_SIZE - 1u].control = (XHCI_TRB_LINK << 10) | (1u << 1) | 1u; /* TC=1, Cycle=1 */
    xhci_flush_l2_range((uintptr_t)&g_cmd_ring[0], (uintptr_t)&g_cmd_ring[XHCI_CMD_RING_SIZE]);

    g_cmd_enqueue = 0;
    g_cmd_cycle = 1;
    uintptr_t crcr_val = (uintptr_t)&g_cmd_ring[0] | 1u; /* RCS=1 */
    REG32(g_xhci_op_base + XHCI_OP_CRCR_LO) = (uint32_t)crcr_val;
    REG32(g_xhci_op_base + XHCI_OP_CRCR_HI) = (uint32_t)(crcr_val >> 32);

    /* 8. Initialize Event Ring */
    memset(g_event_ring, 0, sizeof(g_event_ring));
    xhci_flush_l2_range((uintptr_t)&g_event_ring[0], (uintptr_t)&g_event_ring[XHCI_EVENT_RING_SZ]);

    g_erst[0].seg_base = (uintptr_t)&g_event_ring[0];
    g_erst[0].seg_size = XHCI_EVENT_RING_SZ;
    g_erst[0].rsvd = 0;
    xhci_flush_l2_range((uintptr_t)&g_erst[0], (uintptr_t)(&g_erst[0] + 1));

    g_event_dequeue = 0;
    g_event_cycle = 1;

    REG32(g_xhci_rt_base + 0x20 + 0x08) = 1u; /* ERSTSZ = 1 */
    REG32(g_xhci_rt_base + 0x20 + 0x10) = (uint32_t)(uintptr_t)&g_erst[0];
    REG32(g_xhci_rt_base + 0x20 + 0x14) = (uint32_t)((uintptr_t)&g_erst[0] >> 32);
    uintptr_t erdp_val = (uintptr_t)&g_event_ring[0] | (1u << 3);
    REG32(g_xhci_rt_base + 0x20 + 0x18) = (uint32_t)erdp_val;
    REG32(g_xhci_rt_base + 0x20 + 0x1C) = (uint32_t)(erdp_val >> 32);
    REG32(g_xhci_rt_base + 0x20 + 0x00) |= (1u << 1); /* IMAN.IE = 1 */

    /* 9. Start Controller */
    REG32(g_xhci_op_base + XHCI_OP_USBCMD) |= (XHCI_CMD_RS | XHCI_CMD_INTE);
    t0 = time_get_us();
    while (REG32(g_xhci_op_base + XHCI_OP_USBSTS) & XHCI_STS_HCH) {
        if (time_get_us() - t0 > 100000ULL) break;
        time_delay_us(100);
    }

    g_xhci_present = true;

    /* 10. Power On Root Hub Ports */
    for (uint32_t p = 1; p <= g_max_ports; p++) {
        uintptr_t preg = g_xhci_op_base + XHCI_OP_PORTSC_BASE + (p - 1u) * 0x10u;
        uint32_t val = REG32(preg);
        REG32(preg) = (val & 0x0E000E00u) | XHCI_PORT_PP;
    }

    /* Wait 20 ms for port power */
    time_delay_us(20000);

    printk("[xHCI] VIA VL805 Ready: xHCI v%x.%x, %u ports, %u slots, ctx=%u B\n",
           g_hci_version >> 8, g_hci_version & 0xFFu, g_max_ports, max_slots_en, g_context_size);

    /* 11. Register console input driver for USB Keyboard */
    (void)console_input_register(&g_in_xhci_kbd);

    /* 12. Enumerate connected ports */
    for (uint8_t p = 1; p <= g_max_ports; p++) {
        (void)xhci_enumerate_port(p);
    }

    ylock_release(&g_xhci_lock);
    return 0;
}

bool xhci_is_present(void) {
    return g_xhci_present;
}

bool xhci_is_kbd_present(void) {
    return g_kbd_online;
}

bool xhci_is_mouse_present(void) {
    return g_mouse_online;
}

void xhci_get_mouse(int *x, int *y, uint8_t *buttons, int *wheel) {
    ylock_acquire(&g_xhci_lock);
    if (x) *x = g_mouse_x;
    if (y) *y = g_mouse_y;
    if (buttons) *buttons = g_mouse_buttons;
    if (wheel) *wheel = g_mouse_wheel;
    ylock_release(&g_xhci_lock);
}

void xhci_poll(void) {
    if (!g_xhci_present) return;
    ylock_acquire(&g_xhci_lock);
    xhci_trb_t ev;
    while (xhci_wait_event(0, &ev, 0) == 0) {
        /* Event handled and acknowledged in xhci_wait_event */
    }
    ylock_release(&g_xhci_lock);
}

void xhci_cmd_summary(void) {
    if (!g_xhci_present) {
        if (xhci_init() != 0) {
            cprintf("USB: No xHCI controller found or initialization failed\n");
            return;
        }
    }

    uint32_t cmd = REG32(g_xhci_op_base + XHCI_OP_USBCMD);
    uint32_t sts = REG32(g_xhci_op_base + XHCI_OP_USBSTS);
    uint32_t hcc1 = REG32(g_xhci_bar0 + XHCI_CAP_HCCPARAMS1);

    cprintf("xHCI USB 3.0 Controller (PCIe Port 0, Bus 01:00.0):\n");
    cprintf("  Controller:        VIA Technologies VL805-Q6 (1106:3483)\n");
    cprintf("  Specification:     xHCI v%x.%x\n", g_hci_version >> 8, g_hci_version & 0xFFu);
    cprintf("  Operational State: %s (USBCMD=0x%08x, USBSTS=0x%08x)\n",
            (sts & XHCI_STS_HCH) ? "HALTED" : "RUNNING", (unsigned)cmd, (unsigned)sts);
    cprintf("  Capabilities:      HCCPARAMS1=0x%08x (PPC=%u, CSZ=%u, xECP=0x%x)\n",
            (unsigned)hcc1, (unsigned)((hcc1 >> 3) & 1), (unsigned)g_context_size, (unsigned)((hcc1 >> 8) & 0xFF));
    cprintf("  Downstream Ports:  %u Type-A ports (J11/J12)\n", (unsigned)g_max_ports);
    cprintf("  Device Slots:      %u supported, %u enabled\n",
            (unsigned)g_max_slots, (unsigned)((g_max_slots > XHCI_MAX_SLOTS) ? XHCI_MAX_SLOTS : g_max_slots));
    cprintf("  Context Size:      %u bytes\n", (unsigned)g_context_size);
    cprintf("  BAR0 MMIO Address: 0x%lx\n", (unsigned long)g_xhci_bar0);
}

void xhci_cmd_ports(void) {
    if (!g_xhci_present) {
        if (xhci_init() != 0) {
            cprintf("usb ports: xHCI not available\n");
            return;
        }
    }

    cprintf("xHCI Root Hub Ports:\n");
    cprintf("Port  Status     Speed             Power   PORTSC      Details\n");
    cprintf("----  ---------  ----------------  ------  ----------  -------------------------\n");

    ylock_acquire(&g_xhci_lock);
    for (uint8_t p = 1; p <= g_max_ports; p++) {
        uintptr_t preg = g_xhci_op_base + XHCI_OP_PORTSC_BASE + (p - 1u) * 0x10u;
        uint32_t portsc = REG32(preg);

        bool ccs = (portsc & XHCI_PORT_CCS) != 0;
        bool ped = (portsc & XHCI_PORT_PED) != 0;
        bool pp  = (portsc & XHCI_PORT_PP) != 0;
        uint8_t speed = (portsc >> XHCI_PORT_SPEED_SHIFT) & 0x0Fu;

        const char *speed_str = !ccs ? "-" :
                                (speed == XHCI_SPEED_SUPER) ? "SuperSpeed 5G" :
                                (speed == XHCI_SPEED_HIGH)  ? "High-Speed 480M" :
                                (speed == XHCI_SPEED_FULL)  ? "Full-Speed 12M" :
                                (speed == XHCI_SPEED_LOW)   ? "Low-Speed 1.5M" : "Unknown";

        const char *status_str = !ccs ? "Empty" : (ped ? "Connected" : "Disabled");

        const char *dev_info = "";
        for (uint8_t s = 1; s <= XHCI_MAX_SLOTS; s++) {
            if (g_devices[s].present && g_devices[s].root_port == p && g_devices[s].parent_slot == 0) {
                if (g_devices[s].is_hub) dev_info = "USB 2.0 Hub (Onboard)";
                else if (g_devices[s].has_hid_keyboard) dev_info = "USB Keyboard (HID)";
                else if (g_devices[s].has_hid_mouse) dev_info = "USB Mouse (HID)";
                else dev_info = "USB Device";
                break;
            }
        }

        cprintf(" %2u   %-9s  %-16s  %-6s  0x%08x  %s\n",
                (unsigned)p, status_str, speed_str, pp ? "ON" : "OFF", (unsigned)portsc, dev_info);
    }
    ylock_release(&g_xhci_lock);
}

void xhci_cmd_tree(void) {
    if (!g_xhci_present) {
        if (xhci_init() != 0) {
            cprintf("usb tree: xHCI not available\n");
            return;
        }
    }

    cprintf("xHCI USB Device Tree:\n");
    cprintf("/:  Bus 01.Port 1: Dev 1, Class=root_hub, xHCI Root Hub, 5 Ports\n");

    ylock_acquire(&g_xhci_lock);
    int count = 0;
    for (uint8_t s = 1; s <= XHCI_MAX_SLOTS; s++) {
        if (!g_devices[s].present || g_devices[s].parent_slot != 0) continue;
        count++;

        const char *class_str = (g_devices[s].is_hub) ? "Hub" :
                                (g_devices[s].has_hid_keyboard) ? "HID (Keyboard)" :
                                (g_devices[s].has_hid_mouse)    ? "HID (Mouse)" :
                                (g_devices[s].class_code == 3)  ? "HID" : "Device";

        const char *drv_str = (g_devices[s].is_hub) ? "hub" :
                              (g_devices[s].class_code == 3) ? "usbhid" : "usb";

        cprintf("    |__ Port %u: Dev %u, If 0, Class=%s, Driver=%s, %04x:%04x\n",
                (unsigned)g_devices[s].port_num, (unsigned)s,
                class_str, drv_str, (unsigned)g_devices[s].vid, (unsigned)g_devices[s].pid);

        /* Print downstream devices behind this hub */
        if (g_devices[s].is_hub) {
            for (uint8_t c = 1; c <= XHCI_MAX_SLOTS; c++) {
                if (!g_devices[c].present || g_devices[c].parent_slot != s) continue;
                count++;

                const char *c_class = (g_devices[c].has_hid_keyboard) ? "HID (Keyboard)" :
                                      (g_devices[c].has_hid_mouse)    ? "HID (Mouse)" :
                                      (g_devices[c].class_code == 3)  ? "HID" : "Device";

                const char *c_drv = (g_devices[c].class_code == 3) ? "usbhid" : "usb";

                cprintf("        |__ Port %u: Dev %u, If 0, Class=%s, Driver=%s, %04x:%04x\n",
                        (unsigned)g_devices[c].port_num, (unsigned)c,
                        c_class, c_drv, (unsigned)g_devices[c].vid, (unsigned)g_devices[c].pid);
            }
        }
    }
    ylock_release(&g_xhci_lock);

    if (count == 0) {
        cprintf("    (No devices currently enumerated)\n");
    }
}

void xhci_cmd_kbd(void) {
    if (!g_xhci_present) {
        cprintf("kbd: xHCI controller not present\n");
        return;
    }
    ylock_acquire(&g_xhci_lock);
    bool online = g_kbd_online;
    uint8_t slot = g_kbd_slot;
    uint16_t vid = online ? g_devices[slot].vid : 0;
    uint16_t pid = online ? g_devices[slot].pid : 0;
    ylock_release(&g_xhci_lock);

    cprintf("USB HID Keyboard Status:\n");
    cprintf("  State:       %s\n", online ? "ONLINE" : "No keyboard detected");
    if (online) {
        cprintf("  Device Slot: Slot %u (VID=%04x PID=%04x)\n", (unsigned)slot, vid, pid);
        cprintf("  Protocol:    Boot Protocol (8-byte reports)\n");
        cprintf("  Modifiers:   0x%02x (Shift:%u Ctrl:%u Alt:%u Super:%u)\n",
                g_kbd_xl.mods,
                (g_kbd_xl.mods & 0x22) ? 1 : 0,
                (g_kbd_xl.mods & 0x11) ? 1 : 0,
                (g_kbd_xl.mods & 0x44) ? 1 : 0,
                (g_kbd_xl.mods & 0x88) ? 1 : 0);
        char hint[16];
        usbkbd_compose_hint(&g_kbd_xl, hint, sizeof(hint));
        cprintf("  Compose:     %s (state=%u)\n", hint[0] ? hint : "inactive", g_kbd_xl.compose);
        cprintf("  Typematic:   delay=%u ms, repeat=%u ms\n", USBKBD_REPEAT_DELAY, USBKBD_REPEAT_EVERY);
    }
}

void xhci_cmd_mouse(const char *arg) {
    if (!g_xhci_present) {
        cprintf("mouse: xHCI controller not present\n");
        return;
    }
    ylock_acquire(&g_xhci_lock);
    bool online = g_mouse_online;
    uint8_t slot = g_mouse_slot;
    uint16_t vid = online ? g_devices[slot].vid : 0;
    uint16_t pid = online ? g_devices[slot].pid : 0;
    int x = g_mouse_x;
    int y = g_mouse_y;
    uint8_t btns = g_mouse_buttons;
    int wheel = g_mouse_wheel;
    ylock_release(&g_xhci_lock);

    cprintf("USB HID Mouse Status:\n");
    cprintf("  State:       %s\n", online ? "ONLINE" : "No mouse detected");
    if (!online) return;
    cprintf("  Device Slot: Slot %u (VID=%04x PID=%04x)\n", (unsigned)slot, vid, pid);
    cprintf("  Coordinates: X=%d Y=%d | Buttons: 0x%02x | Wheel: %d\n", x, y, btns, wheel);

    if (arg && strcmp(arg, "test") == 0) {
        cprintf("\nInteractive Mouse Test (press any key to exit)...\n");
        while (!console_has_char()) {
            xhci_poll();
            char left   = (g_mouse_buttons & 1u) ? 'L' : ' ';
            char middle = (g_mouse_buttons & 4u) ? 'M' : ' ';
            char right  = (g_mouse_buttons & 2u) ? 'R' : ' ';

            cprintf("\rMouse: X=%4d Y=%4d | Buttons: [%c%c%c] | Wheel: %4d ",
                    g_mouse_x, g_mouse_y, left, middle, right, g_mouse_wheel);
            time_delay_us(40000); /* 25 Hz update */
        }
        (void)console_getc();
        cprintf("\n");
    }
}
