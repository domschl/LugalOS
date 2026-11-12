# Monocypher (vendored)

* **Upstream:** https://github.com/LoupVaillant/Monocypher
* **Version:** 4.0.3 (tag `4.0.3`, commit `ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f`)
* **Files taken:** `src/monocypher.c`, `src/monocypher.h`, `src/optional/monocypher-ed25519.c`, `src/optional/monocypher-ed25519.h`, `LICENCE.md`.
* **License:** dual CC0-1.0 / BSD-2-Clause, at the user's choice (`LICENCE.md`).
  LugalOS uses it under BSD-2-Clause.
* **Local changes:** none. The files are byte-identical to upstream:

  ```
  f1f838cdd483bdebe0df0ff5c5ed60535e496f769c6a2f933ac4c0b114207123  monocypher.c
  fcaf6ed771358bb4f40fba016f6518ae86ec02b1b877d2cc35ad92d3a26fd7b3  monocypher.h
  ce0d2f8e32ca8f66398ba5b3456cc74327c3eff14e7b950ce7d57be9025cc453  monocypher-ed25519.c
  3a3035181f991a158d0e1c7567258f0bae8ba0f1f23c5512b4a1db1b3c9730ce  monocypher-ed25519.h
  ```

* **Used for:** X25519, ChaCha20, Poly1305, SHA-512, and RFC 8032 Ed25519, behind `kernel/include/kernel/crypto.h`.
  Nothing outside `kernel/crypto.c` calls it directly.
* **Constant time:** Monocypher's code has no secret-dependent branches or indexes. Like
  every portable curve implementation, it assumes the CPU's multiply takes the same time for
  every operand.
* **Updating:** replace the two source files with a newer release, update this note and the
  hashes, and run `make -C tests/host asan` plus `cryptotest` on a board.
