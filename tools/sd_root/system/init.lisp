;; LugalOS System Initialization Script (/sd0/system/init.lisp)

;; 1. Conditionally mount RAMDisk based on architecture capabilities
(if (= (arch) "rv32")
    (mount-ramdisk 64)       ;; 64 KB RAMDisk for RV32 NOMMU (RP2350 / Pico 2)
    (mount-ramdisk 512))     ;; 512 KB RAMDisk for RV64 MMU

;; 2. Clear command history file on boot (configurable)
(write-file "/sd0/system/history.lisp" "")

(display "[Init] LugalOS Lisp System Initialized successfully!\n")

;; 3. Launch interactive Lugal Console Shell
(lsh)
