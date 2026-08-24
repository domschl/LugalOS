# Phase 17b — the clock task, split: a driver that can be confined

**Status: planned 2026-08-24.** Small and structural. No new features, no new
hardware, nothing a user of the clock should be able to see afterwards.

Numbered 17b rather than 18 because it finishes phase 17's own C7 and belongs
to the phase-12 microkernel story; 18 stays reserved for networking and auth.

## Why: C7's third bullet rests on a false premise

Phase 17's C7 says:

> `clockisotest`: every other RP2350 driver task has an isolation test proving
> its PMP domain confines it (`kernel/shell.c:123-139`); the clock task, added
> after that sweep, has none. Adding one is small […]

The premise is that the clock task was skipped because it arrived late. It
was not. It was skipped because **it is not the same kind of thing as the
tasks that sweep covered**, and no isolation test can be written for it as it
stands: `pico_clock_green_task_start()` calls `task_create_sized()` and stops
there — no `mem_domain_add()`, no `task_set_domain()`, no `arch_enter_user()`.
There is no domain to put on trial. It is the only RP2350 driver task still
running in kernel mode.

**Chess answered the same question the other way, and that is the model.**
`chess_ui.c` is not a task at all: it runs inside the shell/Lisp task and
reaches hardware through the `st7735`/`tm1638` facades, which route into
U-mode driver tasks over channels. The isolation tests cover the thin servers
underneath the application — `st7735isotest`, `tm1638isotest`, `blkisotest` —
and nobody ever expected a `chessisotest`.

The clock inverted that. Phase 12's M4.5 made `CLOCK_OP_RUN` *one long served
call*: the entire appliance loop — menu state machine, I2C reads via the
`i2c` task, DCF-77 feed, console interrupt polling, timezone conversion —
runs inside the driver task, which is why `clockstats` reads `calls=1` for a
whole session (`plan/phase12_microkernel_migration.md:1045-1090`). The reason
given was the ~1 kHz row scan, and it was the right call at the time: putting
`chan_call()` on a per-row hot path would have been a cadence nothing else in
this codebase comes close to.

**What that reasoning missed** is that the row cadence does not have to leave
the driver for the *policy* to leave it. An op that scans one whole frame —
eight rows, ~8 ms — and returns whatever the buttons did meanwhile costs ~125
calls/s instead of ~1000, keeps every microsecond-sensitive loop inside the
server where it belongs, and puts the appliance in the caller's task exactly
where chess's is. That is this phase.

Worth stating plainly, because it is the honest accounting: chess's
arrangement is not *more* confined than the clock's. Chess's application code
is equally unprivileged-in-name-only — it runs in the shell task, which is
kernel mode. The difference is that chess's **hardware access** sits behind a
U-mode boundary and the clock's does not. This phase moves the clock's
hardware access behind the same boundary; it does not pretend to confine
either application.

## The shape afterwards

```
  before:  shell/lisp task --[RUN, one call for the whole session]--> clock task
                                                                      ├─ appliance loop
                                                                      ├─ menu state machine
                                                                      ├─ DCF-77 feed
                                                                      ├─ i2c chan_call ──> i2c task
                                                                      └─ row scan + buttons

  after:   shell/lisp task ── appliance loop, menu, DCF-77 feed, console
              ├─ i2c chan_call ─────────────> i2c task
              └─ clock chan_call (~125/s) ──> clock task (U-mode)
                                               └─ frame buffer, row scan,
                                                  buttons, LDR, buzzer
```

`drivers/pico_clock_app.c` and `drivers/pico_clock_ui.c` do not move and
barely change: they already talk to the hardware only through
`drivers/pico_clock_internal.h`, which becomes the **client** side of the
wire rather than a set of in-task direct calls. That header was written as
"everything the app needs from the driver, and nothing else" — it turns out
that is exactly the interface definition this split needs, which is why the
split is affordable at all.

### The wire

One op per logical operation, all short and non-blocking except where noted:

| op | request | response |
|---|---|---|
| `F` scan frame | `now_ms` (8) | key events (n × 2: key/press, held/16 ms) |
| `C` clear | — | — |
| `Z` blank | — | — |
| `H` show time | `h, m, colon` | — |
| `E` show temp | `c` (i8) | — |
| `T` draw text | chars | — |
| `X` draw text at | `col` (i8), chars | — |
| `A` draw bars | scores | — |
| `W` weekday | `dow` | — |
| `I` indicator | `ind, on` | — |
| `S` set brightness | `level` (i8) | — |
| `P` beep | `ms` (2) | — (holds, scanning) |
| `L` read light | — | `light` (2) |
| `G` pin levels | — | `set/up/down` (1) |

`R` (run), `K` (pop key), `B` (keys test), `D` (led walk), `M` (dcf monitor)
all go away: the first because the appliance loop moves out, the second
because `F` returns the events, and the last three because the diagnostics
move to the caller and are written against the ops above. That is not
incidental tidying — every one of them prints to the console, and a U-mode
task cannot.

### The domain, and why it is exactly at the limit

`MEM_DOMAIN_MAX_REGIONS` is 5 on RP2350 (eight PMP entries less the three
shadowing Hazard3's hardwired U-mode grants). The clock server needs:

1. `SIO_BASE` — matrix pins, buttons, buzzer (R/W)
2. `ADC_BASE` — the LDR (R/W)
3. `TIMER0_BASE` — row period and dim pulse timing (**R only**)
4. `.clocktext` — its own executable page (R/X)
5. stack **and** state, one region (R/W)

Five exactly. The one that needs justifying is (5): every other driver keeps
its ustack separate, and `usb_cdc` puts its state in a second region — but
`usb_cdc` needs only two MMIO windows and the clock needs three. The layout
is **stack low, state high**, so the stack grows *down* toward the region
base and an overflow leaves the region and faults, rather than growing up
into the state and corrupting it silently. That is the safe half of the
tradeoff; the unsafe half (state below a downward-growing stack) is what the
separate regions elsewhere exist to avoid, and this ordering avoids it too.

(3) is read-only on purpose: the server needs to *know* the time, never to
set it. Granting `TIMER0` R/W would let a compromised display driver reprogram
the system clock.

## Milestones

**S1 — the split, still kernel-mode.** New ops; `clock_hw_*` becomes routed
facades; the appliance loop runs in the caller's task; diagnostics move
client-side. `pico_clock_green_run()` calls `clock_app_run()` directly.
*Verify on hardware:* the clock looks and behaves exactly as before —
brightness ladder included, since phase 17 §10 landed the same day — the menu
responds, DCF-77 still decodes, `clockstats` now counts ~125/s rather than 1.

**S2 — U-mode.** `.clocktext` page, the combined stack/state region, the five
grants, hand-rolled syscall stubs, an if/else dispatch (never a `switch`: on
this compiler a jump table lands outside the granted text page — the lesson
`st7735_umode_body()` paid for on hardware). Then `clockisotest`, a twenty-line
copy of `st7735_isolation_test()` against the grants above.
*Verify on hardware:* `clockisotest` reports ISOLATED and the task does *not*
exit cleanly (the store must fault), and the clock still runs.

**S3 — housekeeping.** Re-baseline `tools/sizereport-rp2350-clock.json`,
correct C7's note in phase 17, correct phase 12's M4.5 note (its "one call for
the whole loop" verification is now historical), README where it describes the
persona.

## Risks, and what each would look like

- **Display timing.** A frame op means the panel is dark while the caller does
  its ~1 ms I2C read once a second, and dark across each chan_call gap. The
  first is what phase 11 already lived with; the second is new and small.
  *Symptom if wrong:* visible stutter or dimming. *Answer if wrong:* scan two
  frames per call, or move the I2C read behind the same op boundary.
- **DCF-77 sampling drops from ~1 kHz to ~125 Hz.** `dcf77_service_feed()`'s
  contract says "call at 50 Hz or better", so 125 Hz is inside spec — but the
  decoder scores *spacing* in milliseconds, and phase 17 §3 spent real effort
  on that measurement. *Symptom if wrong:* `dcf-monitor` mean quality drops or
  `sync losses` rises against a pre-split run.
- **`.clocktext` does not fit in 4096 bytes.** The server carries glyphs, the
  proportional font and its renderer. *Answer if so:* an 8192-byte NAPOT page
  — allowed, self-aligned, still one region.
- **Stack depth in U-mode.** Measured from the disassembly like
  `st7735_ustack`'s 1392 bytes, not guessed.

---

## As landed, 2026-08-24

**S1 and S2 are done and verified on hardware; S3 is done.** The clock task is
a thin frame-buffer-and-row-scan server running in U-mode under five PMP
grants, the appliance loop runs in the caller's task, and `clockisotest`
reports `ISOLATED` with the probe faulting as it must (`cause 7`, store access
fault, at an `epc` inside `.clocktext`).

### The bug that cost the afternoon: ACCESSCTRL, not PMP

The first U-mode build looked perfect and killed the board's USB. The panel
kept working, which is exactly what made it confusing — and is worth stating
as a rule, because it will happen again: **when the clock task dies, every
client call silently falls back to direct hardware access, so the display
looks identical whether the server is running or dead.** The panel is not
evidence about the server. `/proc/ps` is.

The fault itself:

```
[Trap] User task faulted: cause 5, epc=0x1002800e, addr=0x0 -- terminating the task
```

`0x1002800e` is `lw a5,40(a5)` with `a5 = 0x400b0` — the `TIMER0_TIMERAWL`
read in `srv_time_advance()`, on the first frame of the first session. Not
PMP: **RP2350's ACCESSCTRL**, a Secure/Non-secure filter entirely upstream of
PMP that a task's own domain cannot grant its way around. Every peripheral
resets to Secure-privileged-only, and U-mode is Non-secure + Unprivileged.

This project has now met all three shapes of it, and phase 17b needed all
three at once:

| what | register | fails how |
|---|---|---|
| GPIO (matrix, buttons, buzzer) | `GPIO_NSMASK0`, one bit per pin, no password | **silently** — writes ignored, panel just stays dark |
| ADC (the LDR) | `ACCESSCTRL_ADC` (0x7c), SP/NSP/SU/NSU, needs the `0xacce` password | load/store fault |
| TIMER0 (the server's own clock) | `ACCESSCTRL_TIMER0` (0x98), same shape | load fault — this is the one that bit |

TIMER0 is the one no previous driver needed, because no previous U-mode driver
kept its own clock, and it is the least obvious of the three: kernel-mode code
reads the very same register through `time_get_us()` with none of this
applying, so it works right up until the moment it runs unprivileged.

### How it was found, which is the transferable part

Not by reading code. Four theories died first — CPU starvation of the `usbcdc`
task (the appliance no longer calls `time_delay_us()`, which used to pump
`usb_cdc_task()` about a million times a second), the task dying and falling
back, RP2350's E6 permission-bit reversal on the read-only TIMER0 grant, and
the five-region PMP budget. **S1 killed the first two on its own**: it had the
identical CPU profile and identical fallback behaviour, and USB was fine
across it. E6 and the region budget were cleared by reading
`arch/riscv/common/mem_domain.c`, which handles both correctly.

What actually found it was **a diagnostic build with the appliance not
auto-starting** (`(boot-program)` stubbed to `""`), which gave a live console
on an otherwise identical image — and then the board simply said what was
wrong, in one line, the moment the clock was started by hand.

**And the out-of-band channel that made the rest possible: 9P over ACM1.**
With the console occupied by the appliance, `host/p9lib` against
`/dev/ttyACM1` still read `/proc/ps` and `/proc/kmsg` throughout — including
proving that `usbcdc` was alive and serving the whole time, which is what
finally buried the starvation theory. A second, independent transport into a
running system is worth more than any amount of reasoning about it.

### The second bug, found on the way and fixed here: an interrupt you could lose forever

**Ctrl-C over the USB console did not reach the running appliance**, so the
clock could not be exited from `/dev/ttyACM0`. Measured on the **released
0.13.0 firmware** too, with the same script and the same result, so it
predates phase 17b — but it is exactly the kind of fault an appliance persona
is uniquely able to expose, so it is fixed here rather than filed.

`console_pump()` (`kernel/console.c`) was:

```c
while (!pushback_full() && uart_has_char()) {
    char c = uart_getc();
    if (c == 0x03) g_interrupt_pending = true;   /* the latch, INSIDE the loop */
    pushback_put(c);
}
```

Stopping when the ring is full is right for **data**: it is the back-pressure
that keeps surplus input in the device instead of destroying it (an earlier
version drained the device dry and silently ate everything past the 128th
byte). It was catastrophic for **interrupts**: the latch lived inside that
loop, so a full ring meant no Ctrl-C could ever be latched again for the rest
of the boot. And on an appliance nothing ever drains that ring — the clock
owns the console for as long as it runs — so the state is permanent and the
program becomes impossible to interrupt.

What filled it was our own tooling: 127 bytes of **9P `Tversion` port-probe
frames**. `tests/hw/rp2350.py::_probe_port()` writes one to each candidate
port to classify it, and its comment called them "harmless line noise" on the
console. Harmless as data; not harmless as occupancy. That comment is
corrected now.

**The fix** separates the two jobs the pump had conflated. Buffering input for
a future reader must stop when the ring is full; watching for an interrupt
must never stop. So the latch moved *out* of the drain loop and now runs off
`uart_peek_interrupt()` — a non-consuming scan of the console device's own
ring (`usb_cdc_peek_interrupt()` on RP2350; `false` on QEMU's 16550, whose
FIFO cannot be inspected without taking the byte, so that target keeps its
existing behaviour). Nothing is eaten, nothing depends on having room to store
it, and the byte still reaches whoever eventually drains the ring — the same
"latch it but still queue it" rule the pump already followed, and which
`edit_multiline_box()` depends on for its Ctrl-X Ctrl-C exit.

*Verified on hardware:* with the pushback ring deliberately stuffed with 200
junk bytes — the exact condition that used to wedge it — Ctrl-C exits the
appliance and the shell comes back.

### Verified

- `clockisotest`: `ISOLATED (kernel memory untouched)`, `Task exited cleanly:
  no`, probe faulting inside `.clocktext`.
- `/proc/ps`: the `clock` task shows `Isol PMP` and cycles READY/BLOCKED while
  the appliance runs — i.e. it is genuinely serving frames from U-mode, not
  dead with the client on the fallback path.
- Static confinement check on the linked image: every jump target in
  `.clocktext` lands inside `.clocktext`, and every data reference resolves
  into a granted region — the only out-of-domain reference in the page is the
  isolation probe's own deliberate canary store. `.clocktext` uses ~3.3 KB of
  its 4 KB page; deepest U-mode stack chain ~224 bytes of the 1536 granted.
- ~1 minute soak with the appliance running: USB present throughout, 9P
  responsive, no kernel messages after boot.
- On the shipping (non-diagnostic) image, over the console: `clockstats`
  reads **6270** after ~50 s of appliance — the frame op at ~125/s, which is
  S1's own verification criterion, and the number M4.5 used to report as 1.
- QEMU 261/261 on both targets; all four personas build clean; `sizecheck`
  re-baselined (+2066 bytes: the 2 KB region plus the app's event queue; the
  heap stayed at 95 pages).
- The scan artefact from the S1 report is gone (the frame op closes OE before
  returning, so no row stays lit across the gap) and the DCF monitor's bars
  redraw on change rather than on a 250 ms timer.
