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

    /* --- the radio's text: first in the flash window, one 512 KB block ---------
     * Espressif's Wi-Fi/PHY/coexistence archives and the radio's own open code
     * (45.6): everything the radio's U-mode domain executes or reads from flash.
     * 512 KB, self-aligned at the window's start, so one PMP NAPOT entry grants it
     * (the blob is ~372 KB). The kernel's own text follows at +512 KB. The marker
     * keeps the section alive in a build without the blob, so the image layout --
     * and the flash offset of everything after it -- is the same either way.
     *
     * Declared before .text so that its patterns win the input sections: ld gives an
     * input section to the first statement that matches it. */
    .radio_text ORIGIN(XIP) : {
        _xip_start = .;
        _radio_text_start = .;
        KEEP(*(.radio_text_marker))
        *libnet80211.a:*(.text .text.* .rodata .rodata.* .srodata .srodata.* .rodata_wlog* .iram1 .iram1.* .wifi*iram* .coexiram*)
        *libpp.a:*(.text .text.* .rodata .rodata.* .srodata .srodata.* .rodata_wlog* .iram1 .iram1.* .wifi*iram* .coexiram*)
        *libcore.a:*(.text .text.* .rodata .rodata.* .srodata .srodata.* .rodata_wlog* .iram1 .iram1.* .wifi*iram* .coexiram*)
        *libmesh.a:*(.text .text.* .rodata .rodata.* .srodata .srodata.* .rodata_wlog* .iram1 .iram1.* .wifi*iram* .coexiram*)
        *libespnow.a:*(.text .text.* .rodata .rodata.* .srodata .srodata.* .rodata_wlog* .iram1 .iram1.* .wifi*iram* .coexiram*)
        *libphy.a:*(.text .text.* .rodata .rodata.* .srodata .srodata.* .rodata_wlog* .iram1 .iram1.* .wifi*iram* .coexiram*)
        *libcoexist.a:*(.text .text.* .rodata .rodata.* .srodata .srodata.* .rodata_wlog* .iram1 .iram1.* .wifi*iram* .coexiram*)
        
        *radio_main.c.obj(.text .text.* .rodata .rodata.* .srodata .srodata.*)
        *esp32c6_osi_table.c.obj(.text .text.* .rodata .rodata.* .srodata .srodata.*)
        *plat_esp32c6.c.obj(.text .text.* .rodata .rodata.* .srodata .srodata.*)
        *esp_wifi_regulatory.c.obj(.text .text.* .rodata .rodata.* .srodata .srodata.*)
        *ftm_load_calibration.c.obj(.text .text.* .rodata .rodata.* .srodata .srodata.*)
        . = ALIGN(4);
        _radio_text_end = .;
    } > XIP
    ASSERT(_radio_text_end - _radio_text_start <= 512K,
           "the radio's text has outgrown its 512 KB PMP region (kernel/radio_c6.c)")

    /* --- the flash-resident half --------------------------------------------- */
    .text ORIGIN(XIP) + 512K : {
        _ktext_lo = .;
        . = ALIGN(4);
        *(EXCLUDE_FILE(*osi_impl.c.obj *uprintf.c.obj *nvs_ram.c.obj *uheap.c.obj *radio_libc.c.obj) .text EXCLUDE_FILE(*osi_impl.c.obj *uprintf.c.obj *nvs_ram.c.obj *uheap.c.obj *radio_libc.c.obj) .text.*)
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
        *(EXCLUDE_FILE(*osi_impl.c.obj *uprintf.c.obj *nvs_ram.c.obj *uheap.c.obj *radio_libc.c.obj) .rodata EXCLUDE_FILE(*osi_impl.c.obj *uprintf.c.obj *nvs_ram.c.obj *uheap.c.obj *radio_libc.c.obj) .rodata.*)
        *(EXCLUDE_FILE(*osi_impl.c.obj *uprintf.c.obj *nvs_ram.c.obj *uheap.c.obj *radio_libc.c.obj) .srodata EXCLUDE_FILE(*osi_impl.c.obj *uprintf.c.obj *nvs_ram.c.obj *uheap.c.obj *radio_libc.c.obj) .srodata.*)
        . = ALIGN(4);
        _xip_end = .;
    } > XIP

    /* How many 64 KB MMU pages esp32c6_xip_map() has to map. */
    _xip_pages = (_xip_end - _xip_start + 0xFFFF) >> 16;

    /* 45.3b/45.4.4: the radio shim and its tests, as on the QEMU targets
     * (linker/qemu-rv32.ld says why each piece is where it is). On the C6 the
     * final radio keeps its *text* in the flash window; in this form -- the
     * kernel-object suite and the OS-table shim exercised on the silicon -- it is
     * linked into RAM like the QEMU build, so the same tests run unchanged. One
     * power-of-two, self-aligned block each, so each grant is a single PMP region:
     * 16 KB of code and read-only data, 32 KB of state and heap. */
    .utext_kobj : ALIGN(16384) {
        _utext_kobj_start = .;
        *(.utext_kobj .utext_kobj.*)
        *(.utext_kobj_ro .utext_kobj_ro.*)
        *osi_impl.c.obj(.text .text.* .rodata .rodata.* .srodata .srodata.*)
        *uprintf.c.obj(.text .text.* .rodata .rodata.* .srodata .srodata.*)
        *nvs_ram.c.obj(.text .text.* .rodata .rodata.* .srodata .srodata.*)
        *uheap.c.obj(.text .text.* .rodata .rodata.* .srodata .srodata.*)
        *radio_libc.c.obj(.text .text.* .rodata .rodata.* .srodata .srodata.*)
        . = _utext_kobj_start + 16384;
    } > RAM

    .udata_kobj (NOLOAD) : ALIGN(32768) {
        _udata_kobj_start = .;
        *(.udata_kobj .udata_kobj.*)
        . = _udata_kobj_start + 32768;
    } > RAM

    /* The radio's writable state: the blob's .data and .bss (about 20 KB) and the
     * radio's own, in one self-aligned 32 KB block so one PMP entry grants it.
     * Initialised data and zeroed bss are both *in the image* (the .bss part is a run
     * of zeros the loader writes), because the kernel's startup clears only its own
     * .bss. */
    .radio_data : ALIGN(32768) {
        _radio_data_start = .;
        *libnet80211.a:*(.data .data.* .sdata .sdata.* .dram1 .dram1.*)
        *libpp.a:*(.data .data.* .sdata .sdata.* .dram1 .dram1.*)
        *libcore.a:*(.data .data.* .sdata .sdata.* .dram1 .dram1.*)
        *libmesh.a:*(.data .data.* .sdata .sdata.* .dram1 .dram1.*)
        *libespnow.a:*(.data .data.* .sdata .sdata.* .dram1 .dram1.*)
        *libphy.a:*(.data .data.* .sdata .sdata.* .dram1 .dram1.*)
        *libcoexist.a:*(.data .data.* .sdata .sdata.* .dram1 .dram1.*)
        *radio_main.c.obj(.data .data.* .sdata .sdata.*)
        *esp32c6_osi_table.c.obj(.data .data.* .sdata .sdata.*)
        *plat_esp32c6.c.obj(.data .data.* .sdata .sdata.*)
        *esp_wifi_regulatory.c.obj(.data .data.* .sdata .sdata.*)
        *ftm_load_calibration.c.obj(.data .data.* .sdata .sdata.*)
        *radio_libc.c.obj(.data .data.* .sdata .sdata.*)
        . = ALIGN(4);
    } > RAM
    /* Zeroed by the kernel before every start of the radio (kernel/radio_c6.c): it is
     * not in the image, and the kernel's startup clears only its own .bss. */
    .radio_bss (NOLOAD) : {
        _radio_bss_start = .;
        *libnet80211.a:*(.bss .bss.* .sbss .sbss.* COMMON)
        *libpp.a:*(.bss .bss.* .sbss .sbss.* COMMON)
        *libcore.a:*(.bss .bss.* .sbss .sbss.* COMMON)
        *libmesh.a:*(.bss .bss.* .sbss .sbss.* COMMON)
        *libespnow.a:*(.bss .bss.* .sbss .sbss.* COMMON)
        *libphy.a:*(.bss .bss.* .sbss .sbss.* COMMON)
        *libcoexist.a:*(.bss .bss.* .sbss .sbss.* COMMON)
        *radio_main.c.obj(.bss .bss.* .sbss .sbss.* COMMON)
        *esp32c6_osi_table.c.obj(.bss .bss.* .sbss .sbss.* COMMON)
        *plat_esp32c6.c.obj(.bss .bss.* .sbss .sbss.* COMMON)
        *esp_wifi_regulatory.c.obj(.bss .bss.* .sbss .sbss.* COMMON)
        *ftm_load_calibration.c.obj(.bss .bss.* .sbss .sbss.* COMMON)
        *radio_libc.c.obj(.bss .bss.* .sbss .sbss.* COMMON)
        . = ALIGN(4);
        _radio_bss_end = .;
        . = _radio_data_start + 32768;
    } > RAM
    ASSERT(_radio_bss_end <= _radio_data_start + 32768,
           "the radio's data has outgrown its 32 KB PMP region (kernel/radio_c6.c)")

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
    ASSERT(_heap_end - _kernel_end >= 128K,
           "ESP32-C6 heap has fallen below its 128 KB floor -- something grew .bss or .data; see the header comment")
}
