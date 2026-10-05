# games — MIT-licensed story files built from source

This directory contains adventure games whose **sources are MIT-licensed
and checked in**, together with a deterministic ZILF build and the
committed, reproducible story-file artifact.

Everything here follows one convention per game:

```
games/
  <slug>/
    README.md      # provenance record (required)
    src/           # MIT-licensed source tree, in-tree (required)
    build.sh       # reproducible build -> story file (required)
    <slug>-mit.z3  # committed build artifact (rebuild + diff-testable)
```

* **Provenance rule.** Every `README.md` records: upstream repository,
  pinned commit (or release), license, the reference toolchain
  (ZILF version) and the resulting story-file SHA-256. A rebuild must
  reproduce that hash; if it does not, either the source or the
  toolchain drifted — investigate, then update the record.
* **Header pinning.** `build.sh` pins the story header release/serial
  via ZAPF asm-options (`--asm-options "-r=...,-s=..."`) and
  deliberately does *not* impersonate any retail Infocom binary.
* **Binary-only games do NOT go here.** Story files that only exist as
  binaries (no MIT source) are fetched by `../fetch-games.sh` into
  `../z-data/` and are treated strictly as test oracles, never shipped.

## Current titles

| Slug    | Game          | Source license | Story file |
| ------- | ------------- | -------------- | ---------- |
| `zork1` | Zork I (MIT release) | MIT (Microsoft, 2025-11-20) | `zork1/zork1-mit.z3` |
| `zork2` | Zork II (MIT release) | MIT (Microsoft, 2025-11-20) | `zork2/zork2-mit.z3` |
| `zork3` | Zork III (MIT release) | MIT (Microsoft, 2025-11-20) | `zork3/zork3-mit.z3` |

## Adding a game

1. Create `games/<slug>/src/` and copy the MIT-licensed sources in
   (never a compiled-only dump — there must be a source license chain).
2. Copy `zork1/build.sh` as a template; adjust input file and the
   `-r`/`-s` header pin.
3. Run it, play-test with `zork-host`, commit the `.z3` artifact.
4. Add the provenance record to `<slug>/README.md` and the table above.

Zork II (2026-10) needed one one-character source fix
(`zork2/patches/0001-dreary-room-cond.patch` — an upstream COND-paren
typo ZILCH tolerated and ZILF does not); Zork III compiles clean.
Both records are in the respective `README.md` files.
