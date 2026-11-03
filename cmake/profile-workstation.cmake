# Profile: a workstation with PSRAM and a screen -- the ESP32-P4 boards (NANO,
# LCD-7B, Tab5) and the RP2350-LCD-7 terminal. A Lisp machine with the CAS,
# chess, the games, a C compiler, HTTPS administration.
# What a profile is: cmake/lugalos_layers.cmake.

set(LUGALOS_ENABLE_CC               ON)
set(LUGALOS_ENABLE_ED               ON)
set(LUGALOS_ENABLE_CHESS            ON)
set(LUGALOS_ENABLE_ZMACHINE         ON)
set(LUGALOS_ENABLE_MOONBASE         ON)
set(LUGALOS_ENABLE_CERES            ON)
set(LUGALOS_ENABLE_RIBBON_ANIMATION ON)

# Lisp's pools, in PSRAM (BULK_BSS): 65536 nodes, "basically unlimited" (37.3a,
# 38.5); the node mark bitmap stays in SRAM, 8 KB at this size. The string pool
# at eight times the default of 384 slots (2560 x 32 B and 512 x 128 B, 144 KB).
set(CONFIG_LISP_NODE_POOL   65536)
set(CONFIG_LISP_STRING_POOL 3072)

# Buddy-allocator arena (kernel/balloc.h), in pages. The buddy tree is permanent
# .bss whose size scales with it (2046 bytes at 4 pages, 8190 at 16), and on
# these boards .bss and the heap share the internal SRAM.
set(CONFIG_BALLOC_ARENA_PAGES 4)

# /ram0's cap (38.6): its storage comes from the bulk zone, so the cap is a
# share of PSRAM, not of the heap. `(mount-ramdisk 4096)` can ask for up to this.
set(CONFIG_RAMDISK_MAX_KB 4096)

# The chess transposition table (38.7, sign-off S4): 512 KB from the bulk zone
# (PSRAM). Depth 8 on the bench position took 26.0 s against 33.9 s with 32 KB;
# 1 MB bought 4 % more and 4 MB 5 % -- the owner chose 512 KB.
set(CONFIG_CHESS_TT_KB 512)

# HTTPS: the node's own key, its certificate request and its certificate
# (kernel/tls_cert.c); the record buffers come from the bulk zone.
set(CONFIG_TLS_SERVER 1)
# A claimed node starts the admin server (HTTP, and HTTPS once it has a certificate) when it gets an address.
set(CONFIG_HTTPD_WHEN_CLAIMED 1)

# Two interfaces, one stack, where a board has both: eth0 while its cable has a
# carrier, else wlan0 -- re-decided once a second. Inert without a wired MAC.
set(CONFIG_NET_PREFER_WIRED 1)

# The TLS client (66.3b): MQTT to a broker over TLS, its certificate from the owner's CA (/dev/tlsca).
set(CONFIG_TLS_CLIENT 1)
