# ADR 007: Confining U-mode on the ESP32-C6 with the APM, and the Residue Behind the Factory Bootloader

## Status
Accepted (2026-11-01)

## Context
The ESP32-C6 runs the vendor's Wi-Fi code in U-mode, in a memory domain whose PMP entries grant it its own code and data, the chip's ROM, and five peripheral windows: the modem, PMU/LP_AON, PCR, UART0 and the SAR ADC (`kernel/radio_c6.c`). Behind LugalOS's own loader this is the whole story: no PMP entry is locked, and a U-mode read of anything else faults.

The C6s beside a P4 (the NANO's, the Tab5's) still boot through their factory ESP-IDF bootloader (older than v5.2.2). It leaves four PMP entries locked, and a locked entry applies to U-mode too, with the lowest-numbered matching entry deciding. Measured with `/proc/pmp` and a U-mode read probe (`kernel/umode_probe.c`):

* entry 0: read, write and execute on `0x20000000-0x2fffffff`, the CPU subsystem (the machine-mode PLIC and CLINT);
* entry 15: read and write on `0x60000000-0x600fffff`, every peripheral;
* entries 1 and 2: off, and the ROM read/execute.

A U-mode task with no grants read the PLIC, the interrupt matrix and GPIO on those boards. The kernel had also opened the APM's region 0 to U-mode (REE0) for the radio, so the locked entries were the only fence.

Entry 15 cannot be shadowed in the PMP: a deny entry would need the radio's windows as entries of their own, 14 entries where 12 are left. Entry 0 cannot be overridden at all. Only another bootloader removes both, and rewriting a C6's bootloader over SDIO has no fallback if it is cut short; the Tab5 is sealed, so its C6 is reachable no other way.

## Decision
1. **The APM confines U-mode on every C6.** For the CPU the APM filters the peripherals and LP memory (not HP SRAM or ROM), with per-mode permissions; overlapping regions add up. The HP APM's region 0 covers the whole address space for REE2 only (the modem's DMA and the other masters); region 1 is the SDIO slave's DMA (REE1); one region per radio window grants REE0 read and write. The LP APM does the same for the windows in the LP peripherals. One table (`k_radio_mmio`) feeds both the PMP domain and the APM regions, so the two fences grant the same thing.
2. **The CPU subsystem residue is accepted on the factory-bootloader C6s.** The PLIC and CLINT sit inside the CPU, behind no APM, and entry 0 grants them. On those boards the radio's code can mask or raise machine interrupts.
3. **The bootloader is not replaced over the air.** Over a cable, where the C6's pins are reachable (the NANO), it stays possible; it is not done as part of this decision.

## Consequences
* **Positive**: On every C6, U-mode reaches exactly the radio's five peripheral windows. Measured after the change: the interrupt matrix, GPIO, the CPU peripherals (INTPRI) and LP_IO read 0 from U-mode on the NANO's and the Tab5's C6, and the radio joins as before on all four C6s.
* **Positive**: Defence in depth behind LugalOS's loader too: a PMP mistake in a future domain is caught by the APM for the peripherals.
* **Negative**: On the NANO's and the Tab5's C6 a fault in the radio code can mask or raise machine interrupts: a denial of service (a hung or rebooting C6, which the P4 notices as a dead link), not a way into M-mode (`mtvec` and the PMP are CSRs, the kernel's RAM stays closed).
* **Negative**: An APM denial is silent: reads return 0, writes vanish, no fault. A radio window missing from the table shows up as a radio that misbehaves, not as a trap with an address; the U-mode probe's version registers make the fence visible in `/proc/pmp`.
