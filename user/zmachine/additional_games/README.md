# Additional Z-Machine Games

This directory holds additional interactive fiction games downloaded from the historical preservation archives at [historicalsource](https://github.com/historicalsource) for local gameplay on LugalOS.

> [!IMPORTANT]
> **Licensing & Release Image Policy**:
> The copyright status of compiled commercial Infocom story files is proprietary. These binaries:
> - **Must NEVER be committed to Git** (enforced via `.gitignore`).
> - **Must NEVER be bundled into repository release images** (`lugalos_sd.img`, `flashfs.bin`, UF2 firmware).
> - Are fetched by the user locally using `../fetch-games.sh` for personal, educational, and testing use.
> 
> Shippable open-source game content built from MIT-licensed ZIL sources lives under `../games/` (such as the committed MIT Zork trilogy).

---

## Shipped Game Titles & Topics

| Game | Filename | Size | Spec | Topic & Overview | Status |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Planetfall** (1983) | `planetfall.z3` | 109 KB | v3 | **Sci-Fi Comedy** (Steve Meretzky): You are an Ensign Seventh Class on the Stellar Patrol ship Feinstein, scrubbing decks until disaster strikes and you crash-land on the dying alien world Resida with your companion robot Floyd. | Supported |
| **The Hitchhiker's Guide to the Galaxy** (1984) | `hitchhiker.z3` | 113 KB | v3 | **Satirical Sci-Fi** (Douglas Adams & Steve Meretzky): Wake up as Arthur Dent with a hangover, a bulldozer targeting your home, and Vogons targeting Earth. Don't panic, grab your towel, and solve the Babelfish dispenser. | Supported |
| **Enchanter** (Zork IV) (1983) | `enchanter.z3` | 111 KB | v3 | **Fantasy Magic** (Marc Blank & Dave Lebling): Set in the Zork universe (originally conceived as Zork IV). A novice magic user is sent by the Circle of Enchanters to defeat the evil warlock Krill using learned spell scrolls. | Supported |
| **Starcross** (1982) | `starcross.z3` | 85 KB | v3 | **Hard Sci-Fi** (Dave Lebling): Asteroid prospector in deep space aboard the *Mined-Out* in 2186 discovers a massive, derelict alien cylinder ship in Solar orbit and must explore its complex biosphere. | Supported |
| **Stationfall** (1987) | `stationfall.z3` | 129 KB | v3 | **Sci-Fi Adventure** (Steve Meretzky): The sequel to Planetfall, set five years later. Promoted to Lieutenant, you and Floyd are sent on a dull bureaucratic errand to Space Station Gamma Delta Gamma, only to find it deserted and silent. | Supported |

---

## How to Download

Use the unified fetch script in `user/zmachine/`:

```bash
# Download all additional games to this directory:
./fetch-games.sh

# Or download a specific game:
./fetch-games.sh planetfall
./fetch-games.sh hitchhiker
./fetch-games.sh enchanter
./fetch-games.sh starcross
./fetch-games.sh stationfall
```

The script verifies SHA-256 checksums automatically and is idempotent (already-verified files are preserved).

---

## Compatibility with LugalOS Z-Machine & Auto-Mapper

- **Z-Machine Spec**: LugalOS implements the Version 3 (`.z3`) specification with flat 128 KB address space. All `.z3` titles (Planetfall, Hitchhiker's Guide, Enchanter, Starcross, Stationfall) are 100% compatible.
- **Auto-Mapper Compatibility**: In Infocom Version 3 games compiled with ZIL, global variable `G00` (`HERE`) tracks the current room object number. The auto-mapper hook detects player movement and decodes room names dynamically, plotting the directed graph on the split-screen canvas in real time.

---

## Adding Your Own Story Files

To add other community or custom Z-machine games:
1. Place any compiled `.z3` story file into this directory.
2. Ensure the filename conforms to FAT32 8.3 conventions for microSD storage (e.g. `MYGAME.Z3`).
3. Verify compatibility by running:
   ```bash
   ./fetch-games.sh --verify
   ```

---

## Transferring to a Connected LugalOS Hardware Board

If a board (such as the RP2350-LCD-7 terminal) is plugged in via USB:

```bash
# Sync all games and this README to /sd0/games/ on the board's microSD card:
./fetch-games.sh --sync-sd
```

Or invoke the 9P synchronization tool directly:
```bash
uv run --project host/p9lib python3 tools/p9sync.py --src user/zmachine/additional_games --dst /sd0/games
```

Once transferred, launch any game from the LugalOS interactive shell prompt (`lsh`):
```text
lsh> zmachine planetfall
lsh> zmachine hitchhiker
lsh> zmachine enchanter
lsh> zmachine starcross
lsh> zmachine stationfall
```
