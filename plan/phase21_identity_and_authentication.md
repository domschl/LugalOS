# Phase 21 — Identity and authentication, as a thing in itself

**Status: planned 2026-08-26; I1-I6 and I8 complete 2026-08-29; I7a and I7b
complete 2026-09-01 (§3.3); the phase is done bar two hardware verify
items named in I7b.** Independent of the phase it
grew out of.

**Amended 2026-09-01, before starting I7, in response to a design review that
asked the right question:** are a dedicated identity store and its own record
format re-inventing the wheel, when a filesystem already exists that could
hold the same bytes in a subdirectory? The answer, worked through in §3.3, is
half yes. The valuable half of the idea is **getting `/flash0` out of the
binary and into its own independently flashable region** -- it is over half of
every reflash, it holds ~40 KB of files in a 512 KB image, and it cannot be
written at all today. That was adopted, and it retires this phase's single
riskiest assumption by construction rather than by measurement.

The other half -- storing the record *as files in that filesystem* -- was
declined, on four grounds, none of them "we already built the other thing":
NOR flash erases in 4 KB sectors while FAT32 writes 512-byte blocks, so every
small write becomes read-modify-erase-write with the FAT as a permanent hot
spot; an interrupted FAT32 write corrupts *the filesystem*, where §8's own
verify list asks for "readable as corrupt, not as plausible garbage" and a
CRC'd 4 KB record delivers exactly that in one erase-and-program; the
RAM-resident write path for one sector is small and reviewable where the whole
FAT32 write path is not (§3.2, and note the failure mode there is a hang, not
an error); and `node_identity_init()` runs at `kernel/main.c:161`, before
`palloc_init()` at 206 and `vfs_server_init()` at 217, so sourcing identity
from a filesystem means reordering boot or making identity lazy. The record
format itself is 385 lines plus a 128-line header, with a pluggable
`block_dev_t` backend -- small enough that replacing it would not be a
simplification.

One objection from the same review changed the shape of the answer and is
worth recording, because the first draft got it wrong: putting the filesystem
and the identity sector in *one* region would merely move the coupling, since
updating the filesystem would then require rewriting the identity, and the
flashing host is precisely the party that must not hold it. Hence **three**
segments, not two.

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

**I8 done (2026-08-29), taken out of order: I7 is blocked on hardware
access (below), and I8's own three deliverables don't need it.** A new
`## Identity and Authentication` section in `README.md` -- §7 restated in
full as "Security implications, stated plainly" (what it defends, what it
does not and will not, what it depends on, where it could go, all four as
their own subsections rather than one wall of text); a five-step
provisioning walkthrough (host-side `tools/provision.py`, interactive
`identity provision`/`identity key`, a scoped `peers add`, and a WLAN
credential installed via the derived PSK) with every command output
**verified against a live QEMU boot rather than hand-typed** -- fingerprints,
UIDs, and the exact column widths of `peers`' table all come from a real
session, not an approximation of one; and a "What a wrong fingerprint
looks like" section showing a matching pair and a mismatched pair side by
side, with the operational answer (stop and re-derive from the source,
don't try variations) stated as plainly as §7 itself is.

Next: I7 (the RP2350 flash-sector backend), blocked until bench access to
real RP2350 hardware exists again -- see its own entry above for exactly
what's ready to go the day that's true.

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

* **`/flash0` is a read-only ROMdisk, and it is inside the binary.**
  `flashdisk_write_blocks()` refuses and says so, but the deeper reason is
  the layout: `tools/create_flash_fs.py` compiles the FAT32 image into a C
  array (`g_flash_fs_start[524288]`) that lands in `.rodata`. It is not a
  candidate, and the current fallback key path
  (`/flash0/system/etc/p9key`) is therefore read-only by construction -- fine
  for a key baked into an image, useless for provisioning.
* **More than half of every reflash is that ROMdisk** (re-measured
  2026-09-01, on `rp2350-wifi`): `g_flash_fs_start` sits at `0x1003c4a4`,
  `__flash_binary_end` at `0x100f596c`, so the image is ~982 KB of which
  **512 KB is a filesystem holding about 40 KB of files**. Without it the OS
  is ~470 KB. RP2350 flash is 4 MB, so ~3 MB is unused either way.
* **The last 4 KB sector is a natural home for the record**, and nothing
  grows into it -- its address stays fixed however the OS and the filesystem
  change size. What the original version of this section then leaned on was
  that a **UF2 writes only the blocks it contains**, so reflashing the OS
  *should* leave that sector alone. §3.3 replaces that hope with a layout
  where it is true by construction.
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
RP2350 it is a tiny block device backed by the reserved sector: read straight
from XIP, written by a routine that must not itself be executing from the
flash it is erasing. **The `.scratch_x` answer this section used to give is
stale** -- phase 15 §1.2 handed both scratch banks to the page allocator, and
`linker/rp2350.ld` now carries a hard `ASSERT(SIZEOF(.scratch_x) == 0 &&
SIZEOF(.scratch_y) == 0)` precisely so nothing quietly places there again. I7
therefore has to say where its RAM-resident erase/program routine lives and
pay for it deliberately: either take a page back out of the heap here (and
adjust `_heap_end` in the same commit, as that `ASSERT`'s own comment
instructs) or mark the routine for copy-to-RAM some other way. Naming it is
the point; discovering it at link time is not a plan. On QEMU it is the second
virtio-blk. Everything above -- parsing,
validating, provisioning, the toolset -- is then target-independent and fully
testable on QEMU before any hardware exists.

That is the same shape `netif_t` gave phase 19, and for the same reason: the
milestone that needs hardware should be a *driver*, not a project.

### 3.3 The flash layout: three independently flashable segments

Decided 2026-09-01, replacing "one reserved sector at the end of a
monolithic image". Two separate observations forced it.

**First:** the ROMdisk is half the image (§3.1), it cannot be written, and it
is rebuilt and reflashed on every OS change even though its contents change
almost never. Lifting it out of `.rodata` into its own flash region makes it
independently flashable, halves what an OS reflash rewrites, and gives every
application a place to keep bytes -- not just this phase.

**Second, and this is what settles the *number* of segments:** merging the
filesystem and the identity sector into one region would simply move the
coupling rather than remove it. Updating the filesystem would then mean
rewriting the identity too, so the flashing host would have to *supply* a
device's identity in order to update its files -- and the flash host is
exactly the party that must not hold it. Identity therefore gets a region of
its own, and the build never emits a UF2 that covers it.

```
0x10000000  OS image            1.5 MB reserved   (~470 KB used today)
0x10180000  flash-fs             512 KB
0x10200000  (unallocated)         ~2 MB           -- a writable app FS later
0x103FF000  identity sector         4 KB          -- last sector
```

The three artifacts this produces differ in update cadence *and* in
distribution properties, which is the real argument for the split:

| artifact | per device? | changes |
|---|---|---|
| `lugalos.uf2` | identical everywhere | every build |
| `flashfs.uf2` | identical everywhere | rarely |
| the identity sector | **unique per device** | once, and normally never flashed by a host at all |

On RP2350 the record is minted *by the device* -- OTP chip id for the UID, and
`random_bytes()` over the ring oscillator's `RANDOMBIT` for the key, behind
phase 18's N1 entropy gate -- so a flash host never sees it, which is what §7
wants regardless.
Where fleet provisioning genuinely needs a pre-minted record, it becomes a
deliberate one-off `provision-<serial>.uf2` writing only those 4 KB, which
this layout supports and the merged one did not.

`tools/elf2uf2_rp2350.py` already takes `--base BASE_ADDR`, so emitting a
second UF2 at a different address is nearly free.

**Two things this layout has to get right, both new risks in §10.**
Boundaries must be sector-aligned, because the bootrom erases whole 4 KB
sectors to write 256-byte chunks and a segment ending mid-sector can erase
into its neighbour -- 64 KB alignment makes that structurally impossible.
And with the filesystem no longer inside the ELF, the linker can no longer
`ASSERT` against it directly, so the bases must come from **one** build-system
variable feeding both `linker/rp2350.ld` and UF2 generation, with `ASSERT`s
that each segment ends below the next base.

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

**I7 -- the RP2350 backend.** Unblocked 2026-09-01: bench access to a Pico 2 W
returned during phase 19 R5, and §3.3 replaced this milestone's riskiest
assumption with a layout. Split in two, because the halves are different kinds
of work with different blast radii -- one is build-system, one is a driver --
and only the second needs the record format to exist.

**I7a's boundary measurement: PASS, 2026-09-01 -- and this is the number
this phase has been building on since I2 without ever having it.** Run
before writing any of I7a's code, because if it had failed the rest of the
design would have changed. Method, with no firmware modification at all: a
4 KB pattern (word *i* = `0x5A5A0000 | i`, top bit clear so the Lisp `peek`
primitive prints it as a positive integer) was converted to a UF2 based at
`0x103FF000` with `tools/uf2conv.py -b ... -f 0xE48BFF57`, every block's
target address checked to lie inside the top sector *before* it went near
the board, and flashed. The board still booted -- the OS at `0x10000000` was
untouched -- and `(peek ...)` read the pattern back. Then `lugalos.uf2`
(982 KB, ending at `0x100f596c`) was reflashed over the top, and the pattern
was read again: **byte-identical, at word 0 and word 1023 and fifteen points
between.**

So an OS reflash does not reach the top sector, and §10's narrowed risk is
answered with data rather than with hope. Note what makes seventeen samples
sufficient rather than a shortcut: NOR erase is per-4-KB-sector and
all-or-nothing, so a sector that had been caught by an erase would read
`0xFFFFFFFF` *everywhere*, and word 0 and word 1023 alone would have caught
it. The baseline read before any of this is worth keeping too -- the sector
was erased (`0xFFFFFFFF`) except its very last word, which held
`0xEFEFEFEF`, presumably left by the MicroPython image used to prove the
hardware good during R5. Something already writes near the top of this
flash, which is exactly why the region wants reserving rather than assuming.

**Method note, for whoever runs the next hardware check here.** The
verification was sampled rather than exhaustive because a 64-iteration named
`let` exhausts the Lisp node pool on this persona *on a fresh boot* --
`NODE_POOL_SIZE` is 1024 on RP2350 against 4096 elsewhere (`user/lisp/lisp.c`),
which is the deliberate memory budget, not a regression. The interpreter
degrades exactly as P6 §6.4's test says it should, but the message
("Further evaluation will produce wrong results until the shell restarts")
means a loop-based check can report a *pass* that means nothing. Individual
`(peek ...)` calls allocate little enough to be trustworthy; loops here are
not. Worth an in-firmware comparison routine once I7b gives this region a
`block_dev_t`.

**I7a -- the three-segment flash layout (§3.3).** Lift the FAT32 image out of
`.rodata` into its own flash region, reserve the identity sector at the top of
flash, and emit `lugalos.uf2` and `flashfs.uf2` separately. Bases come from one
build-system variable feeding both `linker/rp2350.ld` and UF2 generation, never
hand-copied. Nothing about identity is required for this milestone to land, and
every persona benefits from it.
*Verify:* on hardware -- the OS image drops to ~470 KB and boots; `flashfs.uf2`
alone updates `/flash0` without touching the OS; **flashing `lugalos.uf2`
leaves the other two regions byte-identical**, checked by writing a known
pattern into the identity sector first and reading it back after (this is the
sector-boundary erase question, and it is the first thing to measure, because
everything else here rests on it); a linker `ASSERT` fires, naming the cause,
when a segment is made to overflow its region.

**I7a done, 2026-09-01.** All four verify items measured on a Pico 2 W:

* **The OS image is 481,792 bytes**, down from 1,006,080 -- the 512 KB
  filesystem is no longer linked in (`g_flash_fs_start` is gone from the ELF
  entirely), and `__flash_binary_end` now sits at `0x10075964`, comfortably
  under `__flashfs_base`.
* **`flashfs.uf2` alone updates `/flash0`** and leaves the OS untouched:
  a marker file added to `tools/sd_root`, the filesystem image reflashed on
  its own, the marker read back from `/flash0` -- with `/proc/buildid`
  unchanged. Ninja does not relink `lugalos.elf` for it either, which is the
  same property seen from the build side.
* **`lugalos.uf2` alone leaves both other regions byte-identical** --
  `/flash0` still reads, and the identity sector still holds the pattern from
  the boundary measurement above.
* **The linker `ASSERT` fires and names the cause.** Forced by moving
  `LUGALOS_FLASHFS_BASE` down to `0x10010000`: *"the OS image has grown past
  its segment and would overwrite the flash-fs -- raise LUGALOS_FLASHFS_BASE
  in cmake/flash_layout.cmake, or shrink the image"*.

**One thing I7b must not be surprised by:** the board used for this work now
has the 4 KB test pattern (`0x5A5A0000 | i`) sitting at
`LUGALOS_IDENTITY_BASE`, deliberately left there as standing evidence that
an OS reflash does not touch it. That sector is therefore *not* erased flash
on this board. **Measured in I7b, and not what this paragraph first
predicted:** `idstore_read()` reports `IDSTORE_UNPROVISIONED`, not
`IDSTORE_CORRUPT`. §4 reserves CORRUPT for *magic present, CRC wrong*, and a
sector whose magic does not match is simply "nothing here" regardless of what
it contains. That is the right split -- CORRUPT should mean "this was a
record and something happened to it", which a foreign pattern never was.

`cmake/flash_layout.cmake` is the single definition of the map, reaching the
compiler as definitions and the linker as `--defsym`. Both RP2350 images link
against `linker/rp2350.ld` and therefore both need those symbols -- found by
`minimal_rp2350.elf` failing to link, which is the right way to find it.

**The independence check earned its place immediately.** `flashfs.uf2` was
first produced inside `lugalos.elf`'s `POST_BUILD`, which only runs when that
target relinks -- so changing a file under `tools/sd_root` regenerated
`flashfs.bin` and left `flashfs.uf2` stale, and the build would hand you an
old filesystem to flash. Nothing about the code was wrong; the *build graph*
was, and only flashing a real board and looking for a marker that never
arrived showed it. It has its own `OUTPUT`/`DEPENDS` rule now. Worth
remembering the next time a verify item looks like a formality.

Two things fell out that were not on the list. The split introduces a new
failure mode -- a board flashed with `lugalos.uf2` alone has erased flash
where the filesystem should be -- so `flashdisk_get_device()` now recognises
all-`0xFF` and says exactly that, rather than letting FAT32 report a bad boot
sector. And the QEMU targets are deliberately unchanged: they keep the
embedded C array, since they have no flash map to place a segment in, so
`tests/runner.py` still exercises the same `/flash0` it always did (296/296).

**I7b -- the identity backend.** OTP chip id for the UID; the reserved sector
for everything else; a RAM-resident erase/program routine whose home is stated
and paid for (§3.2 -- `.scratch_x` is no longer available, and taking a heap
page back means adjusting `_heap_end` in the same commit).
`identity_store_device()` (`kernel/identity.h`) is the one hook this fills in;
everything above it -- the record format, the toolset, the auth wiring, grants,
WLAN storage -- is already written and tested and does not change for this to
land.
*Verify:* on hardware -- two boards report different uids; a provisioned
identity survives a reflash (now by construction, still measured, since a
layout is only as good as its boundaries); an interrupted write leaves the
store readable as *corrupt* rather than as plausible garbage; `wifi join` with
no arguments reads its credentials from the record (plan/open_issues.md's
standing entry, which is what closes when this lands).

Both remain hardware-only by construction -- "two boards", "survives a
reflash", "an interrupted write" cannot be brought forward into QEMU the way
I1-I6 were, which is exactly why I1-I6 were sequenced to need no hardware at
all: this was to be the *only* thing blocked, not a reason the rest of the
phase waited. That sequencing paid off. The one claim retired here is the old
version's second "concrete unknown" -- whether a UF2 reflash preserves blocks
it does not contain. That is no longer the premise the design rests on; I7a's
own verify list measures the narrower, answerable question of whether a
segment's flash erases past its boundary.

**I7b done, 2026-09-01.** `identity_store_device()` is filled in and the
board keeps its identity across reflashes.

* **OTP works, and `board_unique_id()` returns true on hardware for the first
  time**: `uid: 413ed1010581b362 (silicon)`, where every board previously fell
  back to the build seed. The guarded OTP window is indexed as
  `uint16_t[row]`, taken from the SDK's own use of it
  (`hardware_powman/powman.c` reads LPOSC_CALIB that way) rather than inferred
  from the datasheet's register tables, because a wrong stride there yields
  plausible garbage rather than a fault. All-zero or all-ones across the four
  CHIPID rows is treated as a failed read, so a batch of parts can never all
  claim the same "unique" id -- it falls back to the derived floor instead.
  **This changes the node name**: `rp2350-wifi-4941` (build seed) became
  `rp2350-wifi-90f9` (silicon), and the name is the 9P uname.
* **A provisioned identity survives a reflash** -- verified across two full
  OS reflashes, `name source: record` each time. I7a made that structural;
  this is the measurement on top of it.
* **The write path works**: `identity provision`, `identity key --generate`
  and `wlan <ssid> <psk>` all land in flash and read back after a reboot
  (`key fingerprint: f778e265c553c8f3`).
* **`wifi join` with no arguments reads its credentials from the record**, and
  the resulting association is real: with the operator's own PSK stored,
  `cyw43: joining "DOSC"...` with nothing typed, then frames actually arriving
  (`rx 19 frames` within seconds) and the host pinging the board 8/8 at 0%
  loss once `net-config` gave it an address. That is what
  plan/open_issues.md's standing entry was waiting for.

  Worth separating the two runs, because only the pair proves anything. The
  first used a deliberately fake all-zero PSK and proved the *plumbing* --
  the SSID came from the record rather than from the command line. It also
  reported `joined`, which is how the join bug below was found. The second,
  with the real credential, is the positive control: same code path, and this
  time the frame counters move. A single run either way would have been
  consistent with a broken check.

**Where the RAM-resident code lives, and what it cost.** Not `.scratch_x`
(phase 15 owns those banks) and not a section of its own either: `*(.ramfunc)`
is collected *inside* `.data`. A separate section would have needed its own
entry in boot_header.S's `address_mapping_table` -- placing the symbols
correctly while leaving the bytes uncopied, which executes whatever SRAM held
-- and, being executable next to a writable section, made `ld` union them into
a single RWX `PT_LOAD` and say so. Inside `.data` it is copied by the existing
walk and costs its own size and nothing else. The RWX warning that remains is
suppressed at this one link with the reasoning recorded in CMakeLists.txt:
unlike the `.binary_info` case the linker script fixes properly, here both
flags are intended and neither can be dropped.

**The first write on hardware wrote its record perfectly and hung the
board.** Worth recording in full, because the symptom pointed at the wrong
component. `identity provision` went silent and needed a physical BOOTSEL --
but the next boot reported `name source: record`, so the flash write, the
verify and the sector had all been fine. The casualty was the USB console:
erase-plus-program takes ~100 ms with interrupts off, and this board's CDC is
serviced by a task and its interrupts. Starved that long the device controller
stops answering and does not come back -- which `tests/hw/README.md` already
recorded for this driver ("a real USB bus reset, not a close-and-reopen").
`lsusb` confirmed it: the same device number throughout, no re-enumeration,
the host still holding a device that had stopped responding.

The fix is not a workaround but the honest completion of the operation: warn
*before* the write (with a delay, since the next thing the code does is stop
scheduling for a tenth of a second), then reboot. `flash_rp2350_write_sector()`
returns with XIP in the bootrom's generic 03h mode anyway -- slower than what
boot set up, until a reset -- so continuing would have meant a dead console
*and* a degraded system. Verified: the board now writes, reboots itself, and
comes back with the record intact, no hands.

**Two verify items are NOT closed, and neither is hand-waved:**

* **Two boards reporting different uids.** Only one Pico 2 W is on the bench
  with this persona; the other is under the Pico-Clock-Green baseboard and
  reflashing it would disturb a working setup for one number. The read is
  from a factory-programmed per-die field, so the risk is low -- but "low
  risk" and "measured" are different claims and this is the latter's absence.
* **An interrupted write leaving the store readable as corrupt.** Genuinely
  cutting power mid-erase needs timing a physical unplug inside a ~100 ms
  window. Not attempted. Note that the failure this would probe is now
  *narrower* than when it was written: a torn write leaves an erased or
  half-programmed sector whose magic does not match, which reads as
  UNPROVISIONED (see the correction above) rather than as a plausible record
  -- so the dangerous outcome, silently accepting damaged data, needs the
  magic to survive while the fields do not.

**I8 -- documentation.** A README section that states §7 in full, the
provisioning walkthrough, and what a wrong fingerprint looks like.

**Sequencing.** I1-I6 need no hardware at all and ran alongside phase 19. I8
was done ahead of I7 -- its own three deliverables (§7 in full, a provisioning
walkthrough, what a wrong fingerprint looks like) are all QEMU-verifiable, so
nothing about it needed to wait for the one milestone that does.

I7 wanted the same bench visit as phase 19's R4 and now has it. **I7a before
I7b**, and the order is not arbitrary: I7a's boundary measurement is what
tells us whether the layout the rest of this phase now assumes is real, and it
needs no identity code at all to run -- write a pattern into the reserved
sector, flash the OS, read it back. If that fails, I7b's design changes before
a line of it is written, and §10's AT24C32 fallback is where it goes. I7a is
also useful on its own to every persona, so it is not wasted even in that
case.

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

* **A segment's flash erases past its own boundary.** Looks like: a reflash
  silently erases the identity sector, and a board comes back as a different
  node. This is §3.3's narrowed version of what used to be stated as "the
  UF2-preservation assumption", and narrowing it is the point -- the old form
  ("does a UF2 leave blocks it doesn't contain alone?") was a property of the
  whole toolchain and could only be believed or disbelieved, while this one is
  a property of two addresses and is measured directly by I7a's first verify
  item. The mechanism to watch is that the bootrom erases whole 4 KB sectors
  to write 256-byte chunks, so a segment ending mid-sector reaches into its
  neighbour; 64 KB-aligned boundaries make that structurally impossible. If it
  fails anyway, the AT24C32 backend (§3.1) becomes the primary store on boards
  that have one, and provisioning becomes part of flashing on those that do
  not.
* **The segment bases drift apart.** Looks like: an image that boots on one
  build and bricks on the next, or a `flashfs.uf2` that lands on top of the
  OS. With the filesystem no longer inside the ELF, the linker cannot
  `ASSERT` against it directly, so nothing structural stops the linker script
  and the UF2 step from disagreeing. One build-system variable feeds both, and
  each segment `ASSERT`s that it ends below the next base. This replaces the
  old "a wrong sector address bricks the image" entry, which assumed a single
  image with a single end to assert against.
* **Writing flash while executing from it.** Looks like: the board hangs, not
  fails. Interrupts must be off and the routine must not be running from the
  flash it is erasing -- and note that `.scratch_x`, which earlier drafts of
  this document named for the job, is no longer available: phase 15 §1.2 gave
  both scratch banks to the page allocator and `linker/rp2350.ld` asserts they
  stay empty. I7b has to name and pay for its RAM-resident home. Getting this
  wrong is not a clean failure.
* **Splitting the key store breaks a working auth path.** I5 changes what
  `p9_auth_key_for()` means. Phase 19's two-node test is the regression that
  catches it, and it is named in I4's verification for that reason.
* **The toolset leaks a secret.** Looks like a key in a log, a `/proc` file,
  or a bug report. The rule is one line -- fingerprints only -- and the test
  for it belongs in I3 rather than in a review.
* **Scope creep toward a security product.** Looks like certificates. §9
  exists to be pointed at.
