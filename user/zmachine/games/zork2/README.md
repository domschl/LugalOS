# zork2 — Zork II from MIT-licensed source

*Zork II: The Wizard of Frobozz*, rebuilt from the sources that
Microsoft/Open Source Programs Office, Team Xbox, and Activision
released under the **MIT License** on 2025-11-20
("Zork I, II, and III go Open Source", Microsoft Open Source Blog).

## Provenance chain

| Item | Value |
| --- | --- |
| Upstream repository | https://github.com/historicalsource/zork2 |
| Pinned commit | `3da9661098809788a99cef00f00c865c6c204f96` (2025-11-21, "Update README.md") |
| Source license | MIT (see `src/LICENSE`, added by Microsoft's upstream PR) |
| Sources | build-closure `.zil` files (see "Source subset" below) |
| Patches | `patches/0001-dreary-room-cond.patch` (required; see below) |
| Toolchain | ZILF 1.9 + ZAPF (hg `r1694.428d9a205a73`) — PKGBUILD in `../../pkgbuild/zilf-hg/` |
| Build command | `ZILF=zilf ./build.sh` → `zilf zork2.zil zork2-mit.z3 --asm-options "-r=1,-s=112025"` |
| Story file | `zork2-mit.z3` — v3, Release 1, Serial 112025, 89,504 bytes |
| Story SHA-256 | `44a00b301383c2aa76928610b3715229a122962da0bc8a5a903bb54c8a9658a8` |

Header pin: serial `112025` = MIT release date. We deliberately do not
impersonate the retail r63/860811 binary. Trademarks remain with
Activision; only the code is MIT — so the file ships as `zork2-mit.z3`
and no Infocom branding is used.

## Required source patch

`2actions.zil`, `DREARY-ROOM-FCN`: the upstream file closes the `COND`
one paren early, leaving a dangling `(T <PCHECK> <RFALSE>)>` form.
ZILCH silently compiled that as top-level initialization code; ZILF
rejects it (`ZIL0123: expressions of type 'LIST' cannot be compiled`).
Patch 0001 moves one paren so the clause is the `COND`'s else branch —
the author's evident intent and the canonical room-function shape used
by `TINY-ROOM-FCN` directly above it. `src/` itself stays a byte-exact
mirror of the pinned upstream commit; `build.sh` applies `patches/*.patch`
to the scratch tree.

## Source subset

`src/` carries the files in `zork2.zil`'s `INSERT-FILE` closure
(`zork2.zil`, `2dungeon.zil`, `2actions.zil`, the shared `g*.zil`
files) plus `zork2freq.xzap`, `LICENSE` and the upstream docs
(`README.md`, `.chart`, `.errors`, `.record`, `.serial`).  Excluded
from the upstream directory: `crufty.zil` (never inserted — dead
material), the TOPS-20 `.zap` assembly intermediates, and the retail
`zork2.zip` / `COMPILED/` story binaries (not redistributed; the MIT
source chain is what ships).  Upstream's `gsyntax.diffs` /
`gverbs.diffs` patchlets are **not** applied.

## Reproducing

```sh
# 1. Build ZILF (Arch):
makepkg -si ../../pkgbuild/zilf-hg      # or any ZILF >= 1.9
# 2. Rebuild and verify:
./build.sh
sha256sum zork2-mit.z3   # must match the hash above
# 3. Play:
../../zork-host zork2-mit.z3            # LugalOS Z-machine VM
```

Builds are byte-reproducible with the reference ZILF version (verified:
two independent runs produce the SHA-256 above).
