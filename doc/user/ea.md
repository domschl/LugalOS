# ea - LugalOS Embedded Assistant & AI Agent

[Documentation](../README.md) > [User Manual](index.md) > **Embedded Assistant (ea)**

---

The `ea` command launches LugalOS's on-device AI assistant. It integrates an OpenAI-compatible HTTP inference endpoint with LugalOS's built-in Lisp machine, Computer Algebra System (CAS), on-device C compiler (`chibicc`), and system manuals.

## 1. Quick Usage

```sh
# One-shot query
ea "What are the roots of 3x^2 - 12 = 0?"

# Pass explicit model or endpoint URL
ea -m Gemma4-12B-MTP "Calculate determinant of ((1 2)(3 4)) in Lisp"
ea -s http://192.168.1.100:8080/v1 "Show free memory and tasks"

# Show configuration or list models available from server
ea config
ea models
```

## 2. The ReAct Agent Loop

`ea` operates as an autonomous ReAct (Reasoning + Acting) agent:
* **Thought**: Plans the next step.
* **Action**: Invokes an on-device tool (`lisp`, `sh`, `cc`, or `help`).
* **Observation**: Inspects the captured output from the tool execution.
* **Answer**: Synthesizes the final verified solution.

### Available Tools:
* `lisp`: Evaluates symbolic math expressions in the kernel Lisp machine.
* `sh`: Executes system shell commands (`mem`, `ps`, `sensor`, `net`, etc.).
* `cc`: Writes C source to `/sd0/tmp/ea_prog.c`, compiles via `chibicc`, and runs the ELF under U-mode memory confinement.
* `help`: Queries system Gemtext documentation from `/sd0/help/*.gmi`.

### Confirmation and limits
The model chooses each tool call, and its reply comes over the network. So `ea` shows every call (the tool and its code) and runs it only after you answer `y` at the console. Any other key declines, and the model is told so. `ea -y` runs tool calls without asking.

Even with `-y`, a tool call cannot change keys, identity, network configuration or firmware:
* the shell commands `identity`, `peers`, `p9key`, `otakey`, `wlan`, `netcfg`, `mqttcfg`, `poke` and `sdio` refuse;
* the Lisp primitives `poke`, `net-config`, `net-identity` (renaming), `identity-name`, `identity-provision`, `identity-key`, `peers-add`, `peers-remove` and `wlan-set` refuse;
* nothing under a `system/etc/` directory (the 9P keys, the boot scripts) can be written or removed.

Those are for a person at the console. `privselftest` checks the limits.

## 3. Configuration & Secrets Abstraction

LugalOS firmware images remain strictly byte-identical across all boards. Site-specific endpoints and model settings are configured through:

1. **Filesystem Configuration File (`/sd0/etc/llm.conf`)**:
```ini
url = http://ai-server.local:8080/v1
model = default
api_key = none
timeout_ms = 30000
```
2. **Local Static Hosts Mapping (`/sd0/etc/hosts`)**:
```text
127.0.0.1       localhost
192.168.1.100   ai-server.local
```
3. **Dynamic Model Auto-Discovery**:
When `model` is set to `default`, `ea` queries `GET /v1/models` on the server and automatically selects the first active model.

## 4. First-Class Lisp API

The LLM transport layer is exposed directly to the Lisp environment:
* `(llm-chat prompt [system] [model])`: Synchronous call, returns response string.
* `(llm-stream prompt callback [system] [model])`: Streams token strings live to `(callback token)`.
* `(llm-models)`: Returns a list of available model ID strings.
* `(llm-config [url] [model])`: Queries or updates runtime LLM settings.

## 5. Symbolic Mathematics with CAS

`ea` is specialized to write and execute LugalOS CAS scripts:
* Calculus: `(load "/sd0/cas/calculus.lisp")` -> `(diff '(sin (* 2 x)) 'x)`
* Solvers: `(load "/sd0/cas/solve.lisp")` -> `(solve '(= (^ x 2) 9) 'x)`
* Matrices: `(load "/sd0/cas/matrix.lisp")` -> `(mat-det '((1 2)(3 4)))`
* Plotting: `(load "/sd0/cas/plot.lisp")` -> `(plot '(sin x) -3 3)`

---

**Navigation:** [<- User Manual](index.md) | [Documentation Index](../README.md)
