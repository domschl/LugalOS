# Phase 21 — Identity and authentication, as a thing in itself

**Status: planned 2026-08-26; I1-I6 complete 2026-08-29.** Independent of the
phase it grew out of.

**I1 done (2026-08-29).** `kernel/idstore.h`/`.c`: magic/version/length/CRC32
header, typed (TLV) fields, the three states (unprovisioned/corrupt/valid),
and a `block_dev_t`-shaped read/write API, entirely target-independent.
`idstoreselftest` (wired into `tests/runner.py`) checks all five points from
§8's verify list against an in-memory fake block device -- no hardware, no
mounted filesystem, no dependency on I2's not-yet-built QEMU disk. 284/284
QEMU tests pass on both rv32 and rv64. Only `IDSTORE_FIELD_UID` and
`IDSTORE_FIELD_NAME` are defined so far; I4-I6 add device-key, peer-list and
WLAN-credential field types on top of the same format without touching this
code.

**I2 done (2026-08-29).** `drivers/virtio_blk_id.c`: a second, independent
virtio-blk MMIO instance dedicated to the identity store -- direct polled
MMIO, deliberately without the primary disk's "blk" driver-task machinery,
since identity access is boot-time and occasional (§2's write policy already
says so). `kernel/identity.c` gained the record tier at the top of §4's
resolution ladder for both the name and a new device-scope UID
(`node_uid()`/`node_uid_source()`, `NODE_UID_LEN` in `kernel/identity.h`),
reached through a weak `identity_store_device()` hook so the file stays
target-agnostic -- QEMU's driver defines it; I7's RP2350 flash backend will
override the same symbol. `/proc/node` (`fs/vfs_server.c`) reports `uid` and
`uid source` alongside the existing name/mac fields. `tools/provision.py`
builds a record on the host (its own from-scratch CRC32 + TLV writer, not an
import of the kernel's, so the format is exercised two independent ways).

A real bring-up finding, not a documentation footnote: QEMU's riscv `virt`
machine binds virtio-mmio transport slots to `-device virtio-blk-device`
entries in **reverse command-line order** -- the last such device declared
gets the *lowest* MMIO address, which is the first address both drivers'
ascending probes see. `tests/runner.py`'s `QemuSession.start()` therefore
takes an explicit `identity_img_path` that inserts hd1 *before* hd0 on the
command line, rather than appending it via the generic `extra_qemu_args` used
for unrelated devices (net, console) -- appending it there silently swapped
which driver mounted which disk. Found empirically bringing up
`test_identity_store_provisioning`; the working theory is unconfirmed against
QEMU's own source and should be treated as "true for the version tested,"
not as documented behaviour.

RP2350's `sizecheck` caught a real design mistake before it shipped: the
first version kept a permanent 4 KB `idstore_t` static in `kernel/identity.c`
(a lifetime the code never needed -- node_identity_init() only ever reads it
once, at boot) and a permanent 4 KB fake-disk static in `idstore_selftest()`
(needed only for the seconds the test runs). Both are gone now -- the record
is a local consumed within `node_identity_init()` and freed off the stack
when it returns (16 KB boot stack, `linker/rp2350.ld`, comfortably enough
this early), and the self-test's fake disk is `palloc_pages(1)`'d and freed
at the end, the same C5 argument `drivers/ramdisk.c` already makes. Net
static-RAM growth across all three RP2350 personas: **37 bytes**, all of it
the UID's own genuinely-persistent state. Baselines re-recorded
(`tools/sizereport-rp2350*.json`).

286/286 QEMU tests pass (284 + 1 new test × 2 architectures): a guest booted
with a `tools/provision.py`-written disk reports the provisioned name and uid
with `record` as the source of both; the same guest with no second drive at
all -- every test that existed before I2, unmodified -- falls back to the
derived identity exactly as it always did, with `uid: none`.

**I3 done (2026-08-29).** The `identity` command (`kernel/shell.c`) and its
four Lisp equivalents (`identity`, `identity-name`, `identity-provision`,
`identity-key` in `user/lisp/lisp.c`), all thin wrappers over new typed
functions in `kernel/identity.c`
(`node_identity_provision()`/`node_identity_rename_persistent()`/
`node_identity_set_key()`/`node_identity_generate_key()`/`node_devkey()`),
sharing one read-modify-write helper onto the record so setting one field
never silently drops another. `/proc/node` gained a `key fingerprint` line
(`kernel/sha256.c`'s new `key_fingerprint_hex()` -- first 8 bytes of
SHA-256, hex, §5.1). `kernel/idstore.h` gained `IDSTORE_FIELD_DEVKEY`, stored
and fingerprinted from here on but **not yet consulted by the 9P auth path**
-- that is I4's job, deliberately deferred so this milestone is toolset and
storage only. `identity key --generate` refuses outright when
`random_is_hardware()` is false (every QEMU target today), matching §5.1;
`identity key <hex>` refuses a key that is empty, oversized, all one byte
value, or a straight ±1 run across every byte -- not full "trivially
patterned" detection, the two concrete shapes the plan names.

**Scope cut, stated plainly:** `peers`/`peers add`/`peers remove`/`wlan`
from §6 are **not** in I3. Building those commands now, before I5/I6 give
them anything to actually enforce (`p9_handle_tattach()` still grants the
whole namespace to every authenticated key), would ship a command that
*looks* like it restricts access without doing so -- worse than not having
it. They land with I5 and I6, against the field types those milestones
define.

`fs/9p.c`'s `p9_path_is_secret()` now also calls
`kernel/idstore.c`'s new `idstore_path_is_secret()` (§4: "the identity store
must join it"), guarding `/dev/identity`/`/dev/identity0` even though
nothing serves either today -- the store is still never mounted into the
VFS/9P namespace at all, reached only through `block_dev_t` directly from
`kernel/identity.c`. Defensive, on the same reasoning the key-directory
guard is enforced on the server rather than trusted to every caller: a guard
installed only once something reaches for the wrong path is installed too
late.

288/288 QEMU tests pass (286 + 1 new test × 2 architectures,
`test_identity_toolset`): a fresh store provisions and refuses a second
`provision` without `--force`; a persisted rename updates `/proc/node`'s
name and source without moving the MAC; an installed key's raw hex appears
exactly once in the whole exchange -- the shell's own echo of what was
typed -- and never again in any response, only its fingerprint. RP2350
`sizecheck`: **+0 bytes** static RAM across all three personas -- everything
new lives on the stack or in the record itself.

**I4 done (2026-08-29).** `fs/9p.c`'s `p9_auth_key_for()` gained a new tier,
checked right after the console key and ahead of both file-based sources:
`node_devkey()` (I3), answering for any `uname` the same way the flash
fallback always has -- §1.2's "one key store serves both directions" still
holds; the split into per-peer grants is I5's, not this one's.
`p9_auth_have_keys()` gained the matching check, so a record-only key does
not trip `p9auth`'s "no keys configured" warning. Nothing else moved:
`P9_AUTH_KEYS_FILE` and `P9_AUTH_FALLBACK_KEY_FILE` are still read, still in
that order, still after the record -- I4 only says where *this node's own*
key is checked first, not that the SD-card list stops mattering (that is
I5's per-peer-grants job). The console key is untouched, exactly as planned:
still the bootstrap override, still checked first, still never reachable
over 9P.

*(Superseded below, same day: I5 found that "answering for any uname" is
exactly the conflation §1.2 warns about, and removed `node_devkey()` from
`p9_auth_key_for()`'s ladder entirely. Left as written above because it is
what I4 actually shipped and what its own test verified at the time --
`test_identity_record_auth` needed updating when I5 landed, and that update
is the honest record of why.)*

New QEMU test `test_identity_record_auth`: two nodes, each with a blank
identity disk, each running `identity key <hex>` and nothing else --
no `p9key`, no SD-card key file on either side -- authenticate over TCP and
mount each other's namespace. `p9auth` on the server confirms `Keys
configured: yes` sourced from the record alone. The raw key appears exactly
once in the whole exchange (the shell's own echo), matching I3's own
no-key-printed property extended to the path that actually authenticates a
peer. `test_9p_between_nodes_over_tcp` (phase 19's own two-node test, R3b) --
named explicitly in I4's verify list -- passes unmodified, since it never
attaches an identity disk at all and `node_devkey()` simply returns false
for it, falling straight through to the console key exactly as before.

289/289 QEMU tests pass. RP2350 `sizecheck`: **+0 bytes** across all three
personas -- the new tier is a stack-local lookup, nothing persistent added.

**I5 done (2026-08-29).** §5.2, and the real split I4 only gestured at.
`P9_AUTH_KEYS_FILE` grew two columns -- `aname`, `mode` (`ro`/`rw`) --
backward-compatible on read (a bare 2-column line still means unrestricted,
exactly what it always meant); `fs/9p.c` gained `p9_grant_t`,
`p9_grants_list()`/`p9_grants_add()`/`p9_grants_remove()`, and the
`peers`/`peers add`/`peers remove` toolset (`kernel/shell.c`,
`user/lisp/lisp.c`) I3 deliberately left unbuilt. `peers` never prints a
key, only its fingerprint, same discipline as `identity`.

**Enforcement, not just storage.** `p9_handle_tattach()` looks up the
attaching `uname`'s grant and refuses (`"attach: not granted at this
aname"`) unless the requested aname equals the granted one or is a
subtree of it (`p9_path_within()`, the same component-boundary matching
`p9_path_is_secret()` already used) -- checked *before* `p9_fid_alloc()`,
so a refused attach costs no slot in the 8-entry fid table, closing a
latent leak the existing secret-path check already had. A grant's `ro`
sets `p9_fid_entry_t.read_only`, which `p9_handle_twalk()` propagates to
every descendant fid and `Topen` (write/trunc/ORCLOSE)/`Tcreate`/
`Twrite`/`Tremove` all refuse against, each with its own "granted
read-only access" message.

**The split §1.2 called for, made real.** I4's `p9_auth_key_for()` let
`node_devkey()` answer for *any* `uname`, on the same footing as the flash
fallback -- coherent in isolation, but once grants carry scope it means
anyone holding a node's own key walks straight past every grant, which is
exactly the conflation this section exists to end. I5 splits the one
function into two: `p9_auth_own_key()` (console key, then the record --
"how do I prove myself", used only by `fs/p9_link.c`'s client-side
exchange) and `p9_auth_key_for()` (console key, then the grants list, then
the flash fallback -- "who may attach to me", server-side only, and no
longer consults the record at all). `p9_auth_have_keys()` was updated to
match -- it had inherited I4's same mistake, reporting readiness a peer
could never actually act on. `test_identity_record_auth` (I4) needed a
real fix, not just a comment: node A still proves itself from its record
alone, but node B now grants A's key explicitly with `peers add`, the same
command an operator would use, rather than relying on both sides
coincidentally sharing one value.

Two new QEMU tests. `test_identity_grants_scope`, over the same
TCP-bridged virtio-console setup `test_9p_auth_gate` uses (a real p9lib
client asking for whatever aname it likes, not our own client which only
ever asks for `""`): a peer granted `/ram0` is refused at `/` and accepted
at `/ram0`; a `ro` peer's write is refused with "read-only" in the reply;
a removed peer's very next `Tauth` is refused, nothing to invalidate since
nothing was ever cached. `test_identity_grants_cap`: eight `peers add`
calls fill the table, a ninth distinct name is refused with a "full"
message, and updating one of the eight already there still works at 8/8
since that replaces a slot rather than claiming one.

293/293 QEMU tests pass. RP2350 `sizecheck`: **+0 bytes** across all
three personas -- every new structure here lives on the stack, in the
grants file, or in a 9P fid table entry that already existed.

*(A note on how this milestone nearly reported a false regression: manually
smoke-testing `peers add` against `build/rv32/lugalos_sd.img` directly --
not a copy -- and running `(format "/sd0")` to get a writable auth
directory wiped the same image's pre-staged content, which ~10 unrelated
tests depend on. The next full run failed those tests identically on a
second clean attempt, which looked exactly like a real regression until
the actual cause -- a corrupted shared fixture, not new code -- was found.
Fixed by regenerating from the already-staged `build/rv32/sd_root/`. See
`[[feedback_never_format_shared_base_image]]` in memory.)*

**I6 done (2026-08-29).** §5.3, and the `wlan` command I3 left unbuilt.
`kernel/idstore.h` gained two field types, `IDSTORE_FIELD_WLAN_SSID` and
`IDSTORE_FIELD_WLAN_PSK` -- the *derived* 256-bit PSK, never the
passphrase, exactly as §5.3 requires. Lands with phase 19's R5 (the CYW43
driver, not yet started) and is unused before it, same shape as the
device key was between I3 and I4: this milestone is storage and the
toolset, not a radio.

**The derivation lives on the host, and only there.** `tools/provision.py`
gained `derive_wpa2_psk()` -- `hashlib.pbkdf2_hmac("sha1", passphrase,
ssid, 4096, dklen=32)`, the exact construction WPA2 itself defines
(IEEE 802.11i), using Python's stdlib rather than a hand-rolled
PBKDF2/HMAC-SHA1: unlike `crc32_compute()`, there is no on-device
equivalent this needs to stay in step with, since §5.3 is explicit that
4096 iterations of HMAC never run on the board. `--wlan-ssid`/
`--wlan-passphrase` derive and write the PSK; `--selftest` checks the
derivation alone, no image required.

**`kernel/identity.c` gained the same shape as the device key**:
`node_wlan_ssid()`/`node_wlan_psk()`/`node_identity_set_wlan()`, sharing
the record read-modify-write helper -- which grew from a positional
parameter list into an `identity_patch_t` struct along the way, since a
fifth field's worth of positional `NULL`s was the point that stopped
being readable at the call site. `kernel/shell.c`'s `wlan` (report) /
`wlan <ssid> <psk-hex>` and `user/lisp/lisp.c`'s `wlan`/`wlan-set` mirror
`identity`'s fingerprint-only discipline exactly: the SSID prints in
full (it is not a secret -- every AP broadcasts it in its own beacon
frames), the PSK never does, only its fingerprint. The on-device command
takes a hex PSK only, never a passphrase -- there is no code path here
that could accept one.

296/296 tests pass (294 QEMU + 2 host-only, no QEMU involved for the
derivation check). `test_host_wpa2_psk_derivation()`: SSID="IEEE",
passphrase="password" -> the IEEE 802.11i worked example's own PSK,
reproduced independently via Python's stdlib rather than invented
alongside the code being tested, plus determinism/sensitivity property
checks. `test_wlan_credential_roundtrip()`: a disk `tools/provision.py`
wrote a WLAN credential onto is read back correctly at boot (proving
`kernel/idstore.c` and `provision.py`'s independent record writers agree
on this field the same way I2 already proved for uid/name), a runtime
`wlan` install round-trips, and the raw PSK hex appears exactly once in
the whole exchange -- the shell's own echo, never a response. RP2350
`sizecheck`: **+0 bytes** across all three personas.

Next: I7 (the RP2350 flash-sector backend -- the last milestone every
other one has been building storage-agnostic for) and I8 (docs).

Phases 18 and 19 each built a piece of this under pressure from something else
-- an auth gate because a network was coming, a node name because two boards
were about to share a segment -- and the pieces are good but they were never
designed together. This phase designs them together.

**Scope.** What a LugalOS node *is*, how it proves it, where those facts live,
what may modify them, and what the whole arrangement does and does not defend
against. One framework, one storage format, one toolset.

**Out of scope:** an encrypted transport, secure boot, and anything resembling
a PKI. See §9, which says why each is out and what would bring it in.

---

## 0. Why this is its own phase

Three things are already in the tree, built at three different times for three
different reasons:

* **Phase 18 N1/N2** -- HMAC-SHA-256 over a server-chosen nonce, a `Tauth`
  gate, keys in `/sd0/system/etc/auth/keys`, a console key for bootstrap, and
  `p9_link_t.auth_required` so trust is a property of the *wire*.
* **Phase 19 R3's addendum** -- `kernel/identity.c`: a derived name and MAC,
  and the node's name as its 9P `uname`.
* **Phase 19 R3b** -- the client half of the auth exchange, because a node
  that cannot authenticate can only mount peers that ask nothing.

Each is sound. Together they have gaps that only show up when you ask the
question directly:

1. **Nothing is provisioned.** Identity is *derived* from a build seed, so two
   boards flashed from one build are the same node. `/proc/node` says so out
   loud, which is honest and not a solution.
2. **The key lives on removable media.** `/sd0/system/etc/auth/keys` is on a
   card that can be cloned or moved. A device secret on a transferable medium
   is not a device secret.
3. **Authentication without authorization.** Phase 18 §8 deferred it in one
   line -- "Multiple keys identify who; they do not gate *what*" -- and that
   line is still true. Every peer that authenticates gets the whole namespace,
   including the watched directory that *runs Lisp programs by design*.
4. **There is no second kind of secret yet, and there is about to be.** Phase
   19's R5 needs WLAN credentials, and putting them somewhere ad hoc would
   make three storage conventions where there should be one.

None of that is a networking problem, which is why it stopped fitting inside a
networking phase.

## 1. The framework

Three concepts that get used interchangeably and must not be:

| | question | property | today |
|---|---|---|---|
| **Identity** | who is this? | public, *asserted* | derived, not provisioned |
| **Authentication** | is that claim true? | secret, *verified* | works, over one shared key |
| **Authorization** | what may this identity do? | policy, *enforced* | **absent** -- all or nothing |

### 1.1 Identity has three scopes, and they age differently

* **Device** -- bound to this silicon. Survives reflashing, repurposing, and a
  new SD card. This is the **UID**.
* **Instance** -- bound to this box's *role*. A chess board rebuilt as a clock
  keeps its silicon and must not keep its name. This is the **name**.
* **Channel** -- bound to a wire. A local channel and a cable between two
  boards on one desk are trusted by the argument that trusts the boards; a
  network link is not. This already exists as `p9_link_t.auth_required`, and
  it is the one piece of this framework that was designed rather than grown.

Conflating device and instance is the mistake that makes "just write it once
and never touch it" sound right. It is right for exactly one of the three.

### 1.2 A node is both claimant and verifier

Every node attaches to peers (claimant) and accepts attaches from peers
(verifier). Today one key store serves both directions: `p9_auth_key_for()`
answers "what key do I prove myself with" and "what key do I check them
against" from the same file. That is a deliberate simplification and it holds
as long as a segment shares one key. It stops holding the moment §5.2's
per-peer list exists, and the split is called out there rather than discovered.

## 2. What is stored, and the write policy for each

The single most useful decision in this phase is that **"write once" is right
for one field out of five**, and applying it to all of them would be a
design error dressed as rigour.

| field | scope | secret | write policy | why |
|---|---|---|---|---|
| **UID** | device | no | write-once, or read-only from silicon | it *is* the device; changing it is lying |
| **name** | instance | no | freely rewritable | roles change; ARP caches survive it (the MAC does not follow a rename -- `kernel/identity.h`) |
| **device key** | device | **yes** | rotatable by deliberate gesture | a key that can never be rotated turns a compromise into a hardware replacement |
| **peer list** | instance | contains secrets | rewritable | adding and removing peers is routine operation, not reconfiguration |
| **WLAN credential** | instance | **yes** | rewritable | networks change more often than boards do |

**On key rotation.** Making the key immutable costs more than it buys.
Phase 18 §1's threat model already concedes physical access -- "the key sits
in flash; anyone holding the board has it" -- so immutability defends against
nobody who was not already inside the model, while guaranteeing that a leaked
key means a dead board. Rotation needs a deliberate gesture (`--force`), not a
prohibition.

## 3. Where it lives

### 3.1 What the tree actually has

Measured, not assumed:

* **`/flash0` is a read-only ROMdisk.** `flashdisk_write_blocks()` refuses and
  says so. It is not a candidate, and the current fallback key path
  (`/flash0/system/etc/p9key`) is therefore read-only by construction -- fine
  for a key baked into an image, useless for provisioning.
* **RP2350 flash is 4 MB and the image uses about 740 KB.** Roughly 3.2 MB is
  unused. The last 4 KB sector is a natural home, and a **UF2 writes only the
  blocks it contains**, so reflashing the OS should leave it untouched. That
  property is the whole design and it is an *assumption until measured on
  hardware* -- see §10.
* **RP2350 OTP carries a factory-programmed unique chip ID.** The UID needs no
  provisioning at all on that target: it is read, not written. This is a
  better `board_unique_id()` than the flash chip's id that phase 19 sketched.
* **AT24C32, 4 KB, read and write both implemented** -- but only where an RTC
  module is fitted. It survives even a full chip erase, which nothing else
  does. Not universal, so not the primary store; worth supporting as an
  alternate backend where present.
* **The SD card is transferable.** Moving a card moves the identity, which is
  wrong for a device and right for a role. This is the concrete argument for
  getting the key *off* `/sd0`, where phase 18 had to leave it.
* **QEMU has no silicon to bind to**, so a mounted identity volume is the
  honest analogue -- a second virtio-blk device carrying nothing else.

### 3.2 The seam: a block device on both

Model the store as a **`block_dev_t`**, which this tree already has. On
RP2350 it is a tiny block device backed by the reserved sector (read straight
from XIP; written via the bootrom from `.scratch_x`, which exists for exactly
this). On QEMU it is the second virtio-blk. Everything above -- parsing,
validating, provisioning, the toolset -- is then target-independent and fully
testable on QEMU before any hardware exists.

That is the same shape `netif_t` gave phase 19, and for the same reason: the
milestone that needs hardware should be a *driver*, not a project.

## 4. The record

Magic, version, length, CRC32, then typed fields. Three states must be
distinguishable, and a bare magic cannot tell the last two apart:

* **unprovisioned** -- erased flash, all `0xFF`
* **corrupt** -- magic present, CRC wrong: refuse and say so, never
  half-interpret
* **valid** -- parse it

Typed fields rather than a fixed struct, so adding WLAN credentials in I6 does
not invalidate records written by I2. Unknown field types are skipped and
counted, not fatal.

**Secrets must never leave over 9P.** `p9_auth_path_is_secret()` already
refuses to serve the key directory; the identity store must join it, and the
test for that belongs with the store rather than with the server.

**Resolution order**, extending `kernel/identity.c`'s existing ladder:

```
provisioned record  >  board file (CONFIG_NODE_*)  >  derived from build seed
```

The derivation phase 19 built becomes the floor -- what an unprovisioned board
answers to -- which is exactly what it should have been all along.

## 5. The three additions, discussed

### 5.1 Validated keys, and why a fingerprint is the useful part

"Validated" can mean two things and both are worth having:

**Validated at rest.** A provisioner must refuse to mint a key when
`random_is_hardware()` is false. A key that looks fine and is guessable is
worse than an error, and phase 18's N1 already built the entropy check that
answers this. Likewise refuse an all-zero or trivially patterned key supplied
by hand.

**Validated by a human.** The more useful notion. A secret you cannot display
is a secret you cannot confirm you installed correctly -- so the toolset shows
a **fingerprint**: the first 8 bytes of SHA-256 over the key, rendered as hex.
An operator compares the board's fingerprint with the one in their notes, and
neither end ever prints the key. `kernel/sha256.c` is already there. This is
cheap, and it is what turns "I think I flashed the right key" into a check.

### 5.2 A short list of keys granted access -- this is the authorization step

Today `keys` maps `uname -> key`, and *any* successful attach gets the entire
namespace: `/sd0`, `/flash0`, `/proc`, `/dev`, and the watched directory that
runs Lisp programs by design. That is phase 18's own §8 admission, unchanged.

The proposal is that the same list gains a **scope**, turning a list of
identities into a list of *grants*:

```
# name                key(hex)            aname        mode
clock-3f2a            a1b2...             /            rw
chess-91cc            44de...             /sd0/pgn     rw
lab-laptop            7f01...             /            ro
*                     0000...             /proc        ro
```

Two columns, and both are nearly free to enforce because the machinery exists:
`p9_handle_tattach()` **already compares `aname`** between the authenticated
afid and the attach, so restricting which subtree a peer may attach to is a
lookup, not a new mechanism. A read-only mode is a flag consulted by the write
paths that already check `read_only` on a mount.

Bounded like every other table in this kernel: **eight entries**. Revocation is
removing a line -- no revocation list, no expiry, and saying so is part of §7.

**This is where §1.2's simplification breaks.** Once the list describes *other*
nodes' keys, it stops being the answer to "what do I prove myself with". The
node's own key moves into the identity record (§2) and the list becomes purely
inbound. The two must not be the same file after this milestone.

### 5.3 WLAN credentials, and the one thing that makes them different

Phase 19's R5 needs an SSID and a passphrase for the CYW43. They belong in the
same store -- one convention, not three -- but they carry a property the auth
key does not:

> **An auth key could in principle be stored as a verifier. A WPA2 passphrase
> cannot.** The radio firmware needs the passphrase or the PSK derived from
> it, so it must be recoverable plaintext. It is therefore the most exposed
> secret on the device.

There is a concrete mitigation and it costs nothing: **store the derived
256-bit PSK, not the passphrase.** PBKDF2-HMAC-SHA1(passphrase, SSID, 4096) is
what WPA2 uses, the firmware accepts a PSK directly, and the stored value is
then useless for anything except this one SSID. That matters because people
reuse passphrases, and a board on a shelf should not be able to give away a
credential that opens something else. The derivation runs once, at
provisioning time, on the host -- not on the board, where 4096 iterations of
HMAC is a poor use of an RP2350.

One network, plus a note. Multiple SSIDs is a roaming feature and this is a
fixed-installation project.

## 6. The toolset

**On the device.** One command with subcommands, mirrored into Lisp so
`usr_init.lisp` can drive it:

```
identity                     report: uid, name, both sources, MAC, key fingerprint
identity name <name>         set the instance name
identity provision [--force] mint what is missing; refuse to overwrite without --force
identity key <hex>|--generate    install or mint a device key
peers                        list grants: name, fingerprint, aname, mode
peers add <name> <hex> [<aname>] [ro|rw]
peers remove <name>
wlan <ssid> <psk-hex>        install a network credential
```

**Never prints a secret.** Everything shows a fingerprint. That is not
security theatre -- it is what makes the commands safe to run over a 9P link
or paste into a bug report.

**On the host.** `tools/provision.py` builds a QEMU identity image, and derives
a WPA2 PSK from a passphrase so the board never sees the passphrase at all.

**And the bootstrap question, answered explicitly.** Provisioning a fresh
board cannot be authenticated -- there is nothing to authenticate with yet.
The trust anchor is **physical access via the console**, which is the honest
one and matches how `p9key` already works. Provisioning over the network is
refused, not merely discouraged: a device that will accept a new identity from
the network is a device with no identity.

## 7. Security implications, stated plainly

Written here because an unstated limit gets credited as a feature -- the same
reason phase 18 §1 exists.

**What this defends.** The exported namespace against anything else on the
same LAN that does not hold a key. With §5.2, it additionally bounds *what* a
holder of a particular key may reach.

**What it does not defend, and will not:**

* **Physical access.** Secrets sit in flash in the clear. Anyone holding the
  board has them. Inherited unchanged from phase 18 §1, and §2's rotation
  policy is written on the assumption that this is true.
* **Confidentiality on the wire.** 9P frames are cleartext. Authentication
  proves who attached; it hides nothing they then read. An observer on the
  segment sees every byte.
* **Traffic analysis**, obviously, and **denial of service** -- two connection
  slots are two connection slots.
* **A compromised peer.** A grant is to a key, and a key is a bearer token.
  Nothing distinguishes the legitimate holder from whoever copied it.

**What it depends on:**

* **Entropy.** The whole scheme rests on `random_bytes()`. If
  `random_is_hardware()` is false, the provisioner must refuse rather than
  mint (§5.1).
* **The nonce.** Replay across sessions is already defeated by the
  server-chosen nonce, and cross-identity replay by uname and aname being
  inside the MAC. Both are phase 18's and both survive this phase intact.

**Where it could go, in rough order of value per effort:**

1. **Encryption over the same seam.** A Noise-style handshake producing a
   session key, wrapped around a `p9_link_t` rather than inside the 9P server
   -- the transport-agnostic seam is what makes this a driver-shaped job
   rather than a protocol rewrite. This is the largest real gap.
2. **RP2350 secure boot and OTP-backed secrets.** The silicon supports signed
   images and OTP key storage. That moves the key out of readable flash and
   makes "anyone holding the board has it" false for the first time.
3. **Signing the identity record** with a key the record does not contain, so
   a swapped storage medium is detectable rather than silently authoritative.
4. **Expiry and rotation protocol**, so rotation stops requiring a visit.

## 8. Milestones

**I1 -- the record, and a store to put it in.** Format, parser, CRC, the three
states, and the `block_dev_t` seam. Target-independent.
*Verify:* on both QEMU targets, an unprovisioned store reads as unprovisioned,
a corrupted one is refused rather than half-read, an unknown field type is
skipped and counted, and a record written by I1 is read back byte-identical.

**I2 -- the QEMU identity disk and the host provisioner.** A second virtio-blk
device, `tools/provision.py`, and the resolution ladder of §4 wired into
`kernel/identity.c`.
*Verify:* a guest booted with a provisioned disk reports the provisioned name
and uid and says the source is the record; the same guest booted without one
falls back to the derived identity and says *that*; the existing suite is
unaffected either way.

**I3 -- the on-device toolset.** The commands of §6, their Lisp equivalents,
`/proc/node` extended, fingerprints everywhere and secrets nowhere.
*Verify:* no command prints a key; `identity provision` refuses a populated
store without `--force`; a rename does not move the MAC; the identity store is
refused over 9P by the same guard that refuses the key directory.

**I4 -- the device key moves into the record.** Off the SD card, out of the
image. The console key stays as the bootstrap override it was designed to be.
*Verify:* a node with a key only in its identity record authenticates to a
peer and accepts an attach; removing the card does not remove the ability to
do either; and phase 19's two-node test still passes unchanged.

**I5 -- grants: the peer list with scope.** §5.2, including the split of
inbound keys from this node's own key.
*Verify:* a peer granted `/sd0/pgn` cannot attach at `/`; a peer granted `ro`
is refused a `Twrite` and told why; a removed peer is refused immediately;
eight entries fill and the ninth is rejected with a message rather than
silently dropped.

**I6 -- WLAN credentials.** The PSK-not-passphrase rule of §5.3, the host-side
derivation, and the store. Lands with phase 19's R5 and is useless before it.
*Verify:* on the host, a known passphrase and SSID derive the PSK the standard
gives; on the device, the credential round-trips and is never printed.

**I7 -- the RP2350 backend.** OTP chip id for the UID; the reserved flash
sector for everything else; the write path from `.scratch_x`.
*Verify:* on hardware -- two boards report different uids; a provisioned
identity **survives a reflash** (§10's assumption, finally measured); an
interrupted write leaves the store readable as corrupt rather than as
plausible garbage.

**I8 -- documentation.** A README section that states §7 in full, the
provisioning walkthrough, and what a wrong fingerprint looks like.

**Sequencing.** I1-I6 need no hardware at all and can run alongside phase 19.
I7 wants the same bench visit as phase 19's R4.

## 9. Explicitly not in this phase

* **An encrypted transport.** §7's first extension, and a phase of its own: a
  handshake, a cipher, rekeying, and a threat model that actually promises
  confidentiality. Promising it badly is worse than not promising it.
* **Secure boot.** §7's second. It needs OTP fuse policy, a signing key that
  lives somewhere safe, and a bricking story -- none of which is identity.
* **Anything resembling a PKI.** No CA, no certificates, no chains. Eight
  peers on a LAN do not need a naming authority, and building one would be the
  clearest possible case of solving a problem this project does not have.
* **Expiry, revocation lists, and rotation protocols.** Removing a line from
  an eight-entry list is the revocation mechanism. §7 says so out loud.
* **Multiple WLAN networks.** §5.3.
* **User accounts.** `uname` identifies a *node*, not a person. A multi-user
  9P story is a different phase with a different threat model.

## 10. Risks, and what each looks like

* **The UF2-preservation assumption is wrong.** Looks like: a reflash silently
  erases the identity sector, and a board comes back as a different node. This
  is the load-bearing assumption of I7 and it is *measured there*, not before
  -- and if it fails, the AT24C32 backend (§3.1) becomes the primary store on
  boards that have one, and provisioning becomes part of flashing on those
  that do not.
* **Writing flash while executing from it.** Looks like: the board hangs, not
  fails. `.scratch_x` exists for this and interrupts must be off; getting it
  wrong is not a clean failure.
* **A wrong sector address bricks the image.** The address must be `ASSERT`ed
  in the linker script against the image's end, not written down in two places
  and trusted.
* **Splitting the key store breaks a working auth path.** I5 changes what
  `p9_auth_key_for()` means. Phase 19's two-node test is the regression that
  catches it, and it is named in I4's verification for that reason.
* **The toolset leaks a secret.** Looks like a key in a log, a `/proc` file,
  or a bug report. The rule is one line -- fingerprints only -- and the test
  for it belongs in I3 rather than in a review.
* **Scope creep toward a security product.** Looks like certificates. §9
  exists to be pointed at.
