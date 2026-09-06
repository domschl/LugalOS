# Phase 30 — The layer above the registers

**Status: planned, not started, 2026-09-06. Sequenced after phase 31 and
before phase 28.**

*(2026-09-06: `plan/phase31_concurrency_hierarchy.md` was written after this
one and goes first. Its argument applies directly here: this phase moves code
**between contexts** — driver task bodies and U-mode domain construction into
shared code — and that is exactly the operation that silently creates a lock
or channel cycle. Phase 31 builds the checker that makes these migrations
verified rather than hopeful. It also owns an open lock-ordering violation
that should not wait behind a refactor.)*

The reason for this phase's own ordering ahead of 28 is in §0.4: phase 28's Ethernet driver is the next
large new driver this project will write, and it should be written against
this once rather than written twice.

**Milestone letter: `G`.** A–F, H–N and P–T, V–X are spoken for across
`plan/`; `O`, `U`, `Y` and `Z` remain free after this.

## 0. Why this phase exists

### 0.1 The question, as it was asked

Phase 27 brought up a second silicon family, and by E3 it had produced three
copies of the same UART machinery. The question that followed was the right
one to ask:

> While supporting a new hardware platform, we will generate a level of
> either redundancy by implementing drivers several times with duplicate
> code, or need to find the right level of abstraction for hardware-specifics
> and shared code for each interfacing and driver problem. That became
> already clear for UART, but will apply in different degrees to every single
> part of hardware support. Should we define some kind of HAL now?

The answer this phase is built on is **no — not a HAL**, and the reasoning
matters more than the conclusion, because the obvious fix aims at the wrong
half of the problem.

### 0.2 The tree already has a hardware abstraction layer, and phase 27 was its test

Phase 27 §0 states the thesis: *"`arch/riscv/` has three backends and two
memory models, but all three have only ever been aimed at Hazard3 and at
QEMU's `virt`, and the seams that look general are general in the way
untested code is always general. A second platform is how that gets
falsified."*

E2 and E3 ran that experiment. A new boot path, a new memory map, and an
interrupt controller whose programming model Espressif themselves gate behind
a chip-revision `#ifdef`. Here is what needed **no change to a single
client**:

| Seam | What the ESP32-P4 cost it |
|---|---|
| `kernel/include/kernel/devirq.h` | gained a return value in E3; no caller changed |
| `arch/trap.h`: `trap_init()`, `arch_irq_enable()` | one new `#elif` arm; contract unchanged |
| `kernel/ticker.c`: `now()`, `set_deadline()`, `arch_ticker_init()` | one arm |
| `arch/vmm.h` | `arch/riscv/rv32_nommu/` reused verbatim |
| `arch/pmp.h`, `arch/umode.h`, `mem_domain.c` | untouched |
| `block_dev_t`, `net/netif.h`, the device registry, `kernel/chan.h` | untouched |

A CLIC — memory-mapped threshold, `mintstatus` at a non-standard address,
`mcause` carrying `MPP`/`MPIE`/`MPIL` alongside the code — slotted in behind
an unchanged `devirq_attach(irq, handler, ctx)`. Rule 0 of
`plan/phase5_distributed_design.md` §5.1 was right, and the seams it produced
held under exactly the load they were designed for.

**Adding a HAL now would therefore be adding a second one, on top of a first
one that passed its test.**

### 0.3 What actually leaked

Nine drivers in this tree are tasks. Every one of them re-implements the same
lifecycle by hand: the `while (!ep) sched_yield()` wait, the
`chan_serve_wait()`/`chan_serve_reply()` loop, a `*_task_alive()` state
check, a bounded `*_call_with_retry()`, and — on RP2350 — a `mem_domain_t`
built region by region before a one-way `arch_enter_user()` jump.

```
chan_register_task() call sites          9   (uart ×3, spisd, virtio_blk,
                                              i2c_rtc, st7735, tm1638, clock)
arch_enter_user() call sites             7   (the above minus the two QEMU
                                              drivers and the P4's, plus
                                              usb_cdc, which has no endpoint)
mem_domain_add() calls across those      63
```

None of that is hardware. It is the same six-line domain shape — own stack,
the shared `.utext` page, one or more MMIO windows, and the
refuse-rather-than-claim-unverified-isolation rule — written out seven times,
and the same serve loop written out nine times.

**This is not a hardware abstraction layer. It is a missing driver
framework**, and the distinction is the whole design:

* A HAL abstracts *what the hardware does* — `hal_uart_write()`,
  `hal_gpio_set()`. It would attack the register halves of the three UART
  drivers, which share nothing and correctly so.
* A framework abstracts *what a driver is in this kernel* — a task, owning
  one endpoint, serving a request loop, optionally isolated in U-mode. That
  is identical everywhere by construction, because it is this project's own
  design and not any vendor's.

The three UART drivers are the clearest evidence. `uart_16550.c`,
`uart_rp2350.c` and `uart_esp32p4.c` are 513, 1122 and 782 lines. Their
register halves — a 16550, a PL011, and an Espressif UART with a `_SYNC`
commit handshake — have no shared line and must not acquire one; phase 27 §3.2
("confirm register layouts against the TRM, never by inference") exists
because E2's first draft got two clock-gate bits wrong by reasoning from
another Espressif part. What is triplicated is everything *above* the
registers.

And it is not only a cross-board problem. `drivers/i2c_rtc.c` carries two
dispatchers for the same driver — a U-mode one on RP2350 and a kernel-mode
one everywhere else — which is the same duplication on a single board.

### 0.4 Why before phase 28, and not after phase 27

Phase 28 is Ethernet on the P4: a MAC, a clause-22 PHY, and RMII. It is the
largest new driver on the roadmap, and it will be a task like every other
driver here. Written before this phase it becomes copy ten and domain eight;
written after, it is the first driver that never had to know how a driver
task is assembled.

Against that, one real argument for waiting: this phase extracts an
abstraction, and phase 27's own lesson is that abstractions designed from two
data points do not survive the third. That argument does not apply here, and
saying why is the point — **this extraction is from N=9, not N=2.** The nine
sites already exist, already work, and already agree. That is the opposite of
speculative.

The remaining phase 27 milestones (E4–E8) are not blocked by this and this is
not blocked by them. E5 is the one that could plausibly interfere, since it
brings U-mode to the P4 — but `task_create_driver()` is already mode-agnostic
(`kernel/sched.c`), the U-mode entry is a separate step inside each task
body, and §4.3 below is written so that E5's P4 drivers get the extracted
version rather than an eighth copy.

## 1. The four categories, and the right answer for each

The generalization the question asked for. Every piece of hardware support in
this tree falls into one of four categories, and they want opposite
treatment.

### A. Register-level access to a specific peripheral

`drivers/uart_esp32p4.c`'s `UART_CONF0_SYNC`, IO_MUX `MCU_SEL`, the CLIC's
`clicintattr` bytes, the RP2350's `UART0_IMSC`.

**Never shared, across chips or across vendors.** Different silicon, different
registers, and the failure mode of sharing is not a merge conflict but a
plausible-looking wrong value. Phase 27's own record: `UART0_SYS_CLK_EN` and
`UART0_APB_CLK_EN` were written as bits 24 and 25, reasoned from where UART0
sits in the peripheral list, and are actually 18 and 7 — in two different
registers that order their fields differently, so there was no position to
infer. A HAL that made "the UART clock gate" a portable concept would have
made that error easier to write, not harder.

Per-board files. Per-board provenance, recorded in the file. The `hw_ver1`
vs `hw_ver3` split in ESP-IDF is the same rule applied by the vendor to their
own silicon.

### B. Controller seams — which IRQ fired, what time is it, how does memory map, how is a task isolated

`devirq.h`, `arch/trap.h`, `ticker.c`'s `arch_*`, `arch/vmm.h`, `arch/pmp.h`,
`kernel/time.c`'s counter read.

**Already abstracted. Keep extending the same way.** These are few — about six
— and they are stable because they answer questions every platform must
answer. Phase 27 is the evidence (§0.2). The seam is *narrow on purpose*: the
P4's CLIC arm is 200 lines of `trap.c` and one 55-line header, and every one
of `devirq_attach()`'s callers is unaware any of it happened.

The one addition E3 needed was a return value on `devirq_dispatch()`, because
"unhandled" means something different on a level-triggered controller than on
a claim/complete one. That is a seam being *refined by evidence*, which is
what a good seam does.

### C. The driver-as-task pattern

The serve loop, the endpoint registration, the retry wrapper, the liveness
check, the U-mode domain construction, the ISR→`task_unblock()` waiter slots.

**The actual debt, and what this phase builds.** Zero board-specific content;
nine near-identical copies of the serve loop and seven of the U-mode domain.

### D. Device-class contracts

`block_dev_t`, `net/netif.h`, the console binding, the `/dev` registry.

**Already abstracted; held.** `netif_register()` has taken ENC28J60 and CYW43
and phase 28 plugs the P4's EMAC into it unchanged. Nothing to do here.

### The rule that falls out

**Extract at the third implementation, never at the second.**

Two copies is evidence that something recurs. Three is a design. This is
exactly the judgement E2 deferred and E3 made about the UART drivers, and
writing it down means it fires on its own next time instead of needing an
argument. It would also have caught `i2c_rtc.c`'s dual dispatch, and it will
fire on the U-mode domain pattern the moment phase 27's E5 adds an eighth.

The corollary matters as much: **at the second implementation, do nothing but
note it.** Phase 27's E2 was right to refuse — extracting then would have
meant changing two drivers that worked, on two boards, to make room for a
third that had never run.

## 2. What is being built

Two files, and nothing else new:

* **`drivers/include/drivers/driver_task.h`** — the contract.
* **`drivers/driver_task.c`** — the serve loop, the endpoint lifecycle, the
  retry wrapper, and the U-mode domain builder.

### 2.1 The serve loop

Every one of the nine task bodies is this, with a different `switch`:

```c
static void X_task_body(void *arg) {
    while (!g_X_ep) sched_yield();
    for (;;) {
        uint32_t req_len = chan_serve_wait(g_X_ep);
        ... switch on g_X_req[0] ...
        chan_serve_reply(g_X_ep, resp_len);
    }
}
```

The extracted form keeps the switch in the driver and takes everything around
it:

```c
typedef uint32_t (*driver_serve_fn)(void *ctx,
                                    const uint8_t *req, uint32_t req_len,
                                    uint8_t *resp, uint32_t resp_cap);

int driver_task_start(const driver_task_spec_t *spec);
```

where the spec carries the endpoint name, the stack size, the two buffers,
the priority, the serve function and its context.

**Three invariants move with it**, and each is currently a comment repeated in
every driver rather than a property of the mechanism:

1. *Never `printk()` from inside the loop* — a caller can be blocked on this
   endpoint while holding `printk_lock()`, and taking that lock here
   deadlocks against it.
2. *Never call back into anything that could `chan_call()` this same
   endpoint.*
3. *Only this task may touch the `*_hw_*` functions while it is alive.*

Invariant 1 becomes enforceable rather than merely documented, which is worth
more than the lines saved.

### 2.2 The U-mode domain builder

Seven drivers build a `mem_domain_t` by hand. The shape is always: the task's
own U-mode stack (R/W), the shared `.utext` page from `board_text_region()`
(R/X), and one or more MMIO windows (R/W) — then `task_set_domain()`, then
either `arch_enter_user()` or a refusal that says isolation could not be
verified and falls back to direct hardware access.

```c
int driver_umode_enter(const driver_umode_spec_t *spec);
```

The refusal branch is the part that most wants to be shared. It is a safety
property — *refuse rather than claim unverified isolation* — and it is
currently seven separate implementations of one policy, each with its own
message.

### 2.3 What deliberately does not move

* **The register halves.** Category A. Untouched by this phase.
* **The wire protocols.** `'H'`/`'R'`/`'W'` for uart, the blk opcodes, the
  i2c ops — these stay in their drivers. The framework carries bytes and does
  not read them.
* **The UART TX-batching facade.** `uart_rp2350.c`'s `uart_putc()` and
  `uart_flush()` have the USB CDC console mirror woven through them. Sharing
  the batching would mean either a hook in common code that exactly one board
  uses, or a two-of-three extraction — which is the shape that was already
  defensible before phase 27 added a third file. **G4 extracts the task body
  and wire protocol and stops there.** This is the decision phase 27's E3
  recorded, carried here unchanged.
* **The waiter-slot/ISR pattern.** Tempting, and not yet. It looks identical
  in `uart_16550.c` and `uart_esp32p4.c`, but `uart_rp2350.c` serves TX only
  (its RX arrives through two paths and the console pump polls both), so
  there are two implementations of it, not three. **The rule says note it and
  wait.** Phase 28's Ethernet driver will be the third, and that is when to
  look again.

## 3. Three defects phase 27 found in the seams that already exist

Small, independent of the framework, and worth fixing on their own terms.
They are in this phase because they are the same category of work: places
where a board fact leaked into shared code.

1. **`kernel/meminfo.c` assumed the image is one contiguous run from the
   bottom of RAM.** True on three targets, false on the P4, whose linker
   script splits `.bss` and the boot stack into low L2MEM (safe there only
   because they are NOLOAD, under the ROM's download buffers) with everything
   loadable above. E2 added a `CONFIG_BOARD_ESP32P4` arm. The arm is correct
   and the *shape* is wrong: image extent should be a linker-symbol contract
   every script satisfies, not a per-board special case in the file whose
   entire purpose is that its numbers come from the same memory map the image
   was linked against.

2. **`trap.c`'s fatal-dump instruction readback had a hardcoded address
   window.** `0x10000000`–`0x20082000` covers RP2350's XIP flash and QEMU
   virt's RAM. The P4 links at `0x4FF00000` and fell outside it, so every
   fault on that board would have printed `inst=0x00000000` —
   indistinguishable from a genuine zero word. E3 added a board arm using
   `_ram_start`/`_ram_end`. Every target has equivalent symbols; the arm
   should not exist.

3. **`kernel/main.c` couples the global interrupt enable to `ticker_init()`
   succeeding.** `irq_restore(IRQ_ENABLE_BIT)` runs only inside
   `if (ticker_init(100))`. That is a policy accident: a board can legitimately
   want interrupts without preemption, which is precisely the P4 today. E3
   worked around it by setting `mstatus.MIE` inside `trap_init()`, matching
   what the RP2350 arm already did — so the workaround is now in two of four
   arms and the coupling is still in `main.c`. The two decisions should be
   separated: "the interrupt controller is up" and "there is a preemption
   tick" are different facts.

## 4. Milestones

### G0 — The seam inventory, and the rule

Documentation only, no code. `plan/` gains the table from §0.2 in a
maintained form: every hardware seam, the header that declares it, the
implementations behind it, and what phase 27 proved about it. §1's four
categories and the third-implementation rule go somewhere they will be read
before the next driver is written — `CLAUDE.md` or the equivalent, not buried
in a plan document.

Done when: every row of the inventory names a file that exists, and the rule
is stated where a person about to write a second copy of something would meet
it.

### G1 — The three leaks

§3, all three, independently. No framework work; this is the smallest useful
commit in the phase and it can land first.

Done when: the `CONFIG_BOARD_ESP32P4` arms in `kernel/meminfo.c` and
`arch/riscv/common/trap.c`'s `inst=` window are gone, replaced by symbols
every linker script defines; and a board with a working interrupt controller
and no tick gets interrupts enabled without `trap_init()` having to do it
behind `main.c`'s back. Ten presets clean, QEMU 359/359, and the P4 still
reaches a shell with `rx_wakes` growing.

### G2 — `driver_task.c`: the serve loop

§2.1. Migrate exactly two drivers: `drivers/virtio_blk.c` and
`drivers/uart_16550.c`. Both are QEMU-only, so the full test suite is the
verification and no hardware is needed to trust the result.

Done when: those two drivers have no `chan_serve_wait()` loop of their own,
`driver_task.h` documents the three invariants as properties rather than
comments, and QEMU is 359/359 with no test changed.

### G3 — `driver_task.c`: the U-mode domain

§2.2. Migrate the seven `arch_enter_user()` sites. One driver per commit, in
increasing order of consequence: `tm1638` (6 `mem_domain_add`s, a display),
`i2c_rtc`, `st7735`, `pico_clock_green`, `spisd`, `usb_cdc`, `uart_rp2350`
(12, and the console — last, because getting it wrong removes the way to find
out).

This is the milestone that needs hardware. Verification is the RP2350
personas doing what they do: `pinall`, `i2c scan`, `date`, `blkstats`, the
clock display drawing, and the USB CDC console answering on `/dev/ttyACM1`.

Done when: all seven use `driver_umode_enter()`, the refusal path is one
implementation, and each persona has been booted on its board.

### G4 — The UART console family

The debt phase 27's E3 named and did not pay. Extract the task body and wire
protocol from the three console drivers; leave the facade and the register
halves alone (§2.3).

Done when: the three drivers share their task half, ten presets build clean,
QEMU is 359/359, and both boards' consoles work — the P4's checked with
`uartstats`'s `rx_wakes`, the RP2350's on both its console paths.

### G5 — The framework's own documentation

`drivers/include/drivers/driver_task.h` is the file a future driver author
reads first. It should be able to answer "what is a driver in this kernel"
without reading nine examples. One worked example in the header, and
`README.md`'s architecture section updated.

Done when: writing a new driver task requires reading one header, and phase
28 can start from it.

## 5. How it is tested

Three layers, matching the existing convention:

* **QEMU suite (359 tests).** Covers `virtio_blk` and `uart_16550` fully, and
  the serve loop through every test that types at the shell — which is nearly
  all of them. G2's entire verification.
* **RP2350 hardware.** The only way to test G3 and half of G4. Six personas,
  and `tests/hw/` already has the flashing and console tooling.
* **ESP32-P4 hardware.** `tools/p4run.py --cmd` for G1 and G4.

The property that makes this phase safe to do at all: **it changes no
behaviour.** Every milestone's success condition is that everything does
exactly what it did before, with fewer implementations of it. Any observable
difference is a bug, which is a much easier standard to test against than
"the new thing works".

## 6. Explicitly not in this phase

* **No HAL for peripheral registers.** §1 category A. This is the thing the
  phase exists to argue against, and it should not sneak in as "while we are
  here".
* **No new device classes.** Category D held; leave it.
* **No changes to `kernel/chan.h`.** The framework sits on the endpoint API,
  it does not modify it.
* **No changes to any wire protocol.**
* **No performance work.** The serve loop's cost is a function call per
  request; if that ever matters it will be measured, not assumed.
* **No extraction of the ISR waiter-slot pattern.** Two implementations, not
  three. §2.3.
* **Nothing about phase 27's remaining milestones.** E4–E8 proceed
  independently. If E5 adds a P4 U-mode driver before G3 lands, it uses the
  existing pattern and gets migrated with the rest — an eighth copy that is
  already scheduled for deletion is not a problem.

## 7. Risks, and what each looks like

* **The nine sites are less identical than they look.** The likeliest failure,
  and G3 is where it would surface — a driver whose domain needs a fourth
  region or a different refusal policy. Looks like: the spec struct growing a
  field per driver. If that happens the answer is to stop migrating, not to
  keep widening the struct; a framework that is one union of nine special
  cases is worse than nine files.
* **A shared serve loop makes a bug shared too.** True, and the reason G2
  migrates only the two QEMU drivers first: the suite exercises that loop
  through hundreds of shell interactions before any hardware depends on it.
* **RP2350 regressions that only appear on one persona.** Six personas, and
  the test suite covers none of them on real hardware. Mitigated by one
  driver per commit and by ordering the console last.
* **Scope creep into a HAL.** The most likely way this phase goes wrong is by
  succeeding at C and then continuing into A because the momentum is there.
  §6 exists to be re-read.

## 8. Budget

Small by this project's standards, and deliberately so. G1 is an afternoon.
G2 is the design decision plus two migrations. G3 is seven small commits with
hardware verification between them and is the bulk of the phase. G4 is one
family. G0 and G5 are writing.

The measure of success is a negative number: `chan_serve_wait()` call sites
outside `driver_task.c` should go from nine to zero, `arch_enter_user()` from
seven to one, and `mem_domain_add()` from 63 to a handful — with every test
that passed before passing after, unchanged.
