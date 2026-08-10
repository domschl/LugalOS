#ifndef LUGALOS_ARCH_ELF_H
#define LUGALOS_ARCH_ELF_H

#include <stdint.h>
#include <stddef.h>

#define ELF_MAGIC "\x7f" "ELF"
#define EM_RISCV 0xF3
#define PT_LOAD  1

#pragma pack(push, 1)
typedef struct {
    unsigned char e_ident[16];
    uint16_t      e_type;
    uint16_t      e_machine;
    uint32_t      e_version;
    uint32_t      e_entry;
    uint32_t      e_phoff;
    uint32_t      e_shoff;
    uint32_t      e_flags;
    uint16_t      e_ehsize;
    uint16_t      e_phentsize;
    uint16_t      e_phnum;
    uint16_t      e_shentsize;
    uint16_t      e_shnum;
    uint16_t      e_shstrndx;
} elf32_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} elf32_phdr_t;

typedef struct {
    unsigned char e_ident[16];
    uint16_t      e_type;
    uint16_t      e_machine;
    uint32_t      e_version;
    uint64_t      e_entry;
    uint64_t      e_phoff;
    uint64_t      e_shoff;
    uint32_t      e_flags;
    uint16_t      e_ehsize;
    uint16_t      e_phentsize;
    uint16_t      e_phnum;
    uint16_t      e_shentsize;
    uint16_t      e_shnum;
    uint16_t      e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;
#pragma pack(pop)

/* Segment permission bits (ELF p_flags). Used to decide what a loaded
 * segment is granted: executable segments get R|X, everything else R|W, so
 * W^X follows from what the linker declared rather than from the loader
 * assuming a layout (C4). */
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/* The most arguments a program can be given. Exposed so a caller sizing its
 * own argv array agrees with the loader rather than guessing (C3). */
#define USER_ARGV_LIMIT 8

int elf_load_and_run(const char *path);

/* Loads and starts `path` without waiting for it, returning its pid or -1
 * (C2, plan/phase6_memory_and_processes.md).
 *
 * This is what makes "more than one user program at a time" observable rather
 * than merely structural: elf_load_and_run() spins until its program is dead,
 * so however many slots exist, a caller using it can only ever have one.
 *
 * The program's pages are returned at the next load once its task has ended --
 * nothing schedules on a user image, so there is no natural moment earlier
 * than that. See the reaping note in arch/riscv/common/elf.c. */
/* Pages currently held by resident user images, and how many of those the
 * programs actually span (C4).
 *
 * The difference is the NAPOT rounding loss: a region must be a power of two,
 * so a program spanning five pages is given eight. Reported in /proc/meminfo
 * because on a 40-page heap that is a cost worth being able to see rather than
 * infer -- it is the difference between "the heap is full" and "the heap is
 * full of padding". */
void elf_image_stats(uint32_t *alloc_pages, uint32_t *used_pages);

int elf_spawn(const char *path);

/* As above, with arguments. `argv[0]` is the program's own name by
 * convention; the loader does not supply one, so a caller that wants the
 * convention passes it. The strings are copied into the program's own stack
 * page before it starts, so the caller's buffers need not outlive this call
 * (C3). */
int elf_spawn_argv(const char *path, int argc, const char *const *argv);
int elf_load_and_run_argv(const char *path, int argc, const char *const *argv);

#endif /* LUGALOS_ARCH_ELF_H */
