# Phase 60: Agent Context Persistence, Hierarchical Memory & Adaptive Compaction

**Status**: PROPOSED / PLACEHOLDER  
**Target Version**: 0.22.0  
**Follows**:
- Phase 58: On-Device AI Agent (`ea`), LLM Primitives & Math Assistant (`kernel/ea.c`, `net/llm.c`)
- Phase 59: Agent-Driven OS Friction Reduction, Self-Extension & Introspection (`kernel/shell.c`, `user/chibicc/`, `cas/`)

**Target Hardware**:
- Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (`esp32p4-lcd7b`, 32 MB PSRAM)
- M5Stack Tab5 (`esp32p4-tab5`, 32 MB PSRAM)
- Waveshare ESP32-P4-NANO (`esp32p4`, 32 MB PSRAM)
- RP2350 with PSRAM & companion Wi-Fi (`rp2350-terminal`)
- QEMU VirtIO Targets (`rv32`, `rv64`, `rv64-smp`)

---

## 1. Executive Summary & Objective

In Phase 58 and 59, LugalOS implemented an on-device AI assistant (`ea`) capable of executing multi-turn ReAct loops (Thought -> Action -> Observation -> Answer). While intra-request scratchpads (history, response, tool observation) actively leverage the 32 MB PSRAM bulk zone (`palloc_pages_bulk()`), each shell invocation of `ea` currently terminates with buffer deallocation, leaving each command stateless.

Phase 60 establishes persistent, multi-tiered conversational state, intelligent context maintenance, and long-term memory extraction across invocations and reboot cycles.

```
┌───────────────────────────────────────────────────────────────────────────────────┐
│                        HIERARCHICAL MEMORY ARCHITECTURE                           │
├───────────────────────────────────────────────────────────────────────────────────┤
│                                                                                   │
│  TIER 1: Working Session Memory (PSRAM Bulk Zone: 128 KB - 512 KB)               │
│  ─────────────────────────────────────────────────────────────────                │
│  • Ephemeral, high-speed conversational transcript retained across `ea` commands. │
│  • Retains recent turn-by-turn context, tool observations, and code generations.  │
│                                      │                                            │
│                                      ▼ (Threshold Exceeded: > 48 KB / Tokens)     │
│  TIER 2: Adaptive Compaction & LLM Self-Summarization                            │
│  ─────────────────────────────────────────────────────                            │
│  • Background or inline model invocation to summarize older conversation turns.   │
│  • Preserves core intent and mathematical constants while discarding bulky logs.   │
│                                      │                                            │
│                                      ▼                                            │
│  TIER 3: Semantic Rule Extraction & Knowledge Memorization                        │
│  ─────────────────────────────────────────────────────────                        │
│  • Extraction of learned rules, user preferences, and hardware topology facts.     │
│  • Auto-generates structured rules for dynamic system-prompt injection.          │
│                                      │                                            │
│                                      ▼                                            │
│  TIER 4: Persistent Non-Volatile Storage (MicroSD / Flash)                        │
│  ─────────────────────────────────────────────────────────                        │
│  • Serialization of sessions to `/sd0/var/ea/sessions/<id>.json`.                 │
│  • Persistent learned knowledge store in `/sd0/etc/ea_rules.txt`.                 │
│  • Warm reload across board power cycles and OTA reboots.                         │
│                                                                                   │
└───────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Key Architecture Pillars & Focus Areas

### 2.1 Handling of System-Prompt & Dynamic Injection
- **Core Immutable Directives**: Preserve fundamental tool syntax (`Action: <tool>`), Freestanding C constraints, and Lisp CAS rules.
- **Dynamic Context Injection**:
  - Dynamically prepend extracted user rules and session summaries ahead of the active conversation.
  - Automatically incorporate hardware state (e.g. connected I2C sensors, mounted storage roots, active network interfaces) without static prompt bloating.

### 2.2 PSRAM Session Persistence
- **Persistent Session Handle**: Retain `g_ea_session_ctx` in the PSRAM bulk zone rather than invoking `palloc_free()` upon task exit.
- **Lifecycle & Control Primitives**:
  - `ea "<prompt>"`: Appends to the active PSRAM context.
  - `ea new` / `ea reset`: Flushes active working memory.
  - `ea context` / `ea hist`: Inspects current conversation token/byte depth.
  - Interactive REPL mode: Running bare `ea` drops into an `ea>` continuous dialogue loop.

### 2.3 Memory Persistence via SD Card & Flash
- **Session Checkpointing**: Automated or manual snapshotting (`ea save [name]`, `ea load [name]`) to `/sd0/var/ea/`.
- **Reboot Resilience**: Auto-resume option so an agent performing multi-stage tasks or staged OTA reboot verification survives resets.

### 2.4 Elaborate Compaction via LLM Summarization
- **Sliding-Window Compaction**: Triggered when PSRAM transcript reaches a high-water mark (e.g. 48 KB).
- **Two-Pass Recursive Summarization**:
  - The agent invokes a compact summarization prompt on the oldest $N$ turns:
    `"Summarize key facts, decisions, code paths, and mathematical results from these turns into a concise context block."`
  - Replaces raw turn chunks with a distilled `[PRIOR CONTEXT SUMMARY]` entry, keeping context bounded while preserving continuity.

### 2.5 Memorization Rule Extraction
- **Autonomous Rule Distillation**:
  - Detect user corrections, preferences, and recurring patterns (e.g. *"Always plot in gray8 on the LCD-7B"* or *"Use 400 kHz for I2C"*).
  - Prompt the model to extract declarative rules:
    `[RULE: user prefers rational fractions over float decimals]`
  - Commit extracted rules to `/sd0/etc/ea_rules.txt` for permanent injection across all future sessions.

---

## 3. Preliminary Milestones (High-Level Placeholder)

- **60.1**: PSRAM Session Buffer Manager & `cmd_ea` Persistence Lifecycle (`ea new`, `ea hist`, `ea reset`).
- **60.2**: Interactive Multi-Turn REPL Shell (`ea>` prompt, line-editor integration).
- **60.3**: Dynamic System-Prompt Manager & Context Injection.
- **60.4**: SD Card Session Serializer & Checkpoint Store (`/sd0/var/ea/`).
- **60.5**: Autonomous LLM Compaction & Context Summarization Engine.
- **60.6**: Semantic Rule Extraction & Long-Term Memorization (`ea_rules.txt`).
- **60.7**: E2E Verification & Multi-Session Continuity Benchmarks.
