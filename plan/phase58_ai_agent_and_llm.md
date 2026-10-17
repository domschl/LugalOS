# Phase 58: On-Device AI Agent (`ea`), LLM Primitives & Math Assistant

**Status**: PROPOSED / DRAFT  
**Target Version**: 0.20.0  
**Follows**:
- Phase 21: Identity Store & Authentication (`kernel/identity.c`, `kernel/idstore.c`)
- Phase 42 & 43: Lisp Symbolic Foundations & Computer Algebra System (`cas/`)
- Phase 50: Documentation System (`doc/`, `/sd0/help/*.gmi`)
- Phase 51: Shell Usability & Composability (`kernel/shell.c`)
- Phase 53: Gemtext Hypertext Engine & Spartan Protocol (`fs/gemtext.c`, `net/spartan.c`)
- Phase 56: M5Stack Tab5 (`esp32p4-tab5`)
- Phase 57: HiDPI & Anti-Aliased Typography (`drivers/lcdterm.c`, `drivers/font_noto_*`)

**Target Hardware**:
- **Primary Connected Hardware**:
  - Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (`esp32p4-lcd7b`, 32 MB PSRAM, Wi-Fi 6 via C6 SDIO link)
  - M5Stack Tab5 (`esp32p4-tab5`, 32 MB PSRAM, 5" 720×1280 HiDPI IPS, Wi-Fi 6)
  - Waveshare ESP32-P4-NANO (`esp32p4`, 32 MB PSRAM, RMII 100M Ethernet)
- **Secondary Hardware & Emulation**:
  - Raspberry Pi Pico 2 W (`rp2350-wifi`, CYW43439 Wi-Fi) & RP2350-LCD-7 with C6 companion (`rp2350-terminal`)
  - QEMU VirtIO-Net (`rv32`, `rv64`, `rv64-smp`)

**Inference Server Compatibility**:
- Any OpenAI-compatible HTTP inference server (e.g. `llama.cpp` `llama-server`, `vLLM`, `Ollama`, or local proxy).
- Generic REST endpoints: `/v1/chat/completions` (SSE streaming) and `/v1/models`.

---

## 1. Executive Summary & Vision

Across earlier phases, LugalOS developed capabilities that are rare in modern microcontroller operating systems:
1. **A full-featured in-kernel Lisp machine** with bignums, exact rational arithmetic, complex numbers, graphics canvas drawing, and a complete Computer Algebra System (`cas/`).
2. **On-device native C compilation** via `chibicc` (`cc <src> <dst>`), producing native RISC-V ELF binaries executed in isolated U-mode domains.
3. **A unified documentation system** in Gemtext format (`/sd0/help/*.gmi`), covering every system call, shell command, peripheral, and CAS function.
4. **Standard TCP streaming** (`tcp_stream_t`) and in-kernel execution capture (`console_capture()`), allowing commands and expressions to run with their outputs captured silently into RAM buffers.

**Phase 58 brings these pieces together into an on-device AI assistant and agent framework: `ea` (Embedded Agent / Expert Assistant).**

```
+-----------------------------------------------------------------------------------+
|                               LUGALOS AI SUBSYSTEM                                |
|                                                                                   |
|   Interactive Shell / CLI             Lisp REPL & Scripts         Ribbon Window   |
|   $ ea "Solve y'' + y = 0"            (ea "Factor x^4 - 1")       [ Agent Win ]   |
|              │                                │                         │         |
|              └───────────────────────┬────────┴─────────────────────────┘         |
|                                      ▼                                            |
|                       ea ReAct Agent Execution Loop                               |
|                     (Thought -> Action -> Observation)                            |
|                                      │                                            |
|          ┌───────────────────────────┼───────────────────────────┐                |
|          ▼                           ▼                           ▼                |
|    Tool: Lisp / CAS            Tool: Shell / cc            Tool: Help / Doc       |
|    - eval Lisp expressions     - run lsh commands          - query /sd0/help/*.gmi|
|    - symbolic calculus         - compile C via chibicc     - read system manuals  |
|    - canvas plotting           - check sensors/ps/mem      - find function syntax |
|          │                           │                           │                |
|          └───────────────────────────┼───────────────────────────┘                |
|                                      │                                            |
|                                      ▼                                            |
|                        LLM Client & Transport Layer                               |
|             (HTTP/1.1 POST /v1/chat/completions + SSE Stream Parser)              |
|                                      │                                            |
|                                      ▼                                            |
|                           Local Network Transport                                 |
|               TCP Stream (net/tcp.c) over Ethernet / Wi-Fi / VirtIO               |
|                                      │                                            |
+──────────────────────────────────────┼────────────────────────────────────────────+
                                       │ HTTP / LAN
                                       ▼
                 OpenAI-Compatible LLM Server (http://<llm-host>:<port>)
```

Instead of simply being a dumb terminal that pastes text from an LLM, `ea` acts as an active operator on the system. It can consult system documentation when unsure about syntax, write and test Lisp CAS code to solve math problems, write and compile C programs with `chibicc`, inspect hardware sensors, and format clean mathematical and graphical solutions for the user.

Furthermore, by exposing the underlying LLM transport directly to the Lisp environment through native primitives (`(llm-chat ...)`, `(llm-stream ...)`), LugalOS becomes an experimental platform where users can construct, experiment with, and script autonomous agents entirely in Lisp.

---

## 2. Configuration & Infrastructure Abstraction

To ensure portability and prevent leaking private network topologies, hostnames, IP addresses, or API keys, LugalOS strictly separates generic code from site-specific configuration. 

Phase 58 defines a clean 5-level configuration hierarchy:

```
┌───────────────────────────────────────────────────────────────────────────────────┐
│                     CONFIGURATION RESOLUTION HIERARCHY (PRIORITY)                 │
└────────────────────────────────────────┬──────────────────────────────────────────┘
                                         │
 1. CLI Arguments:                       ▼  ea -s http://192.168.1.50:8080/v1 -m gemma
                                         │
 2. Runtime Session / Lisp:              ▼  (llm-config :url "..." :model "...")
                                         │
 3. Filesystem Config File:              ▼  /sd0/etc/llm.conf (URL, Model, Key)
                                         │
 4. Persistent Identity Store (NVRAM):   ▼  llmcfg <url> [model] (Byte-identical Flash)
                                         │
 5. Auto-Discovery & Dynamic Probe:      ▼  Query GET /v1/models -> auto-select first
```

### 2.1 The 5 Configuration Modalities

1. **Explicit CLI Flags (Highest Priority)**:
   - `ea -s <url> -m <model> "<prompt>"`
   - Example: `ea -m my-model "Explain the PMP registers"`
   - Useful for one-off tasks or testing alternative models.

2. **Runtime Lisp Environment**:
   - `(llm-config :url "http://ai-server.local:8080/v1" :model "qwen")`
   - Dynamically rebindable in `/sd0/system/etc/usr_init.lisp` per user session.

3. **Filesystem Configuration File (`/sd0/etc/llm.conf`)**:
   - Stored on the board's microSD card or writable flash partition. Easily managed via the `e` text editor or synchronized from a developer workstation via `tools/p9sync.py`:
     ```ini
     # /sd0/etc/llm.conf - LugalOS LLM Endpoint Configuration
     url = http://ai.local:8080/v1
     model = default
     api_key = none
     timeout_ms = 30000
     temperature = 0.2
     ```

4. **Persistent Identity Store (Flash NVRAM / EEPROM)**:
   - In accordance with LugalOS architectural principles ([`AGENTS.md`](file:///home/dsc/Source/gith/domschl/lugalos/AGENTS.md) and [`kernel/include/kernel/idstore.h`](file:///home/dsc/Source/gith/domschl/lugalos/kernel/include/kernel/idstore.h)), firmware binaries (`lugalos.bin`) remain byte-identical across all boards.
   - Per-board endpoint settings can be persisted in the board's non-volatile identity block:
     ```sh
     $ llmcfg http://192.168.1.100:8080/v1 default-model
     ```
   - Persisted via `node_identity_set_llm()` in `kernel/identity.c`. Survived across reboots and firmware updates.

5. **Host Developer Environment & Test Harness**:
   - Automated hardware tests ([`tests/hw/test_ea.py`](file:///home/dsc/Source/gith/domschl/lugalos/tests/hw/test_ea.py)) and provisioning scripts ([`tools/provision.py`](file:///home/dsc/Source/gith/domschl/lugalos/tools/provision.py)) read site-specific credentials from:
     ```bash
     ~/.config/lugalos/llm.env
     ```
     *(Patterned after existing `wifi.env`, `p4-ports.env`, and `c6-ports.env`)*:
     ```bash
     LUGALOS_LLM_URL=http://<private-server>:<port>/v1
     LUGALOS_LLM_MODEL=<preferred-model>
     ```
   - This file is strictly excluded from version control (`.gitignore`).

### 2.2 Model Auto-Discovery
If the configured model is omitted or specified as `"default"`:
1. `ea` sends a `GET /v1/models` request on first boot or first invocation.
2. It parses the JSON response to retrieve the list of available model identifiers.
3. It selects the first available model reported by the server, storing it in memory for the duration of the session.
4. This ensures that changing the active model on the host inference server requires zero manual reconfiguration on the microcontroller!

---

## 3. Network Transport & Host Resolution

### 3.1 Host Resolution (`<hostname>` $\rightarrow$ IPv4)
LugalOS's network stack currently expects IPv4 dotted quads (`net/spartan.c:148`). To support local hostnames like `http://ai-server.local:8080` or `http://router:8080`, Phase 58 introduces a two-tier host resolution mechanism in `net/dns.c`:

1. **Static Hosts Table (`/sd0/etc/hosts`)**:
   - A lightweight parser checks `/sd0/etc/hosts` for standard IP-hostname pairs:
     ```text
     127.0.0.1       localhost
     192.168.1.100   ai-server.local
     ```
2. **Minimal UDP DNS Client (RFC 1035)**:
   - Sends a standard A-record query on UDP port 53 to the DNS server IP obtained during DHCP configuration (`net/dhcp.c`).
   - Parses the first `A` record returned; times out after 1.5 seconds.
   - Cached in a tiny 4-entry static DNS cache.

### 3.2 HTTP/1.1 Client (`net/http.c`)
LugalOS already implements non-blocking client TCP streams via `tcp_stream_t` in `net/tcp.c`. Phase 58 adds a lightweight HTTP/1.1 client abstraction designed for REST and SSE streaming:

```c
typedef struct http_req {
    const char *url;         /* e.g. "http://ai-server.local:8080/v1/chat/completions" */
    const char *method;      /* "POST" or "GET" */
    const char *content_type;/* "application/json" */
    const char *body;        /* JSON payload */
    uint32_t    body_len;
} http_req_t;

typedef int (*http_chunk_cb)(const uint8_t *data, uint32_t len, void *user_ctx);

int http_post_stream(const http_req_t *req, http_chunk_cb on_chunk, void *user_ctx);
```

#### HTTP Client Invariants:
* **Zero Dynamic Allocation per Request**: Request headers are formatted directly into a static scratch buffer.
* **Non-blocking Pump**: Yields cooperatively via `sched_yield()` and monitors `console_interrupt_requested()` (allowing the user to press `Ctrl-C` to abort a long generation immediately).
* **Chunked & Content-Length Handling**: Properly skips HTTP response headers (`\r\n\r\n`) and streams response body data directly to the consumer callback.

---

## 4. Streaming Token Protocol & JSON Processing

### 4.1 The Value of SSE (Server-Sent Events) on Microcontrollers
Calling an LLM with non-streaming (`"stream": false`) requires buffering the entire JSON response before parsing. For a 2,000-token response, the JSON payload exceeds 8–12 KB, placing unnecessary pressure on microcontroller SRAM.

By requesting `"stream": true`, the OpenAI-compatible server emits Server-Sent Events line by line:
```http
data: {"choices":[{"delta":{"content":"To"},"index":0}],"model":"default-model"}

data: {"choices":[{"delta":{"content":" solve"},"index":0}],"model":"default-model"}

data: [DONE]
```

### 4.2 Streaming Token Unescaper ($O(1)$ RAM Footprint)
Phase 58 implements a streaming parser (`net/llm_stream.c`) that:
1. Buffers incoming bytes only until a newline (`\n`) is reached (~256 bytes buffer).
2. Checks for `data: ` prefix:
   - If line is `data: [DONE]`, cleanly closes the stream.
   - If line contains `"delta":{"content":"..."}`, extracts the escaped string literal.
3. Unescapes JSON character sequences on the fly:
   - `\n` $\rightarrow$ newline (`0x0A`)
   - `\t` $\rightarrow$ tab (`0x09`)
   - `\"` $\rightarrow$ double quote (`0x22`)
   - `\\` $\rightarrow$ backslash (`0x5C`)
   - `\uXXXX` $\rightarrow$ UTF-8 byte sequence
4. Forwards decoded characters directly to the output stream (terminal or string accumulator).

### 4.3 JSON Request Construction
A dedicated serializer formats `/v1/chat/completions` JSON requests without requiring a full DOM tree:
```json
{
  "model": "default-model",
  "messages": [
    {"role": "system", "content": "You are ea, the LugalOS assistant..."},
    {"role": "user", "content": "Factor x^2 - 9 in Lisp"}
  ],
  "temperature": 0.2,
  "stream": true
}
```

---

## 5. First-Class Lisp Integration & Meta-Agent API

A primary requirement of Phase 58 is exposing the LLM transport as native Lisp builtins, enabling users to script and customize intelligent behaviors directly on the device.

### 5.1 Native Lisp Builtins (`user/lisp/builtins_table.h`)

| Builtin Form | Arguments | Description |
| :--- | :--- | :--- |
| `(llm-chat prompt [system] [model])` | `(str [str] [str]) -> str` | Synchronous chat completion. Returns the model's text response as a Lisp string. |
| `(llm-stream prompt callback [system] [model])` | `(str fn [str] [str]) -> nil` | Streams token chunks to `(callback chunk-str)`. Useful for interactive UI updates. |
| `(llm-models)` | `() -> (list of str)` | Queries `/v1/models` from the server and returns a list of available model strings. |
| `(llm-config [:url str] [:model str])` | `(k/v ...) -> list` | Queries or sets active runtime endpoint and default model. |
| `(ea prompt)` | `(str) -> nil` | Invokes the full ReAct agent loop in Lisp, displaying thoughts, tool calls, and final answers. |

### 5.2 Building Agents in Lisp (Self-Hosting Meta-Programming)
Because LugalOS Lisp already provides `eval`, `read`, `lsh` (shell execution), and file I/O, writing an autonomous agent in Lisp requires fewer than 80 lines of code:

```lisp
;; /sd0/lib/agent.lisp - Self-hosting ReAct agent in LugalOS Lisp
(define (ea prompt)
  (let ((sys-prompt "You are ea, an AI agent running inside LugalOS.
Available tools:
Action: lisp
```lisp
<expr>
```
Action: sh
```sh
<command>
```
Answer: <final explanation>")
        (history (list (cons "user" prompt))))
    (ea-loop history sys-prompt 0)))

(define (ea-loop history sys-prompt turn)
  (if (> turn 8)
      (display "\n[ea] Maximum tool turns reached.\n")
      (let* ((resp (llm-chat (history->prompt history) sys-prompt))
             (action (parse-action resp)))
        (display resp)
        (display "\n")
        (if (not action)
            'done
            (let* ((tool (car action))
                   (code (cdr action))
                   (obs (cond
                          ((string=? tool "lisp") (eval-lisp-capture code))
                          ((string=? tool "sh")   (lsh-capture code))
                          (else "Unknown tool"))))
              (display "\nObservation: ")
              (display obs)
              (display "\n")
              (ea-loop (append history
                               (list (cons "assistant" resp)
                                     (cons "user" (string-append "Observation: " obs))))
                       sys-prompt
                       (+ turn 1)))))))
```

---

## 6. The `ea` ReAct Tool Execution Harness

### 6.1 Protocol: Clean Markdown Blocks vs JSON Schema
Instead of requiring brittle and verbose OpenAI JSON schema definitions (`tool_calls`), `ea` uses a lightweight **Markdown ReAct (Reasoning + Acting) loop**.

#### Standard Agent Response Format:
````markdown
Thought: The user wants to find the determinant of a 3x3 matrix. I should load the CAS matrix library and compute it.
Action: lisp
```lisp
(load "/sd0/cas/matrix.lisp")
(mat-det '((1 2 3) (0 4 5) (1 0 6)))
```
````

#### Observation Fed Back to Agent:
```text
Observation: 22
```

#### Final Answer:
````markdown
Thought: The determinant is 22.
Answer: The determinant of the matrix is **22**. Here is the Lisp computation:
```lisp
(load "/sd0/cas/matrix.lisp")
(mat-det '((1 2 3) (0 4 5) (1 0 6)))  ; => 22
```
````

### 6.2 Tool Registry

The `ea` engine provides five fundamental tools to the model:

| Tool Name | Identifier | Execution Mechanism | Output Capture |
| :--- | :--- | :--- | :--- |
| **Lisp / CAS** | `lisp` | Evaluates code in the kernel Lisp engine via `lisp_eval_string()`. | Return value printed to string, or errors. |
| **Shell** | `sh` | Executes shell command via `parse_and_eval_cmd()`. | Captured via `console_capture()`. |
| **C Compiler** | `cc` | Invokes `chibicc` to compile C source to an ELF on `/sd0`. | Captured compiler warnings/errors. |
| **Help Manuals** | `help` | Queries `/sd0/help/<topic>.gmi` or `resolve_help_path()`. | First 40 lines of the relevant manual. |
| **File I/O** | `file` | Reads or writes files on `/sd0` (`cat`, `touch`, `edit`). | File contents or write status. |

### 6.3 Silent Console Output Capture
A critical mechanism in LugalOS is `console_capture()` in `kernel/console.c`. When `ea` invokes a tool:
```c
char out_buf[2048];
console_capture(out_buf, sizeof(out_buf) - 1);

/* Execute command or Lisp expression */
if (is_lisp) {
    lisp_lock();
    lisp_val_t *res = lisp_eval_string(code);
    /* format res into out_buf */
    lisp_unlock();
} else {
    parse_and_eval_cmd(code);
}

uint32_t len = console_capture_end();
out_buf[len] = '\0';
```
This guarantees that intermediate tool runs, temporary errors, and file writes do not corrupt the user's active terminal or ribbon screen.

---

## 7. Mathematical Assistant & CAS Specialization

LugalOS contains a rich symbolic math engine in `cas/`. However, general-purpose LLMs often assume Maxima, SymPy, or standard Common Lisp syntax.

Phase 58 provides `ea` with a specialized system prompt explaining the exact LugalOS CAS conventions:

### 7.1 LugalOS CAS Cheat-Sheet (Provided in System Prompt)
1. **Loading Libraries**:
   - Calculus: `(load "/sd0/cas/calculus.lisp")`
   - Equation Solving: `(load "/sd0/cas/solve.lisp")`
   - Polynomials: `(load "/sd0/cas/poly.lisp")`
   - Matrices: `(load "/sd0/cas/matrix.lisp")`
   - Simplification: `(load "/sd0/cas/simplify.lisp")`
   - Function Plotting: `(load "/sd0/cas/plot.lisp")`
2. **Syntax Rules**:
   - Exponentiation is `(^ x n)`, not `(** x n)` or `(expt x n)`.
   - Equations are quoted: `'(= lhs rhs)`.
   - Expressions to manipulate symbolically must be quoted: `'(+ (* 2 x) 5)`.
3. **Key Functions**:
   - `(diff expr var)`: Symbolic derivative (e.g., `(diff '(sin (* 2 x)) 'x)` $\rightarrow$ `(* 2 (cos (* 2 x)))`).
   - `(solve '(= (^ x 2) 4) 'x)`: Returns list of solutions `((x . 2) (x . -2))`.
   - `(poly-factor '(+ (^ x 2) (* 5 x) 6) 'x)`: Returns factors.
   - `(mat-inv M)` / `(mat-det M)`: Matrix operations.
   - `(plot expr xmin xmax)`: Renders graphic curve directly to LugalOS screen canvas!

### 7.2 Example Walkthrough: Solving & Plotting
When a user types in `lsh`:
```sh
$ ea "Find the roots of x^3 - 4x = 0 and plot the function from -3 to 3"
```
The agent executes:
1. `(load "/sd0/cas/solve.lisp")` followed by `(solve '(= (- (^ x 3) (* 4 x)) 0) 'x)`.
2. Inspects observation: `((x . 0) (x . 2) (x . -2))`.
3. Invokes: `(load "/sd0/cas/plot.lisp")` followed by `(plot '(- (^ x 3) (* 4 x)) -3 3)`.
4. The LugalOS screen switches to the canvas pane and draws the cubic polynomial curve.
5. Returns a clear explanation detailing the three real roots ($x \in \{-2, 0, 2\}$) and confirming the plot on screen.

---

## 8. C Programming & Embedded Troubleshooting Assistant

Beyond mathematics, `ea` can assist users with embedded development using `chibicc`:

### 8.1 Compiling & Debugging On-Device
User prompt:
```sh
$ ea "Write a small benchmark that calculates the first 10,000 Fibonacci numbers modulo 1000000 in C, compile it, and run it"
```
Agent workflow:
1. Writes C source code to `/sd0/tmp/fib.c`.
2. Executes `Action: sh` $\rightarrow$ `cc /sd0/tmp/fib.c /sd0/bin/fib`.
3. If `chibicc` reports a compilation error (e.g. missing include or syntax mistake), `ea` reads the compiler diagnostic from the observation, corrects `/sd0/tmp/fib.c`, and recompiles.
4. Executes `Action: sh` $\rightarrow$ `/sd0/bin/fib`.
5. Reports timing and benchmark output back to the user.

### 8.2 Safety & Confirmation Guardrails
- **Read-Only / Safe Actions**: Lisp evaluations, `help`, reading files, and running benchmarks execute automatically.
- **Destructive Shell Commands**: Commands involving file deletion (`rm`), filesystem formatting (`mkfs`), device reboots (`reboot`), or flash operations (`p4flash`, `c6flash`) require explicit user confirmation `[y/N]`, unless the user passes the `-y` / `--yes` flag to `ea`.

---

## 9. User Interface & Ribbon Workspace Integration

Phase 58 provides three distinct modalities for interacting with `ea`:

### 9.1 1. The Interactive Shell Command (`lsh`)
- One-shot mode:
  ```sh
  $ ea "How much free heap and PSRAM do we have?"
  ```
- Interactive conversation REPL:
  ```sh
  $ ea
  LugalOS Assistant (ea) - Connected to http://ai-server.local:8080/v1 (default-model)
  Type 'exit' to quit, 'model <name>' to switch models.
  
  ea> Explain how to configure Wi-Fi credentials on the ESP32-P4
  ```

### 9.2 2. The Ribbon Agent Pane (`RIBBON_WIN_AGENT`)
On touch-screen terminals (Waveshare LCD-7B and M5Stack Tab5), `ea` can be spawned into its own dedicated Ribbon Window (`F6` or touch menu):
- **Thoughts & Planning**: Rendered in dimmed 4-bpp italic/sans serif text.
- **Tool Invocations**: Highlighted in framed monospace code blocks.
- **Final Responses**: Rendered using Phase 57's high-density proportional Noto typography.
- **Canvas Output**: Automatically opens an adjacent Canvas pane when plotting.

---

## 10. Implementation Milestones

```
+─────────────────────────────────────────────────────────────────────────────+
|                          PHASE 58 EXECUTION ROADMAP                         |
+─────────────────────────────────────────────────────────────────────────────+
  58.1: Network Transport & Hostname Resolution
        ├── /sd0/etc/hosts parser & net/dns.c minimal UDP resolver
        └── net/http.c non-blocking HTTP/1.1 POST client over tcp_stream_t
  58.2: SSE Streaming & Token Parser
        ├── net/llm_stream.c SSE line parser (data: {...})
        └── O(1) memory JSON string unescaper with Ctrl-C interrupt support
  58.3: Native Lisp LLM Primitives & Configuration Hierarchy
        ├── (llm-chat), (llm-stream), (llm-models), (llm-config) builtins
        ├── Model auto-discovery probe from /v1/models
        └── GC root preservation during network I/O (lisp_park)
  58.4: ea ReAct Tool Execution Engine
        ├── kernel/ea.c tool parser (Action: lisp | sh | cc | help)
        ├── Output redirection via console_capture()
        └── Loop controller with turn limit & error recovery
  58.5: Mathematical & CAS Persona Prompting
        ├── Specialized system prompt with LugalOS CAS syntax rules
        └── Verification: calculus, equations, linear algebra, and canvas plots
  58.6: On-Device C Compiler Assistance
        ├── Integration with chibicc (writing .c, compiling, fixing diagnostics)
        └── Execution of user ELFs under U-mode domain confinement
  58.7: UI Integration & Verification Suite
        ├── lsh shell command ea, flag parsing (-s, -m, -y, -i)
        └── Automated test suite in tests/hw/test_ea.py using ~/.config/lugalos/llm.env
```

### Milestone 58.1: Network Transport & Hostname Resolution
- **Deliverables**:
  - `net/dns.c` and `net/include/net/dns.h`: Minimal DNS client (UDP 53) and static hosts resolver (`/sd0/etc/hosts`).
  - `net/http.c` and `net/include/net/http.h`: Non-blocking HTTP POST client over `tcp_stream_t`.
  - Shell command `dns <hostname>` and `curl <url>` for standalone network verification.

### Milestone 58.2: SSE Streaming & Token Parser
- **Deliverables**:
  - `net/llm_stream.c`: Streaming Server-Sent Events parser for `/v1/chat/completions`.
  - JSON token unescaper handling UTF-8, quotes, newlines, tabs.
  - Interactive token rendering to `cprintf()` with responsive `Ctrl-C` cancelation.

### Milestone 58.3: Native Lisp LLM Primitives & Configuration Hierarchy
- **Deliverables**:
  - `user/lisp/builtins_table.h` & `user/lisp/lisp.c`: Primitives `llm-chat`, `llm-stream`, `llm-models`, `llm-config`.
  - Dynamic model auto-discovery: if model is `"default"` or unset, query `/v1/models` and pick the first available model.
  - GC root preservation during network I/O (`lisp_park()`).
  - Configuration loaders: `/sd0/etc/llm.conf` and persistent `idstore` NVRAM `llmcfg`.

### Milestone 58.4: `ea` ReAct Tool Execution Engine
- **Deliverables**:
  - `kernel/ea.c` and `kernel/include/kernel/ea.h`: ReAct parser and loop coordinator.
  - Tool execution dispatchers for `lisp`, `sh`, `cc`, `help`.
  - Clean output capture using `console_capture()`.

### Milestone 58.5: Mathematical & CAS Persona Prompting
- **Deliverables**:
  - Embedded system prompt template tailored to LugalOS CAS conventions.
  - End-to-end verification of calculus (`diff`), polynomial solving (`solve`), matrix determinant (`mat-det`), and graphical curves (`plot`).

### Milestone 58.6: On-Device C Compiler Assistance
- **Deliverables**:
  - Multi-turn C generation, compilation via `chibicc`, diagnostic parsing, code patching, and execution under U-mode isolation.

### Milestone 58.7: UI Integration & Verification Suite
- **Deliverables**:
  - Shell command `ea` in `kernel/shell.c` with flags (`-s`, `-m`, `-y`, `-i`).
  - Python test harness: `tests/hw/test_ea.py` configured via `~/.config/lugalos/llm.env`.
  - System manual: `/sd0/help/ea.gmi` and documentation in `doc/user/ea.md`.

---

## 11. Verification & Test Plan

1. **DNS & HTTP Transport Verification**:
   - `dns ai-server.local` $\rightarrow$ Resolves to configured IP address.
   - `curl http://<host>:<port>/v1/models` $\rightarrow$ Successfully prints JSON model list.
2. **Lisp Primitives Verification**:
   - `(llm-models)` $\rightarrow$ Returns list of available models from server.
   - `(llm-chat "Say Hello" "You are a test assistant")` $\rightarrow$ Returns text response.
3. **ReAct Tool Execution Verification**:
   - `ea "Evaluate (+ 123 456) in Lisp"` $\rightarrow$ Agent runs `(+ 123 456)`, captures `579`, and reports `579`.
   - `ea "Check free memory and disk space"` $\rightarrow$ Agent runs `mem` and `df`, parses output, reports stats.
4. **CAS Mathematical Problem Solving**:
   - `ea "Solve 2x^2 - 8 = 0 using CAS"` $\rightarrow$ Loads `solve.lisp`, invokes `solve`, reports roots $x = \pm 2$.
   - `ea "Find the derivative of cos(3x)"` $\rightarrow$ Loads `calculus.lisp`, invokes `diff`, reports `-3*sin(3x)`.
5. **Hardware Robustness**:
   - Verify uninterrupted token streaming on ESP32-P4 (Wi-Fi & Ethernet) and RP2350 (Wi-Fi).
   - Verify zero SRAM leaks across 50 consecutive agent invocations (`mem` before and after).
   - Verify `Ctrl-C` during generation cleanly resets TCP stream and returns shell prompt in < 50 ms.
