# z-data — retail story binaries for differential testing (not in git)

This directory holds the **retail r119 Zork I story file** used purely
as a behavioral **oracle** for `zork-host` (and frotz) regression
testing.  Shippable game content never lives here — it is built from
MIT sources in `../games/` (see `../games/README.md`).

Populate with:

```sh
cd ..
./fetch-games.sh zork1-oracle   # or: make run  (auto-fetch)
```

The fetch script pins SHA-256 fingerprints; see the header comment of
`../fetch-games.sh` and the oracle record in `../games/zork1/README.md`.
