/* Linker script for LugalOS on the ESP32-C6 (Waveshare ESP32-C6-Zero) --
 * 45.4, plan/phase45_esp32c6.md.
 *
 * ## Why the kernel executes from flash
 *
 * The first form of this script put everything in the C6's 512 KB of SRAM. It
 * did not link: the image is ~540 KB against 496 KB usable, because the kernel
 * has grown since the P4's first boot (net stack, MQTT, NTP, 9P, Lisp, the
 * kernel objects) and has no heap left once it is resident. So .text and .rodata
 * live in flash and are fetched through the cache at 0x42000000 (45.4.2's probe
 * proved the mapping on this silicon), exactly the P4's phase-32 shape, and SRAM
 * holds what must be there: the boot code, .data, .bss, the stack and the heap.
 *
 * ## What is not ours
 *
 * The top 16 KB of SRAM, 0x4087c000..0x40880000, is the *ROM's* working data. The
 * download-mode loader lives in it while we are loaded and -- the reason it stays
 * out of the heap for good -- the ROM's Wi-Fi code keeps its state there (g_osi_
 * funcs_p at 0x4087ff6c; plan §4.5). The usable SRAM therefore ends at 0x4087c000.
 *
 * ## Two images from one ELF
 *
 * The XIP half (.text, .rodata) is extracted raw and written at the OS-image
 * flash address; the RAM half (everything else) is stripped of those sections and
 * becomes the second-stage image the ROM loads (esptool elf2image). See
 * CMakeLists.txt.
 */

OUTPUT_FORMAT(elf32-littleriscv)

MEMORY
{
    /* The flash window: 256 MMU entries of 64 KB (16 MB of virtual space). */
    XIP (rx)  : ORIGIN = 0x42000000, LENGTH = 16M
    RAM (rwx) : ORIGIN = 0x40800000, LENGTH = 496K
}

ENTRY(_start)

SECTIONS
{
    /* --- the RAM-resident boot code ----------------------------------------
     * Everything that runs before the flash is mapped: entry.S, which the ROM
     * jumps to, the trap vector table (which the hardware wants 256-byte aligned
     * and which the first exception must find without flash), and the mapping
     * functions. First in RAM, so _start is at ORIGIN(RAM). */
    .boot : {
        . = ALIGN(4);
        _boot_start = .;
        *(.text.entry)
        *(.boot.text .boot.text.*)
        . = ALIGN(4);
        _boot_end = .;
    } > RAM

    /* Code that must not be fetched from flash while flash is busy (writes, 45.4.3). */
    .ramfunc : ALIGN(4) {
        _ramfunc_start = .;
        *(.ramfunc .ramfunc.*)
        . = ALIGN(4);
        _ramfunc_end = .;
    } > RAM

    /* --- the flash-resident half --------------------------------------------- */
    .text : {
        _ktext_lo = .;
        . = ALIGN(4);
        _xip_start = .;
        *(.text .text.*)
        . = ALIGN(4);
        _ktext_hi = .;
    } > XIP

    /* U-mode-executable code, one page, power-of-two-sized and self-aligned so one
     * PMP NAPOT entry grants it (kernel/board.c's board_text_region()). In RAM, not
     * flash: U-mode code keeps running when the cache is off for a flash write. */
    .utext : ALIGN(4096) {
        _utext_start = .;
        *(.utext .utext.*)
        . = _utext_start + 4096;
    } > RAM

    .rodata : {
        . = ALIGN(4);
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        . = ALIGN(4);
        _xip_end = .;
    } > XIP

    /* How many 64 KB MMU pages esp32c6_xip_map() has to map. */
    _xip_pages = (_xip_end - _xip_start + 0xFFFF) >> 16;

    /* No AT> anywhere: LOADADDR(.data) == ADDR(.data), so _sidata == _data_start and
     * entry.S skips its copy loop -- the loader (load-ram, and the ROM for a flash
     * boot) places .data at its final address itself. */
    _sidata = LOADADDR(.data);

    .data : {
        . = ALIGN(4);
        _data_start = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        . = ALIGN(4);
        _data_end = .;
    } > RAM

    .ustacks512 (NOLOAD) : ALIGN(512) {
        _ustacks_start = .;
        *(.ustacks512)
        . = ALIGN(512);
        _ustacks_end = .;
    } > RAM

    .bss (NOLOAD) : {
        . = ALIGN(4);
        _bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > RAM

    /* 16 KB boot stack, the RP2350/P4 figure. */
    .stack (NOLOAD) : {
        . = ALIGN(16);
        _stack_bottom = .;
        . += 16k;
        _stack_top = .;
    } > RAM

    . = ALIGN(4096);
    _kernel_end = .;
    _heap_end = ORIGIN(RAM) + LENGTH(RAM);

    _ram_start = ORIGIN(RAM);
    _ram_end   = ORIGIN(RAM) + LENGTH(RAM);
    _text_start = ADDR(.text);

    /* The image is NOT one contiguous run from the bottom of RAM (as on the P4):
     * the RAM half and the flash half are added, which is what /proc/meminfo means
     * by "image". */
    _image_bytes = (_bss_end - _ram_start) + (_xip_end - _xip_start);
    _inst_lo     = _xip_start;
    _inst_hi     = _ram_end;

    PROVIDE(task_trampoline = _ktext_lo);
    PROVIDE(sched_yield     = _ktext_lo);
    PROVIDE(task_exit       = _ktext_lo);
    ASSERT(task_trampoline >= _ktext_lo && task_trampoline < _ktext_hi,
           "task_trampoline outside .text: sched_check_incoming()'s ra window is wrong")
    ASSERT(sched_yield >= _ktext_lo && sched_yield < _ktext_hi,
           "sched_yield outside .text: sched_check_incoming()'s ra window is wrong")
    ASSERT(task_exit >= _ktext_lo && task_exit < _ktext_hi,
           "task_exit outside .text: sched_check_incoming()'s ra window is wrong")

    /* The ROM's data at the top of SRAM is not ours. */
    ASSERT(_heap_end <= 0x4087c000,
           "the heap reaches into the ROM's working data at 0x4087c000 -- see the header comment")

    /* The flash half must fit the OS-image partition (cmake/flash_layout_esp32c6.cmake). */
    ASSERT(_xip_end - _xip_start <= 0x200000,
           "the OS image has outgrown its 2 MB flash partition -- see cmake/flash_layout_esp32c6.cmake")

    /* The heap floor (phase 15 §6): 256 KB, the RP2350's figure, with the 16 KB of
     * ROM data already excluded from the region. */
    ASSERT(_heap_end - _kernel_end >= 192K,
           "ESP32-C6 heap has fallen below its 192 KB floor -- something grew .bss or .data; see the header comment")
}
