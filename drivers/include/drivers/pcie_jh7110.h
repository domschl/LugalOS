#ifndef LUGALOS_DRIVERS_PCIE_JH7110_H
#define LUGALOS_DRIVERS_PCIE_JH7110_H

#include <stdbool.h>
#include <stdint.h>

#define JH7110_PCIE_PORT_COUNT  2

/* Base physical addresses for JH7110 PCIe Root Complexes */
#define JH7110_PCIE0_BASE       0x2B000000UL
#define JH7110_PCIE1_BASE       0x2C000000UL

/* ECAM Configuration Spaces (256 MiB each, mapped in Sv39) */
#define JH7110_PCIE0_CFG_BASE   0x940000000ULL
#define JH7110_PCIE1_CFG_BASE   0x9C0000000ULL

/* 32-bit Memory Windows (128 MiB each) */
#define JH7110_PCIE0_MEM32_BASE 0x30000000UL
#define JH7110_PCIE0_MEM32_SIZE 0x08000000UL

#define JH7110_PCIE1_MEM32_BASE 0x38000000UL
#define JH7110_PCIE1_MEM32_SIZE 0x08000000UL

/* 64-bit Memory Windows (1 GiB each) */
#define JH7110_PCIE0_MEM64_BASE 0x900000000ULL
#define JH7110_PCIE0_MEM64_SIZE 0x40000000ULL

#define JH7110_PCIE1_MEM64_BASE 0x980000000ULL
#define JH7110_PCIE1_MEM64_SIZE 0x40000000ULL

/* GPIO Pin Assignments */
#define JH7110_PCIE0_GPIO_VBUS  25  /* active high USB Type-A VBUS power */
#define JH7110_PCIE0_GPIO_PERST 26  /* active low reset */
#define JH7110_PCIE0_GPIO_PRSN  27  /* presence / clkreq */
#define JH7110_PCIE0_GPIO_PWREN 32  /* active high power */

#define JH7110_PCIE1_GPIO_PWREN 21  /* active high power to U24 SCT2250FPA */
#define JH7110_PCIE1_GPIO_PERST 28  /* active low reset */
#define JH7110_PCIE1_GPIO_PRSN  29  /* presence / clkreq */

/* Discovered PCI device descriptor */
#define PCI_MAX_BARS            6
#define PCI_MAX_DEVICES_PER_PORT 8

typedef struct {
    uint8_t  port;
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  base_class;
    uint8_t  sub_class;
    uint8_t  prog_if;
    uint8_t  revision;
    bool     is_bridge;
    struct {
        uint64_t base;
        uint64_t size;
        bool     is_io;
        bool     is_64bit;
        bool     prefetch;
    } bars[PCI_MAX_BARS];
} pci_device_t;

/* Controller state per Root Complex */
typedef struct {
    uint8_t      port_id;
    uintptr_t    reg_base;
    uintptr_t    cfg_base;
    uint32_t     mem32_base;
    uint32_t     mem32_size;
    uint32_t     mem32_alloc;
    bool         link_up;
    uint8_t      gen;           /* 1 or 2 */
    uint8_t      lanes;         /* 1 */
    uint8_t      device_count;
    pci_device_t devices[PCI_MAX_DEVICES_PER_PORT];
} jh7110_pcie_port_t;

/* Core driver API */
int  jh7110_pcie_init(void);
bool jh7110_pcie_link_up(int port);
int  jh7110_pcie_scan(int port);

uint32_t jh7110_pcie_cfg_read(int port, int bus, int dev, int fn, int offset, int size);
void     jh7110_pcie_cfg_write(int port, int bus, int dev, int fn, int offset, uint32_t val, int size);

const jh7110_pcie_port_t *jh7110_pcie_get_port(int port);
const pci_device_t       *jh7110_pcie_find_device(uint16_t vendor_id, uint16_t device_id);
const pci_device_t       *jh7110_pcie_find_class(uint8_t base_class, uint8_t sub_class);

/* Shell diagnostic helpers */
void pcie_cmd_summary(void);
void pcie_cmd_scan(void);
void pcie_cmd_info(int port);

#endif /* LUGALOS_DRIVERS_PCIE_JH7110_H */
