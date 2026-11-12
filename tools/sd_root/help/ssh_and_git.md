# SSH-2 & Git Clients

[User Manual](user.md) > **SSH-2 & Git Clients**

---

LugalOS includes a freestanding, MIT-licensed **SSH-2 client** (`ssh`, `ssh-keygen`) and **Git client** (`git`) for workstations and PSRAM-equipped terminals (`visionfive2`, `esp32p4`, `esp32p4-lcd7b`, `esp32p4-tab5`, `rp2350-terminal`, and QEMU). Non-PSRAM microcontrollers exclude both clients to preserve internal SRAM and flash budgets.

All large session, compression, and packfile buffers are allocated dynamically from the PSRAM bulk zone (or DDR on 64-bit targets) and freed on exit.

---

## 1. SSH Key Management (`ssh-keygen`)

Generate an Ed25519 keypair before authenticating with public-key auth:

```sh
ssh-keygen [-f PATH] [-C COMMENT]
```

* **Default key path**: `/nvme0/.ssh/id_ed25519` when NVMe is mounted, otherwise `/sd0/.ssh/id_ed25519`.
* **Files written**:
  * `<path>`: 32-byte raw Ed25519 seed.
  * `<path>.pub`: OpenSSH public-key line (`ssh-ed25519 AAAA... comment`), ready to append to `~/.ssh/authorized_keys` on a remote host.
* **Fingerprint**: Prints the `SHA256:...` fingerprint of the generated key.
* **Hardware entropy**: Key generation requires a live hardware entropy source (`randtest` must report `RANDTEST_OK`, including the JH7110 hardware TRNG on VisionFive 2).

---

## 2. SSH-2 Client (`ssh`)

Connect to remote SSH-2 servers for interactive terminal sessions or single-command execution:

```sh
ssh [-p PORT] [-i KEY] [-P PASS] USER@HOST [CMD...]
```

### Supported Algorithms
* **Key Exchange**: `curve25519-sha256`
* **Cipher / AEAD**: `chacha20-poly1305@openssh.com`
* **Host Keys**: `ssh-ed25519`, `ecdsa-sha2-nistp256`
* **Authentication**: `publickey` (Ed25519) and `password`

### Host Key Verification (`known_hosts`)
On first connection to a host, `ssh` prints the server's key type and `SHA256:` fingerprint and prompts for Trust-On-First-Use (TOFU) confirmation (`yes`/`no`). Accepted keys are saved to `/nvme0/.ssh/known_hosts` (or `/sd0/.ssh/known_hosts`). Subsequent connections verify the server key against the stored entry and abort if it changes.

### Interactive PTY & Remote TUIs
When `CMD` is omitted, `ssh` requests a PTY with `TERM=xterm-256color` matching the local `vtterm` grid dimensions and forwards keyboard events (`keyseq_encode`) including arrows, Home/End, Insert/Delete, PgUp/PgDn, `F1`-`F12`, Backspace, and Ctrl/Alt modifiers.

LugalOS `vtterm` supports alternate screen buffers (`?1049h/l`, `?1047h/l`, `?47h/l`), `CHA` (`CSI G`), `VPA` (`CSI d`), `ICH`/`DCH`/`ECH` (`CSI @`/`P`/`X`), `RI` (`ESC M`), `DECSC`/`DECRC` (`ESC 7/8`, `CSI s/u`), and `DSR` (`CSI 6 n`), allowing full-screen remote tools such as `vim`, `nano`, `less`, and `htop` to run and restore the local shell prompt on exit.

### Non-Interactive Execution
Passing `CMD...` runs the command via SSH `exec`, streams stdout/stderr to the local console, and returns the remote exit status:

```sh
ssh alice@192.168.178.10 "uname -a"
```

---

## 3. Git Client (`git`)

LugalOS provides a freestanding Git client built on SHA-1 loose objects, `.git/index` v2, RFC 1950/1951 `zlib` compression (`kernel/zlib.c`), packfile v2 (`PACK`) with `OFS_DELTA` / `REF_DELTA` reconstruction, and Smart HTTP/HTTPS (`http://`, `https://`) or SSH (`ssh://`) transport.

```sh
git [-C DIR] CMD [ARGS...]
```

If `-C DIR` is omitted, `git` defaults to `/nvme0/repo` (when NVMe is mounted) or `/sd0/repo`.

### Supported Subcommands

| Command | Description |
| :--- | :--- |
| `git init [dir]` | Initialize an empty repository with `.git/HEAD` (`ref: refs/heads/main`), `objects/`, and `refs/heads/`. |
| `git clone URL [DIR]` | Clone a repository over `http://`, `https://`, or `ssh://`, saving `.git/origin_url` and checking out the default branch. |
| `git status` | List tracked files and sizes from `.git/index`. |
| `git diff` | Compare working-tree files against `.git/index` SHA-1 hashes and report modified or deleted files. |
| `git add FILE` | Hash `FILE` as a loose blob object and stage it in `.git/index`. |
| `git commit -m MSG` | Build recursive tree objects from `.git/index`, write a commit object, and advance `HEAD`. |
| `git log` | Walk commit history from `HEAD` and print commit hashes and messages. |
| `git checkout REF` | Switch to a branch or commit hash and materialize the tree into the working directory. |
| `git fetch` | Fetch updated objects and remote tracking refs from `origin`. |
| `git pull` | Fetch from `origin` and update the local branch and working tree. |
| `git push [url]` | Collect reachable commits/trees/blobs, build a v2 packfile, and push via `git-receive-pack` (`http://`, `https://`, or `ssh://`). |

### Example Workflow

```sh
git init /nvme0/hello
echo "int main(void) { return 0; }" > /nvme0/hello/main.c
git -C /nvme0/hello add main.c
git -C /nvme0/hello commit -m "Initial commit"
git -C /nvme0/hello log
```

```sh
git clone https://example.org/user/repo.git /nvme0/repo
git -C /nvme0/repo status
git -C /nvme0/repo pull
```
