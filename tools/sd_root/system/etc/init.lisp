;; LugalOS System Initialization Script (<vol>/system/etc/init.lisp)

;; 1. Conditionally mount RAMDisk based on architecture capabilities
(if (= (arch) "rv32")
    (mount-ramdisk 64)       ;; 64 KB RAMDisk for RV32 NOMMU (RP2350 / Pico 2)
    (mount-ramdisk 512))     ;; 512 KB RAMDisk for RV64 MMU

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

;; 6. Launch interactive Lugal Console Shell
(lsh)
