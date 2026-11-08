#include "drivers/pcie_jh7110.h"
#include "kernel/printk.h"
#include "kernel/time.h"
#include <string.h>

#define SYS_CRG_BASE        0x13020000UL
#define STG_CRG_BASE        0x10230000UL
#define STG_SYSCON_BASE     0x10240000UL
#define SYS_GPIO_BASE       0x13040000UL

/* PLDA XpressRICH3 controller registers */
#define PLDA_GEN_SETTINGS   0x80u
#define PLDA_PCIE_PCI_IDS   0x9Cu
#define PLDA_PCI_MISC       0xB4u
#define PLDA_PCIE_WINROM    0xFCu
#define PLDA_PMSG_RX        0x3F0u

#define PLDA_RP_ENABLE      (1u << 0)
#define PLDA_FUNCTION_DIS   (1u << 15)
#define PLDA_WINROM_64BIT   (1u << 3)
#define PLDA_PMSG_LTR       (1u << 2)

/* Address Translation (ATR) registers */
#define XR3PCI_ATR_BASE     0x800u
#define XR3PCI_ATR_STRIDE   0x20u
#define XR3PCI_ATR_SRC_LO   0x00u
#define XR3PCI_ATR_SRC_HI   0x04u
#define XR3PCI_ATR_TRSL_LO  0x08u
#define XR3PCI_ATR_TRSL_HI  0x0Cu
#define XR3PCI_ATR_PARAM    0x10u

#define XR3PCI_ATR_PARAM_MEM   0u
#define XR3PCI_ATR_PARAM_CFG   1u

/* PCI Standard Registers */
#define PCI_REG_VID_DID     0x00u
#define PCI_REG_COMMAND     0x04u
#define PCI_REG_REV_CLASS   0x08u
#define PCI_REG_HDR_TYPE    0x0Eu
#define PCI_REG_BAR0        0x10u

#define PCI_CMD_IO          (1u << 0)
#define PCI_CMD_MEM         (1u << 1)
#define PCI_CMD_BUS_MASTER  (1u << 2)

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* single task: driver initialization and shell diagnostics; guarded by: lockless read after init */
static jh7110_pcie_port_t g_pcie_ports[JH7110_PCIE_PORT_COUNT];
static bool g_pcie_initialized = false;

/* GPIO helper for StarFive JH7110 SYS GPIO */
static void jh7110_gpio_set_output(uint32_t gpio, int value) {
    if (gpio >= 64) return;
    volatile uint32_t *doen = (volatile uint32_t *)(SYS_GPIO_BASE + (gpio & ~3u));
    volatile uint32_t *dout = (volatile uint32_t *)(SYS_GPIO_BASE + 0x40u + (gpio & ~3u));
    uint32_t shift = (gpio & 3u) * 8u;

    uint32_t v = *dout;
    v &= ~(0x7fu << shift);
    v |= ((uint32_t)(value ? 1 : 0)) << shift;
    *dout = v;

    v = *doen;
    v &= ~(0x3fu << shift);
    *doen = v;
}

static void jh7110_pcie_clock_reset_init(int port) {
    /* 1. Enable NOC_BUS_CLK_STG_AXI in SYS_CRG */
    REG32(SYS_CRG_BASE + 0x180) |= (1u << 31);

    /* 2. Enable port clocks in STG_CRG */
    if (port == 0) {
        REG32(STG_CRG_BASE + 0x20) |= (1u << 31); /* u0_clk_pcie_axi_mst0 */
        REG32(STG_CRG_BASE + 0x24) |= (1u << 31); /* u0_clk_pcie_apb */
        REG32(STG_CRG_BASE + 0x28) |= (1u << 31); /* u0_clk_pcie_tl */
    } else {
        REG32(STG_CRG_BASE + 0x2C) |= (1u << 31); /* u1_clk_pcie_axi_mst0 */
        REG32(STG_CRG_BASE + 0x30) |= (1u << 31); /* u1_clk_pcie_apb */
        REG32(STG_CRG_BASE + 0x34) |= (1u << 31); /* u1_clk_pcie_tl */
    }
    REG32(STG_CRG_BASE + 0x38) |= (1u << 31);     /* clk_pcie_01_slv_dec_main */

    /* 3. Deassert STGCRG resets: bits 11..16 for PCIe0, 17..22 for PCIe1 */
    uint32_t rst_mask = (port == 0) ? (0x3Fu << 11) : (0x3Fu << 17);
    REG32(STG_CRG_BASE + 0x74) &= ~rst_mask;
    uint64_t t0 = time_get_us();
    while ((REG32(STG_CRG_BASE + 0x78) & rst_mask) != rst_mask) {
        if (time_get_us() - t0 > 50000u) {
            printk("[PCIe%d] Warning: reset deassert timeout (status=0x%x)\n",
                   port, REG32(STG_CRG_BASE + 0x78));
            break;
        }
    }
}

static void jh7110_pcie_set_atr(uintptr_t reg_base, int table, uint64_t src,
                                uint64_t trsl, uint32_t log2_size, uint32_t param) {
    uintptr_t base = reg_base + XR3PCI_ATR_BASE + (uint32_t)table * XR3PCI_ATR_STRIDE;
    uint32_t src_lo = ((uint32_t)src & 0xFFFFF000u) | (log2_size << 1) | 1u;

    REG32(base + XR3PCI_ATR_SRC_LO)  = src_lo;
    REG32(base + XR3PCI_ATR_SRC_HI)  = (uint32_t)(src >> 32);
    REG32(base + XR3PCI_ATR_TRSL_LO) = (uint32_t)trsl & 0xFFFFF000u;
    REG32(base + XR3PCI_ATR_TRSL_HI) = (uint32_t)(trsl >> 32);
    REG32(base + XR3PCI_ATR_PARAM)   = param;
}

static int jh7110_pcie_bringup_port(int port) {
    jh7110_pcie_port_t *p = &g_pcie_ports[port];
    p->port_id = (uint8_t)port;
    p->reg_base = (port == 0) ? JH7110_PCIE0_BASE : JH7110_PCIE1_BASE;
    p->cfg_base = (port == 0) ? JH7110_PCIE0_CFG_BASE : JH7110_PCIE1_CFG_BASE;
    p->mem32_base = (port == 0) ? JH7110_PCIE0_MEM32_BASE : JH7110_PCIE1_MEM32_BASE;
    p->mem32_size = (port == 0) ? JH7110_PCIE0_MEM32_SIZE : JH7110_PCIE1_MEM32_SIZE;
    p->mem32_alloc = p->mem32_base;
    p->link_up = false;
    p->device_count = 0;

    /* 1. Power on device & assert reset */
    if (port == 0) {
        jh7110_gpio_set_output(JH7110_PCIE0_GPIO_PWREN, 1); /* VL805 3.3V power */
        jh7110_gpio_set_output(JH7110_PCIE0_GPIO_VBUS, 1);  /* USB Type-A 5V VBUS power */
        jh7110_gpio_set_output(JH7110_PCIE0_GPIO_PERST, 0); /* PERST# assert */
    } else {
        jh7110_gpio_set_output(JH7110_PCIE1_GPIO_PWREN, 1); /* M.2 U24 3.3V power */
        jh7110_gpio_set_output(JH7110_PCIE1_GPIO_PERST, 0); /* PERST# assert */
    }

    /* 2. Clock and reset setup */
    jh7110_pcie_clock_reset_init(port);

    /* 3. STG SYSCON configuration */
    uint32_t rp_nep_off = (port == 0) ? 0x130u : 0x2E0u;
    uint32_t awfun_off  = (port == 0) ? 0x0C4u : 0x274u;
    uint32_t arfun_off  = (port == 0) ? 0x0C0u : 0x270u;
    uint32_t lnksta_off = (port == 0) ? 0x1B8u : 0x368u;

    /* Set Root Port mode */
    REG32(STG_SYSCON_BASE + rp_nep_off) |= (1u << 8);

    /* RefClk source = 2, CLKREQ enable = 1 */
    uint32_t v = REG32(STG_SYSCON_BASE + awfun_off);
    v &= ~(0xC0000u | 0x400000u);
    v |= (2u << 18) | (1u << 22);
    REG32(STG_SYSCON_BASE + awfun_off) = v;

    /* Disable physical functions 1..3 */
    for (uint32_t i = 1; i < 4; i++) {
        v = REG32(STG_SYSCON_BASE + arfun_off);
        v &= ~0x7FFF00u;
        v |= ((i << 9) << 8);
        REG32(STG_SYSCON_BASE + arfun_off) = v;

        v = REG32(STG_SYSCON_BASE + awfun_off);
        v &= ~0x7FFFu;
        v |= (i << 9);
        REG32(STG_SYSCON_BASE + awfun_off) = v;

        REG32(p->reg_base + PLDA_PCI_MISC) |= PLDA_FUNCTION_DIS;
    }
    /* Restore func 0 */
    REG32(STG_SYSCON_BASE + arfun_off) &= ~0x7FFF00u;
    REG32(STG_SYSCON_BASE + awfun_off) &= ~0x7FFFu;

    /* 4. PLDA Core configuration */
    REG32(p->reg_base + PLDA_GEN_SETTINGS) |= PLDA_RP_ENABLE;
    REG32(p->reg_base + PLDA_PCIE_PCI_IDS) = 0x06040002u; /* Bridge class 0x060400, Rev 0x02 */
    REG32(p->reg_base + PLDA_PMSG_RX) &= ~PLDA_PMSG_LTR;  /* Disable LTR forwarding */
    REG32(p->reg_base + PLDA_PCIE_WINROM) |= PLDA_WINROM_64BIT;

    /* 5. Address Translation (ATR) Tables */
    /* Table 0: ECAM configuration space (256 MiB = 2^28) */
    jh7110_pcie_set_atr(p->reg_base, 0, p->cfg_base, 0, 28, XR3PCI_ATR_PARAM_CFG);

    /* Table 1: 32-bit Memory space (128 MiB = 2^27) */
    jh7110_pcie_set_atr(p->reg_base, 1, p->mem32_base, p->mem32_base, 27, XR3PCI_ATR_PARAM_MEM);

    /* Table 2: 64-bit Memory space (1 GiB = 2^30) */
    uint64_t mem64 = (port == 0) ? JH7110_PCIE0_MEM64_BASE : JH7110_PCIE1_MEM64_BASE;
    jh7110_pcie_set_atr(p->reg_base, 2, mem64, mem64, 30, XR3PCI_ATR_PARAM_MEM);

    /* 6. Hold PERST# low for 100 ms, then deassert */
    time_delay_us(100000);
    if (port == 0) {
        jh7110_gpio_set_output(JH7110_PCIE0_GPIO_PERST, 1);
    } else {
        jh7110_gpio_set_output(JH7110_PCIE1_GPIO_PERST, 1);
    }

    /* 7. Poll link training status (bit 5: PLDA_DATA_LINK_ACTIVE) */
    uint64_t t_link = time_get_us();
    while ((REG32(STG_SYSCON_BASE + lnksta_off) & 0x20u) == 0) {
        if (time_get_us() - t_link > 200000u) {
            printk("[PCIe%d] Link down (timeout 200 ms, lnksta=0x%x)\n",
                   port, REG32(STG_SYSCON_BASE + lnksta_off));
            p->link_up = false;
            return -1;
        }
    }

    /* Link is up! Mandatory 100 ms reset recovery delay before config requests */
    p->link_up = true;
    p->gen = 2;   /* Gen2 5.0 GT/s */
    p->lanes = 1; /* x1 */
    time_delay_us(100000);

    /* Configure Root Port Bridge (Bus 0 Dev 0 Func 0) */
    /* 1. Bus Numbers: Primary = 0, Secondary = 1, Subordinate = 0xFF */
    uint32_t bus_cfg = (0x00u) |        /* Primary Bus Number = 0 */
                       (0x01u << 8) |   /* Secondary Bus Number = 1 */
                       (0xFFu << 16);   /* Subordinate Bus Number = 0xFF */
    jh7110_pcie_cfg_write(port, 0, 0, 0, 0x18, bus_cfg, 4);

    /* 2. Memory Base & Limit (1 MB granularity, bits [31:20] shifted to [15:4]) */
    uint16_t mem_base = (uint16_t)((p->mem32_base >> 16) & 0xFFF0u);
    uint16_t mem_limit = (uint16_t)(((p->mem32_base + p->mem32_size - 1) >> 16) & 0xFFF0u);
    jh7110_pcie_cfg_write(port, 0, 0, 0, 0x20, (uint32_t)mem_base | ((uint32_t)mem_limit << 16), 4);

    /* 3. Enable Bus Mastering, Memory Space, and I/O Space in Command register */
    uint32_t cmd = jh7110_pcie_cfg_read(port, 0, 0, 0, PCI_REG_COMMAND, 2);
    cmd |= (PCI_CMD_IO | PCI_CMD_MEM | PCI_CMD_BUS_MASTER);
    jh7110_pcie_cfg_write(port, 0, 0, 0, PCI_REG_COMMAND, cmd, 2);

    printk("[PCIe%d] Link UP: Gen%d x%d (lnksta=0x%x, bridge bus=0x%06x mem=0x%04x..0x%04x cmd=0x%04x)\n",
           port, p->gen, p->lanes, REG32(STG_SYSCON_BASE + lnksta_off),
           jh7110_pcie_cfg_read(port, 0, 0, 0, 0x18, 4) & 0xFFFFFFu,
           mem_base, mem_limit,
           jh7110_pcie_cfg_read(port, 0, 0, 0, PCI_REG_COMMAND, 2) & 0xFFFFu);

    return 0;
}

uint32_t jh7110_pcie_cfg_read(int port, int bus, int dev, int fn, int offset, int size) {
    if (port < 0 || port >= JH7110_PCIE_PORT_COUNT) return 0xFFFFFFFFu;
    jh7110_pcie_port_t *p = &g_pcie_ports[port];
    if (!p->link_up && bus > 0) return 0xFFFFFFFFu;

    /* On root bus (0) and link bus (1), only device 0 exists */
    if (bus <= 1 && dev > 0) return 0xFFFFFFFFu;

    uintptr_t addr = p->cfg_base + (((uintptr_t)bus << 20) |
                                    ((uintptr_t)dev << 15) |
                                    ((uintptr_t)fn << 12) |
                                    ((uintptr_t)offset & ~3u));

    uint32_t val = REG32(addr);
    if (size == 1) {
        return (val >> ((offset & 3) * 8)) & 0xFFu;
    } else if (size == 2) {
        return (val >> ((offset & 2) * 8)) & 0xFFFFu;
    }
    return val;
}

void jh7110_pcie_cfg_write(int port, int bus, int dev, int fn, int offset, uint32_t val, int size) {
    if (port < 0 || port >= JH7110_PCIE_PORT_COUNT) return;
    jh7110_pcie_port_t *p = &g_pcie_ports[port];
    if (!p->link_up && bus > 0) return;
    if (bus <= 1 && dev > 0) return;

    uintptr_t addr = p->cfg_base + (((uintptr_t)bus << 20) |
                                    ((uintptr_t)dev << 15) |
                                    ((uintptr_t)fn << 12) |
                                    ((uintptr_t)offset & ~3u));

    if (size == 4) {
        REG32(addr) = val;
    } else if (size == 2) {
        uint32_t shift = (offset & 2) * 8;
        uint32_t cur = REG32(addr);
        cur &= ~(0xFFFFu << shift);
        cur |= (val & 0xFFFFu) << shift;
        REG32(addr) = cur;
    } else if (size == 1) {
        uint32_t shift = (offset & 3) * 8;
        uint32_t cur = REG32(addr);
        cur &= ~(0xFFu << shift);
        cur |= (val & 0xFFu) << shift;
        REG32(addr) = cur;
    }
}

int jh7110_pcie_scan(int port) {
    if (port < 0 || port >= JH7110_PCIE_PORT_COUNT) return -1;
    jh7110_pcie_port_t *p = &g_pcie_ports[port];
    p->device_count = 0;
    p->mem32_alloc = p->mem32_base;

    for (int bus = 0; bus <= 1; bus++) {
        for (int dev = 0; dev <= 0; dev++) {
            for (int fn = 0; fn < 8; fn++) {
                if (bus == 1 && fn == 0) {
                    time_delay_us(20000); /* 20 ms delay before querying downstream endpoint */
                }
                uint32_t id = jh7110_pcie_cfg_read(port, bus, dev, fn, PCI_REG_VID_DID, 4);
                if (id == 0xFFFFFFFFu || id == 0x00000000u) {
                    if (fn == 0) break; /* Not a multi-function device */
                    continue;
                }

                if (p->device_count >= PCI_MAX_DEVICES_PER_PORT) break;
                pci_device_t *d = &p->devices[p->device_count++];
                memset(d, 0, sizeof(*d));
                d->port = (uint8_t)port;
                d->bus = (uint8_t)bus;
                d->dev = (uint8_t)dev;
                d->func = (uint8_t)fn;
                d->vendor_id = (uint16_t)(id & 0xFFFFu);
                d->device_id = (uint16_t)(id >> 16);

                uint32_t class_rev = jh7110_pcie_cfg_read(port, bus, dev, fn, PCI_REG_REV_CLASS, 4);
                d->revision   = (uint8_t)(class_rev & 0xFFu);
                d->prog_if    = (uint8_t)((class_rev >> 8) & 0xFFu);
                d->sub_class  = (uint8_t)((class_rev >> 16) & 0xFFu);
                d->base_class = (uint8_t)((class_rev >> 24) & 0xFFu);

                uint8_t hdr = (uint8_t)jh7110_pcie_cfg_read(port, bus, dev, fn, PCI_REG_HDR_TYPE, 1);
                d->is_bridge = ((hdr & 0x7Fu) == 0x01u);

                /* Size and assign BARs for endpoints */
                if (!d->is_bridge) {
                    for (int b = 0; b < PCI_MAX_BARS; b++) {
                        uint32_t bar_off = PCI_REG_BAR0 + b * 4;
                        uint32_t orig = jh7110_pcie_cfg_read(port, bus, dev, fn, bar_off, 4);
                        jh7110_pcie_cfg_write(port, bus, dev, fn, bar_off, 0xFFFFFFFFu, 4);
                        uint32_t mask = jh7110_pcie_cfg_read(port, bus, dev, fn, bar_off, 4);
                        jh7110_pcie_cfg_write(port, bus, dev, fn, bar_off, orig, 4);

                        if (mask == 0 || mask == 0xFFFFFFFFu) continue;

                        d->bars[b].is_io = (mask & 1u) != 0;
                        d->bars[b].is_64bit = (!d->bars[b].is_io && ((mask & 0x6u) == 0x4u));
                        d->bars[b].prefetch = (!d->bars[b].is_io && ((mask & 0x8u) != 0));

                        uint32_t size_mask = d->bars[b].is_io ? (mask & ~3u) : (mask & ~0xFu);
                        uint64_t size = (~size_mask) + 1u;
                        d->bars[b].size = size;

                        /* Allocate from 32-bit window with natural alignment */
                        p->mem32_alloc = (p->mem32_alloc + ((uint32_t)size - 1)) & ~((uint32_t)size - 1);
                        uint32_t assigned = p->mem32_alloc;
                        p->mem32_alloc += (uint32_t)size;

                        d->bars[b].base = assigned;
                        jh7110_pcie_cfg_write(port, bus, dev, fn, bar_off, assigned, 4);

                        if (d->bars[b].is_64bit && b + 1 < PCI_MAX_BARS) {
                            b++;
                            jh7110_pcie_cfg_write(port, bus, dev, fn, PCI_REG_BAR0 + b * 4, 0, 4);
                        }
                    }

                    /* Enable Bus Master and Memory Space */
                    uint32_t cmd = jh7110_pcie_cfg_read(port, bus, dev, fn, PCI_REG_COMMAND, 2);
                    cmd |= (PCI_CMD_MEM | PCI_CMD_BUS_MASTER);
                    jh7110_pcie_cfg_write(port, bus, dev, fn, PCI_REG_COMMAND, cmd, 2);
                }

                /* Check if single-function */
                if (fn == 0 && (hdr & 0x80u) == 0) break;
            }
        }
    }

    return p->device_count;
}

int jh7110_pcie_init(void) {
    if (g_pcie_initialized) return 0;

    printk("[PCIe] Initializing StarFive JH7110 PCIe Root Complexes...\n");
    for (int port = 0; port < JH7110_PCIE_PORT_COUNT; port++) {
        jh7110_pcie_bringup_port(port);
        if (g_pcie_ports[port].link_up) {
            jh7110_pcie_scan(port);
        }
    }

    g_pcie_initialized = true;
    return 0;
}

bool jh7110_pcie_link_up(int port) {
    if (port < 0 || port >= JH7110_PCIE_PORT_COUNT) return false;
    return g_pcie_ports[port].link_up;
}

const jh7110_pcie_port_t *jh7110_pcie_get_port(int port) {
    if (port < 0 || port >= JH7110_PCIE_PORT_COUNT) return NULL;
    return &g_pcie_ports[port];
}

const pci_device_t *jh7110_pcie_find_device(uint16_t vendor_id, uint16_t device_id) {
    for (int p = 0; p < JH7110_PCIE_PORT_COUNT; p++) {
        const jh7110_pcie_port_t *port = &g_pcie_ports[p];
        for (int d = 0; d < port->device_count; d++) {
            if (port->devices[d].vendor_id == vendor_id &&
                port->devices[d].device_id == device_id) {
                return &port->devices[d];
            }
        }
    }
    return NULL;
}

const pci_device_t *jh7110_pcie_find_class(uint8_t base_class, uint8_t sub_class) {
    for (int p = 0; p < JH7110_PCIE_PORT_COUNT; p++) {
        const jh7110_pcie_port_t *port = &g_pcie_ports[p];
        for (int d = 0; d < port->device_count; d++) {
            if (port->devices[d].base_class == base_class &&
                port->devices[d].sub_class == sub_class) {
                return &port->devices[d];
            }
        }
    }
    return NULL;
}

static const char *pci_class_name(uint8_t base, uint8_t sub) {
    if (base == 0x01) {
        if (sub == 0x08) return "Non-Volatile Memory (NVMe)";
        if (sub == 0x01) return "IDE Controller";
        if (sub == 0x06) return "SATA Controller";
        return "Mass Storage Controller";
    }
    if (base == 0x02) return "Network Controller";
    if (base == 0x03) return "Display Controller";
    if (base == 0x06) {
        if (sub == 0x04) return "PCI-to-PCI Bridge";
        return "Bridge Device";
    }
    if (base == 0x0C) {
        if (sub == 0x03) return "USB Controller (xHCI/EHCI)";
        return "Serial Bus Controller";
    }
    return "Unknown Device";
}

void pcie_cmd_summary(void) {
    printk("StarFive JH7110 PCIe Root Complexes:\n");
    for (int p = 0; p < JH7110_PCIE_PORT_COUNT; p++) {
        const jh7110_pcie_port_t *port = &g_pcie_ports[p];
        printk("  pcie%d: %s (Gen%d x%d), %d device(s) found, mem32: 0x%08x..0x%08x\n",
               p, port->link_up ? "LINK UP" : "LINK DOWN",
               port->gen, port->lanes, port->device_count,
               port->mem32_base, port->mem32_alloc);
    }
}

void pcie_cmd_scan(void) {
    printk("PCIe Bus Scan:\n");
    printk("Port Bus:Dev.Fn  Vendor:Device  Class  Description\n");
    printk("---- ----------  -------------  -----  --------------------------------\n");
    for (int p = 0; p < JH7110_PCIE_PORT_COUNT; p++) {
        const jh7110_pcie_port_t *port = &g_pcie_ports[p];
        if (!port->link_up) {
            printk("[%d]  --:--.-      ----:----      --     (Port Link Down)\n", p);
            continue;
        }
        for (int i = 0; i < port->device_count; i++) {
            const pci_device_t *d = &port->devices[i];
            printk("[%d]  %02x:%02x.%d      %04x:%04x      %02x%02x   %s\n",
                   p, d->bus, d->dev, d->func,
                   d->vendor_id, d->device_id,
                   d->base_class, d->sub_class,
                   pci_class_name(d->base_class, d->sub_class));
            for (int b = 0; b < PCI_MAX_BARS; b++) {
                if (d->bars[b].size > 0) {
                    printk("      BAR%d: 0x%lx (size %lu KB%s%s)\n",
                           b, (unsigned long)d->bars[b].base,
                           (unsigned long)(d->bars[b].size / 1024),
                           d->bars[b].is_64bit ? ", 64-bit" : ", 32-bit",
                           d->bars[b].prefetch ? ", prefetch" : "");
                }
            }
        }
    }
}

void pcie_cmd_info(int port) {
    if (port < 0 || port >= JH7110_PCIE_PORT_COUNT) {
        printk("Invalid port index %d (valid: 0 or 1)\n", port);
        return;
    }
    const jh7110_pcie_port_t *p = &g_pcie_ports[port];
    printk("PCIe Port %d Information:\n", port);
    printk("  MMIO Register Base: 0x%lx\n", (unsigned long)p->reg_base);
    printk("  ECAM Config Base:   0x%lx\n", (unsigned long)p->cfg_base);
    printk("  32-bit Mem Window:  0x%08x - 0x%08x (allocated to 0x%08x)\n",
           p->mem32_base, p->mem32_base + p->mem32_size - 1, p->mem32_alloc);
    printk("  Link Status:        %s\n", p->link_up ? "LINK UP" : "LINK DOWN");
    if (p->link_up) {
        printk("  Negotiated Speed:   Gen%d (5.0 GT/s)\n", p->gen);
        printk("  Negotiated Width:   x%d\n", p->lanes);
    }
    printk("  Attached Devices:   %d\n", p->device_count);
}
