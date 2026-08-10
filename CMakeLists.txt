cmake_minimum_required(VERSION 3.20)

project(lugalos C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

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
set(USER_PROGRAMS uhello uisolate uspin)
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
foreach(uprog ${USER_PROG_STAGED})
    list(APPEND SD_STAGE_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/${uprog}.stripped
                ${SD_STAGE}/${uprog}.elf)
endforeach()

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/flash_fs.c
    ${SD_STAGE_COMMANDS}
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/create_flash_fs.py ${CMAKE_CURRENT_BINARY_DIR}/flash_fs.c ${SD_STAGE}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/create_flash_fs.py ${CMAKE_CURRENT_SOURCE_DIR}/tools/create_sd_image.py
            uhello.elf uisolate.elf uspin.elf
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
    kernel/meminfo.c
    kernel/uaccess.c
    kernel/ticker.c
    kernel/time.c
    kernel/ipc.c
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
    user/chibicc/main.c
    user/chibicc/preprocess.c
    user/chibicc/tokenize.c
    user/chibicc/parse.c
    user/chibicc/codegen.c
    user/ed/ed.c
)

if(LUGALOS_TARGET STREQUAL "RP2350")
    set(SOURCES arch/riscv/rp2350/boot_header.S ${SOURCES} drivers/uart_rp2350.c drivers/spisd_rp2350.c arch/riscv/rp2350/bootrom.c)
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
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/create_sd_image.py ${CMAKE_SOURCE_DIR}/build/lugalos_sd.img ${SD_STAGE} --if-missing
    COMMAND ${UF2CONV_PY} --family rp2350_riscv --base 0x10000000 lugalos.bin -o lugalos.uf2
    COMMENT "Generated lugalos.bin, lugalos.asm, lugalos.uf2, and shared disk image build/lugalos_sd.img"
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


