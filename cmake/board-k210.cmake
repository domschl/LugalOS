# Per-board facts for the Kendryte K210 on a Maix Bit (the 65.1 probe's
# board), consumed by cmake/gen_config.cmake to produce lugalos_config.h.
# Phase 65 §1.5: every number here is a measurement from
# plan/phase65_k210_probe_report.txt or a schematic read, not a datasheet
# transcription.

# 6 MB of SRAM at 0x8000_0000, the whole writable address map apart from
# devices. There is no MMU, no PMP, no S-mode: one domain, M-mode only,
# cooperative (D2). The kernel image, heap, and stack share it; palloc
# gets what is left after the image -- sized generously here and trimmed
# by the palloc self-test report once the image size is known (65.3).
set(CONFIG_PALLOC_MAX_PAGES 1024)

# The buddy arena (kernel/balloc.h), in pages: the RP2350/P4 figure.
set(CONFIG_BALLOC_ARENA_PAGES 4)

# The console: UARTHS, the SiFive-style UART at 0x3800_0000 whose pins are
# hardwired to the board's CH340/CH552 USB link (measured: BROM divisor
# 0x0d38, 115200-8N1; the ISP shares the same wires and also runs at
# 115200, so a loaded image needs no divider write to talk).
set(CONFIG_UART0_BASE      0x38000000)

# Kernel tick/clock source: the CLINT at 0x0200_0000 (mtime at +0xBFF8,
# mtimecmp at +0x4000, msip at +0x0 per hart). The generic M-mode ticker
# hardcodes this base. Its rate was measured in 65.1 as CPU/50 = 8 MHz at
# the BROM's default 400 MHz PLL setup -- *not* the 10 MHz the generic
# ticker assumes; kernel/time.c carries the K210 rate so the wall clock is
# honest, and the 1.25 tick error is listed as a known gap until 65.6
# replaces the nominal with the board fact.
set(CONFIG_K210_MTIME_HZ   8000000)

# CPU clock as the BROM leaves it (measured via mcycle/mtime = 50 and the
# probe's mtime rate; D.1's 400 MHz is the PLL default the BROM applies).
set(CONFIG_CPU_FREQ_MHZ    400)

# Lisp-light on the first bring-up target, like the C6: configuration and
# small scripts, not the CAS. Revisit when the 6 MB heap is measured.
set(CONFIG_LISP_NODE_POOL   4096)
set(CONFIG_LISP_STRING_POOL 256)
