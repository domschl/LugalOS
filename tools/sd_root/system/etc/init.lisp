;; LugalOS System Initialization Script (<vol>/system/etc/init.lisp)

;; 1. Mount a RAM disk -- but only where one is actually needed.
;;
;;    Its storage comes from the page allocator now (C5), not from a fixed
;;    array in the kernel image, so this decision is about how much of the heap
;;    /ram0 costs rather than about a compile-time constant.
;;
;;    On RP2350 that cost is sharp in a way that is worth spelling out. The
;;    heap is 160 KB and starts at 0x20058000; a PMP region must be a
;;    power of two and self-aligned, so the only 128 KB-aligned address inside
;;    the heap is 0x20060000. A 64 KB RAM disk allocated first-fit lands across
;;    exactly that boundary -- so it does not merely consume pages, it halves
;;    the largest user image the loader can ever place. If /sd0 is there, it is
;;    somewhere to put scratch files that costs no RAM at all, and skipping the
;;    RAM disk is straightforwardly better.
;;
;;    And when there is no card, 64 KB rather than something smaller: this
;;    FAT32 layout spends 48 of its sectors on reserved space and two FATs, so
;;    a 32 KB volume is more than two-thirds metadata and a 16 KB one is
;;    smaller than its own bookkeeping. There is no good small size, which is
;;    what makes "none at all" the right answer when there is an alternative.
;;
;;    (board) rather than (arch): RP2350 and QEMU's virt board are both "rv32",
;;    but one has 512 KB of SRAM and the other 128 MB.
(if (= (board) "rp2350")
    (if (mounted? "/sd0")
        (display "[Init] /sd0 is writable; no RAM disk needed\n")
        (mount-ramdisk 64))
    (if (= (arch) "rv32")
        (mount-ramdisk 64)
        (mount-ramdisk 512)))

;; 2. Command search path (C1). Typing a bare name at the shell runs
;;    /<vol>/system/bin/<name>.elf from the first volume in this list that
;;    has it; a path containing a '/' is always taken literally. Read it back
;;    with `cat /proc/path`, and ask where a name resolves with (which "x").
;;
;;    The default is most-volatile-first, so a utility written to /ram0 wins
;;    over the one shipped in flash -- which is what makes a freshly compiled
;;    program usable by name straight away. It also means anything that can
;;    write /ram0/system/bin can shadow every system utility, so this line is
;;    where a board with different trust requirements says otherwise.
(path-set "ram0 sd0 flash0")

;; 3. Clear command history file on boot (configurable)
(write-file "/sd0/system/history.lisp" "")

;; 4. Kernel log and device policy (B0). Both registries are inspectable...
;;      (devices)      -- what hardware this board probed; also /proc/devices
;;      (klog-sinks)   -- where kernel log output currently goes
;;    ...and bindable here, at boot, rather than compiled into kernel_main():
;;      (klog-detach "console")  -- stop kernel log reaching this terminal.
;;                                  Nothing is lost: the ring keeps recording,
;;                                  so `cat /proc/kmsg` still has it, and so
;;                                  does a remote node reading that file.
;;      (p9-serve "uartslip")    -- serve 9P on a link named in the registry
;;      (mount-remote "peer" "usbnet")  -- mount a peer over a named link
;;    Dedicated links (vconsole on QEMU, usbnet/ACM1 on RP2350) are already
;;    served from boot via DEV_F_BACKGROUND_9P, so nothing is required here
;;    by default. Use (dev-present? "name") to branch on what this board has
;;    instead of on which target it was built for.

;; 5. Console ownership (B4). The kernel log and the interactive console are
;;    now separate streams, so they can be routed independently:
;;      (console-device)          -- which device owns the terminal
;;      (console-bind "usb")      -- hand the terminal to another device
;;      (klog-detach "console")   -- stop kernel diagnostics reaching it,
;;                                   without silencing the shell
;;    The console is also a service: anything that can write to /srv/console
;;    can emit on it, including a remote node over 9P. So "this UART carries
;;    kernel messages until a login shell takes it over" is a decision made
;;    here, at boot, rather than one compiled into the kernel.

(display "[Init] LugalOS Lisp System Initialized successfully!\n")

;; 6. Launch interactive Lugal Console Shell -- unless something more
;;    specific applies, checked in priority order:
;;
;;    a) /sd0/system/etc/usr_init.lisp, if present (found by request,
;;       2026-08-12). A live, runtime override with no rebuild/reflash
;;       needed -- what a *general-purpose* program like chess uses to
;;       become a board's boot persona (e.g. straight into (chess)),
;;       since the same chess build is just as often run interactively on
;;       a non-dedicated board, where auto-starting it would be wrong.
;;       Checked with (read-file ...) first rather than calling (load ...)
;;       directly, since load prints "cannot open file" on a miss --
;;       expected and silent here on every board that doesn't have one,
;;       not an error. Expected to end with whatever persona it wants --
;;       (chess), (lsh), or anything else -- since nothing runs after
;;       this line either way.
;;
;;    b) (boot-program), a *build-time* fact (L4,
;;       plan/phase11_pico_clock_green.md) for hardware that has no
;;       other purpose to opt out of -- the clock persona has no SD card
;;       to put a usr_init.lisp on even if it wanted one, and unlike
;;       chess there is no "interactive, non-dedicated" use of this
;;       board to preserve.
;;
;;    c) Otherwise, the interactive shell.
;;    (clock) is followed by (lsh) rather than being the true last form the
;;    way usr_init.lisp's own choice of persona is (comment (a) above) --
;;    an appliance with no SD card to set the time from needs some way
;;    back to a shell after Ctrl-C, to run `date`. Harmless for every
;;    other persona too: (boot-program) is "" there, so this is just
;;    (lsh) alone, same as always.
(if (not (= (read-file "/sd0/system/etc/usr_init.lisp") ""))
    (load "/sd0/system/etc/usr_init.lisp")
    (if (= (boot-program) "clock")
        (begin (clock) (lsh))
        (lsh)))
