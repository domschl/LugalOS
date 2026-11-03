# Profile: a radio for a host -- the ESP32-C6-MINI beside an ESP32-P4-NANO, a
# Wi-Fi modem over SDIO (LLP). The host owns the IP stack's services.
# What a profile is: cmake/lugalos_layers.cmake.

set(LUGALOS_ENABLE_CC               OFF)
set(LUGALOS_ENABLE_ED               OFF)
set(LUGALOS_ENABLE_CHESS            OFF)
set(LUGALOS_ENABLE_ZMACHINE         OFF)
set(LUGALOS_ENABLE_MOONBASE         OFF)
set(LUGALOS_ENABLE_CERES            OFF)
set(LUGALOS_ENABLE_RIBBON_ANIMATION OFF)

set(CONFIG_BALLOC_ARENA_PAGES 4)
