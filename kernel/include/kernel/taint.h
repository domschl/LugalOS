#ifndef LUGALOS_KERNEL_TAINT_H
#define LUGALOS_KERNEL_TAINT_H

/* The node's taint (45.9, plan/phase45_esp32c6.md §2): a node that links code it cannot audit says
 * so, as Linux does in /proc/sys/kernel/tainted. Set at build time by what is linked -- CONFIG_TAINT_BLOB
 * comes from the build that links Espressif's Wi-Fi libraries (cmake/radio_esp32c6.cmake via
 * CMakeLists.txt) and from nowhere else -- so it cannot be wrong at run time.
 *
 * The label is a statement about trust, not about containment: the blob runs in a confined U-mode
 * domain, which keeps a fault in it from corrupting the kernel, but the radio's MAC is a bus master and
 * PMP governs only the CPU. A peer is entitled to weigh it.
 *
 * Machine-readable on purpose: /proc/node always carries `taint: <list>` ("none" when clean), the boot
 * banner and /proc/version append ` tainted: <list>` only when there is something to say. */
#if defined(CONFIG_TAINT_BLOB)
#define LUGALOS_TAINT "blob(espressif-wifi)"
#define LUGALOS_TAINTED 1
#else
#define LUGALOS_TAINT "none"
#define LUGALOS_TAINTED 0
#endif

#endif
