#ifndef LUGALOS_DRIVERS_XHCI_H
#define LUGALOS_DRIVERS_XHCI_H

#include <stdbool.h>
#include <stdint.h>

/* xHCI Capability Register Offsets (relative to BAR0) */
#define XHCI_CAP_CAPLENGTH          0x00u   /* 1 byte */
#define XHCI_CAP_HCIVERSION         0x02u   /* 2 bytes */
#define XHCI_CAP_HCSPARAMS1         0x04u   /* 4 bytes */
#define XHCI_CAP_HCSPARAMS2         0x08u   /* 4 bytes */
#define XHCI_CAP_HCSPARAMS3         0x0Cu   /* 4 bytes */
#define XHCI_CAP_HCCPARAMS1         0x10u   /* 4 bytes */
#define XHCI_CAP_DBOFF              0x14u   /* 4 bytes */
#define XHCI_CAP_RTSOFF             0x18u   /* 4 bytes */

/* xHCI Operational Register Offsets (relative to BAR0 + CAPLENGTH) */
#define XHCI_OP_USBCMD              0x00u
#define XHCI_OP_USBSTS              0x04u
#define XHCI_OP_PAGESIZE            0x08u
#define XHCI_OP_DNCTRL              0x14u
#define XHCI_OP_CRCR_LO             0x18u
#define XHCI_OP_CRCR_HI             0x1Cu
#define XHCI_OP_DCBAAP_LO           0x30u
#define XHCI_OP_DCBAAP_HI           0x34u
#define XHCI_OP_CONFIG              0x38u
#define XHCI_OP_PORTSC_BASE         0x400u  /* Port 1 starts here, stride 0x10 */

/* USBCMD bit definitions */
#define XHCI_CMD_RS                 (1u << 0)   /* Run / Stop */
#define XHCI_CMD_HCRST              (1u << 1)   /* Host Controller Reset */
#define XHCI_CMD_INTE               (1u << 2)   /* Interrupter Enable */
#define XHCI_CMD_HSEE               (1u << 3)   /* Host System Error Enable */

/* USBSTS bit definitions */
#define XHCI_STS_HCH                (1u << 0)   /* Host Controller Halted */
#define XHCI_STS_HSE                (1u << 2)   /* Host System Error */
#define XHCI_STS_EINT               (1u << 3)   /* Event Interrupt */
#define XHCI_STS_PCD                (1u << 4)   /* Port Change Detect */
#define XHCI_STS_CNR                (1u << 11)  /* Controller Not Ready */

/* PORTSC bit definitions */
#define XHCI_PORT_CCS               (1u << 0)   /* Current Connect Status */
#define XHCI_PORT_PED               (1u << 1)   /* Port Enabled / Disabled */
#define XHCI_PORT_OCA               (1u << 3)   /* Over-Current Active */
#define XHCI_PORT_PR                (1u << 4)   /* Port Reset */
#define XHCI_PORT_PLS_MASK          (0x0Fu << 5)/* Port Link State */
#define XHCI_PORT_PP                (1u << 9)   /* Port Power */
#define XHCI_PORT_SPEED_MASK        (0x0Fu << 10)
#define XHCI_PORT_SPEED_SHIFT       10
#define XHCI_PORT_CSC               (1u << 17)  /* Connect Status Change (W1C) */
#define XHCI_PORT_PEC               (1u << 18)  /* Port Enable Change (W1C) */
#define XHCI_PORT_WRC               (1u << 19)  /* Warm Port Reset Change (W1C) */
#define XHCI_PORT_OCC               (1u << 20)  /* Over-Current Change (W1C) */
#define XHCI_PORT_PRC               (1u << 21)  /* Port Reset Change (W1C) */
#define XHCI_PORT_PLC               (1u << 22)  /* Port Link State Change (W1C) */
#define XHCI_PORT_CEC               (1u << 23)  /* Config Error Change (W1C) */

/* Port Speeds */
#define XHCI_SPEED_FULL             1u  /* 12 Mbps (USB 2.0 / 1.1 Full) */
#define XHCI_SPEED_LOW              2u  /* 1.5 Mbps (USB 1.1 Low) */
#define XHCI_SPEED_HIGH             3u  /* 480 Mbps (USB 2.0 High) */
#define XHCI_SPEED_SUPER            4u  /* 5 Gbps (USB 3.0 SuperSpeed) */

/* TRB Types */
#define XHCI_TRB_NORMAL             1u
#define XHCI_TRB_SETUP              2u
#define XHCI_TRB_DATA               3u
#define XHCI_TRB_STATUS             4u
#define XHCI_TRB_LINK               6u
#define XHCI_TRB_ENABLE_SLOT        9u
#define XHCI_TRB_DISABLE_SLOT       10u
#define XHCI_TRB_ADDRESS_DEVICE     11u
#define XHCI_TRB_CONFIG_EP          12u
#define XHCI_TRB_EVAL_CTX           13u
#define XHCI_TRB_RESET_EP           14u
#define XHCI_TRB_STOP_EP            15u
#define XHCI_TRB_NOOP_CMD           23u
#define XHCI_TRB_TRANSFER_EVENT     32u
#define XHCI_TRB_CMD_COMPLETION     33u
#define XHCI_TRB_PORT_STATUS_CHANGE 34u

/* 16-byte Transfer Request Block (TRB) */
typedef struct __attribute__((packed)) {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

/* 16-byte Event Ring Segment Table (ERST) Entry */
typedef struct __attribute__((packed)) {
    uint64_t seg_base;
    uint32_t seg_size;
    uint32_t rsvd;
} xhci_erst_entry_t;

/* Standard USB Device Descriptor (18 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_desc_t;

/* Standard USB Configuration Descriptor (9 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_desc_t;

/* Standard USB Interface Descriptor (9 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} usb_interface_desc_t;

/* Standard USB Endpoint Descriptor (7 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_desc_t;

/* Standard USB Hub Descriptor (USB 2.0 §11.23.2.1) */
typedef struct __attribute__((packed)) {
    uint8_t  bDescLength;
    uint8_t  bDescriptorType;    /* 0x29 for Hub */
    uint8_t  bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t  bPwrOn2PwrGood;     /* in 2ms units */
    uint8_t  bHubContrCurrent;
    uint8_t  DeviceRemovable;    /* bitmap */
    uint8_t  PortPwrCtrlMask;    /* legacy */
} usb_hub_desc_t;

/* Enumerated Device Record */
typedef struct {
    bool     present;
    uint8_t  slot_id;
    uint8_t  port_num;          /* parent port number (1-based) */
    uint8_t  root_port;         /* root hub port (1-based) */
    uint8_t  parent_slot;       /* 0 if root hub */
    uint8_t  speed;
    uint16_t vid;
    uint16_t pid;
    uint8_t  class_code;
    uint8_t  subclass_code;
    uint8_t  protocol;
    bool     is_hub;
    uint8_t  hub_num_ports;
    char     product_name[32];
    /* Endpoint info for HID */
    bool     has_hid_keyboard;
    bool     has_hid_mouse;
    uint8_t  ep_in_addr;
    uint8_t  ep_in_ctx_idx;
    uint16_t ep_in_mps;
    uint8_t  ep_in_interval;
} xhci_device_t;

/* Driver public API */
int xhci_init(void);
bool xhci_is_present(void);
void xhci_poll(void);
bool xhci_is_kbd_present(void);
bool xhci_is_mouse_present(void);
void xhci_get_mouse(int *x, int *y, uint8_t *buttons, int *wheel);

/* Shell diagnostics */
void xhci_cmd_summary(void);
void xhci_cmd_ports(void);
void xhci_cmd_tree(void);
void xhci_cmd_kbd(void);
void xhci_cmd_mouse(const char *arg);

#endif /* LUGALOS_DRIVERS_XHCI_H */
