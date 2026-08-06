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

set(CMAKE_C_FLAGS "${COMMON_CFLAGS} -march=${ARCH_MARCH} -mabi=${ARCH_MABI}")
set(CMAKE_ASM_FLAGS "${COMMON_ASFLAGS} -march=${ARCH_MARCH} -mabi=${ARCH_MABI}")
set(CMAKE_EXE_LINKER_FLAGS "-T${LINKER_SCRIPT} -nostdlib -Wl,--gc-sections")

# Include Directories
include_directories(
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
endif()

# Generate Embedded Flash FAT32 image (flash_fs.c)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/flash_fs.c
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/create_flash_fs.py ${CMAKE_CURRENT_BINARY_DIR}/flash_fs.c ${CMAKE_CURRENT_SOURCE_DIR}/tools/sd_root
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/create_flash_fs.py ${CMAKE_CURRENT_SOURCE_DIR}/tools/create_sd_image.py
    COMMENT "Generating embedded Flash FAT32 image flash_fs.c"
)

# Core Sources
set(SOURCES
    kernel/main.c
    kernel/printk.c
    kernel/time.c
    kernel/ipc.c
    kernel/sched.c
    kernel/shell.c
    kernel/line_editor.c
    kernel/ubsan.c

    arch/riscv/common/entry.S

    arch/riscv/common/trap.c
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
    set(SOURCES arch/riscv/rp2350/boot_header.S ${SOURCES} drivers/uart_rp2350.c drivers/spisd_rp2350.c)
else()
    # QEMU targets: VirtIO block device + 16550 UART
    list(APPEND SOURCES drivers/uart_16550.c drivers/virtio_blk.c)
endif()

# Architecture-specific memory management source
if(LUGALOS_TARGET MATCHES "RV32_NOMMU" OR LUGALOS_TARGET STREQUAL "RP2350")
    list(APPEND SOURCES arch/riscv/rv32_nommu/vmm.c)
else()
    list(APPEND SOURCES arch/riscv/rv64_mmu/vmm.c)
endif()

# Executable Output
add_executable(lugalos.elf ${SOURCES})
target_link_libraries(lugalos.elf PRIVATE gcc)

# UF2 converter: official uf2conv.py with RP2350 RISC-V family support
set(UF2CONV_PY python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/uf2conv.py)

# Generate Raw Binary, Disassembly, Shared SD Disk Image, and UF2 Firmware
add_custom_command(TARGET lugalos.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary lugalos.elf lugalos.bin
    COMMAND ${CMAKE_OBJDUMP} -d -S lugalos.elf > lugalos.asm
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/build"
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/create_sd_image.py ${CMAKE_SOURCE_DIR}/build/lugalos_sd.img --if-missing
    COMMAND ${UF2CONV_PY} --family rp2350_riscv --base 0x10000000 lugalos.bin -o lugalos.uf2
    COMMENT "Generated lugalos.bin, lugalos.asm, lugalos.uf2, and shared disk image build/lugalos_sd.img"
)

if(LUGALOS_TARGET STREQUAL "RP2350")
    add_executable(minimal_rp2350.elf arch/riscv/rp2350/boot_header.S tools/minimal_rp2350.c arch/riscv/common/entry.S)
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


