# p256-m (vendored)

* **Upstream:** https://github.com/mpg/p256-m (Manuel Pégourié-Gonnard)
* **Version:** commit `44af59e0cff5d3b1d653bc333814077ef830e1bd` (2022-06-15, upstream
  `master`; the project has no release tags). The same code ships in Mbed TLS / TF-PSA-Crypto
  as `drivers/p256-m`.
* **Files taken:** `p256-m.c`, `p256-m.h`, `LICENSE`, `apache-2.0.txt`.
* **License:** Apache License 2.0 (`LICENSE`, `apache-2.0.txt`).
* **Local changes:** one line in `p256-m.c`: the inline-assembly path for 32-bit Arm is also
  excluded on AArch64 (`!defined(__aarch64__)`). Upstream enables it for any `__ARM_ARCH >= 6`
  with profile 'A', which an AArch64 compiler matches too, so the host harnesses
  (`tests/host`) failed to build on an ARM64 host. The RISC-V targets are unaffected.
  `p256-m.h` is byte-identical to upstream:

  ```
  eb6d2e9f970d584ea91c5cd9a38afd287f1488eb280dc01f8e3ebeab4c3a94c9  p256-m.c (upstream)
  afde827539a853188801143e122488352ec1e62b8c2359b36a98cd9353954b8b  p256-m.c (as vendored)
  63b619b90fe87e063d11a97983768a1eea671d102859a9f4ca5dd591186e3b9b  p256-m.h
  ```

* **Used for:** P-256 ECDSA (TLS certificates, update-image signatures) and ECDH, behind
  `kernel/include/kernel/crypto.h`. `kernel/crypto.c` supplies the library's one hook,
  `p256_generate_random()`, from `random_bytes()`. The wrappers refuse key generation and
  signing when `random_is_hardware()` is false, because a predictable ECDSA nonce gives away
  the private key.
* **Constant time:** built without `MUL64_IS_CONSTANT_TIME`, so 32x32 products are assembled
  from 16-bit pieces. That is p256-m's conservative default for a core whose 64-bit multiply
  timing is unknown. Defining it per chip, once the TRMs confirm a data-independent `mulhu`
  (Hazard3, ESP32-C6, ESP32-P4), is a measured speed-up for later.
* **Updating:** replace the two source files, re-apply the local change, update this note and
  the hashes, and run
  `make -C tests/host asan` plus `cryptotest` on a board.
