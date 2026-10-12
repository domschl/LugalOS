# llp

The host-side reference implementation of **LLP**, the LugalOS link protocol
(`doc/llp.md`): the link between an ESP32-P4 and its ESP32-C6, or an RP2350 and
a C6-Zero, carrying Ethernet frames and 9P (plan/phase49_c6_network_nodes.md).

It is the protocol's *reference*: `kernel/llp.c` is checked against it by
`tests/host/llp_host.c`, through the vectors this package generates.

```bash
cd host/llp
uv sync                                    # add --extra serial for the serial tools (49.3)
uv run pytest -q                           # its own tests, including a lossy-line soak
uv run llp-vectors > ../../tests/host/llp_vectors.txt          # regenerate after a protocol change
uv run llp-vectors --check ../../tests/host/llp_vectors.txt    # what the runner checks
pyrefly check
```

| Module | What |
|---|---|
| `llp.wire` | frames, SLIP + CRC-32 serial binding (`llp_serial_*` in C) |
| `llp.link` | the state machine: handshake, credits, PROBE, keepalive (`llp_link_*` in C) |
| `llp.vectors` | the vector file's generator and format |

A protocol change goes here first, then the vectors are regenerated, then
`kernel/llp.c` follows until `make -C tests/host asan` agrees.
