# Toolchain for the Kendryte K210 (Maix Bit) -- 65.3, .
#
# The K210's C0 core is RV64IMAF_C but the kernel is built rv64imac/lp64
# (no F, M-mode, no MMU -- the 65.1 measurements and the AGENTS.md
# integer-only ABI rule close it); the -march string is chosen in
# CMakeLists where the sources are compiled, and this file only finds the
# tools. The generic riscv64-elf cross-compiler already used for every
# other RISC-V target in this tree builds it -- no vendor
# kendryte-toolchain (its GCC 8.2.0 predates the zicsr/zifencei split and
# its libgcc is lp64f-only, exactly what D2 forbids in an image).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(LUGALOS_TARGET "K210" CACHE STRING "LugalOS Target" FORCE)

# Cross compiler search order
find_program(RISCV_GCC NAMES riscv64-elf-gcc riscv-none-elf-gcc
                             riscv64-unknown-elf-gcc riscv64-linux-gnu-gcc)
find_program(RISCV_OBJCOPY NAMES riscv64-elf-objcopy riscv-none-elf-objcopy
                                 riscv64-unknown-elf-objcopy riscv64-linux-gnu-objcopy)
find_program(RISCV_OBJDUMP NAMES riscv64-elf-objdump riscv-none-elf-objdump
                                 riscv64-unknown-elf-objdump riscv64-linux-gnu-objdump)

if(NOT RISCV_GCC)
    message(FATAL_ERROR "Could not find a valid RISC-V cross-compiler")
endif()

set(CMAKE_C_COMPILER ${RISCV_GCC})
set(CMAKE_ASM_COMPILER ${RISCV_GCC})
set(CMAKE_OBJCOPY ${RISCV_OBJCOPY})
set(CMAKE_OBJDUMP ${RISCV_OBJDUMP})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
