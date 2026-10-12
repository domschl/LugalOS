# LugalOS Driver Framework & Hardware Isolation

In traditional monolithic operating systems, device drivers execute in the same privileged address space as the kernel. A single bug or stray pointer in a third-party driver can corrupt kernel data structures and crash the entire system.

In LugalOS, **every driver runs as an independent User-Mode task**, strictly confined to a hardware-enforced memory domain.

---

## 1. Confined U-Mode Driver Tasks

Each driver in LugalOS is structured as a dedicated task:
* **UART Driver (`uart_task`)**: Confined to UART0 register window and its circular FIFO buffer.
* **USB CDC ACM Driver (`usb_cdc_task`)**: Confined to USB controller registers and DPRAM.
* **Block Storage Driver (`blk_task`)**: Confined to SPI / SDMMC peripheral registers.
* **I2C Bus Driver (`i2c_task`)**: Manages the shared I2C bus for RTC and environmental sensors.
* **Display Driver (`lcd_task`)**: Confined to display DMA descriptors and framebuffer memory.

```
┌────────────────────────────────────────────────────────────────────────┐
│                   ISOLATED DRIVER DOMAINS (U-MODE)                     │
├────────────────────┬────────────────────┬──────────────────────────────┤
│    UART DRIVER     │     I2C DRIVER     │         STORAGE DRIVER       │
│  PMP: UART0 Window │  PMP: I2C0 Window  │      PMP: SPI1 / SDMMC Window│
│  State: Private    │  State: Private    │      State: Private          │
└─────────┬──────────┴─────────┬──────────┴──────────────┬───────────────┘
          │ chan_call()        │ chan_call()             │ chan_call()
┌─────────▼────────────────────▼─────────────────────────▼───────────────┐
│                       LUGALOS MICROKERNEL CORE                         │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Hardware Register Windows (PMP Grants)

During task initialization, the driver framework grants the task access only to the physical registers it requires:

```c
mem_domain_t *dom = mem_domain_create();
/* Grant code and private stack */
mem_domain_grant_region(dom, code_start, code_size, PERM_RX);
mem_domain_grant_region(dom, stack_start, stack_size, PERM_RW);
/* Grant only the specific peripheral register window */
mem_domain_grant_device(dom, UART0_BASE, UART0_SIZE, PERM_RW);
task_set_domain(driver_task, dom);
```

### Verification via Isolation Self-Tests
To prove that hardware confinement is real rather than assumed, every driver includes a dedicated `<driver>isotest` command (e.g. `uartisotest`, `blkisotest`, `i2cisotest`, `usbisotest`):
* The test deliberately causes the driver task to store into kernel memory outside its grant.
* Hardware raises an Access Fault trap immediately.
* The test verifies that a canary in kernel memory remains completely untouched.

---

## 3. Interrupt Handling & Event Notification

Hardware interrupts (IRQs) are handled via a two-stage model:
1. **Top Half (Kernel Trap Handler)**: Acknowledges the hardware interrupt, masks the line if necessary, and unblocks the driver's task queue or posts an event.
2. **Bottom Half (U-Mode Driver Task)**: The driver task wakes up in its isolated memory domain, drains hardware FIFOs, and handles the request asynchronously.
