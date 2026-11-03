# Profile: the chess computer -- a Pico 2 with an ST7735 screen, a TM1638
# keypad and an SD card (the rp2350-chess and rp2350-smp presets).
# What a profile is: cmake/lugalos_layers.cmake.

set(LUGALOS_ENABLE_CC               ON)
set(LUGALOS_ENABLE_ED               ON)
set(LUGALOS_ENABLE_CHESS            ON)
set(LUGALOS_ENABLE_ZMACHINE         ON)
set(LUGALOS_ENABLE_MOONBASE         ON)
set(LUGALOS_ENABLE_CERES            ON)
set(LUGALOS_ENABLE_RIBBON_ANIMATION ON)

# Buddy-allocator arena (kernel/balloc.h), in pages: .bss and the heap are the
# same budget on a Pico 2.
set(CONFIG_BALLOC_ARENA_PAGES 4)
