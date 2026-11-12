# StarFive VisionFive 2 Hardware Acceleration Architecture & Cognitive Core

This document details the hardware acceleration architecture of the StarFive JH7110 SoC (VisionFive 2 v1.3B) in LugalOS, evaluates the onboard GPU (Imagination PowerVR BXS-4-64), analyzes the 2D display and overlay engines (VeriSilicon DC8200), and presents the design of the integer-only multicore INT8 GEMM (General Matrix Multiply) cognitive inference core across SiFive U74 Harts 1..4.

---

## 1. Imagination PowerVR BXS-4-64 GPU Architectural Evaluation

The StarFive JH7110 SoC integrates an **Imagination PowerVR BXS-4-64 MC1** GPU core clocked up to 600 MHz. The GPU is designed for OpenGL ES 3.2, Vulkan 1.2, OpenCL 3.0, and Android NN HAL workloads.

### Hardware Interface & Control Flow
1. **Firmware Processor & Microcode Dependency:**
   * The BXS-4-64 contains an internal firmware co-processor (Meta/RISC-V management core) that requires a proprietary binary microcode blob (`rgx.fw.*`) uploaded by the host operating system into physical memory at boot.
   * The GPU cannot execute shader pipelines, rasterization, or compute dispatches without this proprietary firmware blob running inside the GPU subsystem.
2. **Host-Firmware Communication Queues (FWCCB):**
   * Host communication relies on circular shared-memory queues: Firmware Circular Command Buffers (FWCCB) and Host-Firmware Shared Virtual Memory (SVM).
   * Synchronization uses complex fence primitives, host interrupts, and firmware event notification registers.
3. **GPU MMU & Virtual Address Space:**
   * The BXS-4-64 possesses its own multi-level page table MMU requiring page-by-page mapping and dynamic translation table synchronization between the host RISC-V MMU (Sv39) and the GPU BXS MMU.

### Architectural Decision for Freestanding Microkernel
* **Freestanding Philosophy:** LugalOS operates under strict `-nostdlib -ffreestanding` constraints with pure static/linear memory domains and zero un-auditable binary blobs in the microkernel core.
* **Firmware Blob Incompatibility:** Incorporating megabytes of opaque vendor firmware blobs and maintaining a multi-ring Vulkan/OpenGL userland runtime within a lightweight microkernel contradicts the core LugalOS mission of verifiable, deterministic computing.
* **Strategic Verdict:** The PowerVR BXS-4-64 GPU hardware is reserved for future Linux virtualization, guest shims, or isolated driver domains. For native LugalOS operation, graphics, windowing, and cognitive inference are accelerated via **native DMA hardware (DC8200)** and **optimized multicore RISC-V integer vector math (INT8 GEMM)**.

---

## 2. DC8200 Display Controller 2D & Overlay Engine

The VeriSilicon DC8200 display processor provides native hardware offloading that relieves the CPU from scanout rasterization and compositing overhead:

### 1. Hardware Cursor Engine 0 (`0x1468u`)
* The DC8200 incorporates dedicated hardware cursor planes (Cursor 0 at `0x1468u`, Cursor 1 at `0x24E8u`).
* When active, a 16x16 or 32x32 cursor sprite is stored directly in the display controller's SRAM.
* Moving the mouse updates the cursor position registers (`0x1470u` / `DC_DISPLAY_CURSOR0_XY`) in hardware during VBLANK.
* The hardware mixer overlays the cursor over the active scanout plane at scanout time, eliminating dirty rectangle tracking, background save/restore buffers, and CPU blit operations.

### 2. Multi-Plane Overlay Compositing
* The DC8200 supports multiple independent overlay planes:
  * **Layer 0 (Base Scanout Plane):** Full-screen desktop workspace (1080p, 1440p, or 4K RGB565).
  * **Layer 1 (Overlay Plane, `0x1540u`):** Hardware popup menus, modals, or on-screen keyboard rendered independently without dirtying the desktop window buffer.
  * **Polyphase Scaling Filters (`0x2500u..0x2510u`):** Hardware Lanczos/bicubic polyphase filter engines enabling real-time upscaling (e.g. 1080p to 4K) with zero CPU cycles.

---

## 3. Multicore INT8 GEMM (General Matrix Multiply) Engine

To enable on-device cognitive computing, embedding models, and local neural network inference (Phase 81) under the strict **Zero Floating-Point** kernel rule (`sstatus.FS = 0`), LugalOS implements a high-throughput integer-only GEMM engine (`kernel/gemm.c`).

### Mathematical Formulation
Given quantized weight tensor $W \in \mathbb{Z}^{M \times K}$ and activation tensor $X \in \mathbb{Z}^{K \times N}$:
$$C_{i, j} = \sum_{k=0}^{K-1} W_{i, k} \cdot X_{k, j}$$

For memory hierarchy optimization, activation matrix $X$ is stored in transposed layout $B^T \in \mathbb{Z}^{N \times K}$, ensuring that the dot-product traverses contiguous memory lines:
$$C_{i, j} = \sum_{k=0}^{K-1} W_{i, k} \cdot B^T_{j, k}$$

### Quantized Linear Layer
Quantized activation clamping maps 32-bit accumulated integer sums back into 8-bit signed integer space with optional bias:
$$Y_{i, j} = \text{clamp}_{\text{int8}}\left( (C_{i, j} + \text{bias}_j) \gg \text{shift} \right)$$
where $\text{clamp}_{\text{int8}}(v) = \min(127, \max(-128, v))$.

### Optimization Techniques on SiFive U74
1. **8x Unrolled Inner Dot Product:**
   * Traverses $K$-dimension in 8-byte blocks using 64-bit register widths.
   * Maps onto the dual-issue pipeline of the SiFive U74 core, performing parallel memory loads and integer multiply-accumulates without pipeline bubbles.
2. **RV64 Instruction Acceleration:**
   * Utilizes RV64 `Zba` (`sh1add`, `sh2add`, `sh3add`) for zero-overhead pointer indexing across matrix strides.
   * Utilizes RV64 `Zbb` (`clz`, `ctz`, byte manipulation) for fast quantized activation saturation.
3. **SMP Workload Partitioning across 4 Cores:**
   * Matrix rows $M$ are divided equally among Harts 1..4:
     * Hart 1 (Master): Rows $0 \dots M/4 - 1$
     * Hart 2 (Worker 1): Rows $M/4 \dots 2M/4 - 1$
     * Hart 3 (Worker 2): Rows $2M/4 \dots 3M/4 - 1$
     * Hart 4 (Worker 3): Rows $3M/4 \dots M - 1$
   * Persistent worker tasks (`gemm_w1`, `gemm_w2`, `gemm_w3`) wait on atomic job sequence tags (`__atomic_load_n(&req_seq, __ATOMIC_ACQUIRE)`), eliminating task creation overhead.

### Benchmark Results on VisionFive 2 Silicon
Matrix size: $128 \times 128 \times 128$ (2,097,152 multiply-accumulates per iteration, 20 iterations):

| Configuration | Cores | Harts Used | Latency / Iter | Throughput (MMACs/s) | Equiv. MFLOPS |
| :--- | :---: | :---: | :---: | :---: | :---: |
| Single Core | 1 | Hart 1 | 24.2 ms | 86.6 MMACs/s | 173.2 MFLOPS |
| Dual Core | 2 | Harts 1..2 | 12.4 ms | 169.1 MMACs/s | 338.2 MFLOPS |
| **Quad Core** | **4** | **Harts 1..4** | **6.4 ms** | **327.7 MMACs/s** | **655.4 MFLOPS** |

* Multi-core speedup: **3.78x** over single-core execution on 4 cores.
* Numerical validation: **100% exact match** with reference single-core calculations.
* Throughput exceeds the Phase 79 target of 200 MFLOPS by over **3.2x**, providing the compute backbone for quantized edge embeddings in Phase 81.
