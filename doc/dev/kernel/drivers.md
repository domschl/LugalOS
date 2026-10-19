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

---

## 4. Calling a Driver Task

An endpoint serves one request at a time. A caller that finds it busy waits its turn with
`chan_call_wait(ep, req, len, resp, max, wait_ms)` (`kernel/chan.h`), or with
`driver_task_call()` / `driver_task_call_wait()` for a `driver_task_t`:
* Waiting callers are served in arrival order. The caller releasing the endpoint hands it
  straight to the next one, so a client issuing requests back to back cannot keep it from a
  client that was already waiting.
* `CHAN_WAIT_MS` (50 ms) is the usual bound; block devices use 500 ms, the local 9P link 1 s.
* A context re-entering an endpoint it holds is refused at once rather than queued behind
  itself. So is a caller holding a spinlock, or one in an interrupt handler.
* Plain `chan_call()` never waits. It is for the few callers that must not queue, such as a
  UART writer that would otherwise wait behind a READ waiting for a key.

**A facade touches the hardware itself only when the task is not running**
(`driver_task_alive()` false: during boot, or if the task never started). If the call fails
while the task is alive, the facade reports an error (or drops a display update). The task may
still be driving the hardware, and a second access beside it collides on the bus.

Timed waits on hardware read the clock *before* the condition, so the last test always comes
after the deadline (`time_poll_until_us()`, `kernel/time.h`). In the other order, a task
preempted past the deadline gives up on a reading taken before the preemption.

`chanqtest` checks the queue under real tasks; `i2cstats` counts I2C failures by cause.
