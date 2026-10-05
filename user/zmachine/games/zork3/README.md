# zork3 — Zork III from MIT-licensed source

*Zork III: The Dungeon Master*, rebuilt from the sources that
Microsoft/Open Source Programs Office, Team Xbox, and Activision
released under the **MIT License** on 2025-11-20
("Zork I, II, and III go Open Source", Microsoft Open Source Blog).

## Provenance chain

| Item | Value |
| --- | --- |
| Upstream repository | https://github.com/historicalsource/zork3 |
| Pinned commit | `3ec9ed412b5f3cafe65d83c727d07db1fe4a86a8` (2025-11-21, "Update README.md") |
| Source license | MIT (see `src/LICENSE`, added by Microsoft's upstream PR) |
| Sources | build-closure `.zil` files (see "Source subset" below) |
| Patches | none — compiles clean under ZILF 1.9 |
| Toolchain | ZILF 1.9 + ZAPF (hg `r1694.428d9a205a73`) — PKGBUILD in `../../pkgbuild/zilf-hg/` |
| Build command | `ZILF=zilf ./build.sh` → `zilf zork3.zil zork3-mit.z3 --asm-options "-r=1,-s=112025"` |
| Story file | `zork3-mit.z3` — v3, Release 1, Serial 112025, 84,924 bytes |
| Story SHA-256 | `6eb69f0237229a023142cce849bcf241ef5e2ec5b06236f85150a4a4ba649430` |

Header pin: serial `112025` = MIT release date. We deliberately do not
impersonate the retail r25/860811 binary. Trademarks remain with
Activision; only the code is MIT — so the file ships as `zork3-mit.z3`
and no Infocom branding is used.

## Source subset

`src/` carries the files in `zork3.zil`'s `INSERT-FILE` closure
(`zork3.zil`, `3dungeon.zil`, `3actions.zil`, the shared `g*.zil`
files) plus `zork3freq.xzap`, `LICENSE` and the upstream docs
(`README.md`, `.chart`, `.errors`, `.record`, `.serial`).

The upstream directory is a working-directory snapshot and also carries
an older generation of un-prefixed sources (`main.zil`, `dungeon.zil`,
`actions.zil`, `parser.zil`, `verbs.zil`, `syntax.zil`, `clock.zil`,
`macros.zil`, `tm.zil`, `shadow.zil`, `demons.zil`) that `zork3.zil`
never inserts, plus `gmain.cmp` (a ZILCH table dump ZILF ignores) and
the retail `zork3.zip` / `COMPILED/` story binaries.  None of those are
mirrored here — only the build closure ships.

## Reproducing

```sh
# 1. Build ZILF (Arch):
makepkg -si ../../pkgbuild/zilf-hg      # or any ZILF >= 1.9
# 2. Rebuild and verify:
./build.sh
sha256sum zork3-mit.z3   # must match the hash above
# 3. Play:
../../zork-host zork3-mit.z3            # LugalOS Z-machine VM
```

Builds are byte-reproducible with the reference ZILF version (verified:
two independent runs produce the SHA-256 above).
