# zork1 — Zork I from MIT-licensed source

*Zork I: The Great Underground Empire*, rebuilt from the sources that
Microsoft/Open Source Programs Office, Team Xbox, and Activision
released under the **MIT License** on 2025-11-20
("Zork I, II, and III go Open Source", Microsoft Open Source Blog).

## Provenance chain

| Item | Value |
| --- | --- |
| Upstream repository | https://github.com/historicalsource/zork1 |
| Pinned commit | `97b7b3d68c075dd9af7da499c3e9690ada3471fd` (2025-11-21, "Update README.md") |
| Source license | MIT (see `src/LICENSE`, added by Microsoft's upstream PR) |
| Sources | `src/*.zil`, `src/parser.cmp` (compiled MUD tables), support files |
| Toolchain | ZILF 1.9 + ZAPF (hg `r1694.428d9a205a73`) — PKGBUILD in `../../pkgbuild/zilf-hg/` |
| Build command | `ZILF=zilf ./build.sh` → `zilf zork1.zil zork1-mit.z3 --asm-options "-r=1,-s=112025"` |
| Story file | `zork1-mit.z3` — v3, Release 1, Serial 112025, 84,258 bytes |
| Story SHA-256 | `e7e789a67ac87a5c42eee507bca223fdf8ae94c020110942686d5927daa6e6ca` |

Header pin: serial `112025` = MIT release date. We deliberately do not
impersonate the retail r119/880429 binary. Trademarks remain with
Activision; only the code is MIT — so the file ships as `zork1-mit.z3`
and no Infocom branding is used.

## Reproducing

```sh
# 1. Build ZILF (Arch): 
makepkg -si ../../pkgbuild/zilf-hg      # or any ZILF >= 1.9
# 2. Rebuild and verify:
./build.sh
sha256sum zork1-mit.z3   # must match the hash above
# 3. Play:
../../zork-host zork1-mit.z3            # LugalOS Z-machine VM
make -f ../../Makefile run-mit
```

Builds are byte-reproducible with the reference ZILF version (verified:
two independent runs produce the SHA-256 above).  A different ZILF
version may emit different code — re-hash and update this record
deliberately if you intentionally upgrade the toolchain.

## Relationship to the retail r119 binary

The upstream repository *also* carries the retail story file at
`COMPILED/zork1.zip` (a raw v3 image despite the extension; release
119, serial 880429, SHA-256 `37084966…b64d79`).  `../..fetch-games.sh`
can fetch it into `z-data/` as the **differential-testing oracle** for
`zork-host`.  It is a test fixture, not a shipping artifact: the game
we distribute is the MIT-built `zork1-mit.z3`.  Note that nobody has
demonstrated the released source compiles byte-identically to any
retail binary — expect (benign) prose and size differences, e.g. MIT
build 84,258 bytes vs retail 86,838 bytes.

## Legal notes

* What is MIT: the ZIL source code *and the game content expressed in
  it* (all prose lives in `.zil` strings), hence the compiled output
  `zork1-mit.z3` is an MIT-derived artifact we may ship in-tree and in
  `lugalos_sd.img`.
* What is NOT MIT: Infocom/ZORK trademarks and packaging artwork
  (explicitly excluded by the Microsoft release).
