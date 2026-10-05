# Toolchain for the ESP32-C6 (Waveshare ESP32-C6-Zero, and the C6-MINI on the
# ESP32-P4-NANO) -- 45.1, plan/phase45_esp32c6.md.
#
# The C6's HP core is RV32IMAC (datasheet: no F, no FPU), so the generic RISC-V
# cross-compiler already used for every other target in this tree builds it,
# exactly as for the P4 (cmake/toolchain-esp32p4.cmake) -- no riscv32-esp-elf,
# no IDF toolchain. The -march string is chosen where the sources are
# compiled; this file only finds the tools.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(LUGALOS_TARGET "ESP32C6" CACHE STRING "LugalOS Target" FORCE)

# Cross compiler search order
find_program(RISCV_GCC NAMES riscv64-elf-gcc riscv-none-elf-gcc riscv32-elf-gcc
                             riscv64-unknown-elf-gcc riscv32-unknown-elf-gcc
                             riscv64-linux-gnu-gcc)
find_program(RISCV_OBJCOPY NAMES riscv64-elf-objcopy riscv-none-elf-objcopy riscv32-elf-objcopy
                                 riscv64-unknown-elf-objcopy riscv32-unknown-elf-objcopy
                                 riscv64-linux-gnu-objcopy)
find_program(RISCV_OBJDUMP NAMES riscv64-elf-objdump riscv-none-elf-objdump riscv32-elf-objdump
                                 riscv64-unknown-elf-objdump riscv32-unknown-elf-objdump
                                 riscv64-linux-gnu-objdump)

if(NOT RISCV_GCC)
    message(FATAL_ERROR "Could not find a valid RISC-V GCC cross-compiler")
endif()

set(CMAKE_C_COMPILER ${RISCV_GCC})
set(CMAKE_ASM_COMPILER ${RISCV_GCC})
set(CMAKE_OBJCOPY ${RISCV_OBJCOPY})
set(CMAKE_OBJDUMP ${RISCV_OBJDUMP})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
