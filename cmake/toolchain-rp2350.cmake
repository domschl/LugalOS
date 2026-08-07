set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(LUGALOS_TARGET "RP2350" CACHE STRING "LugalOS Target" FORCE)

# Cross compiler search order
find_program(RISCV_GCC NAMES riscv64-elf-gcc riscv-none-elf-gcc riscv32-elf-gcc riscv64-linux-gnu-gcc)
find_program(RISCV_OBJCOPY NAMES riscv64-elf-objcopy riscv-none-elf-objcopy riscv32-elf-objcopy riscv64-linux-gnu-objcopy)
find_program(RISCV_OBJDUMP NAMES riscv64-elf-objdump riscv-none-elf-objdump riscv32-elf-objdump riscv64-linux-gnu-objdump)

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
