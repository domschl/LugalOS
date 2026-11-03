# Profile: a stand-alone sensor node -- the ESP32-C6-Zero, the Pico 2 W
# (rp2350-wifi) and the rp2350-sensor board. Its own IP, I2C sensors, MQTT, its
# 9P server and the admin page once claimed. No compiler, no editor, no games.
# What a profile is: cmake/lugalos_layers.cmake.

set(LUGALOS_ENABLE_CC               OFF)
set(LUGALOS_ENABLE_ED               OFF)
set(LUGALOS_ENABLE_CHESS            OFF)
set(LUGALOS_ENABLE_ZMACHINE         OFF)
set(LUGALOS_ENABLE_MOONBASE         OFF)
set(LUGALOS_ENABLE_CERES            OFF)
set(LUGALOS_ENABLE_RIBBON_ANIMATION OFF)

# Buddy-allocator arena (kernel/balloc.h), in pages: .bss and the heap are the
# same budget on these boards.
set(CONFIG_BALLOC_ARENA_PAGES 4)

# A claimed node starts the admin server when it gets an address: the owner
# signs in at NAME.local.
set(CONFIG_HTTPD_WHEN_CLAIMED 1)

# Sized for a node without a screen (66.3): half the log ring (half the dmesg
# history), two lines of shell history, four terminals (the console and 9P
# shells). Each is a page or so of a heap the radio, TCP and the sensors share.
set(CONFIG_KLOG_RING_KB   4)
set(CONFIG_SHELL_HISTORY  2)
set(CONFIG_MAX_VTERMS     4)
