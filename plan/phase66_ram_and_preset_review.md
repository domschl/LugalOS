# Phase 66 — RAM and Preset Configuration Review

**Status: placeholder. Written 2026-10-29. Not scheduled; independent of phase 52.**
Milestone scheme: `66.1`, `66.2`, …

---

## 0. Scope (to be written)

To be elaborated when the phase is scheduled. Prompted by the Pico 2 W's heap (80 pages) and the
Tab5's and LCD-7B's low RAM (56 and 328 bytes free at the 2026-10-29 build), and by the wish for
presets that do not all carry everything.

Target: to find strategies to allow maximal scalability of the LugalOS platform. Ideally allowing 
to dynamically scale LugalOS to platforms with even less RAM (e.g. ESP-C3, 320kB)

### 0.1 Topics for analysis

- Unnecessary use of SRAM (either avoidance or PSRAM if available)
- Stack usage and stack safety
- Unnecessary use of flash, if resources can be moved to SD for a given preset

#### 0.1.1 Presets

- Resource-usage of each process, service, and application is configured platform- and preset-specific
- Stricter exclusion of functionality that is of lower priority for a given preset

### 0.1.2 Dynamic processes

- Some means to swap out processes in favor of some foreground process?
- Evaluation of process-groups (e.g. shared stack? allocations?)

### 0.2 Extreme measures: build-time Unikernel

Evaluation of a 'Unikernel' concept for appliance-like presets: in order to bundle more required functionality with a given preset, the micro-kernel idea is replaced by a build-time construction of a Unikernel that eliminates the micro-kernel's process-, stack/heap- and IPC-overhead.

- Starting point needs to be the micro-kernel sources, and the Unikernel is strictly build-time derived from a fully functioning micro-kernel.
- This requires probably some 'glue'-layer that is able to 'fold' multiple micro-kernel processes into a single space?

Design-goal: keep architecture strictly micro-kernel, but allow for build-time optimization that grants all advantages of a monolith.

