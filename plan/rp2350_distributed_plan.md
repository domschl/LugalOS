# LugalOS Focused Distributed Architecture & QEMU/Hardware Interconnect Plan

> **Architectural Goal**: A focused, pragmatic implementation roadmap to scale LugalOS from a single microkernel into a distributed network of specialized Plan 9 style micro-nodes (Storage Nodes, Display Nodes, Workstations).
> 
> **Core Strategy**: Prune non-essential protocols (CAN bus, complex Wi-Fi blobs) and leverage **USB / UART as the universal bridge** connecting QEMU simulation instances to physical RP2350 hardware nodes seamlessly over a single cable.

---

## 1. Transparent 9P IPC Abstraction Layer

Plan 9 achieved transparent resource distribution because **everything is a 9P file server**. LugalOS extends its Plan 9 `/srv/` IPC namespace model so that local microkernel IPC calls and remote node RPCs share an identical application contract.

```mermaid
flowchart TD
    subgraph Host Workstation / QEMU Node ["Workstation Node (QEMU or RP2350)"]
        App["App / Lisp REPL"] --> VFS["VFS Router (/srv/ /dev/)"]
        VFS -->|Local PID| LocalIPC["Microkernel IPC (sys_ipc_call)"]
        VFS -->|Remote Node| NetGateway["9P Transport Gateway (/srv/net)"]
    end

    subgraph Physical / Simulated Interconnect ["Universal Interconnect Bridge"]
        NetGateway -->|USB CDC / UART / SPI DMA| Wire["Single USB Cable or Serial Wire"]
    end

    subgraph Target Node ["Target Micro-Node (RP2350 or QEMU)"]
        Wire --> RemoteGateway["9P Server Daemon"]
        RemoteGateway --> LocalVFS["Remote VFS Server"]
        LocalVFS --> HW["ST7789 Display / FAT32 SD / EEPROM"]
    end
```

### 9P Micro-Packet Protocol
All LugalOS node communication uses a compact 9P2000 subset:
- **`Tversion` / `Rversion`**: Version negotiation & node auto-discovery.
- **`Tattach` / `Rattach`**: Mount remote namespace (e.g., attach to remote `/srv/display` or `/sd0/`).
- **`Twalk` / `Rwalk`**: Traverse remote directory paths.
- **`Topen` / `Ropen`**: Open remote file/device handles.
- **`Tread` / `Rread`**: Fetch data from remote node.
- **`Twrite` / `Rwrite`**: Send frame or command payloads to remote node.
- **`Tclunk` / `Rclunk`**: Close handle.

---

## 2. Universal Interconnect Strategy: Single-Cable USB & UART Options

To avoid requiring multiple USB cables and CP2102 adapters during development, LugalOS supports 3 serial/interconnect wiring strategies:

```mermaid
flowchart LR
    subgraph CableOpt1 ["Option A: Native RP2350 USB CDC (Single Cable)"]
        PicoUSB["Pico 2 USB Cable"] <-->|/dev/ttyACM0 (Console)| HostPC1["Host PC"]
        PicoUSB <-->|/dev/ttyACM1 (9P Network)| HostPC1
    end

    subgraph CableOpt2 ["Option B: CP2102 Multiplexed UART (Single Cable)"]
        CP2102["CP2102 Adapter"] <-->|/dev/ttyUSB0 (COBS/SLIP Escaped)| HostPC2["Host PC / QEMU"]
    end

    subgraph CableOpt3 ["Option C: Headless Test Mode"]
        CP2102_2["CP2102 Adapter"] <-->|Dedicated 9P Transport| QEMU_Runner["Test Runner"]
    end
```

### Cable Minimization Strategies

#### 1. Option A: Native RP2350 USB CDC ACM Driver (Single USB Cable)
- **Mechanism**: Once LugalOS boots, the RP2350 native USB hardware controller (`0x50110000`) takes control of the onboard USB connector.
- **Composite Ports**: A USB CDC ACM driver presents **two virtual serial ports** over the single micro-USB / USB-C flashing cable:
  - **`/dev/ttyACM0`**: Interactive shell (`lsh`) and UART debug console.
  - **`/dev/ttyACM1`**: High-speed 9P network interconnect to host QEMU / Python test runner!
- **Advantage**: Zero extra wiring, 12 Mbps USB Full-Speed bandwidth, single USB cable for flashing, console, and 9P networking!

#### 2. Option B: CP2102 Multiplexed SLIP/COBS UART (Single Cable)
- **Mechanism**: If using the CP2102 UART adapter (`/dev/ttyUSB0`), binary COBS / SLIP packet framing encapsulates 9P network packets alongside ASCII `printk` log lines.
- **Advantage**: Single serial port carries both human console logs and binary 9P packets.

#### 3. Option C: Headless Interconnect Mode (For Automated Tests)
- **Mechanism**: For automated integration tests, `lsh` shell output is disabled/bypassed, dedicating the serial channel 100% to 9P packet exchange.

---

## 3. Pruned & Focused Hardware Selection

To keep the roadmap tight, focused, and immediately testable:

| Technology | Status | Role in LugalOS Roadmap |
| :--- | :--- | :--- |
| **USB CDC / UART Serial Link** | **Phase 2 (Primary Bridge)** | Universal single-cable link for QEMU-QEMU, QEMU-HW, and board-to-board. |
| **Direct SPI Link** | **Phase 3 (High-Speed HW)** | Board-to-board high-speed DMA interconnect (up to 62.5 Mbps). |
| **WIZnet W5500 Ethernet** | **Phase 5 (Deferred)** | Wired Ethernet backbone (integrated when HW modules arrive). |
| **CAN Bus (MCP2515/PIO)** | **Pruned / Removed** | *Removed from core plan to maintain tight focus.* |
| **CYW43439 Wi-Fi Blobs** | **Pruned / Deferred** | *Deferred to prevent complex vendor firmware dependencies.* |

---

## 4. QEMU-Based Multi-Node Testing Strategy

Testing distributed operating system logic on physical hardware alone is slow and difficult to debug. LugalOS will implement a 3-tier QEMU testing matrix in `tests/runner.py`:

```mermaid
sequenceDiagram
    participant Host as Python Test Runner
    participant Q1 as QEMU Client (Workstation)
    participant Q2 as QEMU Server (Display Node)
    participant HW as Physical RP2350 Node

    Note over Host, Q2: Test Suite 1: Pure Simulated Multi-Node (QEMU-to-QEMU)
    Host->>Q2: Spawn QEMU Node 2 (Display Server) listening on TCP 4444
    Host->>Q1: Spawn QEMU Node 1 (Client) connected to TCP 4444
    Q1->>Q2: Send 9P Tattach & Twrite /srv/display
    Q2-->>Q1: Return 9P Rwrite Success
    Q1->>Host: Verification Passed

    Note over Host, HW: Test Suite 2: Heterogeneous Network (QEMU-to-Hardware)
    Host->>HW: Flash LugalOS to RP2350 (USB CDC / /dev/ttyUSB0)
    Host->>Q1: Spawn QEMU Client forwarding 9P packets to hardware
    Q1->>HW: Send 9P Twrite /sd0/test.txt over single USB link
    HW-->>Q1: Return 9P Rwrite Success
```

---

## 5. Refined Phased Implementation Roadmap

```mermaid
timeline
    title LugalOS Focused Implementation Roadmap
    Phase 1 : 9P Frame Encoder/Decoder (fs/9p.c)
            : In-Memory Loopback Transport (drivers/loopback_net.c)
            : Full 9P Loopback Verification Test (tests/runner.py)
            : /srv/ Remote Binding Infrastructure
    Phase 2 : Universal USB / UART Network Transport (drivers/uart_net.c)
            : QEMU-to-QEMU Interconnect (TCP Sockets)
            : Single-Cable QEMU-to-Hardware Interconnect (/dev/ttyACM1 / /dev/ttyUSB0)
            : Multi-Node Automated Test Integration in tests/runner.py
    Phase 3 : High-Speed SPI DMA Interconnect (RP2350 Hardware)
            : RP2350 Board-to-Board 62.5 Mbps Link
    Phase 4 : Special-Purpose Micro-Nodes
            : Remote Storage Node (Exports /sd0/ via 9P)
            : Remote Display Node (Exports ST7789 Framebuffer via 9P)
    Phase 5 : Hardware Ethernet Extension (W5500 Integration)
            : Socket-Based Network Extension
```

### Detailed Phase 1 Specification (9P Loopback Engine)

Phase 1 will implement the full original 9P serialization suggestion in pure software without requiring any physical network hardware:

1. **`fs/9p.c` / `fs/include/fs/9p.h`**:
   - Packets: `Tversion`/`Rversion`, `Tattach`/`Rattach`, `Twalk`/`Rwalk`, `Topen`/`Ropen`, `Tread`/`Rread`, `Twrite`/`Rwrite`, `Tclunk`/`Rclunk`.
2. **`drivers/loopback_net.c`**:
   - In-memory ring buffer transport that echoes 9P requests directly to the 9P server engine within the kernel.
3. **VFS `/srv/` Binding**:
   - Mount virtual remote service `/srv/loopback_test`.
4. **Automated Verification**:
   - Complete 9P loopback tests integrated into `tests/runner.py` verifying full round-trip file reads/writes over 9P.

---

## 6. Summary Comparison of Active Interconnect Options

| Interconnect Technology | Target Environment | Throughput | Implementation Complexity | Primary Use Case |
| :--- | :--- | :--- | :--- | :--- |
| **Native RP2350 USB CDC** | RP2350 Hardware & PC | 12 Mbps | Medium (USB Controller) | **Single-Cable Flashing, Console (/dev/ttyACM0) & 9P (/dev/ttyACM1)** |
| **UART Serial Transport** | QEMU & RP2350 Hardware | 115.2 Kbps – 3 Mbps | **Very Low** | **Universal Bridge (QEMU-QEMU, QEMU-HW, HW-HW)** |
| **RP2350 Direct SPI DMA** | RP2350 Hardware | Up to 62.5 Mbps | Low | **High-Speed Board-to-Board Node Link** |
| **WIZnet W5500 SPI Ethernet** | RP2350 Hardware (Phase 5) | 15–20 Mbps | Medium | **Wired Ethernet Infrastructure Network** |
