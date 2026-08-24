cmake_minimum_required(VERSION 3.20)

project(lugalos C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Emits build/<target>/compile_commands.json -- editor tooling (clangd) has
# no other way to learn this project's real include paths, which are
# cross-compilation flags set below, not anything a default heuristic would
# guess. Without this, clangd falls back to guessing and gets nearly every
# header wrong; see the repo-root .clangd file for which build directory's
# database it's pointed at.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Determine Target Configuration (Default: RV32 NOMMU)
if(NOT DEFINED LUGALOS_TARGET)
    set(LUGALOS_TARGET "RV32_NOMMU" CACHE STRING "LugalOS target architecture (RV32_NOMMU, RV64_MMU, RP2350, K210)")
endif()

message(STATUS "Configuring LugalOS for target: ${LUGALOS_TARGET}")

# Default to a size-optimized build if the caller didn't request a specific
# one -- this is bare-metal/embedded code (RP2350 has limited flash), and an
# unset CMAKE_BUILD_TYPE previously meant no optimization flag at all
# (effectively -O0) regardless of what the caller intended (see A6/V6 in
# plan/2026-08-07_review_and_remediation.md: measured -Os at -17% text,
# -48% data on this codebase vs. the unoptimized default). Callers can still
# override with e.g. -DCMAKE_BUILD_TYPE=Debug.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "MinSizeRel" CACHE STRING "Choose the build type." FORCE)
endif()

# Compiler & Linker Flags
set(COMMON_CFLAGS "-Wall -Wextra -pedantic -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -mcmodel=medany -ggdb")
set(COMMON_ASFLAGS "-ffreestanding -nostdlib -mcmodel=medany -ggdb")

if(LUGALOS_TARGET STREQUAL "RV32_NOMMU")
    set(ARCH_MARCH "rv32imac_zicsr_zifencei")
    set(ARCH_MABI "ilp32")
    set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker/qemu-rv32.ld")
    add_compile_definitions(CONFIG_TARGET_RV32=1 CONFIG_NOMMU=1 CONFIG_MODE_M=1)
elseif(LUGALOS_TARGET STREQUAL "RV64_MMU")
    set(ARCH_MARCH "rv64gc_zicsr_zifencei")
    set(ARCH_MABI "lp64d")
    set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker/qemu-rv64.ld")
    add_compile_definitions(CONFIG_TARGET_RV64=1 CONFIG_MMU=1 CONFIG_MODE_S=1)
elseif(LUGALOS_TARGET STREQUAL "RP2350")
    set(ARCH_MARCH "rv32imac_zicsr_zbs")
    set(ARCH_MABI "ilp32")
    set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker/rp2350.ld")
    add_compile_definitions(CONFIG_TARGET_RV32=1 CONFIG_NOMMU=1 CONFIG_MODE_M=1 CONFIG_BOARD_RP2350=1)
elseif(LUGALOS_TARGET STREQUAL "K210")
    set(ARCH_MARCH "rv64gc_zicsr_zifencei")
    set(ARCH_MABI "lp64d")
    set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker/qemu-rv64.ld")
    add_compile_definitions(CONFIG_TARGET_RV64=1 CONFIG_MMU=1 CONFIG_MODE_S=1 CONFIG_BOARD_K210=1)
else()
    message(FATAL_ERROR "Unknown LUGALOS_TARGET: ${LUGALOS_TARGET}")
endif()

# Append rather than overwrite: CMAKE_C_FLAGS/CMAKE_ASM_FLAGS may already
# carry caller-supplied flags (-DCMAKE_C_FLAGS=..., a CMake preset, etc.),
# which an unconditional set() here used to silently discard.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_CFLAGS} -march=${ARCH_MARCH} -mabi=${ARCH_MABI}")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${COMMON_ASFLAGS} -march=${ARCH_MARCH} -mabi=${ARCH_MABI}")
# No -T here: user programs (below) link against a different script, and two
# -T options on one command line is not a well-defined way to override one.
# Each target names the script it wants.
set(CMAKE_EXE_LINKER_FLAGS "-nostdlib -Wl,--gc-sections")

# Include Directories
include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/riscv/include
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/include
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/include
    ${CMAKE_CURRENT_SOURCE_DIR}/fs/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libc/include
    ${CMAKE_CURRENT_SOURCE_DIR}/user/lisp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/user/chibicc/include
    ${CMAKE_CURRENT_SOURCE_DIR}/user/ed/include
    ${CMAKE_CURRENT_SOURCE_DIR}/user/chess/include
)

# UBSan only makes sense on QEMU targets (bare metal has no abort/printf runtime)
if(LUGALOS_TARGET STREQUAL "RP2350" OR LUGALOS_TARGET STREQUAL "K210")
    option(ENABLE_SANITIZERS "Enable UBSan runtime sanitizers for bug catching" OFF)
else()
    option(ENABLE_SANITIZERS "Enable UBSan runtime sanitizers for bug catching" ON)
endif()

if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all)

    # -fno-sanitize-recover=all makes the compiler treat the UBSan handler as
    # noreturn; if the handler merely logs and returns (the historical
    # behavior here), execution falls through into whatever the compiler
    # assumed was unreachable. UBSAN_PANIC makes the handlers actually halt,
    # so a fault is a deterministic stop instead of silently-continuing
    # corruption -- this is what lets the QEMU test runner detect faults at
    # all instead of them being swallowed mid-test.
    option(UBSAN_PANIC "Halt the system on UBSan fault instead of only logging" ON)
    if(UBSAN_PANIC)
        add_compile_definitions(CONFIG_UBSAN_PANIC=1)
    endif()
endif()

# Software feature flags (F0, plan/phase8_feature_flags.md): which optional,
# self-contained subsystems get built in at all. A build persona choice
# ("sensor node doesn't need cc"), orthogonal to which board, so these are
# plain cache options rather than facts in cmake/board-*.cmake. Default ON
# everywhere -- this phase changes what *can* be built, not what tests exercise
# by default. Baked into lugalos_config.h below so C code gates uniformly with
# #if CONFIG_ENABLE_CC regardless of target.
option(LUGALOS_ENABLE_CC "Build the in-kernel C compiler (cc, chibicc)" ON)
option(LUGALOS_ENABLE_ED "Build the ed line editor" ON)

# Same mechanism, RP2350-only hardware (H3, plan/phase9_chess_computer.md).
# Three independent flags, not one -- the TM1638 keypad/7-segment/LEDs are
# useful on their own in a board persona with no display attached, so it
# would be wrong to bundle it with the canvas driver. CONFIG_ENABLE_CHESS is
# defined now with no consumer yet (H4 is what makes it real, the same shape
# F0 defined ENABLE_CC/ENABLE_ED before F1/F2 had anything to gate).
option(LUGALOS_ENABLE_ST7735 "Build the ST7735 TFT canvas driver (RP2350 only)" ON)
option(LUGALOS_ENABLE_TM1638 "Build the TM1638 7-segment/keypad/LED driver (RP2350 only)" ON)
option(LUGALOS_ENABLE_CHESS "Build the chess engine (RP2350 only; H4, not yet implemented)" ON)

# Same mechanism again (L1, plan/phase11_pico_clock_green.md): the
# Pico-Clock-Green baseboard persona wires GP10-13 (SPI1/SD card on every
# other RP2350 persona) to its own display hardware instead, so that persona
# builds with this OFF. Default ON -- unlike ENABLE_ST7735/TM1638/CHESS
# above, spisd_rp2350.c isn't gated out of CMakeLists.txt's source list at
# all; it stubs itself internally via #if CONFIG_ENABLE_SPISD instead, so
# fs/vfs_server.c's spisd_get_device() call sites need no matching #if.
option(LUGALOS_ENABLE_SPISD "Build SD-card support via SPI1 (RP2350 only)" ON)

# L2 (plan/phase11_pico_clock_green.md): the SM16106/SM5166P LED matrix +
# LDR driver for the Pico-Clock-Green baseboard persona specifically.
# Default OFF, opposite of ENABLE_ST7735/TM1638/CHESS/SPISD above -- those
# four all have their pin facts defined in the *default* board file
# (cmake/board-rp2350.cmake), so defaulting ON is harmless there. This one's
# CONFIG_CLOCK_* pins only exist in cmake/board-rp2350-clock.cmake, so
# defaulting it ON would break the default persona's build the moment
# something actually consumed those undefined macros.
option(LUGALOS_ENABLE_PICO_CLOCK_GREEN "Build the Pico-Clock-Green SM16106/SM5166P LED matrix driver (RP2350 only)" OFF)

# D2 (plan/phase17_clock_ui_and_dcf77.md): the DCF-77 longwave receiver wired
# to GP27/GP28 on the Pico-Clock-Green persona. Default OFF for the same
# reason ENABLE_PICO_CLOCK_GREEN is: its CONFIG_DCF77_* pin facts exist only
# in cmake/board-rp2350-clock.cmake, so defaulting it ON would break every
# other persona's build the moment something consumed those macros.
option(LUGALOS_ENABLE_DCF77 "Build the DCF-77 longwave time-signal receiver driver (RP2350 only)" OFF)

# --- U-mode user programs (B6, plan/phase5_distributed_design.md §5.4) ------
#
# Separately linked binaries, loaded from the filesystem and run in U-mode by
# arch/riscv/common/elf.c. They are built here rather than checked in because
# they are target-specific (RV32 vs RV64, ilp32 vs lp64d), and the filesystem
# image they ship in is generated per build anyway.
#
# The flags are the position-independence contract that linker/user.ld
# documents:
#   -mcmodel=medany                 PC-relative code and data (from COMMON_CFLAGS)
#   -mno-relax / -Wl,--no-relax     keep the linker from rewriting those into
#                                   gp-relative accesses; nothing sets gp here
#   -msmall-data-limit=0            ...and keep globals out of .sdata, which is
#                                   what gp-relative addressing exists for
#   -fno-jump-tables                a switch table is absolute addresses in .rodata
#   -fno-sanitize=all               UBSan calls land in the *kernel's* runtime
#
# A program that violates any of this does not fault mysteriously at run time:
# it either fails to link, or trips one of user.ld's ASSERTs by name.
set(USER_LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/linker/user.ld)
set(USER_PROGRAMS uhello uisolate uspin uargs uchan ubig uwx)
set(USER_PROG_STAGED "")

foreach(uprog ${USER_PROGRAMS})
    add_executable(${uprog}.elf user/progs/${uprog}.c)
    target_compile_options(${uprog}.elf PRIVATE
        -fno-sanitize=all -Os -mno-relax -msmall-data-limit=0
        -fno-jump-tables -fno-common -fno-tree-loop-distribute-patterns)
    target_include_directories(${uprog}.elf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/user/progs)
    target_link_options(${uprog}.elf PRIVATE
        -T${USER_LINKER_SCRIPT} -nostdlib -static -Wl,--no-relax -Wl,--build-id=none)
    set_target_properties(${uprog}.elf PROPERTIES LINK_DEPENDS ${USER_LINKER_SCRIPT})
    # Stripped: the loader reads the program headers out of the file, and an
    # unstripped binary is mostly debug sections that would be embedded
    # verbatim into the kernel's flash image.
    add_custom_command(TARGET ${uprog}.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} --strip-all $<TARGET_FILE:${uprog}.elf>
                ${CMAKE_CURRENT_BINARY_DIR}/${uprog}.stripped
        COMMENT "Stripping user program ${uprog}.elf")
    list(APPEND USER_PROG_STAGED ${uprog})
endforeach()

# Generate Embedded Flash FAT32 image (flash_fs.c)
#
# Built from a staging directory rather than straight from tools/sd_root, so
# the generated user programs ship alongside the checked-in sources without
# either being written into the source tree.
set(SD_STAGE ${CMAKE_CURRENT_BINARY_DIR}/sd_root)
set(SD_STAGE_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${SD_STAGE}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/tools/sd_root ${SD_STAGE})
# Staged into system/bin/, which is where the shell's search path looks (C1,
# plan/phase6_memory_and_processes.md). Putting them at the volume root would
# still work when named by full path, but then nothing on any volume would be
# reachable by bare name and the convention would exist only on paper.
list(APPEND SD_STAGE_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SD_STAGE}/system/bin)
foreach(uprog ${USER_PROG_STAGED})
    list(APPEND SD_STAGE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/${uprog}.stripped
                ${SD_STAGE}/system/bin/${uprog}.elf)
endforeach()

# The staged files are inputs, so editing one has to rebuild the image.
# Without this, changing init.lisp left the flash image holding the previous
# copy and no build step disagreed -- which surfaces as unrelated 9P transport
# tests failing on content that "should" match, because they compare what the
# image serves against what the source now says. CONFIGURE_DEPENDS so that
# adding a file re-runs the glob rather than needing a manual re-configure.
file(GLOB_RECURSE SD_ROOT_FILES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/tools/sd_root/*)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/flash_fs.c
    ${SD_STAGE_COMMANDS}
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/create_flash_fs.py ${CMAKE_CURRENT_BINARY_DIR}/flash_fs.c ${SD_STAGE}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/create_flash_fs.py ${CMAKE_CURRENT_SOURCE_DIR}/tools/create_sd_image.py
            ${SD_ROOT_FILES}
            uhello.elf uisolate.elf uspin.elf uargs.elf uchan.elf ubig.elf uwx.elf
    COMMENT "Generating embedded Flash FAT32 image flash_fs.c"
)

# Build-id header, regenerated (write-if-changed) before every build so it
# tracks commits without a re-configure. See cmake/gen_build_id.cmake.
#
# M4.5, plan/phase12_microkernel_migration.md: the OBJECT_DEPENDS fix below
# (fs/vfs_server.c PROPERTIES OBJECT_DEPENDS ${LUGALOS_BUILD_ID_HEADER})
# turned out not to fully close the staleness gap it was written to close --
# found recurring again, months later, on a build that had nothing to do
# with vfs_server.c itself. Root cause: OBJECT_DEPENDS only tells ninja "this
# object depends on this *file*"; it does not tell ninja *what produces that
# file*, since add_custom_target() (used below) has no OUTPUT of its own for
# CMake to wire in. Ninja was therefore free to check vfs_server.c.o against
# whatever mtime lugalos_build_id.h already happened to have -- possibly
# *before* this run's regeneration had touched it -- rather than being forced
# to run the regeneration first. `touch fs/vfs_server.c` before rebuilding
# masked the symptom every time it was tried (a real recompile always picks
# up the current header content off disk regardless of ninja's dependency
# graph), which is exactly why it looked fixed each time.
#
# Fixed properly this time with a real producer/consumer edge: an
# add_custom_command(OUTPUT ...) actually tells CMake "this command produces
# this file," which is what makes OBJECT_DEPENDS below wire in a real
# ordering edge, not just an mtime comparison against an orphan path. The
# SYMBOLIC _always_rebuild marker is the standard CMake+Ninja idiom for
# "always considered out of date" (Ninja otherwise assumes any OUTPUT that
# already exists on disk, with no changed input, needs no rework) -- an empty
# command with no real output file, marked SYMBOLIC so its own "existence"
# is never mtime-checked, only ever treated as permanently dirty.
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/_always_rebuild
    COMMAND ${CMAKE_COMMAND} -E true)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/_always_rebuild PROPERTIES SYMBOLIC 1)

set(LUGALOS_BUILD_ID_HEADER ${CMAKE_CURRENT_BINARY_DIR}/lugalos_build_id.h)
execute_process(COMMAND ${CMAKE_COMMAND}
    -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${LUGALOS_BUILD_ID_HEADER}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_build_id.cmake)
add_custom_command(OUTPUT ${LUGALOS_BUILD_ID_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${LUGALOS_BUILD_ID_HEADER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_build_id.cmake
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/_always_rebuild
    COMMENT "Refreshing build id")
add_custom_target(lugalos_build_id ALL DEPENDS ${LUGALOS_BUILD_ID_HEADER})

# fs/vfs_server.c embeds LUGALOS_BUILD_ID (served as /proc/buildid) -- without
# this, ninja has no ordering edge between "regenerate the header" and
# "compile the one file that includes it" (add_dependencies() below only
# orders the custom target before the final *link*, not before individual
# compiles), so vfs_server.c.o could be compiled from the *previous* run's
# header content while this run's freshly-regenerated header sits on disk
# unused until the next build. Found by hand: tests/hw/flash.py's --verify
# reported a build-id mismatch on three consecutive real flashes even
# though the board visibly had the new firmware (a working heartbeat LED
# that could not have existed in the old build) -- `touch fs/vfs_server.c`
# and rebuilding made the mismatch vanish, confirming a stale object, not a
# bad flash. OBJECT_DEPENDS is the property that actually closes this: it
# forces both the ordering and the rebuild-on-change ninja would otherwise
# only get by accident of GCC's own header-scanning already having seen a
# newer mtime by the time it ran.
set_source_files_properties(fs/vfs_server.c PROPERTIES OBJECT_DEPENDS ${LUGALOS_BUILD_ID_HEADER})

# Per-board config header (K0, plan/phase7_kernel_config.md): platform
# defaults and, where the board has real GPIO pins, a pin map -- generated
# write-if-changed before every build, same shape as lugalos_build_id above.
# K210 has no board file yet (no real drivers reference it today); config
# generation is skipped there rather than inventing placeholder data.
# Overridable (L1, plan/phase11_pico_clock_green.md): a single LUGALOS_TARGET
# can have more than one board persona sharing the same arch/ABI/linker
# script -- e.g. RP2350's default chess-computer persona vs. the
# Pico-Clock-Green baseboard persona, which wires several of the same pins
# to different hardware. Callers select a non-default persona with
# -DLUGALOS_BOARD_FILE=cmake/board-rp2350-clock.cmake (or via a
# CMakePresets.json preset); the branch below only fires when nothing
# overrode it already.
if(NOT DEFINED LUGALOS_BOARD_FILE)
    if(LUGALOS_TARGET STREQUAL "RV32_NOMMU")
        set(LUGALOS_BOARD_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/board-rv32-nommu.cmake")
    elseif(LUGALOS_TARGET STREQUAL "RV64_MMU")
        set(LUGALOS_BOARD_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/board-rv64-mmu.cmake")
    elseif(LUGALOS_TARGET STREQUAL "RP2350")
        set(LUGALOS_BOARD_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/board-rp2350.cmake")
    endif()
endif()

if(DEFINED LUGALOS_BOARD_FILE)
    set(LUGALOS_CONFIG_HEADER ${CMAKE_CURRENT_BINARY_DIR}/lugalos_config.h)
    execute_process(COMMAND ${CMAKE_COMMAND}
        -DBOARD=${LUGALOS_BOARD_FILE} -DOUT=${LUGALOS_CONFIG_HEADER}
        -DENABLE_CC=${LUGALOS_ENABLE_CC} -DENABLE_ED=${LUGALOS_ENABLE_ED}
        -DENABLE_ST7735=${LUGALOS_ENABLE_ST7735} -DENABLE_TM1638=${LUGALOS_ENABLE_TM1638}
        -DENABLE_CHESS=${LUGALOS_ENABLE_CHESS} -DENABLE_SPISD=${LUGALOS_ENABLE_SPISD}
        -DENABLE_PICO_CLOCK_GREEN=${LUGALOS_ENABLE_PICO_CLOCK_GREEN}
        -DENABLE_DCF77=${LUGALOS_ENABLE_DCF77}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_config.cmake)
    # add_custom_command(OUTPUT ...), not a bare add_custom_target(COMMAND ...)
    # -- the same distinction lugalos_build_id.h above already depends on, and
    # for the same reason. A target that merely *runs* the generator declares
    # no producer/consumer edge on the file it writes, so OBJECT_DEPENDS below
    # would have nothing real to hang an ordering edge from, and
    # add_dependencies(lugalos.elf lugalos_config) only orders the target
    # before the final *link*, never before an individual compile.
    #
    # Found 2026-08-22 while board-scaling CONFIG_BALLOC_ARENA_PAGES (§1.1,
    # plan/phase15_memory_reclamation.md): editing only cmake/board-*.cmake
    # and rebuilding produced a binary whose kernel/balloc.c.obj had been
    # compiled against the *previous* config -- the boot log reported the old
    # 64 KB arena while build/rv32/lugalos_config.h on disk already said 4
    # pages. A second, otherwise-identical `cmake --build` fixed it. This is
    # exactly the stale-object failure the build-id block above records
    # ("a build-id mismatch on three consecutive real flashes"), which was
    # fixed there and left unfixed here.
    #
    # Worse here than there, too: the build id is a cosmetic string, while
    # this header carries the board's pin map and its memory sizing. A stale
    # object means a flashed image driving the wrong GPIOs, or sizing an
    # allocator against a heap that is not the one it got.
    add_custom_command(OUTPUT ${LUGALOS_CONFIG_HEADER}
        COMMAND ${CMAKE_COMMAND}
            -DBOARD=${LUGALOS_BOARD_FILE} -DOUT=${LUGALOS_CONFIG_HEADER}
            -DENABLE_CC=${LUGALOS_ENABLE_CC} -DENABLE_ED=${LUGALOS_ENABLE_ED}
            -DENABLE_ST7735=${LUGALOS_ENABLE_ST7735} -DENABLE_TM1638=${LUGALOS_ENABLE_TM1638}
            -DENABLE_CHESS=${LUGALOS_ENABLE_CHESS} -DENABLE_SPISD=${LUGALOS_ENABLE_SPISD}
            -DENABLE_PICO_CLOCK_GREEN=${LUGALOS_ENABLE_PICO_CLOCK_GREEN}
            -DENABLE_DCF77=${LUGALOS_ENABLE_DCF77}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_config.cmake
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/_always_rebuild
                ${LUGALOS_BOARD_FILE}
                ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_config.cmake
        COMMENT "Refreshing board config")
    add_custom_target(lugalos_config ALL DEPENDS ${LUGALOS_CONFIG_HEADER})
endif()

# Core Sources
set(SOURCES
    kernel/main.c
    kernel/printk.c
    kernel/klog.c
    kernel/console.c
    kernel/device.c
    kernel/board.c
    kernel/chan.c
    kernel/palloc.c
    kernel/scratch.c
    kernel/balloc.c
    kernel/meminfo.c
    kernel/path.c
    kernel/uaccess.c
    kernel/ticker.c
    kernel/time.c
    kernel/timezone.c
    # N1 (plan/phase18_networking_and_auth.md): SHA-256/HMAC-SHA-256 for the
    # 9P Tauth gate. Target-independent and built everywhere, including the
    # QEMU targets that will never have a network cable -- an auth exchange's
    # fiddly part is its arithmetic, and arithmetic should not be debugged by
    # flashing (same argument as drivers/dcf77_decode.c above).
    kernel/sha256.c
    # N1: the nonce source for that gate, plus the measurement that says
    # whether the ring oscillator behind it is worth trusting.
    kernel/random.c
    kernel/ipc.c
    kernel/devirq.c
    kernel/sched.c
    kernel/shell.c
    kernel/line_editor.c
    kernel/ubsan.c

    arch/riscv/common/entry.S
    arch/riscv/common/switch.S
    arch/riscv/common/umode.S
    arch/riscv/common/umode.c
    arch/riscv/common/mem_domain.c

    arch/riscv/common/trap.c
    arch/riscv/common/pmp_probe.c
    arch/riscv/common/elf.c
    drivers/ramdisk.c
    # D1 (plan/phase17_clock_ui_and_dcf77.md): the DCF-77 frame decoder is
    # target-independent -- it consumes timestamped samples, never a register
    # -- so it is built and self-tested on every target, including the QEMU
    # ones that will never have a radio attached. That is the point: the frame
    # logic is the fiddly part and it should not be debugged by flashing.
    drivers/dcf77_decode.c
    # C3 (plan/phase17_clock_ui_and_dcf77.md): same reasoning, same shape --
    # the menu state machine takes key events and returns screen descriptions,
    # so it builds and self-tests on targets with neither buttons nor a panel.
    drivers/pico_clock_ui.c
    drivers/flashdisk.c
    drivers/i2c_rtc.c
    drivers/at24c32.c
    drivers/loopback_net.c
    drivers/uart_net.c
    drivers/usb_cdc.c
    ${CMAKE_CURRENT_BINARY_DIR}/flash_fs.c
    fs/fat32.c
    fs/vfs_server.c
    fs/9p.c
    fs/p9_link.c
    fs/p9_chan.c
    libc/string.c
    user/lisp/lisp.c
)

# Optional subsystems (F1/F2, plan/phase8_feature_flags.md): each is a
# self-contained leaf with exactly one outside touchpoint, gated to match in
# lisp.c/shell.c via the CONFIG_ENABLE_CC/CONFIG_ENABLE_ED generated defines.
if(LUGALOS_ENABLE_CC)
    list(APPEND SOURCES
        user/chibicc/main.c
        user/chibicc/pools.c
        user/chibicc/preprocess.c
        user/chibicc/tokenize.c
        user/chibicc/parse.c
        user/chibicc/codegen.c)
endif()
if(LUGALOS_ENABLE_ED)
    list(APPEND SOURCES user/ed/ed.c)
endif()
if(LUGALOS_ENABLE_CHESS)
    # H4 (plan/phase9_chess_computer.md): engine core is portable (builds on
    # every target, not just RP2350) so it's testable fast on QEMU before
    # ever touching real hardware; chess_ui.c's interactive TM1638/ST7735
    # game loop is itself internally guarded to RP2350 + those two flags,
    # same file either way -- chess_selftest() (no hardware dependency)
    # always builds.
    list(APPEND SOURCES
        user/chess/src/chess_platform.c
        user/chess/src/bitboard.c
        user/chess/src/position.c
        user/chess/src/movegen.c
        user/chess/src/zobrist.c
        user/chess/src/evaluation.c
        user/chess/src/tt.c
        user/chess/src/search.c
        user/chess/src/perft.c
        user/chess/src/pgn.c
        user/chess/src/chess_ui.c)
endif()

if(LUGALOS_TARGET STREQUAL "RP2350")
    set(SOURCES arch/riscv/rp2350/boot_header.S ${SOURCES} drivers/uart_rp2350.c drivers/spisd_rp2350.c arch/riscv/rp2350/bootrom.c)
    # H1/H2 (plan/phase9_chess_computer.md): independently toggleable, per H3
    # -- the TM1638 keypad/7-segment/LEDs are useful without the display
    # attached at all, so they don't share a flag.
    if(LUGALOS_ENABLE_ST7735)
        list(APPEND SOURCES drivers/st7735_rp2350.c)
    endif()
    if(LUGALOS_ENABLE_TM1638)
        list(APPEND SOURCES drivers/tm1638_rp2350.c)
    endif()
    # L2 (plan/phase11_pico_clock_green.md): Pico-Clock-Green persona only
    # (LUGALOS_ENABLE_PICO_CLOCK_GREEN defaults OFF -- see the option() above).
    if(LUGALOS_ENABLE_PICO_CLOCK_GREEN)
        list(APPEND SOURCES drivers/pico_clock_green_rp2350.c)
        # C2 (plan/phase17_clock_ui_and_dcf77.md): the font is all .rodata and
        # has no hardware in it, but it has no other consumer either, so it
        # follows the display flag rather than becoming a target-wide source.
        list(APPEND SOURCES drivers/pico_clock_font.c)
        list(APPEND SOURCES drivers/pico_clock_app.c)
    endif()
    # D2 (plan/phase17_clock_ui_and_dcf77.md): independent of the display
    # flag above -- the receiver is a separate module on separate pins, and
    # the probe diagnostic is most useful with the display *not* running.
    if(LUGALOS_ENABLE_DCF77)
        list(APPEND SOURCES drivers/dcf77_rp2350.c)
        list(APPEND SOURCES drivers/dcf77_service.c)
    endif()
else()
    # QEMU targets: VirtIO block device + 16550 UART + VirtIO console (9P link, A3)
    list(APPEND SOURCES drivers/uart_16550.c drivers/virtio_blk.c drivers/virtio_console.c)
endif()

# Architecture-specific memory management source
if(LUGALOS_TARGET MATCHES "RV32_NOMMU" OR LUGALOS_TARGET STREQUAL "RP2350")
    list(APPEND SOURCES arch/riscv/rv32_nommu/vmm.c)
else()
    list(APPEND SOURCES arch/riscv/rv64_mmu/vmm.c)
endif()

# M5, plan/phase12_microkernel_migration.md: -fno-jump-tables for every
# driver file with UATTR-tagged U-mode code (kernel/shell.c's own probes
# are small enough to have never triggered this, but every driver's
# dispatch loop is squarely at risk). user/progs/${uprog}.elf's own
# COMMON_CFLAGS comment already names the reason ("a switch table is
# absolute addresses in .rodata") for a different motivation
# (position-independence); the same fact bites UATTR code for its own
# reason -- a switch (or, found on real hardware in M5 Phase 4, an
# if/else chain GCC's switch-conversion pass reconstructs into a table
# just the same) compiles into a jump table stored in ordinary .rodata,
# outside the .utext/.st7735text page the U-mode domain actually grants,
# so the indirect jump through it takes a load access fault. Applied to
# the whole file rather than only the UATTR functions within it -- CMake
# has no finer granularity than a translation unit, and the cost (a
# marginally larger, marginally slower jump table-free dispatch for any
# *other* switch in these same files) is immaterial next to what a silent
# jump table would risk again elsewhere in the same file later.
set_source_files_properties(
    drivers/uart_rp2350.c
    drivers/tm1638_rp2350.c
    drivers/i2c_rtc.c
    drivers/st7735_rp2350.c
    drivers/spisd_rp2350.c
    drivers/usb_cdc.c
    drivers/pico_clock_green_rp2350.c
    drivers/pico_clock_font.c
    PROPERTIES COMPILE_OPTIONS -fno-jump-tables)

# Every object, not an enumerated subset: lugalos_config.h is reached
# transitively (kernel/palloc.h and kernel/balloc.h both include it, and most
# of the tree includes one of those), so a hand-maintained list would go stale
# silently -- which is the failure this edge exists to prevent in the first
# place. Costs nothing when the config is unchanged: gen_config.cmake is
# write-if-changed, so the header's mtime does not move and ninja rebuilds
# nothing.
#
# Set last, after every list(APPEND SOURCES ...) above has run, and folded
# together with fs/vfs_server.c's pre-existing build-id dependency rather than
# overwriting it -- OBJECT_DEPENDS is a single property, so a second
# set_source_files_properties() on the same file would silently replace the
# first and reintroduce the stale-build-id bug this file already fixed once.
if(DEFINED LUGALOS_BOARD_FILE)
    set_source_files_properties(${SOURCES}
        PROPERTIES OBJECT_DEPENDS ${LUGALOS_CONFIG_HEADER})
    set_source_files_properties(fs/vfs_server.c
        PROPERTIES OBJECT_DEPENDS "${LUGALOS_CONFIG_HEADER};${LUGALOS_BUILD_ID_HEADER}")
endif()

# `sizereport` (§6, plan/phase15_memory_reclamation.md): where static RAM went,
# per source file, and what that leaves for the heap.
#
# A companion to the ASSERT in linker/rp2350.ld rather than a substitute. The
# linker floor catches a regression; this one attributes it. Not part of `all`
# -- it is a question you ask, not a step every build pays for.
#
#     cmake --build build/rp2350 --target sizereport
#     cmake --build build/rp2350 --target sizecheck    # fails if it grew
#
# `sizecheck` compares against tools/sizereport-<board>.json; re-baseline with
# `python3 tools/sizereport.py <elf> --update <baseline>` when growth is
# intended, so the new numbers land in a reviewable diff.
if(DEFINED LUGALOS_BOARD_FILE)
    get_filename_component(_board_stem "${LUGALOS_BOARD_FILE}" NAME_WE)
    string(REPLACE "board-" "" _board_stem "${_board_stem}")
    set(LUGALOS_SIZE_BASELINE
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/sizereport-${_board_stem}.json)

    add_custom_target(sizereport
        COMMAND ${CMAKE_COMMAND} -E echo "== static RAM, ${_board_stem} =="
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/sizereport.py
                $<TARGET_FILE:lugalos.elf> --nm ${CMAKE_NM}
        DEPENDS lugalos.elf
        USES_TERMINAL)

    add_custom_target(sizecheck
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/sizereport.py
                $<TARGET_FILE:lugalos.elf> --nm ${CMAKE_NM}
                --check ${LUGALOS_SIZE_BASELINE}
        DEPENDS lugalos.elf
        USES_TERMINAL)
endif()

# Executable Output
add_executable(lugalos.elf ${SOURCES})
add_dependencies(lugalos.elf lugalos_build_id)
if(DEFINED LUGALOS_BOARD_FILE)
    add_dependencies(lugalos.elf lugalos_config)
endif()

# Relink when the linker script changes. CMake does not infer this, so an
# edit to the memory layout silently produced a stale binary -- which cost a
# confusing debugging round when a 2 MB alignment appeared not to take effect.
set_target_properties(lugalos.elf PROPERTIES LINK_DEPENDS ${LINKER_SCRIPT})
target_link_options(lugalos.elf PRIVATE -T${LINKER_SCRIPT})
target_link_libraries(lugalos.elf PRIVATE gcc)

# UF2 converter: official uf2conv.py with RP2350 RISC-V family support
set(UF2CONV_PY python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/uf2conv.py)

# Generate Raw Binary, Disassembly, Shared SD Disk Image, and UF2 Firmware
add_custom_command(TARGET lugalos.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary lugalos.elf lugalos.bin
    COMMAND ${CMAKE_OBJDUMP} -d -S lugalos.elf > lugalos.asm
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/build"
    # Per build directory, NOT shared across targets. The image carries the
    # user programs, and those are target-specific -- an RV32 binary on an RV64
    # kernel is not a portability question, it is a different instruction set.
    # One shared image meant whichever target built last decided what every
    # other target's tests loaded. It stayed hidden for a long time because a
    # simple program survives being the wrong ISA; uargs, which dereferences
    # pointers, does not, and faults on a 32-bit address sign-extended to 64.
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/create_sd_image.py ${CMAKE_CURRENT_BINARY_DIR}/lugalos_sd.img ${SD_STAGE} --if-stale
    COMMAND ${UF2CONV_PY} --family rp2350_riscv --base 0x10000000 lugalos.bin -o lugalos.uf2
    COMMENT "Generated lugalos.bin, lugalos.asm, lugalos.uf2, and this target's lugalos_sd.img"
)

if(LUGALOS_TARGET STREQUAL "RP2350")
    add_executable(minimal_rp2350.elf arch/riscv/rp2350/boot_header.S tools/minimal_rp2350.c arch/riscv/common/entry.S)
    target_link_options(minimal_rp2350.elf PRIVATE -T${LINKER_SCRIPT})
    set_target_properties(minimal_rp2350.elf PROPERTIES COMPILE_FLAGS "-Dminimal_main=kernel_main")
    add_custom_command(TARGET minimal_rp2350.elf POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary minimal_rp2350.elf minimal_rp2350.bin
        COMMAND ${UF2CONV_PY} --family rp2350_riscv --base 0x10000000 minimal_rp2350.bin -o minimal_rp2350.uf2
        COMMENT "Generated minimal_rp2350.uf2 standalone hardware test"
    )
endif()


# Automated Test Suite Target
enable_testing()
add_test(NAME LugalOS_Suite COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/runner.py)

add_custom_target(test-all
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/runner.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running LugalOS Automated Test Suite"
)


