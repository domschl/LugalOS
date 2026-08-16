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
set(LUGALOS_BUILD_ID_HEADER ${CMAKE_CURRENT_BINARY_DIR}/lugalos_build_id.h)
execute_process(COMMAND ${CMAKE_COMMAND}
    -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${LUGALOS_BUILD_ID_HEADER}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_build_id.cmake)
add_custom_target(lugalos_build_id ALL
    COMMAND ${CMAKE_COMMAND}
        -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${LUGALOS_BUILD_ID_HEADER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_build_id.cmake
    COMMENT "Refreshing build id")

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
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_config.cmake)
    add_custom_target(lugalos_config ALL
        COMMAND ${CMAKE_COMMAND}
            -DBOARD=${LUGALOS_BOARD_FILE} -DOUT=${LUGALOS_CONFIG_HEADER}
            -DENABLE_CC=${LUGALOS_ENABLE_CC} -DENABLE_ED=${LUGALOS_ENABLE_ED}
            -DENABLE_ST7735=${LUGALOS_ENABLE_ST7735} -DENABLE_TM1638=${LUGALOS_ENABLE_TM1638}
            -DENABLE_CHESS=${LUGALOS_ENABLE_CHESS} -DENABLE_SPISD=${LUGALOS_ENABLE_SPISD}
            -DENABLE_PICO_CLOCK_GREEN=${LUGALOS_ENABLE_PICO_CLOCK_GREEN}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_config.cmake
        COMMENT "Refreshing board config")
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
    kernel/balloc.c
    kernel/meminfo.c
    kernel/path.c
    kernel/uaccess.c
    kernel/ticker.c
    kernel/time.c
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
    user/lisp/lisp_compile.c
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


