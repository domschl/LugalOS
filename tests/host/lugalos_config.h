/* The host harnesses' stand-in for the generated per-board config: only what
 * the modules they build read. A QEMU RV64 board, which is what the host's
 * LP64 type sizes match. */
#ifndef LUGALOS_CONFIG_H
#define LUGALOS_CONFIG_H
#define CONFIG_PALLOC_MAX_PAGES 4096
#define CONFIG_PALLOC_BULK_PAGES 512
#define CONFIG_ENABLE_CC 1
#define CONFIG_NODE_PERSONA "host"
#endif
