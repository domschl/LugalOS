# The two layers a preset is made of (phase 66.2): a board and a profile.
#
#   board    cmake/board-<name>.cmake: what the hardware is. Addresses, pins,
#            memory sizes, what the hardware can do (NETWORK_CLAIM: a setup
#            access point), tuning that only makes sense on that hardware
#            (CHESS_HOT_RAM: the RP2350's XIP cache), and which drivers its
#            hardware needs (LUGALOS_ENABLE_SPISD, ...).
#   profile  cmake/profile-<name>.cmake: what the node is for. Which software
#            it carries (LUGALOS_ENABLE_CC, ...) and the policy and sizes that
#            follow from that (Lisp pools, TLS, the RAM disk, ...).
#
# A preset (CMakePresets.json) names a toolchain, a board, a profile, and the
# two build choices that are neither: LUGALOS_ENABLE_SMP and LUGALOS_DEV.
#
# Included by CMakeLists.txt at configure time and by cmake/gen_config.cmake
# at build time, so both read the layers the same way and check them the same
# way: a key in the wrong layer is a configure error, not a silent override.

# Software a profile chooses. A profile sets every one of these.
set(LUGALOS_PROFILE_FEATURES CC ED CHESS ZMACHINE MOONBASE CERES RIBBON_ANIMATION)

# Drivers a board chooses, for hardware it carries. OFF unless the board says so.
set(LUGALOS_BOARD_DRIVERS SPISD SDMMC ST7735 TM1638 PICO_CLOCK_GREEN DCF77 GPS)

# CONFIG_* keys that are policy: only a profile may set them.
set(LUGALOS_PROFILE_KEYS
    CONFIG_LISP_NODE_POOL CONFIG_LISP_STRING_POOL
    CONFIG_BALLOC_ARENA_PAGES
    CONFIG_RAMDISK_MAX_KB
    CONFIG_CHESS_TT_KB
    CONFIG_TLS_SERVER CONFIG_HTTPD_WHEN_CLAIMED
    CONFIG_NET_PREFER_WIRED
    CONFIG_ENABLE_NTP_SERVER
    CONFIG_TIMEZONE
    CONFIG_KLOG_RING_KB CONFIG_SHELL_HISTORY CONFIG_MAX_VTERMS
    CONFIG_CLOCK_COLON_BLINK CONFIG_CLOCK_BOOT_BEACON
    CONFIG_DCF77_AUTO_HOUR CONFIG_DCF77_AUTO_MIN CONFIG_DCF77_AUTO_ENABLED
    CONFIG_DCF77_P0_LOG CONFIG_DCF77_P0_NTP CONFIG_DCF77_P0_PERIOD_S CONFIG_DCF77_P0_PORT
)

# The names a layer file sets: every `set(NAME` at the start of a line.
function(_lugalos_layer_names file out)
    file(STRINGS "${file}" _lines REGEX "^[ \t]*set\\([ \t]*[A-Z0-9_]+")
    set(_names "")
    foreach(_l ${_lines})
        string(REGEX REPLACE "^[ \t]*set\\([ \t]*([A-Z0-9_]+).*" "\\1" _n "${_l}")
        list(APPEND _names ${_n})
    endforeach()
    set(${out} ${_names} PARENT_SCOPE)
endfunction()

# Fails the configure (or the build) when a key is in the wrong layer.
function(lugalos_check_layers board profile)
    _lugalos_layer_names("${board}" _bn)
    _lugalos_layer_names("${profile}" _pn)
    foreach(_n ${_bn})
        if(_n IN_LIST LUGALOS_PROFILE_KEYS)
            message(FATAL_ERROR "${board} sets ${_n}, which is policy: it belongs in a profile (cmake/lugalos_layers.cmake)")
        endif()
        if(_n MATCHES "^LUGALOS_ENABLE_(.*)$" AND NOT CMAKE_MATCH_1 IN_LIST LUGALOS_BOARD_DRIVERS)
            message(FATAL_ERROR "${board} sets ${_n}: a board chooses only drivers (${LUGALOS_BOARD_DRIVERS})")
        endif()
    endforeach()
    foreach(_n ${_pn})
        if(_n MATCHES "^CONFIG_" AND NOT _n IN_LIST LUGALOS_PROFILE_KEYS)
            message(FATAL_ERROR "${profile} sets ${_n}, which is a board fact or not a known policy key (cmake/lugalos_layers.cmake)")
        endif()
        if(_n MATCHES "^LUGALOS_ENABLE_(.*)$" AND NOT CMAKE_MATCH_1 IN_LIST LUGALOS_PROFILE_FEATURES)
            message(FATAL_ERROR "${profile} sets ${_n}: a profile chooses only software (${LUGALOS_PROFILE_FEATURES})")
        endif()
    endforeach()
    foreach(_f ${LUGALOS_PROFILE_FEATURES})
        if(NOT "LUGALOS_ENABLE_${_f}" IN_LIST _pn)
            message(FATAL_ERROR "${profile} does not say whether it builds ${_f} (set LUGALOS_ENABLE_${_f} ON or OFF)")
        endif()
    endforeach()
endfunction()

# Reads both layers into the caller's scope. Every driver starts OFF, so what
# a board does not name is not built, whatever an older CMakeCache.txt holds.
macro(lugalos_read_layers board profile)
    lugalos_check_layers("${board}" "${profile}")
    foreach(_d ${LUGALOS_BOARD_DRIVERS})
        set(LUGALOS_ENABLE_${_d} OFF)
    endforeach()
    include("${board}")
    include("${profile}")
    # The old name for CERES, still read by gen_config.cmake.
    set(LUGALOS_ENABLE_SCROLLER ${LUGALOS_ENABLE_CERES})
endmacro()
