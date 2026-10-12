# ADR 001: Zero Floating-Point in Kernel / M-Mode (`mstatus.FS = 0`)

## Status
Accepted (2026-08-10)

## Context
RISC-V microcontrollers vary significantly in hardware floating-point support. While the RP2350 (Hazard3) and ESP32-C6 do not feature a hardware standard FPU, other chips or virtual machines may. In embedded kernels, supporting floating-point registers in the scheduler requires saving and restoring 32 floating-point registers (`f0`–`f31`) plus status registers (`fcsr`) on every context switch, doubling context switch latency and consuming significant per-task kernel stack memory. Furthermore, compiler-generated soft-float calls (`libm`) introduce non-deterministic latencies, rounding errors, and memory bloat.

## Decision
The LugalOS kernel boots with `mstatus.FS = 0` (FPU disabled in Machine Status). 
1. Any hardware floating-point instruction executed in the kernel or user tasks triggers an immediate **Illegal Instruction** trap.
2. All mathematical routines across the kernel, drivers, graphics, and user space must use pure fixed-point scaling (e.g. $10^6$ fixed-point for transcendental functions) or exact integer/rational arithmetic.
3. The Computer Algebra System (CAS) operates strictly over exact integers, arbitrary-precision bignums, exact rationals $\mathbb{Q}$, and Gaussian rationals $\mathbb{Q}(i)$.

## Consequences
* **Positive**: Context switch overhead is minimized; kernel stacks are compact; zero floating-point register save/restore latency.
* **Positive**: Absolute mathematical determinism across all targets; zero IEEE-754 rounding inaccuracies in algebraic computations.
* **Negative**: User algorithms requiring floating point must be written using fixed-point integer scaling.
