#ifndef LUGALOS_KERNEL_RANDOM_H
#define LUGALOS_KERNEL_RANDOM_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Unpredictable bytes — N1, plan/phase18_networking_and_auth.md.
 *
 * This exists for the nonce in phase 18's `Tauth` challenge, and a
 * challenge-response is only as good as its nonce: repeat one and a captured
 * response replays forever. That is the whole reason this is a kernel service
 * with a measurement attached rather than three lines inside the auth code.
 *
 * ## Not every target has an entropy source, and this says so
 *
 * RP2350 has a ring oscillator whose sampled phase noise is a real physical
 * source. The QEMU targets have nothing of the kind, and rather than
 * pretending otherwise (a deterministic generator dressed up as random is
 * worse than an honest absence, because it looks fine in every test),
 * random_is_hardware() reports which case you are in. The auth path is
 * expected to consult it and refuse to run a network-facing gate on a target
 * that cannot produce a nonce worth the name.
 */

/* Fills `out` with `len` unpredictable bytes.
 *
 * On a target with a hardware source this is that source, whitened through
 * SHA-256 (see kernel/random.c for the construction and why the raw bits are
 * never handed out directly). On a target without one it is derived from the
 * timer and a counter -- adequate for tests, useless against an adversary,
 * and flagged as such by random_is_hardware(). */
void random_bytes(void *out, uint32_t len);

/* True if the bytes come from a physical entropy source. */
bool random_is_hardware(void);

/* Human-readable name of the source, for boot messages and /proc. */
const char *random_source(void);

/* Measures the RAW source -- bias, longest run, and serial correlation --
 * over `bits` samples, at the spacing random_bytes() uses and at no spacing
 * at all, and prints the comparison. `bits` is clamped to something a shell
 * command can finish.
 *
 * The raw bits are what needs measuring: reporting on the whitened output
 * would only ever demonstrate that SHA-256 works. Returns 0 if the source
 * looks usable (or if there is no hardware source to test, in which case it
 * says so and stops). */
int random_selftest(unsigned bits);

#endif /* LUGALOS_KERNEL_RANDOM_H */
