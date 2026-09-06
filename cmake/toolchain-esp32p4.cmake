# Toolchain for the ESP32-P4 (Waveshare ESP32-P4-NANO) -- E2,
# plan/phase27_esp32p4_bringup.md.
#
# Byte-identical to cmake/toolchain-rp2350.cmake apart from LUGALOS_TARGET,
# and that is the finding rather than laziness: the P4's HP cores are
# RV32IMAFC (E0 section 5), so the same generic RISC-V cross-compiler this
# tree has always used builds for them with no new tool, no ESP-IDF, and no
# vendor fork of GCC. E1 already proved it on silicon with the same compiler
# and the same -march string.
#
# The search order carries the names Linux distributions ship as well as the
# ones a from-source build produces: on Arch the package is
# riscv64-elf-gcc, on Debian/Ubuntu riscv64-unknown-elf-gcc, and
# tools/build_minimal_esp32p4.sh already had to learn both.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(LUGALOS_TARGET "ESP32P4" CACHE STRING "LugalOS Target" FORCE)

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
