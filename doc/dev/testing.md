# LugalOS Testing Framework & QA Guide

LugalOS maintains a comprehensive multi-tier testing framework spanning host unit tests, AddressSanitizer (ASan) memory safety harnesses, automated QEMU regression suites, and physical hardware-in-the-loop (HIL) silicon verification.

---

## 1. Multi-Tier Testing Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│                        TIER 4: HARDWARE SILICON                        │
│   tests/hw/test_rp2350.py   •   tests/hw/test_esp32p4.py               │
│   tests/hw/test_esp32c6.py  •   tests/hw/lcdterm_shots.py (PBM diffs)  │
└───────────────────────────────────▲────────────────────────────────────┘
                                    │ Silicon Verification
┌───────────────────────────────────┴────────────────────────────────────┐
│                    TIER 3: AUTOMATED QEMU RUNNER                       │
│   tests/runner.py (376+ tests across RV32 NOMMU, RV64 MMU, RV64 SMP)   │
│   Preemption • Memory Domains • Copy-Always Channels • FAT32 • Net • VM│
└───────────────────────────────────▲────────────────────────────────────┘
                                    │ Emulation & Architecture
┌───────────────────────────────────┴────────────────────────────────────┐
│                    TIER 2: HOST HARNESSES (ASan/UBSan)                 │
│   tests/host/ (FAT32 engine, chibicc, 9P protocol, embedded IP stack)   │
└───────────────────────────────────▲────────────────────────────────────┘
                                    │ Memory Safety & Host Sanitizers
┌───────────────────────────────────┴────────────────────────────────────┐
│                     TIER 1: PURE ALGORITHMIC TESTS                     │
│   tests/test_lisp_cas.py (Exact arithmetic, limits, Gaussian rationals)│
│   tests/test_cas_simplify.py (AST canonical forms, polynomials)        │
│   tools/gendocs.py --check (Documentation parity verification)         │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Tier 1: Pure Algorithmic Unit Tests

Fast host-based Python test suites verifying core mathematical, algebraic, and documentation algorithms:

```bash
# Verify exact arithmetic, complex numbers, integrals, limits, and matrices
python3 tests/test_lisp_cas.py

# Verify canonical CAS simplification and polynomial factorization
python3 tests/test_cas_simplify.py

# Verify zero documentation drift between Markdown and on-device help
python3 tools/gendocs.py --check
```

---

## 3. Tier 2: Host Harnesses with AddressSanitizer & UBSan

To ensure memory safety and catch buffer overflows, memory leaks, or undefined behavior before flashing hardware, LugalOS compiles core standalone subsystems natively for Linux using `clang`/`gcc` with `-fsanitize=address,undefined`:

* **FAT32 Engine Harness**: Tests cluster allocation, directory traversal, and corrupt partition recovery under aggressive fuzzing.
* **`chibicc` Compiler Harness**: Compiles C test suites natively, verifying parser correctness and abstract syntax trees.
* **9P2000 Protocol Harness**: Tests wire serialization, packet framing, and auth handshakes.
* **IP Stack Harness**: Fuzzes TCP/UDP packet processing and reassembly queues.

---

## 4. Tier 3: Automated QEMU Regression Runner (`tests/runner.py`)

The primary continuous integration and regression suite is [`tests/runner.py`](file:///home/dsc/Source/gith/domschl/lugalos/tests/runner.py):

```bash
# Run the complete automated test suite
python3 tests/runner.py
```

### Coverage
The runner executes over **376 distinct test cases** across three virtual targets:
1. **RV32 NOMMU Target**: Validates PMP memory domain isolation, cooperative/preemptive task switches, and device driver confinements.
2. **RV64 MMU Target**: Validates Sv39 3-level page tables, virtual address space creation and destruction, and 64-bit bignum arithmetic.
3. **RV64 SMP Multicore Target**: Validates dual-hart scheduler lock hand-offs, atomic `amoswap` contention, and cross-hart thread synchronization.

---

## 5. Tier 4: Hardware-in-the-Loop Silicon Suites (`tests/hw/`)

Hardware tests communicate with physical development boards over serial and USB CDC interfaces. They are safe to run speculatively: if a target board is not connected, the test skips cleanly rather than failing.

### Running Silicon Suites
```bash
# Test RP2350 microcontrollers (RP2350-LCD-7, Pico 2, Pico 2 W)
python3 tests/hw/test_rp2350.py

# Test ESP32-P4 boards (Tab5, LCD-7B, or NANO)
python3 tests/hw/test_esp32p4.py --board tab5
python3 tests/hw/test_esp32p4.py --board lcd7b

# Test ESP32-C6-Zero
python3 tests/hw/test_esp32c6.py

# Test CYW43439 Wi-Fi link and network traffic
python3 tests/hw/test_wifi.py
```

### Visual Regression Testing (`lcdterm_shots.py`)
For display presets (RP2350-LCD-7 and ESP32-P4-LCD-7B), [`tests/hw/lcdterm_shots.py`](file:///home/dsc/Source/gith/domschl/lugalos/tests/hw/lcdterm_shots.py) plays a fixed terminal and ribbon scenario, fetches PBM screenshots over 9P, and compares them **pixel-by-pixel** against golden baseline images (masking the RTC clock). This guarantees that terminal rendering, font glyphs, and window splits never suffer visual regression.
