# Phase 28 — Ethernet on the P4, which is why the P4 is here

**Status: IN PROGRESS. Z0–Z3 done, 2026-09-12. PAUSED at Z4** for
`plan/phase32_esp32p4_execute_in_place.md` — the P4's heap reached its floor
and the fix is a phase of its own; see §Z3a. Prerequisites all met: phase 27 (the
platform) is COMPLETE, phase 30 (the driver framework) is COMPLETE, and
phase 31 is complete through Y5f (kernel logging cannot deadlock). The
ordering argument for putting both of those first is in
`plan/phase30_driver_framework.md` §0.4 and `plan/phase31_concurrency_hierarchy.md` §17;
this is the phase they were sequenced ahead of.

**Milestone letter: `Z`.** A–G, H–N, P–T and V–Y are spoken for across
`plan/`; O, U and Z are free, and `O` is unusable in a terminal next to a
zero. Phase 29 takes one of the remaining two.

## 0. Why this phase exists

`plan/phase27_esp32p4_bringup.md` §0 states the reason for buying this
silicon in one clause — *"the destination is a wired NTP server (phase 29),
and the reason to want the P4 at all is `SOC_EMAC_IEEE1588V2_SUPPORTED`"* —
and then spends a whole phase deliberately not touching it. §7 of that
document lists Ethernet under "Explicitly not in this phase" with the driest
line in the file: *"**Ethernet** — phase 28. The reason the P4 was chosen,
and still not now."*

Now.

### 0.1 What this phase is not

It is not a networking phase. It is a driver phase that happens to end with a
network, and the distinction decides its whole shape.

Phase 19 built an IP stack, and `CMakeLists.txt` compiles all of it into the
P4 image **today**: `net/netif.c`, `net/stack.c`, `net/arp.c`, `net/ipv4.c`,
`net/icmp.c`, `net/udp.c`, `net/tcp.c`, `net/ntp.c`, `net/ntp_server.c`,
`net/mqtt.c`, `net/mqttd.c`. The comments there say why — *"built for every
target ... a board with no interface registers none, and the registry is then
empty rather than absent"*. Boot the P4 and type `net` and it answers "no
network", which is a true statement made by working code.

So the deliverable is one `netif_t` — a name, a MAC, and four function
pointers — whose `poll`, `send_frame`, `recv_frame` and `link_up` reach the
P4's EMAC. `plan/hardware_seams.md` §2 already committed to this:

> `netif_register()` has taken the ENC28J60, the CYW43 and virtio-net
> unchanged, and phase 28 plugs the P4's EMAC into it unchanged. **Nothing is
> owed here.**

That sentence is a prediction, and this phase is the test of it. If phase 28
has to change `net/include/net/netif.h`, the seam was never general and
§2 was wrong — which is a finding worth having, recorded either way in Z7.

### 0.2 What is genuinely new, and it is exactly one thing

Three drivers have registered a `netif_t` before. None of them moved a byte
by DMA.

* The **ENC28J60** is a SPI part: `enc_send_frame()` shifts the frame out
  over a bus, one byte at a time, and the CPU has touched every one of them.
* The **CYW43** is the same story with a gSPI transaction around it.
* **virtio-net** has descriptor rings, but under QEMU, where the "device"
  reads guest memory through the emulator with no cache in the path.

The P4's EMAC has a bus-master DMA engine that reads and writes L2MEM while
the HP cores reach that same L2MEM **through L1 cache** — `soc_caps.h` says
`SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE 1`, and IDF wraps every descriptor and
buffer touch in `esp_cache_msync()` accordingly
(`components/esp_eth/src/mac/esp_eth_mac_esp_dma.c`, its `DMA_CACHE_WB` and
`DMA_CACHE_INVALIDATE` macros). **This kernel has no cache-maintenance code
at all today.** §3 is about that and nothing else, because it is the one part
of this phase that cannot be borrowed from an existing driver in this tree.

The P4 has already charged this project once for believing memory is what it
looks like: E7 spent a session on a heap that was handing out the L2 cache's
own storage, a bug whose signature was *"writes above `0x4ff80000` decay to
zero with no store to blame"*. Non-coherent DMA has the same signature, and
it is worth naming that in advance rather than rediscovering it.

### 0.3 The single new abstraction question, asked now

`plan/phase30_driver_framework.md` §2 deferred one extraction with a precise
trigger:

> **The waiter-slot/ISR pattern.** Tempting, and not yet. It looks identical
> in `uart_16550.c` and `uart_esp32p4.c`, but `uart_rp2350.c` serves TX only
> ... so there are two implementations of it, not three. **The rule says note
> it and wait.** Phase 28's Ethernet driver will be the third, and that is
> when to look again.

Z6 is where that gets looked at, and it is deliberately the *last* milestone
before the tests. Extracting a pattern from a driver that does not yet work
is how you get a shared abstraction shaped like a bug.

## 1. The hardware, with provenance for every number

Every line below was read out of the TRM, the NANO schematic, or ESP-IDF's
generated headers at `~/gith/esp/`. None was inferred from another Espressif
part. The rule is `plan/phase27_esp32p4_bringup.md` §3.2's, and it exists
because two clock-gate bits were nearly wrong in E2 by exactly that kind of
reasoning.

### 1.1 The MAC

| Fact | Value | Where it came from |
|---|---|---|
| Register base | `0x50098000` | `DR_REG_EMAC_BASE` = `DR_REG_HPPERIPH0_BASE` (`0x50000000`) + `0x98000`, `soc/esp32p4/register/hw_ver1/soc/reg_base.h` |
| TRM chapter | 55, p. 3223 | TRM contents |
| Structure | `EMAC_CORE` + `EMAC_MTL` + `EMAC_DMA` | TRM §55, "Ethernet MAC consists of three layers" — a Synopsys DesignWare core; the TRM's own register text says *"the DWC_EMAC subsystem"* |
| Interrupt source | **92** (`ETH_MAC_INTR`) | TRM Table 13.4-1, `COREx_ETH_MAC_INT_MAP_REG`. Neighbours: 89 `GMII_PHY_INTR`, 90 `LPI_INTR`, 91 `PMT_INTR`. Cross-checks against `soc/interrupts.h`'s enum, whose UART0 entry is 31 — the number `drivers/uart_esp32p4.c` already uses. |
| Register headers | `soc/esp32p4/register/hw_ver1/soc/emac_{reg,mac_struct,dma_struct,ptp_struct}.h` | `hw_ver1`, because this board is chip revision v1.3 |

### 1.2 The PHY, and a pleasant surprise

IP101GRI, a **clause-22** PHY. The schematic
(`~/gith/esp/datasheet/ESP32-P4-NANO-schematic.pdf`) shows a 25 MHz crystal
across its `X1`/`X2` and the part's `M_CLKO` pin driving the MAC, which
settles the clock direction: **the PHY generates the 50 MHz RMII reference
and the P4 consumes it.** IDF agrees — `ETH_ESP32_EMAC_DEFAULT_CONFIG()` for
the P4 is `.clock_mode = EMAC_CLK_EXT_IN`.

The surprise is in IDF v6.1-dev: `components/esp_eth/src/phy/` contains only
`esp_eth_phy_802_3.c` and `esp_eth_phy_generic.c`. There is no longer an
`esp_eth_phy_ip101.c` in-tree, and the generic driver touches only standard
clause-22 registers — BMCR(0), BMSR(1), PHYIDR1(2), PHYIDR2(3), ANAR(4),
ANLPAR(5). **So the PHY half of this phase needs no vendor-specific code at
all**, which is a real reduction in scope and the first clause-22 MDIO PHY in
this tree (the ENC28J60's PHY is internal and register-mapped, and the CYW43
has none).

The PHY's address is **not** assumed. The schematic has `PHY_AD0`/`PHY_AD3`
strap nets and IDF's examples default to 1; Z2's done-condition is a scan.

### 1.3 The pins — and a warning about "tidying" them

From `ETH_ESP32_EMAC_DEFAULT_CONFIG()` for `CONFIG_IDF_TARGET_ESP32P4`
(`components/esp_eth/include/esp_eth_mac_esp.h`), corroborated for *this*
board by Waveshare's own
`~/gith/esp/ESP32-P4-Platform/examples/esp-idf/15_eth2ap/sdkconfig.defaults`,
which overrides MDC/MDIO/reset to the same numbers phase 27 §1 recorded:

| Signal | GPIO | Route |
|---|---|---|
| MDC | 31 | GPIO matrix |
| MDIO | 52 | GPIO matrix |
| PHY reset | 51 | plain GPIO output |
| RMII REF_CLK (in) | 50 | IO_MUX `FUNC_GPIO50_EMAC_RMII_CLK_PAD` |
| TX_EN | 49 | IO_MUX `FUNC_GPIO49_EMAC_PHY_TXEN_PAD` |
| TXD0 | 34 | IO_MUX `FUNC_GPIO34_EMAC_PHY_TXD0_PAD` |
| TXD1 | 35 | IO_MUX `FUNC_GPIO35_EMAC_PHY_TXD1_PAD` |
| CRS_DV | 28 | IO_MUX `FUNC_GPIO28_EMAC_PHY_RXDV_PAD` |
| RXD0 | 29 | IO_MUX `FUNC_GPIO29_EMAC_PHY_RXD0_PAD` |
| RXD1 | 30 | IO_MUX `FUNC_GPIO30_EMAC_PHY_RXD1_PAD` |

**The RMII data pins are IO_MUX, not matrix** (`SOC_EMAC_MII_USE_GPIO_MATRIX`
covers MII; RMII is pad-selected), and each signal has its own short list of
legal pads in `components/esp_hal_emac/esp32p4/emac_periph.c`. The lists
overlap in an inviting way — there is a low group (28–36), a middle group
(40–48) and a high group (49–54) — and **this board uses a mixed selection**:
RX and TXD from the low group, TX_EN and the clock from the high group. Worse,
three of the pads in those same lists are in use here as something else:
GPIO31 is MDC but is also `EMAC_PHY_RXER_PAD`, GPIO51 is the PHY reset but is
also `EMAC_PHY_RXDV_PAD`, and GPIO52 is MDIO but is also
`EMAC_PHY_RXD0_PAD`. Anyone who "simplifies" this table to one contiguous
group will produce a driver that configures perfectly and receives nothing.
The pin table is a board fact; it goes in `cmake/board-esp32p4-nano.cmake`
with this paragraph attached, and it is Z0.

### 1.4 The clock sequence, which is not one-shot

TRM §55.5.1 gives the RMII bring-up as an explicit ordered procedure, and two
things in it matter more than the individual bits:

1. **The gates live in two different controllers.** `HP_SYS_CLKRST` holds
   `EMAC_SYS_CLK_EN`, `EMAC_RMII_CLK_EN`, `EMAC_RX_CLK_EN`, `EMAC_TX_CLK_EN`
   and the source-selects, but the external reference arriving on the pad is
   gated by **`LP_AONCLKRST_HP_PAD_EMAC_TXRX_CLK_EN`** — a register in the
   LP always-on domain. This is the same shape as the trap recorded for the
   UART in phase 27 (its two gates are in `SOC_CLK_CTRL1` bit 18 and
   `SOC_CLK_CTRL2` bit 7, with no inferable relationship). Miss the LP gate
   and everything reads back correct with no clock in the building.
2. **`EMAC_RX_CLK_DIV_NUM` and `EMAC_TX_CLK_DIV_NUM` depend on line speed.**
   The TRM says "according to the EMAC line speed (10 Mbit/s or 100 Mbit/s)".
   Line speed is not known until auto-negotiation completes. **So clock
   configuration is not a thing `emac_init()` finishes** — part of it is a
   callback from link-up, and the milestone order in §4 reflects that (Z1
   sets up what it can, Z4 closes the loop). Designing Z1 as if it were
   one-shot is the single likeliest way to lose an evening in this phase.

Also from that procedure: `HP_SYS_PHY_INTF_SEL = 4` selects RMII, and with
an external reference `HP_SYS_CLKRST_PAD_EMAC_REF_CLK_EN` and
`HP_SYS_CLKRST_REF_50M_CLK_EN` are both **disabled** — the P4 must not drive
a clock onto a pad the PHY is already driving.

Every bit position above is a *name* in this plan, not a number. The numbers
get read off the TRM's bit diagrams in Z1, by the render-and-count method in
`plan/phase27_esp32p4_bringup.md` §3.2, and cross-checked against
`hw_ver1/soc/hp_sys_clkrst_reg.h`.

### 1.5 The MAC address

The P4 carries a factory MAC in eFuse BLK1 bits 0..47 (`MAC` /
`MAC_FACTORY` in `components/efuse/esp32p4/esp_efuse_table.csv`; eFuse base
is `DR_REG_LPPERIPH_BASE + 0xD000`). Today the P4 has no
`board_unique_id()` override — that is RP2350 OTP code in
`drivers/idstore_rp2350.c` — so `kernel/identity.c` falls through to
`derive_mac()` and this board answers ARP with a locally-administered address
derived from a build seed. Two P4s flashed from one build would share it.

There is an IEEE-registered address burned into the silicon four inches away.
Z5 reads it. This also gives the P4 a real `board_unique_id()`, which is
worth more than the MAC: it is what makes phase 21's identity record and the
`uid` field mean something on this board.

## 2. Where it plugs in

```
    9P server                (fs/9p.c)              already built
    p9_link_t                                        already built
    net/tcp.c, net/ipv4.c, net/arp.c                 already built, compiled in
    netif_t                  <- the seam             already built
    drivers/emac_esp32p4.c   <- THE WHOLE PHASE      new
```

The driver is a **kernel-mode driver task with no memory domain**, built on
`drivers/include/drivers/driver_task.h`. `plan/phase30_driver_framework.md`
§4 named this file in advance as the framework's first real customer: *"phase
28's EMAC starts from it — a kernel-mode driver task with no domain, which is
the example case exactly."*

Kernel mode, not U-mode, and the reason is G4's finding rather than
convenience. A PMP domain restricts only privilege levels *below* the one
that programs it, so on a M-mode kernel a domain is enforced only in U-mode —
and U-mode costs `task_block()`, `.rodata` and calls into other drivers'
kernel text. An EMAC driver needs to block on a ring, needs to log, and needs
cache-maintenance intrinsics. It is not a U-mode driver, and the header's
"read this before reaching for it" note says so before the API does.

`net/netif.c` owns the counters. The driver counts its *own* hardware errors
separately — descriptor error bits, FIFO overflow, the DMA's abort status —
because "a frame was dropped" and "the MAC reported a receive-FIFO overflow"
are different facts, and `netif.h` is explicit that a driver "puts them
somewhere of its own".

## 3. The cache, which is the actual risk

Stated plainly so that it is impossible to skip:

* The HP cores read and write L2MEM through L1 cache.
* The EMAC's DMA reads and writes L2MEM directly.
* Therefore **every descriptor and every frame buffer needs explicit cache
  maintenance in both directions**, and getting it wrong produces a driver
  that works intermittently, works differently at different optimisation
  levels, and works perfectly the moment you add a `printk()` next to the bug.

The obligations, which Z2 discharges:

1. **Write-back before handing to the DMA.** Setting a descriptor's OWN bit
   and then starting the DMA is only correct if the descriptor and its buffer
   have reached memory first. Write-back, *then* set OWN, *then* write-back
   the descriptor, *then* poke the poll-demand register — the ordering is the
   whole point and the middle step is the one that gets dropped.
2. **Invalidate before reading what the DMA wrote.** Both the descriptor
   (to see OWN clear and the length) and the buffer.
3. **Alignment and exclusive ownership of cache lines.** A descriptor ring
   must be aligned to the L1 line and must not share a line with anything the
   CPU writes, or an invalidate will discard a neighbour's store. The L2
   cache's line is configured to 64 bytes by `esp32p4_l2_cache_shrink()` in
   `arch/riscv/common/trap.c`; **the L1 line is a separate fact and gets read
   from the TRM in Z2, not assumed equal to it.**
4. **The maintenance primitives themselves.** This kernel has none. Z2 adds
   the two it needs — a write-back and an invalidate over an address range —
   in `arch/riscv/common/`, beside `esp32p4_l2_cache_shrink()`, which is
   already the place where "this chip's cache is not what it looks like"
   lives. Two functions, not a cache API.

Where the rings live also matters. `linker/esp32p4.ld` gives `RAM` 384 KB at
`0x4ff40000`, stopping at `0x4ffa0000` where the shrunk L2 cache's storage
begins. The descriptors and buffers are static `.bss` in that region like
every other allocation in this kernel — there is no allocation on the netif
path, by `netif.h`'s own contract — and their size comes out of the same
budget as everything else. Z2 states the figure it costs.

## 4. Milestones

### Z0 — The board file: every pin and every number, before any code

**Done, 2026-09-12.** Ten presets build with zero warnings, QEMU suite
363/363 in 182 s — no behaviour change, as expected of a milestone that adds
only unused constants. One decision made while doing it: the PHY address is
*not* pinned. `CONFIG_EMAC_PHY_ADDR` is `0xFF`, outside clause-22's 5-bit
range, meaning "scan"; Z1 replaces it with the measured value. The schematic's
strap nets and IDF's example default of 1 both suggest an answer, and neither
is evidence.

Add the pin table from §1.3 and the EMAC base, interrupt source and PHY
address to `cmake/board-esp32p4-nano.cmake`, in that file's existing style —
a flat list of numbers, each with the sentence saying where it came from.
Extend `arch/riscv/include/arch/esp32p4_intr.h`'s allocation table with the
EMAC's CLIC line, which is the file's whole reason for existing ("a collision
is a visible edit rather than a coincidence").

**Done when:** the board file names all ten GPIOs with the §1.3 warning
attached; `ESP32P4_CLIC_IRQ_EMAC` is allocated and distinct from
`ESP32P4_CLIC_IRQ_UART0`; the tree still builds all ten presets with no
warnings and no behaviour change.

### Z1 — Clocks, reset, pins, and MDIO reaching the PHY

**Done, 2026-09-12, first hardware run.** `emac scan` on the board:

```
EMAC at 0x50098000, MDIO on MDC=GPIO31 MDIO=GPIO52. Scanning 0..31:
   1: PHYIDR1=0x0243 PHYIDR2=0x0c54  OUI 00-90-c3 model 5 rev 4
  1 PHY at 1, as the board file says. OK.
```

OUI `00-90-c3` is IC Plus and model 5 is the IP101G family, so the part is
confirmed by its own silicon rather than by the silkscreen. The address is
**1**; the strap nets resolve to the same value IDF's examples default to,
and that agreement is a coincidence worth nothing — the board file records
the measurement. `tests/hw/test_esp32p4.py` is 13/13 with the new case.

Four things learned that the plan did not have:

* **The MAC's software reset is the single best test of the whole clock and
  pad configuration**, and it comes free. `EMAC_BUSMODE.SWR` cannot clear
  until the PHY supplies the 50 MHz RMII reference — the register's own text
  says *"it is essential that all PHY inputs clocks ... are present for the
  software reset completion"*. So reaching the MDIO scan at all proves the
  three clock controllers, the seven IO_MUX pads and the reset line are
  right, and it fixes the ordering: **the PHY must come out of reset before
  the MAC is reset**, not after.
* **The gates are spread across three controllers, not two.** §1.4 said
  `HP_SYS_CLKRST` and `LP_AONCLKRST`; the bus clock is in
  `HP_SYS_CLKRST_SOC_CLK_CTRL1`, the RMII/RX/TX gates in
  `PERI_CLK_CTRL00`/`01`, and both the pad's always-on gate *and the EMAC's
  peripheral reset* are in `LP_CLKRST`. §1.4 also overstated the LP trap:
  `HP_PAD_EMAC_TXRX_CLK_EN` resets to **1**, so it is on unless something
  clears it. Written explicitly anyway, which is the cheap half of the
  lesson.
* **The P4 has 57 GPIOs, so every one-bit-per-pin register is two
  registers.** This board puts four EMAC signals above the 0..31 split
  (TX_EN 49, RMII_CLK 50, PHY reset 51, MDIO 52) and MDC at 31, exactly on
  the boundary. The compiler caught one constant `1u << 51`; it could not
  catch the same bug behind a runtime pin number, which would have written
  bit 20 of the low word and failed silently. Hence accessors, not macros —
  and note the shape of the trap: the naive version is correct for precisely
  the pin a casual test tries first.
* **The MDC divider is deliberately the most conservative one available.**
  `EMAC_CR` divides the CSR clock, which on this chip is the system clock,
  which this kernel has never configured and therefore does not know. The
  asymmetry settles it without a measurement: IEEE 802.3 caps MDC at 25 MHz,
  so too small a divider breaks the bus while too large only costs
  microseconds. `CSR/124` it is, until something here knows the system
  clock.

The TRM §55.5.1 sequence from §1.4, with the bit positions read off the TRM's
bit diagrams and cross-checked against `hp_sys_clkrst_reg.h`. Pad routing.
PHY reset asserted and released with the datasheet's timing. Then the MDIO
master: `EMAC_MIIADDR`/`EMAC_MIIDATA`, with the MDC divider derived from the
bus clock.

The smallest possible proof, and the one that catches the LP gate: read
registers 2 and 3 from every address 0..31 and report which ones answer.

**Done when:** a shell command on the P4 prints exactly one responding PHY
address, with a non-zero, non-`0xFFFF` identifier; that address and the
identifier it reported are written into the board file as measured facts;
`node` and the console are unaffected.

### Z2 — Descriptor rings and cache maintenance, proved without a cable

**Done, 2026-09-12.** `emac loopback` on the board:

```
EMAC loopback: 4 RX + 2 TX descriptors of 64 B, buffers 1536 B
  rings cost 9600 B of .bss; L1 line 64 B (TRM 9.3.3.2 p909)
    60 B: ok      64 B: ok      65 B: ok     128 B: ok
   512 B: ok    1500 B: ok    1514 B: ok
EMAC loopback: 7 passed, 0 failed
```

Rings cost **9600 bytes of .bss** (4 RX + 2 TX descriptors at 64 B, six
buffers at 1536 B), out of the 384 KB that .bss and the heap share. The L1
line is **64 bytes**, TRM §9.3.3.2 p. 909: *"64 KB of data cache (dcache)
with a 64 B block size, two-way set associative"*.

**The cache maintenance is proven load-bearing, not defensive** — which was
the open question §3 could only assert. Stubbing both primitives to no-ops
and changing nothing else does not cost the occasional frame; it fails
everything immediately:

```
    60 B: nothing came back (0), DMA status 0x00680084
    65 B: send refused (-2)      <- and every size after it
```

`-2` is the transmit path finding `OWN` still set: the DMA had cleared it in
memory and the CPU was reading a stale line. That is the entire hazard in one
observation, and it is why this driver cannot be written the way the ENC28J60
and CYW43 drivers are.

Three findings the plan did not have, two of them bugs the first run caught:

* **Store-and-forward is not available on this chip, and the symptom is
  size-dependent.** Every frame of 512 B and up failed with receive overflow
  (DMA status bit 4) because RSF holds a whole frame in the MTL FIFO and
  **the P4's receive FIFO is 256 bytes**. IDF disables RSF for every target
  except the original ESP32, commented only as *"Rx FIFO is only 256B"*.
  Threshold mode (64 bytes) instead. Note the shape of this one: the four
  smallest test sizes passed, so a test that only tried short frames would
  have called this driver working.
* **`ACS` (automatic pad/CRC stripping) is a trap that looks like the
  solution.** `netif.h` wants frames with no FCS, and there is a MAC bit
  named for exactly that — but the DWC_EMAC strips only when the length/type
  field is **below 1536**, i.e. only for 802.3 length-framed packets. Every
  Ethernet II frame is above it (IPv4 is type 0x0800 = 2048), so ACS does
  nothing for real traffic while looking like it should. The FCS comes off in
  software, unconditionally. First run showed it as every frame returning
  exactly four bytes long.
* **Chained mode, not ring mode.** Descriptors are padded to a full cache
  line each so that invalidating one to read its status cannot discard a
  neighbour's setup (IDF pads for the same reason, commented only as "due to
  cache arrangement"). That padding makes the hardware's 32-byte stride
  assumption wrong, so each descriptor names its successor explicitly and the
  DMA never computes a stride. The alternative — ring mode with
  `DESC_SKIP_LEN` — works too and is one more number to get wrong.

`tests/hw/test_esp32p4.py` is 14/14.

The DMA descriptor layout from `emac_dma_struct.h` and TRM §55, the ring
setup, and the two cache primitives from §3. Proved in **MAC internal
loopback** — the MAC's own loopback bit, no PHY, no wire — so that a frame
that goes in and comes out has exercised the descriptors, the cache
maintenance and the buffer ownership protocol with the cable and the PHY
still out of the picture.

This ordering is deliberate and it is phase 27 §0's rule reapplied: a failure
here has one candidate cause. A first frame attempted on a live wire has
four.

**Done when:** a shell command sends N frames in MAC loopback and receives N
back byte-identical, including a 1514-byte one and a 60-byte one; it passes
with the data cache enabled; the `.bss` cost of the rings is stated; the L1
line size used for alignment is cited to its TRM page.

### Z3 — Link up

**Done, 2026-09-12.**

```
EMAC: link UP, 100 Mbit/s full duplex, MACCONFIG 0x00c0c80c
EMAC linktest: down noticed in 201 ms, back up in 2712 ms at 100 Mbit/s full duplex
```

The far end agrees, and says something stronger than "link up" —
`ethtool enp0s31f6` reports **"Link partner advertised auto-negotiation:
Yes"**, so the P4 is genuinely negotiating rather than being parallel-detected.
The flap in the host's log is the §5.1 sequence, and now settles straight to
100/Full with no 10/Half fallback.

* **The divisors.** 50 MHz reference, so the MAC's RX/TX domain must be
  25 MHz at 100 Mbit/s and 2.5 MHz at 10 Mbit/s; the field holds
  (divider − 1), giving **1** and **19**. Reset value is 1 — so Z1's working
  100 Mbit/s link was luck dressed as a decision until `emac_apply_link()`
  existed. A 10 Mbit/s link with the 100 Mbit/s divisor does not fail
  cleanly, it corrupts.
* **Link-down is tested without a human.** "Reports the link is up" is the
  half a hardcoded `return true` would pass. BMCR's POWERDOWN bit drops the
  carrier indistinguishably from a pulled cable as far as the MAC can see, so
  the suite proves both directions by itself. 201 ms is the 200 ms poll
  rate-limit plus one exchange; 2712 ms is auto-negotiation, matching the
  host's own 2.7 s.
* **BMSR's link bit latches low**, so one read reports a recovered link as
  still down — once per recovery, an intermittent bug with a latency of
  exactly one poll. Read twice, trust the second.

**Confirmed by a real cable pull**, not only by the PHY-powerdown proxy:
unplugged, the board reports `link DOWN (no carrier)` and the host reports
`NO-CARRIER` / `Link detected: no`; replugged, both return to 100 Mbit/s full
duplex. The command returns promptly in both states rather than spinning.

That pull also demonstrated §5.1's second trap live: carrier loss made
NetworkManager deactivate the profile and **flush `192.168.77.1/24`**, leaving
the device `unavailable`. It restored on replug (quickly, since `manual` has
no DHCP wait), but **every P4 reset flaps carrier**, and Z4 pings while Z5
mounts 9P over TCP. A reflash-then-ping would fail intermittently because the
*host* momentarily had no address — which looks exactly like a driver that
does not transmit. The `ignore-carrier` drop-in in §5.1 is what prevents it,
and is now backed by an observation rather than a prediction.

### Z3a — The P4's heap margin is now zero, and that is a decision to make

Not a milestone; a finding Z3 forced, recorded before Z4 trips over it.

`linker/esp32p4.ld` asserts a 128 KB heap floor. After Z3 the figure is
**exactly 131072 bytes — margin +0**. The next line of code anywhere in this
kernel fails that assert.

The mechanism is worth stating because it is not the obvious one: `.bss` is
NOLOAD in LOWRAM and costs the heap **nothing**, so the rings' 9600 bytes are
not the problem. The heap is `_heap_end − _kernel_end`, and `_kernel_end`
follows `.text`/`.rodata`/`.data` — all RAM-resident, because this image is
`esptool load-ram`-loaded rather than executed from flash. So on this board
**code costs heap and buffers do not**, which is the reverse of the intuition
carried over from the RP2350.

Z3 already took the cheap reclamation: the driver's runtime diagnostics were
rewritten so the prose lives in comments (free) and the printed message
carries only facts (1595 bytes of strings, down from 2019), and the two
copying `emac_tx`/`emac_rx` forms were removed since they have no caller
until Z4.

The options, none of which should be taken silently:

1. **Execute in place from flash.** This script's own header says E6 would
   "get most of that 227 KB back", and the image is still RAM-loaded. This is
   the real fix and it is a phase of its own.
2. **Trim `.text` elsewhere** — the script's other suggestion (171 KB of
   `.text` against 63 KB of `.rodata`).
3. **Lower the floor deliberately**, accepting that this board can no longer
   host the largest placeable user image. Legitimate, but it weakens a guard
   that exists for a stated reason and is the user's call, not a way to get
   past a failing build.

**Decided, 2026-09-12: option 1.** Written up as
`plan/phase32_esp32p4_execute_in_place.md`, milestones U0–U6. The measurement
that settles it: `.text` is 186 200 bytes and `.rodata` 68 904, so moving both
into flash-mapped address space frees **249 KB** and takes the heap from
128 KB to roughly 375 KB — a change of category rather than an optimisation,
and enough that no foreseeable milestone has to think about it again.

**Phase 28 resumes at Z4 after phase 32's U0–U3 and U5.** U4 (booting with no
host attached) is also in scope there, decided 2026-09-12: phase 28 does not
need it, but phase 29 does — a stratum-1 server that requires a laptop to boot
is not a server — and it is cheaper done while the boot path is open.

**Outcome, 2026-09-13: phase 32 is done through U5 and this phase is
unblocked.** The heap is **372 KB**, against the 128 KB that stopped Z4 — and
against the prediction of 376, four short because the RAM-resident boot
section costs a page. The floor that fired here has been raised to 360 KB and
is now derived from what actually remains in RAM rather than inherited from
the RP2350.

Two things Z4 inherits that did not exist when this section was written:

* **The board boots with nothing attached**, so a Z5 node on the LAN is a
  node, not a thing a laptop has to start. `load-ram` is gone; flash with
  `uv run tools/p4flash.py`.
* **`.text` is fetched from flash**, which the EMAC driver never notices —
  but its DMA rings are `.bss`, and `.bss` now lives in LOWRAM where it
  competes with the boot stack rather than with the heap. Z2 sized the rings
  at 4 RX + 2 TX partly because `.bss` was tight; that pressure moved rather
  than vanished, and the figure to watch is `_bss_end` against
  `_stack_bottom`.

Clause-22 auto-negotiation: BMCR restart, BMSR link-status poll, ANAR/ANLPAR
resolution to speed and duplex. Feed the result into the MAC's configuration
**and into `EMAC_RX_CLK_DIV_NUM`/`EMAC_TX_CLK_DIV_NUM`** per §1.4 — this is
the milestone that closes the loop Z1 could not.

**Done when:** with the cable in, the board reports link up, 100 Mbit/s, full
duplex, and the laptop's `enp0s31f6` reports `LOWER_UP` with a partner; with
the cable out it reports link down within a second and does not spin; a
`link_up()` implementation exists that never blocks.

### Z4 — The first frame on the wire

`send_frame()`, `recv_frame()` and a non-blocking `poll()` against the real
PHY. Frame filtering: our unicast, broadcast, and the MAC's own FCS check
and strip (`netif.h` requires the frame handed up to carry no FCS, "both
check and strip it").

**Done when:** `tcpdump -i enp0s31f6` on the laptop shows a frame the P4
sent, and the P4's unclaimed-frame latch (`net_unclaimed_count()` /
`net_take_unclaimed()` — the diagnostic phase 19 built for exactly this
moment) shows the head of a frame the laptop sent, with the right source MAC
and EtherType.

#### Z4 — DONE, 2026-09-15

`drivers/emac_esp32p4.c` grew a `netif_t` (`eth0`), registered from
`kernel/board.c` as `DEV_KIND_NETIF` exactly like the ENC28J60, so
`net_stack_attach()` and `net_task_start()` in `kernel/main.c` pick it up
with no P4-specific code above the driver. The frame path is zero-copy:
`poll()` peeks at the head descriptor and holds it, `recv_frame()` copies out
and releases. The FCS needed no new work — `emac_rx_peek()` was already
stripping it, and the MAC checks it.

Destination filtering is the MAC's, not a `memcmp()`: `MACFRAMEFILTER` = 0 is
precisely "perfect unicast match against Address0, plus broadcast", so a
frame that is neither is dropped before it occupies a descriptor.

**Evidence, on the bench against the laptop at 192.168.77.1:**

| direction | witness | result |
|---|---|---|
| P4 → laptop | 5 ICMP echo replies accepted by the laptop's own stack | 5/5, 0.6–2.6 ms RTT |
| P4 → laptop | `net txtest 25`, counted at `enp0s31f6`'s `rx_packets` | 25 sent, **25** arrived (control window: 0) |
| laptop → P4 | `emac stats` last-rx latch | `dst 02:4c:47:87:a4:b9 src f8:75:a4:68:1d:85 type 0x0800` |

**The literal capture, once `tcpdump` was installed and given
`cap_net_raw` (2026-09-16):**

```
10:09:31.808694 02:4c:47:87:a4:b9 > ff:ff:ff:ff:ff:ff, ethertype 0x88b5, length 60:
        0x0000:  4c55 4741 4c4f 532d 4e45 5449 462d 5445  LUGALOS-NETIF-TE
        0x0010:  5354 0000 0000 0000 0000 0000 0000 0000  ST..............
```

— six frames, sequence numbers incrementing, source MAC ours. That is the
first half of the done-condition verbatim.

The **unclaimed-frame latch is still not exercised**, and the reason is worth
stating so nobody records this milestone as fully closed: reaching it needs a
frame that is neither IPv4 nor ARP, so something has to *send* one, and
`cap_net_raw` on `tcpdump` grants capture, not transmission. A raw socket for
a sender (or an IPv6 address on the host interface) is all it needs.
`net rxtest` is written and waiting. What stands in for it meanwhile is the
driver's own last-rx latch, which showed a frame the laptop sent with the
right source MAC and EtherType — one layer lower than net/stack.c's latch,
because the frame was legitimately claimed.

Locked in by `test_emac_frames` in `tests/hw/test_esp32p4.py` (18 tests now).

##### The bug this milestone actually found, which is worth the space

The receive address filter lives in the **RMII receive clock's domain, and
that clock does not run until the link is up**. The station address is
therefore written during bring-up into a register that takes it — it reads
back byte-perfect, forever — while the *filter* never sees it and goes on
comparing against its all-ones reset value.

What that produces is a uniquely misleading failure:

- broadcast is accepted, because `ff:ff:ff:ff:ff:ff` is a perfect match
  against a filter still holding all ones;
- so **ARP works**, and the peer resolves us and caches our address;
- the link reports up, at the right speed and duplex;
- `emac stats` shows the correct address in the correct register;
- and every unicast frame addressed to us is silently dropped. Not counted
  as an error anywhere — `missed` stays 0, because a filter reject is not a
  missed frame.

It was found by turning the filter off (`emac promisc on`): ping went from
0/4 to 4/4 with nothing else changed. It was then narrowed by writing each
register separately while the link was up — rewriting `MACFRAMEFILTER` alone
changed nothing, rewriting **Address0 alone fixed it**, with the register's
before and after values byte-identical. The write itself, not the value, is
what takes effect.

The fix: `emac_link_poll()` rewrites the address on every poll that sees the
link up, and `emac_netif_poll()` — the netif's own `poll()` — calls
`emac_link_poll()`. Both halves are load-bearing, and each one replaced an
attempt that looked right and failed on hardware:

1. **In `emac_apply_link()`, on a link change.** Wrong: the nastiest version
   of this bug involves no change at all. `emac link` re-probes, the probe
   resets the MAC and wipes Address0, the cable never moved, so the next poll
   sees the same speed and duplex, reports no change, and nothing is
   rewritten. Caught by the first hardware run: 17/18, Z4 failing *because*
   Z3 ran before it and reset the MAC.
2. **Once per link-up, keyed on a flag `emac_start()` cleared.** Wrong: the
   single write it makes happens on the first poll that sees the link up,
   which is precisely when the receive clock has just started and has not
   settled. The write does not latch, the flag records that it did, and it is
   never retried. Also 17/18.
3. **Unconditionally, in `emac_link_poll()`.** Right, and still 0/4 on ping —
   because after the interface has come up once, *nothing calls that
   function*. `net/stack.c`'s `net_autoconfig_once()` asks `netif_link_up()`
   only until the first time it answers yes, then latches `done`. From then
   on the only caller is a human typing `net` or `emac link`.

Hence the last piece: `poll()` is the one call that keeps coming, so it is
what drives the link poll. Retrying forever is immune to all three failures —
a write that does not take is simply followed by another one 200 ms later —
and it costs one MDIO exchange per 200 ms, which `emac_link_poll()` was
already rate-limited to.

Two notes for anyone touching this driver:

- `emac_probe()` and `emac_start()` reset the MAC and reinitialise the rings.
  Calling `emac link` on a board with a live `eth0` is disruptive, not
  read-only; `emac_netif_restarted()` is what keeps it from being worse than
  disruptive (it drops any held descriptor, which would otherwise be handed
  up after the DMA had taken the buffer back).
- `emac stats` and `emac promisc on|off` exist because of this bug and stay
  because the question recurs: an interface that is up and silent has several
  causes that look identical from outside, and those two commands separate
  them in one step each.

### Z5 — A node on the LAN

`netif_register()`, the eFuse MAC and `board_unique_id()` from §1.5, and
`net_set_address()`. The segment is the laptop's: **192.168.77.0/24, the
laptop at 192.168.77.1**. The P4 takes a static address on it with a zero
gateway, which `net_set_address()` documents as legitimate — *"a zero gateway
means 'no router on this segment', which is a legitimate configuration and
not an error"*. There is no DHCP server on this link and this phase adds no
DHCP client (§6).

`net_task_start()` then runs the pump, and everything above the seam is phase
19's, unchanged.

**Done when, in order:**

* `net` on the P4 reports the interface, the eFuse MAC with source
  `silicon`, and the address;
* the laptop's `arping` gets a reply, and the laptop's ARP table shows the
  P4's factory OUI rather than a locally-administered address;
* `ping 192.168.77.x` from the laptop succeeds, 100 packets, zero loss;
* a 9P mount over TCP from the laptop lists `/proc` and reads
  `/proc/kmsg` — the path phase 19 R3 built, now over a wire this board has
  never had;
* **`net/include/net/netif.h` is unmodified.** If it is not, say so loudly
  and record what §2 of `plan/hardware_seams.md` got wrong.

#### Z5 — DONE, 2026-09-16

`drivers/efuse_esp32p4.c` reads the factory MAC from eFuse BLK1 and the
128-bit `OPTIONAL_UNIQUE_ID` from BLK2, and `kernel/identity.c` gained a
`board_factory_mac()` rung between `CONFIG_NODE_MAC` and `derive_mac()`.
`node_mac_source()` now answers **`silicon`** on this board instead of
`derived (build seed)`.

Byte order was the risk worth guarding, because a reversed MAC is still a
plausible-looking MAC. Two independent checks agree: the driver refuses any
address whose first octet has the multicast or locally-administered bit set
(the reverse of this chip's address starts `53:`, which is multicast and would
have been caught), and the value it reports is **identical to esptool's own**
reading of the same part — `80:f1:b2:d2:f0:53`.

| done-condition | result |
|---|---|
| `net` reports the interface and the eFuse MAC with source `silicon` | ✅ `mac: 80:f1:b2:d2:f0:53 (silicon)`, `uid: eaad894f48809797 (silicon)` |
| laptop's ARP table shows the factory OUI, not a local address | ✅ `192.168.77.2 lladdr 80:f1:b2:d2:f0:53 REACHABLE` |
| `ping`, 100 packets, **zero loss** | ✅ **3000 echoes, zero loss** across three 1000-packet runs — see below, the cause was ours |
| 9P mount over TCP lists `/proc`, reads `/proc/kmsg` | ✅ 15 entries; 2735 B, 42 lines |
| `net/include/net/netif.h` unmodified | ✅ untouched, so §2 of `plan/hardware_seams.md` stands |

Locked in by `test_node_identity_from_silicon` and `test_lan_node` in
`tests/hw/test_esp32p4.py`. The second asserts zero loss and is **red** until
the transmit defect is fixed; it is left red on purpose.

##### The transmit loss: we were driving the RMII pads too hard

`pad_iomux()` forced `FUN_DRV = 3` — maximum drive — on every RMII pad,
justified in the code by "strongest drive; RMII runs at 50 MHz". That is an
assumption wearing the clothes of a reason, and it was wrong: ESP-IDF
configures these same pads by setting the function and the pull mode and
**never touching drive strength**, leaving the default of 2. Maximum drive
into short unterminated traces buys edge rate and pays in overshoot and
ringing, which at 50 MHz is corrupted bits at the far end.

| path | forced drv=3 | default drv=2 |
|---|---|---|
| MAC-internal loopback (no pads) | 3500/3500 | 3500/3500 |
| PHY loopback (RMII pins + PHY) | 3494/3500, **1 corrupt** | **3500/3500** |
| the wire, 1000 echoes | 991/1000, 983/1000 | **1000/1000** ×3 |

**How it was cornered, which is the transferable part.** Two loopback modes
bisect the physical path: MAC-internal never leaves the chip, PHY loopback
(BMCR bit 14) drives the real transmit path and the RMII pins and is turned
around inside the IP101G. The first stayed perfect while the second lost *and
corrupted* frames — and corruption while every error counter stays clean is a
signal-integrity signature, not a logic one. That is what moved the search off
the descriptor path, where it had been stuck for hours, and onto the pins.

Everything else was checked against ESP-IDF and the TRM and found correct: the
RMII clock configuration matches `emac_ll_clock_enable_rmii_input` exactly,
the dividers match IDF's own arithmetic, `hw_ver1` and `hw_ver3` agree on
every field this driver touches (the board is rev v1.3), and the pin
assignment is identical to IDF's CI config for P4 + IP101. Drive strength was
the single place we had overridden the vendor, and the override was the bug.

##### Three testing lessons this milestone cost, which are the durable part

**A sample too small to distinguish working from broken is not evidence.** Z4
was reported done on three-to-five pings. A 1–2% loss survives that about 95%
of the time, so those runs could not have detected it, and the interface
shipped with a receive race discarding 3–15% of frames. `test_lan_node` uses
one hundred packets for exactly this reason. The receive race itself is
instructive: `emac_rx_peek()` returns NULL both for "nothing ready" and for
"errored frame", and the poll loop re-read the descriptor afterwards to tell
them apart — so a frame arriving in that window had `OWN` newly cleared, was
scored a hardware error, and was released without being handed up. `RDES0`
read `0x00660320`: FS, LS, length 102, not one error bit set.

**A diagnostic can be defeated by the system it runs inside.** Underneath
every one of those four broken stress tests, `netsrv` was consuming the
frames the diagnostic had just sent: `net/stack.c` polls `emac_netif_poll()`
forever, and since Z4 registered `eth0` the loopback path has been sharing
its descriptor rings with a live interface. Measured, not supposed — the
interface's own rx counter climbed by 65 during one 700-frame run, and two
identical runs scored 450/700 and 674/700. The **shipped Z2 test** had been
racing the same way since Z4 and passing on timing: seven frames finish
inside about one scheduler slice. `g_diag_owns_rings` now makes the netif
yield the rings to a diagnostic, and with it the same test reports 2100/2100.

**A diagnostic is not trustworthy because it is elaborate.** Four successive
versions of `emac loopback stress` each measured their own defect rather than
the hardware — releasing descriptors the DMA still owned, assuming strict
one-in-one-out ordering, draining a single frame per send — and each produced
a confident number (`385 lost`, `1439 lost`, `750 unaccounted`) that was
worth nothing. Two of those bugs also existed latently in the **shipped Z2
test**, which released on timeout the same way; they are fixed there too. The
current version verifies each frame against its own length through the pure
`loopback_byte()`, so ordering cannot corrupt it, but until it agrees with a
known-good path its output should be treated as unvalidated.

### Z6 — Look again at the waiter-slot/ISR pattern

The trigger phase 30 §2 set: this driver is the third implementation.
Compare it against `uart_16550.c` and `uart_esp32p4.c` and decide — with the
driver working, not before. **A decision either way is the done-condition.**
"Extracted, here" and "not extracted, because the third instance turned out
to differ in X" are both successful outcomes; only leaving it unexamined is
a failure.

#### Z6 — DECIDED: not extracted, because the third instance is not an instance

**2026-09-16. The done-condition is a decision, and the decision is no.**

Phase 30 §2 held the waiter-slot/ISR pattern at two implementations and named
this driver as the tiebreaker: *"Phase 28's Ethernet driver will be the third,
and that is when to look again."* Looked. **The EMAC contains no instance of
the pattern at all** — no ISR, no `task_block()`, no waiter slot. So the count
is still two, exactly where phase 30 left it, and its rule applies unchanged.

| driver | RX waiter | TX waiter |
|---|---|---|
| `uart_16550.c` | yes | yes |
| `uart_esp32p4.c` | yes | yes, plus a `may_block` split |
| `uart_rp2350.c` | no | yes |
| **`drivers/emac_esp32p4.c`** | **no ISR at all** | **no** |

**Why the EMAC is polled is a contract, not an omission.** `net/netif.h` says
of `poll()`: *"Must never block: callers invoke it from polling loops."* An
ISR-and-waiter-slot receive path is precisely a blocking receive path, so it
is not available at this seam. `net/stack.c`'s `netsrv` task is already the
pump, and the driver is a `netif_t` rather than a thread of control.

Transmit is the one place the pattern *could* have gone: `netif.h` permits
`send_frame()` to block "for as long as the hardware needs to accept the
buffer", and `emac_netif_send_frame()` currently spins on
`emac_tx_buffer()` with `time_delay_us(10)`. It never actually waits — the
ring drains at line rate and `tx_errors` has stayed 0 across every soak,
including 500-frame bursts at ~147000 frames/s. A TX-done interrupt would add
an ISR, a waiter slot and the lost-wakeup invariant below to remove a spin
that does not happen.

**What the two real instances share is an invariant, not code.** The subtle
part is one rule:

> nothing between the fast-path miss and `task_block()` may restore
> interrupts early, or an interrupt landing in that gap finds the task still
> RUNNING rather than BLOCKED, `task_unblock()` no-ops, the ISR has "served" a
> wakeup nobody was asleep for, and the wait never ends.

That rule is identical in both files. Almost nothing around it is. The 16550
arms and disarms level-triggered `IER` bits and needs no acknowledge, because
reading `LSR`/`RBR` clears the condition; the P4 masks *everything that fired*
unconditionally and must then write `UART_INT_CLR` to take the CLIC's pending
bit down. The P4 additionally splits `_blocking` from `_polling`, because
`uart_flush()` reaches it by two routes and blocking on the fallback route is
the deadlock that route exists to escape — the 16550 has no analogue.

Extracting today would therefore mean a two-instance extraction of roughly
twenty lines, in which the differences *are* the parts that matter, and the
shared residue is a rule rather than a routine. The cheap form of the
extraction already exists and is the right one: `uart_esp32p4.c` says
*"Copied in shape from drivers/uart_16550.c, where the same comment is the
record of having got it wrong first."*

**When to look again.** A genuine third instance needs all three of: an ISR,
a task blocked on it, and both directions. The likely candidates are a DMA
completion path (SPI or SD), or the EMAC itself if interrupt-driven receive
is ever wanted — but that last one is not a driver decision, it is a change
to `netif_t`, and it should be argued there rather than here.

##### A second prediction reality declined

§2 of this plan said the driver would be `driver_task.h`'s first real
customer: *"a kernel-mode driver task with no domain, which is the example
case exactly."* It is not a driver task either, and for the same underlying
reason: `netif_t` plus `netsrv` already supply the thread of control, so a
second one would be a task whose only job is to be polled by another task.

Both predictions failed in the same direction, which is worth naming because
it is a fact about the architecture rather than about this driver: **a device
that already has a device-class contract (category D) does not usually need
the driver-as-task pattern (category C).** The contract brings its own pump.
`block_dev_t` is the same shape — `virtio_blk.c` and `flashdisk.c` are called,
not scheduled. The drivers that *do* need C are the ones with no contract
above them, which is why the nine that got it are consoles, sensors and
buses.

### Z7 — Tests, and the two documents that owe an update

`tests/hw/test_esp32p4.py` grows Ethernet tests — link state, the loopback
selftest from Z2, an ARP round trip, a ping, and a 9P-over-TCP fetch — on the
same self-loading pattern as its existing twelve. `plan/hardware_seams.md` §2
records whether its prediction held. `plan/open_issues.md`'s P4
single-ACM/no-downlink entry changes shape exactly as it predicted it would
(*"a node on the LAN needs no downlink cable"*), and gets rewritten or
closed. `README.md` and the `esp32p4` preset description gain the interface.

**Done when:** `tests/hw/test_esp32p4.py` passes at its new total with the
cable in; it *skips rather than fails* the wire tests with the cable out, and
says which; the QEMU suite is unchanged at 363/363 and the RP2350 at 25/25.

## 5. How it is tested

The laptop is the far end. `enp0s31f6`, MAC `f8:75:a4:68:1d:85`, reports
`<BROADCAST,MULTICAST,UP,LOWER_UP>` — **`LOWER_UP` means carrier is present
right now**, so the cable is in and the PHY on the NANO is powered and
negotiating before a line of driver code exists. That is a useful baseline in
both directions: if carrier ever drops, that is the cable or the board and
not the driver; and it means Z1's "the PHY answers MDIO" is expected to
succeed rather than hoped for.

### 5.1 The far end has to be configured, and it has two traps of its own

Found on 2026-09-12, before any driver code existed, and both would have
presented as driver bugs.

**Trap one: the link flap on every P4 reset is normal, and is not the
laptop's doing.** The kernel log shows

```
e1000e ... enp0s31f6: NIC Link is Up 10 Mbps Half Duplex, Flow Control: None
e1000e ... enp0s31f6: NIC Link is Down                       (1.3 ms later)
e1000e ... enp0s31f6: NIC Link is Up 100 Mbps Full Duplex    (2.7 s later)
```

That is autonegotiation, not a policy decision: 10 Mbit/s half duplex is the
**parallel-detect fallback** an Ethernet PHY reports when it establishes link
before autoneg resolves, and 2.7 s is one negotiation cycle. The trigger is
on the *P4* side — its IP101GRI resetting — which happens on every power
cycle, every `esptool` reset and every assert of GPIO51. **So this sequence
will appear on every reflash for the whole of this phase, and it is not a
symptom.** It becomes one only if the final state is not 100 Mbit/s full
duplex; Z3's done-condition is written against that final line, not against
the absence of a flap.

**Trap two: the far end resyncs when carrier returns, and that is the far
end's business, not a thing to engineer around.**

Observed: when the cable came out, NetworkManager deactivated the profile and
flushed `192.168.77.1/24`, leaving the device `unavailable`; when it went back
in, the address returned. A PHY reset produces the same flap, and this board
resets on every reflash.

**The rule this phase adopts (2026-09-12, the user's call, and it is the right
one): we cannot edit every host that will ever be plugged into this board, so
resync time after a link flap is normal and the driver must work with it.**
There is no host-side fix in this phase's requirements. A switch that takes
30 seconds of spanning-tree before forwarding, a host that runs a DHCP client,
a laptop that deactivates on carrier loss — all of these are the environment,
not a misconfiguration, and a driver that only works on a cooperatively
configured peer is not finished.

What that actually demands, and it is all on our side:

* **The driver must recover unaided.** Z3 already does: link-down is noticed
  in 201 ms and the link renegotiates in 2712 ms with no intervention. Nothing
  latches, nothing needs a reset, and `net`'s address is ours and survives.
* **The tests must wait rather than assume.** A reflash-then-ping that fires
  immediately can fail because the *host* is still resyncing, which looks
  exactly like a driver that does not transmit. Z4 and Z5's tests poll for
  reachability with a generous deadline instead of sleeping a guessed
  interval, and report "the far end never came back" distinctly from "the
  frame was not answered".

The one host-side change that *is* required is not about carrier at all: the
profile must be `ipv4.method manual`, because with `method=auto` NM waits for
a DHCP lease that cannot arrive on this segment and so never applies the
static address it already has. That was done on 2026-09-12. `ignore-carrier`
would shorten the resync window on this particular laptop and is deliberately
**not** relied upon.

`ipv4.never-default=yes` is set on that profile and must stay: wifi is the
real uplink and this segment has no router, which is why Z5 passes a zero
gateway (§4).

**Precondition, checked at the start of each session and not assumed:**

```sh
ip -br addr show enp0s31f6     # expect UP and 192.168.77.1/24, not UP alone
```

and after any reflash, give the far end time to resync before concluding
anything about the driver.

Nothing before Z5 needs the address — Z1-Z4 are on-board or raw-frame work,
and `tcpdump` sees frames on an address-less interface — but Z5 does. A ping
that fails because the *host* has no address on the segment looks exactly
like a driver that does not transmit, which is the two-candidate-cause
situation §0 of phase 27 exists to forbid. So the check comes first, every
time.

### 5.2 Instruments

In the order the milestones need them: `mdio`-level shell
commands on the board (Z1), an on-board loopback selftest (Z2), `ip link`
and `ethtool` on the laptop (Z3), `tcpdump` (Z4), `arping`, `ping` and the
`fuse-p9` mount (Z5).

One instrument caveat carried from prior sessions, because it has produced
false regressions twice: a P4 test failure is not a P4 test failure until the
board has been shown to answer `uname` on its console. See
`plan/open_issues.md` and the two-port split — `CH343P`/`ttyACM0` resets,
`CP2102`/`ttyUSB0` talks.

## 6. Explicitly not in this phase

* **No timing claims, no PTP, not one PTP register read.** Phase 27's
  addendum already wrote this rule for this phase — *"**No timing claims in
  phase 28.** The moment Ethernet works, the temptation to measure will be
  considerable, and the whole reason for three phases is to not do that
  yet"* — and `plan/phase25_gps_ntp_server.md` §5 is why: the reference has
  to be better than the thing being measured. `emac_ptp_struct.h` stays
  unopened until phase 29.
* **No DHCP client.** The segment is a point-to-point link to a statically
  configured laptop. A DHCP client is a protocol implementation with its own
  state machine and its own phase, and adding it here would mean a bring-up
  failure could be either the driver or the lease.
* **No PSRAM.** Still phase 27 §7's "not now". The rings fit in L2MEM.
* **No second interface, no gateway persona.** `NETIF_MAX` is 2 and this
  board fills one slot.
* **No U-mode domain for this driver.** §2.
* **The microSD slot — not here, and it is not needed for configuration.**
  The NANO has one, on the P4's SDMMC host (`SOC_SDMMC_HOST_SUPPORTED`, two
  slots, IO_MUX pins, DMA-capable). It is worth having. But the motivating
  question — "somewhere to keep configuration" — is **already answered on
  this board**: E6 gave it a writable 512 KB `/flash0`
  (`cmake/flash_layout_esp32p4.cmake`), phase 21's identity record lives
  there, and `net`'s address is set the same way every other persona in this
  tree sets it. Nothing in Z0–Z7 wants for storage.

  Folding it in anyway would cost three new things at once — an SDMMC host, a
  removable-media block device, and FAT on a card whose contents this project
  did not write — and one of them has a trap already visible in the
  reference: on the P4 the SD card's VDD comes from an **on-chip LDO
  (channel 4 on this board)** that must be enabled before the slot will
  respond at all, so "card not detected" and "LDO off" present identically.
  Three new causes for every Ethernet symptom is precisely the mistake
  phase 27 §0 was written to prevent.

  **It gets its own milestone after Z7, or its own phase** — the facts found
  while checking are recorded in §7 so the next person does not re-derive
  them. What it is genuinely *for* is bulk storage: a log that outlives a
  reflash, and captured packets or PPS traces in phase 29 that a 512 KB
  flash cannot hold.

## 7. Parked, with the facts attached

**microSD on the P4-NANO.** `SOC_SDMMC_NUM_SLOTS 2`, `SOC_SDMMC_USE_IOMUX 1`
and `SOC_SDMMC_USE_GPIO_MATRIX 1` (both routes exist), `SOC_SDMMC_DATA_WIDTH_MAX 8`,
`SOC_SDMMC_PSRAM_DMA_CAPABLE 1`. `DR_REG_SDMMC_BASE` = `0x50083000`.
`SOC_SDMMC_IO_POWER_EXTERNAL 1` — and the trap above: Waveshare's
`examples/esp-idf/09_sdmmc` uses `sd_pwr_ctrl_new_on_chip_ldo()` with
`ldo_chan_id` defaulting to **4** for `IDF_TARGET_ESP32P4`. Reference:
`components/sdmmc/` and `components/esp_driver_sdmmc/`. This tree already has
a block-device seam and a FAT implementation (`drivers/flashdisk.c`,
`drivers/spisd_rp2350.c`, `fs/`), so the filesystem half is not new work.

**The waiter-slot/ISR pattern.** Z6 owns the decision; if Z6 declines to
extract, the reason belongs here.

## 8. What phase 29 inherits

A P4 that is a node on a LAN, with a driver whose descriptor rings, cache
discipline and interrupt path have been proved on traffic — which is the
whole prerequisite for touching `EMAC_SYSTEMTIMESECONDS_REG` and
`EMAC_SYSTEMTIMENANOSECONDS_REG` at all. Phase 29's argument, that hardware
timestamping *deletes* phase 25's hardest open question rather than merely
hosting it on faster silicon, depends on this phase having left no doubt
about whether a frame arrived when the driver says it did.

## Verified against primary sources, 2026-09-15

Re-checked before resuming at Z4, since both decisions below are load-bearing
and were originally taken from reasoning rather than from the page.

* **RSF off.** `esp-idf/components/esp_hal_emac/emac_hal.c:190-201` reads
  `#if SOC_IS(ESP32)` enable / `#else` disable, with the comment *"Disable
  Receive Store Forward (Rx FIFO is only 256B)"*. The driver's claim is
  verbatim correct and still current in the checked-out IDF.
* **ACS off.** The TRM's `EMACPADCRCSTRIP` description: *"the MAC strips the
  Pad or FCS field on the incoming frames only if the value of the length
  field is less than 1,536 bytes. All received frames with length field
  greater than or equal to 1,536 bytes are passed to the application without
  stripping"*, and the receive-path text repeats it against `0x600`. 1536 is
  0x600 and IPv4's EtherType is 0x0800, so ACS would never strip an Ethernet
  II frame. Leaving it off and accounting for the 4-byte FCS in RDES0's length
  is right.
* **XIP window.** The TRM gives external flash as `0x4000_0000 ~ 0x43FF_FFFF`
  (64 MB), so `linker/esp32p4.ld`'s `ASSERT(ADDR(.text) >= 0x40000000 &&
  ADDR(.text) < 0x44000000)` matches the map exactly.

**One thing found that Z4 will need and no note recorded:** the receive
watchdog is **enabled by default**, and cuts frames above **2048 bytes**
(DA+SA+LT+data+pad+FCS). Disabling it via `EMACWATCHDOG` still leaves a hard
16 KB cut-off. Standard 1518-byte Ethernet is unaffected, so this is not a bug
today -- it is a limit to know about before anything larger is attempted.
