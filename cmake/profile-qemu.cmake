# Profile: the QEMU test targets (rv32, rv64, rv64-smp). Everything built, so
# the automated suite (tests/runner.py) exercises every subsystem it can.
# What a profile is: cmake/lugalos_layers.cmake.

set(LUGALOS_ENABLE_CC               ON)
set(LUGALOS_ENABLE_ED               ON)
set(LUGALOS_ENABLE_CHESS            ON)
set(LUGALOS_ENABLE_ZMACHINE         ON)
set(LUGALOS_ENABLE_MOONBASE         ON)
set(LUGALOS_ENABLE_CERES            ON)
set(LUGALOS_ENABLE_RIBBON_ANIMATION ON)

set(CONFIG_LISP_NODE_POOL 65536)

# Buddy-allocator arena (kernel/balloc.h), in pages: 16 = 64 KB. Free against
# QEMU's 128 MB; the hardware profiles take 4, where the buddy tree's .bss and
# the arena come out of the same half-megabyte as the heap.
set(CONFIG_BALLOC_ARENA_PAGES 16)

# The NTP server (net/ntp_server.c), so the suite can query it.
set(CONFIG_ENABLE_NTP_SERVER 1)

# HTTPS: the node's own key, its certificate request and its certificate (kernel/tls_cert.c).
set(CONFIG_TLS_SERVER 1)
