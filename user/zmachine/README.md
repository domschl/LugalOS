# zmachine — Z-machine v3 Interpreter for LugalOS

Pure C implementation of a Z-machine (Version 3) interpreter, developed
against **Zork I: The Great Underground Empire**, runnable on LugalOS
targets (RP2350, ESP32-P4) and on the host test harness.

## Source & Provenance — the reproduction chain

The game we ship is **built from MIT-licensed sources that are part of
this repository** — no opaque binaries, no download scripts needed for
shippable content:

```
src (MIT ZIL, in-tree)          games/zork1/src/*.zil
  │                              pinned: historicalsource/zork1 @ 97b7b3d6
  ▼                              license: MIT (Microsoft release 2025-11-20)
ZILF + ZAPF                     pkgbuild/zilf-hg/ (fixed AUR PKGBUILD)
  │  zilf zork1.zil … --asm-options "-r=1,-s=112025"
  ▼
games/zork1/zork1-mit.z3        v3, Release 1, Serial 112025
  │  sha256 e7e789a6…e6ca (byte-reproducible, recorded in README)
  ▼
zork-host / LugalOS firmware    this interpreter
```

On 2025-11-20 Microsoft open-sourced the Zork I–III sources under the
MIT License
([announcement](https://opensource.microsoft.com/blog/2025/11/20/preserving-code-that-shaped-generations-zork-i-ii-and-iii-go-open-source/)).
The sources — including all game prose — live in `games/zork1/src/`
with their `LICENSE`; the compiled output is MIT-derived and therefore
commit- and shippable (trademarks remain excluded — hence the
`zork1-mit` naming and no Infocom branding).

**Test oracle only:** the retail r119/880429 binary (sha256
`37084966…b64d79`) can be fetched with `./fetch-games.sh` into
`z-data/` (git-ignored) for differential testing against frotz and
`zork-host`.  It is never shipped.  Note the released source does not
rebuild byte-identically to any retail binary; oracle comparisons are
prose/behavior-level.

## Design Notes

* Z-machine v3: flat 128K byte address space, 61 opcodes, pure
  16-bit integer arithmetic (no floating point — compliant with the
  LugalOS kernel no-FPU rule).
* Freestanding-friendly core: no libc dependency; I/O via
  `getc`/`putc` callbacks so the same sources build for RV32
  firmware and for the host ASan test harness.
* Story file may stay in Flash XIP or be loaded to RAM; save/restore
  dumps dynamic RAM to `/sd0` / `/flash0`.

## Layout

| File        | Purpose                                             |
| ----------- | --------------------------------------------------- |
| `zvm.c`     | Opcode dispatch, stack, branches, calls             |
| `zmem.c`    | Memory model, byte/word access, header, checksum    |
| `ztext.c`   | Z-character decoding, abbreviations, print          |
| `zparse.c`  | `aread`: line input + dictionary tokenize           |
| `zsave.c`   | Save/restore state serialization (LZS3 format)      |
| `host_main.c` | Host test harness (the only libc-dependent file)  |
| `games/`      | MIT-source games: `src/`, `build.sh`, committed `.z3` (see `games/README.md`) |
| `pkgbuild/`   | Fixed AUR PKGBUILD for the ZILF/ZAPF toolchain        |
| `zfront.c/h`  | LugalOS kernel front (lsh/Lisp command, bulk memory)  |
| `z-data/`     | Fetched retail binaries — test oracles only, git-ignored |

## Reference Documents — EXTERNAL

The specs are deliberately **not** stored in the LugalOS repository.
All reference material is maintained in the independent reference
repository `~/gith/zmachine/` (or `~/Source/gith/zmachine`):

| Document | Location | sha256 |
| -------- | -------- | ------ |
| `zmach06e.txt` — "The Z Machine" v0.6 (O'Rear/Graham, 1989); era-correct v3 encodings (§7.2), call/object/verify semantics | `gith/zmachine/docs/` | `0db47763…3827a` |
| `z-spec10.pdf` — Standard of the Z-Machine 1.0 (Andrew Plotkin) | `gith/zmachine/docs/` | `fab17f79…fce6d` |
| `spec-zip.pdf` — low-level instruction-format cross-check | `gith/zmachine/docs/` | `a3b6fd1e…452821` |
| `frotz-2.55` — oracle interpreter, patched with `FROTZ_TRACE` PC trace in `interpret()` | `gith/zmachine/frotz-2.55/` | (from ifarchive `interpreters/frotz/frotz-2.55.tar.gz`) |
| `zork_scheme/` — full MIT ZIL source of Zork I | `gith/zmachine/zork_scheme/` | (from historicalsource/zork1) |

Citations like "zmach06e §7.2" or "z-spec10 §11" in the source
comments refer to these documents.

## Host Development Build

```sh
make -f Makefile run-mit # ASan/UBSan build, play the MIT-built
                         # games/zork1/zork1-mit.z3 (committed)
make -f Makefile run     # play with the fetched retail r119 oracle
./games/zork1/build.sh   # rebuild zork1-mit.z3 from src/ (needs ZILF)
```

Only `host_main.c` uses libc; the interpreter core is freestanding C
so the same sources build for RV32 firmware.

## LugalOS Build Integration

* **Kernel sources:** the `LUGALOS_ENABLE_ZMACHINE` option (default ON)
  adds the freestanding core (`zmem.c`, `zvm.c`, `ztext.c`, `zparse.c`,
  `zsave.c`) plus the kernel front `zfront.c` to `lugalos.elf` on every
  target.  `host_main.c` is never part of a firmware build.
* **Commands:** `zmachine` at the `lsh` prompt, or `(zmachine "name")`
  in Lisp.  A bare name resolves against `/sd0/games/<name>.z3`
  (default `zork1`); an explicit path works too.  `QUIT` or Ctrl-C
  (polled between instructions, the chess J2 mechanism) ends the
  session.  `save`/`restore` write `<story>.lzs` next to the story file.
* **Memory contract (chess's discipline, plan/phase38_psram.md):**
  idle cost is **zero** — nothing in `.bss`, nothing allocated at boot
  (verified: `-DLUGALOS_ENABLE_ZMACHINE=OFF` differs only in `.text`,
  `bss` is byte-identical).  While a game runs, the 128 KB address
  space, the resident story image (for `RESTART` without re-reading
  the SD), and the ~52 KB VM struct all come from
  `palloc_pages_bulk()` — PSRAM on boards that have it, with the SRAM
  fallback counted in `/proc/meminfo` — and every block is released on
  every exit path (verified on QEMU: bulk returns to its idle free
  count after `Ctrl-C`, `SRAM fallbacks 0`).
* **SD layout:** the top-level CMake stages `games/zork1/zork1-mit.z3`
  to `/sd0/games/` under the plain-8.3 name **`ZORK1.Z3`** (the volume
  carries no long-filename entries).  It lands in `flashfs.bin` /
  `flashfs.uf2` (RP2350 flashfs segment), in `lugalos_sd.img` (QEMU),
  and in `build/<preset>/sd_root/` for the `tools/p9sync.py` card-
  update flow.  Story-file edits are build dependencies: they re-run
  the flash-image rule.
* **Verified:** rp2350-terminal and rv32 presets compile the core
  clean; the full `tests/runner.py` suite passes with the new `/sd0`
  directory (membership-style assertions only).

## Status

* **M1 — memory & header model: DONE.** `zmem.c` loads `zork1.z3`,
  validates the v3 header, verifies the checksum ($bf44 OK), and
  resolves the geometry: initial PC `$50d5`, dictionary `$3899`,
  objects `$3e6`, globals `$2b0`, dynamic end `$2c12`, abbrev table
  `$1f0` (all raw byte addresses).
* **M2 — `zvm.c`: DONE.** Full Version 3 instruction set (61 opcodes),
  long/short/variable operand forms, routine stack with v3 frame
  discipline (locals from routine header words, accumulator = value
  stack: store→0 pushes, reading `$00` pops), branches (offset-0/1
  return semantics), object tree, property tables, 16-bit arithmetic
  with spec edge cases (`test` subset semantics, `je` single-operand
  never branches, `verify` branches when checksum MATCHES).
  **Verified instruction-by-instruction against frotz 2.55** (patched
  with a PC trace): the first 407 instructions — game boot through the
  banner to the first `aread` prompt — match frotz's trace exactly.
* **M3 — `ztext.c`: DONE.** Z-character decoding (A0/A1/A2 + shift +
  extended 10-bit ZSCII), abbreviation table (header `$18` as a raw
  byte address; entries are word addresses), `print`/`print_ret`/
  `print_obj`/`print_addr`/`print_paddr`, full game text output.
* **M4 — `zparse.c` + `aread`: DONE.** Line input (interpreter echo,
  raw-mode stdin in the host harness), lower-case store, v3 parse
  buffer (byte 1 = word count; 4-byte entries: dictionary byte
  address | char count | text offset), separator handling, dictionary
  binary search with the v3 word encoding (see note 5).
  **Differential test vs frotz 2.55: identical game prose** for a
  multi-room session (mailbox, leaflet, house entry, lamp, grue death).
* **M5a — save/restore/restart: DONE.** v3 `SAVE`/`RESTORE` (0OP $5/
  $6) via `io.save`/`io.restore` transport callbacks; host writes
  `<story>.lzs` in LugalOS's own format (zsave.c: full state — PC,
  RNG, value stack, call frames, dynamic memory). v3 semantics per
  zmach06e: save success branches; restore success resumes execution
  *after the SAVE instruction* of the saved game. `RESTART` reinitial
 izes from the story image (XIP-friendly: the firmware path reloads
  from flash).
* **M5b — console callbacks + build preset: play Zork on RP2350.

## Field Notes (spec traps found the hard way)

1. **v3 header layout differs from z-spec10.pdf §11.2.** The era-
   correct Infocom layout (zmach06e.txt Table 3 + frotz `H_*` defs,
   verified empirically) is: PC = word `$06` (raw byte address, NOT
   `$08`+8), dictionary = word `$08`, objects = `$0A`, globals = `$0C`,
   dynamic-memory end = `$0E`, flags 2 = `$10`. All raw byte addresses
   except the high-memory base at `$04` (stored /2). The dictionary
   field holding the odd address `$3899` proves there is no /2 scaling.
2. **Long-form 2OP types are abbreviated**: type bit 0 means a ONE-
   BYTE constant and 1 a variable; a 16-bit constant only exists in
   variable-form instructions (`%00`).
3. **`EXT` escape `$BE` is illegal in v3** (v5+ only).
4. **No PC folding, ever.** v1-3 address a flat 128K space; the
   header high-base at `$04` is a paging hint only. `zork1.z3` is
   `$15336` bytes and calls routines at `$1090d`; an experimental
   "wrap PC at $10000 back to high_base" rule corrupted exactly
   those references and none before — the worst kind of latent bug.
   Related: never truncate addresses to `uint16_t` in `loadw/loadb/
   storew/storeb` address arithmetic.
5. **v3 dictionary words look like code and aren't.** Each entry's
   text field is 2 × 16-bit words holding 6 Z-characters in 15 bits
   (3 per word) with **bit 15 set on the final word** (frotz
   `encode_text`). The second word therefore looks exactly like an
   instruction stream (`8d 64 01 60` ≈ `print_paddr …`) — disassem-
   bling around the dictionary will lie. Lookup encodes the same way
   with pad z-char 5.
6. **The store-result byte lives in the instruction stream.** Like
   frotz's `store()` macro, a result instruction (e.g. long-form
   `4f 01 00 00` = `loadw g1 #0 -> acc`) carries its destination
   byte appended, consumed by the store operation, not by operand
   decoding. This is also why long-form instructions end up 4 bytes
   although each long-form operand type is a single byte (note 2).
7. Testing methodology: frotz is fetched from ifarchive
   (interpreters/frotz/frotz-2.55.tar.gz), patched with a fprintf PC
   trace in `interpret()` (gated by env `FROTZ_TRACE`), and used as
   the oracle for `zork-host -t`. For end-to-end prose diffs, build
   the **dumb** interface (`src/dumb/*.c` + `frotz_common.a`, compile
   with `-std=gnu17` — the c23 `bool` typedef in frotz.h otherwise
   breaks it): plain text on stdout, clean diffable output.
