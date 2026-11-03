# Profile: the network gateway -- ENC28J60 Ethernet, USB 9P and a UART1
# downlink on a Pico 2 (rp2350-gateway).
# What a profile is: cmake/lugalos_layers.cmake.

set(LUGALOS_ENABLE_CC               ON)
set(LUGALOS_ENABLE_ED               ON)
set(LUGALOS_ENABLE_CHESS            OFF)
set(LUGALOS_ENABLE_ZMACHINE         ON)
set(LUGALOS_ENABLE_MOONBASE         ON)
set(LUGALOS_ENABLE_CERES            ON)
set(LUGALOS_ENABLE_RIBBON_ANIMATION ON)

set(CONFIG_BALLOC_ARENA_PAGES 4)
