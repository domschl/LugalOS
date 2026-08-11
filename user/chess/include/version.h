/*
 * Vendored verbatim from ~/gith/domschl/LugalChess (engine/include/version.h),
 * H4, plan/phase9_chess_computer.md. LUGALCHESS_EMBEDDED's guard --
 * `defined(__riscv) && !defined(__linux__) && !defined(__unix__) &&
 * !defined(__APPLE__) && !defined(_WIN32)` -- fires correctly on LugalOS's
 * bare-metal freestanding RISC-V build with no changes needed: the
 * cross-compiler predefines __riscv, and none of the hosted-OS macros are
 * ever defined on a -ffreestanding -nostdlib build. See H0's note in
 * plan/phase9_chess_computer.md.
 */

#ifndef VERSION_H
#define VERSION_H

#define LUGALCHESS_NAME "Lugal Chess"
#define LUGALCHESS_VERSION "0.1.0"

// Hardware platform preprocessor detection
#if defined(__riscv) && (__riscv_xlen == 32)
#  define LUGALCHESS_PLATFORM "RISCV32"
#elif defined(__riscv) && (__riscv_xlen == 64)
#  define LUGALCHESS_PLATFORM "RISCV64"
#elif defined(__riscv)
#  define LUGALCHESS_PLATFORM "RISC-V"
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define LUGALCHESS_PLATFORM "AArch64"
#elif defined(__arm__) || defined(_M_ARM) || defined(__ARM_ARCH)
#  define LUGALCHESS_PLATFORM "ARM32"
#elif defined(__x86_64__) || defined(_M_X64)
#  define LUGALCHESS_PLATFORM "x86_64"
#elif defined(__i386__) || defined(_M_IX86)
#  define LUGALCHESS_PLATFORM "x86"
#else
#  define LUGALCHESS_PLATFORM "Unknown"
#endif

// Embedded microcontroller target detection (RP2040 / RP2350 ARM32 or RISCV32 on Pico SDK)
#if defined(PICO_ON_DEVICE) || defined(PICO_BOARD) || (defined(__riscv) && !defined(__linux__) && !defined(__unix__) && !defined(__APPLE__) && !defined(_WIN32)) || (defined(__arm__) && !defined(__linux__) && !defined(__unix__) && !defined(__APPLE__) && !defined(_WIN32))
#  define LUGALCHESS_EMBEDDED 1
#endif

// 7-segment display platform string (max 8 digits for 8-segment TM1638 display)
#if defined(__riscv)
#  define LUGALCHESS_7SEG_PLATFORM "rISc0.1.0"
#elif defined(__arm__) || defined(_M_ARM) || defined(__ARM_ARCH)
#  define LUGALCHESS_7SEG_PLATFORM "ARM 0.1.0"
#else
#  define LUGALCHESS_7SEG_PLATFORM " 0.1.0  "
#endif

#endif // VERSION_H
