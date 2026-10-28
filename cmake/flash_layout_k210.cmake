# The K210 / Maix Bit flash map (65.5), the flash_layout_esp32p4.cmake job.
#
# One definition, two consumers: the C code (compile definitions below)
# and tools/k210flash.py (which reads this file's values through the
# build's generated header), so the writer and the kernel cannot
# disagree about where anything lives.
#
# ## What the part is
#
# GD25LQ128, 16 MB, JEDEC C8 60 18, measured on the board
# (tools/spi_nor_probe_k210.c). The same pad set is what the K210 BROM
# reads at reset, which fixes the one address nothing can move: offset
# 0. BROM's flash boot consumes [flag:1][length:4][image][sha256:32]
# there — the framing kflash's ISP writes, which is how the format is
# known good — and then jumps to the image at 0x80000000. Everything
# else in this map is ours.
#
# ## The map
#
#   0x00000000  BROM boot image: the LugalOS loader              64 KB
#   0x00010000  boot record A                                     4 KB
#   0x00011000  boot record B                                     4 KB
#   0x00012000  reserved (alignment slack below slot A)           56 KB
#   0x00020000  slot A: kernel image (ota header + load blob)     2 MB
#   0x00220000  slot B                                            2 MB
#   0x00420000  /flash0 FAT32 image                               2 MB
#   0x00620000  spare                                             ~9.9 MB
#   0x00FFF000  identity sector                                   4 KB
#
# Every boundary is at least sector-aligned (the erase unit), and every
# *growing* segment is 64 KB-aligned so it can eat the spare with a
# 64 KB block erase and no neighbour damage. Slot images are 2 MB each
# against a stripped kernel well under 1.5 MB — the slot is the unit a
# bad update can waste, so it gets the generous-but-not-wasteful P4
# sizing, and 64 KB alignment is load-bearing here in a way the P4's
# MMU imposes on its own: the loader copies a slot into SRAM in chunks
# and the copy loop is simpler when nothing straddles a 64 KB block.
#
# The loader gets 64 KB and sits at offset 0 because the BROM reads
# there and nowhere else; its own framing (sha256 trailer) lives inside
# that window. The boot records follow immediately so a provisioning
# tool erases 0..0x11fff as one 72 KB burst and nothing between the
# records and the loader can drift.
#
# The identity sector is the top sector — the same policy as every
# board: it is the last thing any growth from below reaches, it is
# never written by a flashing tool, and reflashing the world leaves a
# provisioned device provisioned.

set(LUGALOS_K210_FLASH_SIZE       0x01000000)   # 16 MB, GD25LQ128

set(LUGALOS_K210_LOADER_BASE      0x00000000)
set(LUGALOS_K210_LOADER_SIZE      0x00010000)   # 64 KB, BROM framing inside

set(LUGALOS_K210_BOOTREC_BASE     0x00010000)   # two sectors, ota_bootrec_t
set(LUGALOS_K210_BOOTREC_SIZE     0x00001000)

set(LUGALOS_K210_SLOT_A_BASE      0x00020000)
set(LUGALOS_K210_SLOT_B_BASE      0x00220000)
set(LUGALOS_K210_SLOT_SIZE        0x00200000)   # 2 MB each

set(LUGALOS_K210_FLASHFS_BASE     0x00420000)
set(LUGALOS_K210_FLASHFS_SIZE     0x00200000)   # 2 MB — must match the FAT32 image

# The floor every runtime write is checked against: below this lies the
# loader and the boot machinery, which only a deliberate host reflash
# (or the loader's own boot-record word clears) may touch.
set(LUGALOS_K210_FLASH_WRITABLE_FLOOR 0x00020000)

set(LUGALOS_K210_IDENTITY_BASE    0x00FFF000)   # last sector
