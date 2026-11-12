# Phase 82 — Native SSH-2 Client, Terminal Emulation Completeness & Git Client

**Status**: COMPLETE  
**Target Hardware**:
- **Primary**: **StarFive VisionFive 2 v1.3B** (`visionfive2` preset: 8 GiB LPDDR4, Gigabit Ethernet, 250 GB NVMe `/nvme0` with FAT32 LFN).
- **PSRAM Terminal Targets (`CONFIG_PSRAM_BYTES`)**:
  - **ESP32-P4 Terminals** (`esp32p4-lcd7b`, `esp32p4-tab5`, `esp32p4`): 32 MB PSRAM bulk zone, SDMMC `/sd0`, RMII Ethernet / C6 SDIO networking.
  - **RP2350 Terminal** (`rp2350-terminal`): 8 MB PSRAM bulk zone, SD `/sd0`, 7" LCD terminal (`vtterm`).
  - **PSRAM Allocation Rule**: All large runtime state (SSH packet/KEX buffers, `vtterm` alternate-screen shadow buffer, `tinfl` 32 KB DEFLATE window, Git object/delta/pack buffers) MUST be allocated dynamically from the **PSRAM bulk zone** (`palloc` bulk allocator) on demand and freed on exit—**zero static `.bss` arrays in internal SRAM** and all tables `const` in Flash `.rodata`.
- **Non-PSRAM Microcontroller Exclusion**: `rp2350`, `rp2350-clock`, `rp2350-gateway`, `rp2350-wifi`, `rp2350-sensor`, `rp2350-chess`, `rp2350-smp`, and `esp32c6*` keep `CONFIG_SSH_CLIENT` and `CONFIG_GIT_CLIENT` disabled so their SRAM/Flash baselines remain untouched.
- **Regression**: Host harnesses (ASan/UBSan), QEMU (`rv64`, `rv64-smp`, `rv32`, `rv32-fold`).

**License Policy**: Strictly **MIT-compatible** throughout. Zero GPL/LGPL code (no `libgit2`, `git`, `libssh`, or `OpenSSL`). Reuses existing in-tree cryptography (`monocypher` CC0-1.0/BSD-2-Clause, `p256-m` Apache-2.0, `kernel/crypto.c`, `net/tls.c`) plus minimal permissive DEFLATE (`miniz` MIT or `uzlib` zlib-license).

**Goal**: Enable LugalOS workstations (particularly VisionFive 2 with `/nvme0` storage and 1080p/4K HDMI terminal) to:
1. Open interactive remote SSH sessions (`ssh user@host`) capable of running full-screen TUI editors and tools (`vim`, `nano`, `less`, `htop`, `tmux`).
2. Clone, inspect, commit, pull, and push Git repositories (`git clone`, `status`, `add`, `commit`, `log`, `diff`, `pull`, `push`) directly on `/nvme0` over HTTPS and SSH.

---

## 0. Existing Foundations & Gap Summary

| Capability | Existing In-Tree | Missing / Work Required |
| :--- | :--- | :--- |
| **Symmetric AEAD & KEX** | `third_party/crypto/monocypher/` (`ChaCha20-Poly1305`, `X25519`), `p256-m/` (`P-256` ECDSA/ECDH) | None for `curve25519-sha256` & `chacha20-poly1305@openssh.com` |
| **Hashes & KDF** | `kernel/sha256.c`, `kernel/crypto.c` (`SHA-1`, `HMAC-SHA-256`, `HKDF-SHA-256`) | `SHA-512` + RFC 8032 `ssh-ed25519` (`monocypher-ed25519`, CC0/BSD-2) |
| **TLS 1.3** | `net/tls.c` (client & server: `ChaCha20-Poly1305`, `X25519`, `ECDSA P-256`) | Wire `net/tls.c` into `net/http.c` for `https://` URLs |
| **DEFLATE / zlib** | None | Permissive `tinfl` inflate + minimal deflate (`miniz` MIT / `uzlib` zlib) |
| **VT Terminal** | `drivers/vtterm.c` (UTF-8, `CUU/CUD/CUF/CUB/CUP`, `EL/ED`, `DECSTBM`, `IL/DL`, SGR inverse/luma) | Alternate screen (`?1049h/l`), `CHA`/`VPA`/`ICH`/`DCH`/`ECH`/`RI`/`DECSC`/`DSR`, key encoder |

---

## 1. Milestones

### 82.1 — Terminal Emulation Completeness (`drivers/vtterm.c` & Key Encoding)
Make `vtterm` compatible with `TERM=xterm-256color` / `TERM=vt100` remote TUIs and editors:
1. **Alternate Screen Buffer**: Implement CSI private modes `?1049h`/`?1049l` (save cursor + switch to/from alternate grid) and `?47h`/`?47l` so `vim`, `nano`, `less`, and `htop` preserve shell scrollback.
2. **Missing `ncurses` / Editor Sequences**:
   - `CSI G` (`CHA` — Cursor Horizontal Absolute) and `CSI d` (`VPA` — Vertical Position Absolute).
   - `CSI @` (`ICH` — Insert Character), `CSI P` (`DCH` — Delete Character), `CSI X` (`ECH` — Erase Character).
   - `ESC M` (`RI` — Reverse Index, scrolling down at top of `DECSTBM` region).
   - `ESC 7` / `ESC 8` (`DECSC`/`DECRC`) and `CSI s` / `CSI u` (Save/Restore Cursor).
   - `CSI 6 n` (`DSR` — Device Status Report, replying `\033[row;colR` to the active input stream).
3. **Key-to-Escape Encoder (`kernel/keyseq_encode.c` or `net/ssh_pty.c`)**:
   - Reverse-map LugalOS `key_event_t` values (arrows, Home/End, Insert/Delete, PgUp/PgDn, `F1`–`F12`, Backspace `0x7f`, Ctrl/Alt modifiers) into standard xterm VT sequences for remote PTY forwarding.
4. **Optional Color Cell Upgrade (RGB565 targets)**:
   - On high-RAM panels (VisionFive 2, Tab5, LCD-7B), optionally store 4-bit/8-bit foreground + background palette indices per `vtterm` cell while keeping 1-bpp monochrome fallback for RP2350.

### 82.2 — Cryptographic & Compression Foundations (MIT-Clean)
1. **RFC 8032 `Ed25519` + `SHA-512`**:
   - Vendor Monocypher's `monocypher-ed25519` (`SHA-512` + `crypto_ed25519_*`, CC0-1.0 / BSD-2-Clause) into `third_party/crypto/monocypher/` and expose `crypto_ed25519_sign` / `crypto_ed25519_verify` / `crypto_sha512` in `kernel/include/kernel/crypto.h`.
2. **HTTPS Client Support (`net/http.c`)**:
   - Extend `http_parse_url()` and `http_request()` in `net/http.c` to recognize `https://` (default port 443) and wrap `net/tcp.c` sockets in `net/tls.c` TLS 1.3 sessions with SNI, HTTP/1.1 `Transfer-Encoding: chunked` decoding, and redirect (`301`/`302`/`307`/`308`) handling.
3. **Freestanding `DEFLATE` / `zlib` (`third_party/zlib/` or `fs/deflate.c`)**:
   - Add a small, zero-float, freestanding MIT/zlib-licensed `inflate` + `deflate` module (e.g., `miniz` `tinfl`/`tdefl` subset or `uzlib`) with `zlib` header/Adler-32 framing for Git objects and packfiles.

### 82.3 — Freestanding SSH-2 Client (`net/ssh.c`, `ssh` & `ssh-keygen` commands)
1. **Transport & Key Exchange (RFC 4253 / 8731)**:
   - Version banner exchange (`SSH-2.0-LugalOS_0.21`), binary packet framing, `KEXINIT` negotiation.
   - `curve25519-sha256` key exchange + `chacha20-poly1305@openssh.com` authenticated encryption.
   - Host key verification for `ssh-ed25519` and `ecdsa-sha2-nistp256` against `/nvme0/system/ssh/known_hosts` (or `/sd0/system/ssh/known_hosts`), with interactive TOFU prompt on first connect.
2. **User Authentication (`ssh-userauth`, RFC 4252)**:
   - Public-key auth (`publickey` with Ed25519/P-256 keys stored in `/nvme0/system/ssh/id_ed25519` or identity store) and `password` / `keyboard-interactive` fallback.
   - `ssh-keygen` shell command to generate Ed25519 keypairs and print OpenSSH `ssh-ed25519 AAAA...` public key lines.
3. **Connection & Interactive PTY (`ssh-connection`, RFC 4254)**:
   - Session channel open, `pty-req` (`TERM=xterm-256color`, live `vtterm` dimensions), `shell`, `window-change` on layout resize, and non-interactive `exec` mode (`ssh host "cmd"` and programmatic channel API for Git-over-SSH).

### 82.4 — Git Client (`user/git/` or `net/git.c`, `git` command)
Focused, practical Git workflow built directly on FAT32 LFN (`/nvme0/`):
1. **Core Object Store & Working Tree**:
   - `.git/HEAD`, `.git/refs/heads/*`, `.git/refs/remotes/*`, `.git/config`.
   - Loose object read/write (`zlib` + `SHA-1` from `kernel/crypto.c` for `blob`, `tree`, `commit`, `tag`).
   - `.git/index` v2 reader/writer (stat cache, staged entries, conflict-free add/checkout).
2. **Packfile & Delta Engine**:
   - Packfile v2 (`PACK`) parser and index generator: `OBJ_COMMIT`, `OBJ_TREE`, `OBJ_BLOB`, `OBJ_TAG`, `OBJ_OFS_DELTA`, `OBJ_REF_DELTA` delta reconstruction.
   - Minimal packfile v2 writer for `push` (using DEFLATE stored/fixed-Huffman blocks + SHA-1 trailer).
3. **Remote Transports (`pkt-line` + Smart Protocol)**:
   - **HTTPS (`https://`)**: `info/refs?service=git-upload-pack` / `git-receive-pack`, POST negotiation, side-band-64k progress/pack demux, HTTP Basic / Bearer token auth (from `/nvme0/system/git/credentials` or prompt).
   - **SSH (`ssh://` / `user@host:path`)**: Spawns `git-upload-pack` / `git-receive-pack` over `82.3` SSH `exec` channel.
4. **Shell Subcommands**:
   - `git init`, `git clone <url> [dir]`, `git status`, `git add <paths>`, `git commit -m <msg>`, `git log [-n N]`, `git diff`, `git checkout <ref>`, `git fetch`, `git pull`, `git push`.

### 82.5 — Testing, Hardening & Documentation
1. **Host ASan/UBSan Harnesses (`tests/host/`)**:
   - `vtterm_host.c`: Fuzz/unit tests for `?1049h/l`, `CHA`/`VPA`/`ICH`/`DCH`/`ECH`/`RI`/`DSR`.
   - `ssh_host.c`: Packet framing, `curve25519-sha256` KEX transcript hash, `chacha20-poly1305@openssh.com` AEAD, Ed25519 signature verification.
   - `git_host.c`: DEFLATE roundtrip, SHA-1 object hashing, `OFS_DELTA`/`REF_DELTA` packfile unpacking, `pkt-line` parsing.
2. **QEMU & Live Hardware Tests**:
   - `tests/hw/test_visionfive2.py`: Live `https://` fetch, SSH loopback/LAN session & `exec`, and `git clone`/`commit`/`push` against a test server on the host LAN.
3. **Zero Non-PSRAM Microcontroller Regression & PSRAM Budget Verification**:
   - Verify `tools/precommit.py` passes all 18 presets, internal SRAM heap baselines remain flat on `rp2350-terminal` and `esp32p4-*` (all workspace buffers reside in PSRAM bulk zone), and non-PSRAM `rp2350*` / `esp32c6*` baselines are unchanged.
   - Smoke-test `ssh` and `git` on `esp32p4-lcd7b` / `esp32p4-tab5` (PSRAM + network) and `rp2350-terminal` (local `/sd0` `git` / PSRAM `vtterm` alternate screen & SSH client where network transport is available).
4. **Documentation**:
   - Update `doc/user/shell.md` and `doc/admin/networking.md`, regenerate on-device help via `python3 tools/gendocs.py --all`.

---

## 2. Verification & Acceptance Criteria

* `ssh user@host` opens an interactive session where `vim` / `nano` / `htop` / `less` render cleanly and exit back to the preserved shell prompt.
* `ssh user@host "uname -a"` executes non-interactively and returns remote exit status.
* `git clone https://... /nvme0/src/repo` clones a real multi-commit repository with packfile deltas onto NVMe, followed by `git status`, `git add`, `git commit`, `git log`, and `git push` (over HTTPS or SSH).
* `python3 tools/precommit.py` passes all 18 presets, racescan, gendocs, and RAM baselines.

---

## 3. Implementation & Verification Summary

- **82.1 Complete**: `drivers/vtterm.c` (`?1049h/l`, `?1047h/l`, `?47h/l`, `CHA`, `VPA`, `ICH`, `DCH`, `ECH`, `RI`, `DECSC`/`DECRC`, `DSR` via `vtterm_take_dsr()`), `drivers/screen.c` (`SCREEN_ALT_SHADOW`), and `kernel/keyseq.c` (`keyseq_encode()` + round-trip selftest).
- **82.2 Complete**: `third_party/crypto/monocypher/monocypher-ed25519.{c,h}` (`SHA-512` + RFC 8032 `Ed25519`), `net/http.c` (`https://` over `net/tls.c`), `kernel/zlib.{c,h}` (freestanding RFC 1950/1951 inflate/deflate + Adler-32/CRC-32), plus JH7110 hardware TRNG support in `kernel/random.c` for VisionFive 2.
- **82.3 Complete**: `net/ssh.c` + `net/ssh_cli.c` (`curve25519-sha256`, `chacha20-poly1305@openssh.com`, `ssh-ed25519` / `ecdsa-sha2-nistp256`, `known_hosts` TOFU, `ssh-keygen`, interactive PTY and `exec`).
- **82.4 Complete**: `net/git.c` + `net/git_pack.c` + `net/git_cli.c` (`init`, `clone`, `status`, `diff`, `add`, `commit`, `log`, `checkout`, `fetch`, `pull`, `push` over Smart HTTP/HTTPS and SSH).
- **82.5 Complete**: Host ASan/UBSan (`zlib_host`, `ssh_host`, `git_host`), QEMU (`quick.py screen`, `keyselftest`, `cryptotest`), live VisionFive 2 (`randtest`, `cryptotest`, `ssh-keygen`, `vtselftest`, `git init`), `doc/user/shell.md` + `doc/user/ssh_and_git.md` (`gendocs.py --all --check`), and `python3 tools/precommit.py` (all 18 presets, racescan, RAM baselines) passed.

---

## 4. Future Extensions

1. **HTTPS Root CA Collection & Chain Verification**:
   - Currently `git` over `https://` sets `.tls_insecure = true` in `net/git_cli.c` because LugalOS has no public root CA store on VFS (e.g. `/nvme0/etc/ssl/certs` or `/sd0/etc/ssl/certs`) and `net/tls.c` only validates direct leaf certificates against a single local `lugalos-ca` P-256 key (and only negotiates `ECDSA P-256` + `X25519` + `ChaCha20-Poly1305`).
   - Adding a curated root CA bundle on `/sd0`/`/nvme0`, multi-certificate chain verification in `net/tls.c`, and (optionally) RSA-PSS/PKCS#1v1.5 verification would enable fully verified `https://` remotes without `.tls_insecure`.
2. **`scp` Support**:
   - Add an `scp` shell command layered on `net/ssh.c` (`exec` channel running `scp -f` / `scp -t` or SFTP subsystem) to copy files between local VFS (`/nvme0`, `/sd0`, `/ram0`) and remote SSH hosts.
