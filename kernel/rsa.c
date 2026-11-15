/* RSA signature verification (PKCS#1 v1.5 and PSS) up to 4096-bit, integer-only. */

#include "kernel/rsa.h"
#include "kernel/crypto.h"
#include "kernel/sha256.h"
#include "kernel/sha512.h"
#include <string.h>

#define RSA_MAX_WORDS (RSA_MAX_BYTES / 4u)

static void bytes_to_words(uint32_t *w, uint32_t nwords, const uint8_t *b, uint32_t blen) {
    memset(w, 0, nwords * sizeof(uint32_t));
    for (uint32_t i = 0; i < blen; i++) {
        uint32_t pos = blen - 1u - i;
        w[pos >> 2] |= (uint32_t)b[i] << ((pos & 3u) * 8u);
    }
}

static void words_to_bytes(uint8_t *b, uint32_t blen, const uint32_t *w) {
    for (uint32_t i = 0; i < blen; i++) {
        uint32_t pos = blen - 1u - i;
        b[i] = (uint8_t)(w[pos >> 2] >> ((pos & 3u) * 8u));
    }
}

static int cmp_words(const uint32_t *a, const uint32_t *b, uint32_t w) {
    for (uint32_t i = w; i > 0; i--) {
        if (a[i - 1u] != b[i - 1u]) return a[i - 1u] < b[i - 1u] ? -1 : 1;
    }
    return 0;
}

static void sub_words(uint32_t *a, const uint32_t *b, uint32_t w) {
    uint64_t borrow = 0;
    for (uint32_t i = 0; i < w; i++) {
        uint64_t diff = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)diff;
        borrow = (diff >> 63) & 1u;
    }
}

static uint32_t mont_n0inv(uint32_t n0) {
    uint32_t inv = 1u;
    for (int i = 0; i < 5; i++) {
        inv = inv * (2u - n0 * inv);
    }
    return (uint32_t)(0u - inv);
}

/* z = a * b * R^(-1) mod n, where R = 2^(32*w). */
static void mont_mul(uint32_t *z, const uint32_t *a, const uint32_t *b,
                     const uint32_t *n, uint32_t w, uint32_t n0inv) {
    uint32_t t[RSA_MAX_WORDS + 2u];
    memset(t, 0, (w + 2u) * sizeof(uint32_t));

    for (uint32_t i = 0; i < w; i++) {
        uint64_t carry = 0;
        uint32_t bi = b[i];
        for (uint32_t j = 0; j < w; j++) {
            uint64_t p = (uint64_t)a[j] * bi + t[j] + carry;
            t[j] = (uint32_t)p;
            carry = p >> 32;
        }
        uint64_t s = (uint64_t)t[w] + carry;
        t[w] = (uint32_t)s;
        t[w + 1u] = (uint32_t)(s >> 32);

        uint32_t m = t[0] * n0inv;
        uint64_t p0 = (uint64_t)m * n[0] + t[0];
        carry = p0 >> 32;
        for (uint32_t j = 1; j < w; j++) {
            uint64_t p = (uint64_t)m * n[j] + t[j] + carry;
            t[j - 1u] = (uint32_t)p;
            carry = p >> 32;
        }
        s = (uint64_t)t[w] + carry;
        t[w - 1u] = (uint32_t)s;
        t[w] = t[w + 1u] + (uint32_t)(s >> 32);
        t[w + 1u] = 0;
    }

    if (t[w] != 0 || cmp_words(t, n, w) >= 0) {
        sub_words(t, n, w);
    }
    memcpy(z, t, w * sizeof(uint32_t));
}

/* out = sig^e mod n. Returns false on invalid inputs. */
static bool rsa_modexp(uint8_t *out, const uint8_t *sig, uint32_t sig_len,
                       const uint8_t *n, uint32_t n_len,
                       const uint8_t *e, uint32_t e_len) {
    if (n_len == 0 || n_len > RSA_MAX_BYTES || sig_len != n_len || e_len == 0 || e_len > 8u) {
        return false;
    }
    if ((n[n_len - 1u] & 1u) == 0) {
        return false; /* modulus must be odd for Montgomery */
    }
    /* Reject all-zero exponent */
    uint32_t e_nonzero = 0;
    for (uint32_t i = 0; i < e_len; i++) e_nonzero |= e[i];
    if (e_nonzero == 0) return false;

    uint32_t w = (n_len + 3u) / 4u;
    uint32_t nw[RSA_MAX_WORDS];
    uint32_t sw[RSA_MAX_WORDS];
    bytes_to_words(nw, w, n, n_len);
    bytes_to_words(sw, w, sig, sig_len);

    if (cmp_words(sw, nw, w) >= 0) return false;

    uint32_t n0inv = mont_n0inv(nw[0]);

    /* Compute R^2 mod n by doubling 1 mod n for 2*32*w bits. */
    uint32_t r2[RSA_MAX_WORDS];
    memset(r2, 0, w * sizeof(uint32_t));
    r2[0] = 1u;
    uint32_t total_bits = 2u * 32u * w;
    for (uint32_t i = 0; i < total_bits; i++) {
        uint32_t carry = 0;
        for (uint32_t j = 0; j < w; j++) {
            uint32_t next_c = r2[j] >> 31;
            r2[j] = (r2[j] << 1) | carry;
            carry = next_c;
        }
        if (carry || cmp_words(r2, nw, w) >= 0) {
            sub_words(r2, nw, w);
        }
    }

    uint32_t base[RSA_MAX_WORDS];
    uint32_t acc[RSA_MAX_WORDS];
    uint32_t one[RSA_MAX_WORDS];
    memset(one, 0, w * sizeof(uint32_t));
    one[0] = 1u;

    mont_mul(base, sw, r2, nw, w, n0inv);
    mont_mul(acc, one, r2, nw, w, n0inv);

    bool started = false;
    for (uint32_t bi = 0; bi < e_len; bi++) {
        uint8_t eb = e[bi];
        for (int bit = 7; bit >= 0; bit--) {
            if (started) {
                mont_mul(acc, acc, acc, nw, w, n0inv);
            }
            if ((eb >> bit) & 1u) {
                started = true;
                mont_mul(acc, acc, base, nw, w, n0inv);
            }
        }
    }

    mont_mul(acc, acc, one, nw, w, n0inv);
    words_to_bytes(out, n_len, acc);
    return true;
}

static uint32_t hash_len_for(int hash_alg) {
    switch (hash_alg) {
    case RSA_HASH_SHA256: return 32u;
    case RSA_HASH_SHA384: return 48u;
    case RSA_HASH_SHA512: return 64u;
    default: return 0u;
    }
}

static void hash_buf(int hash_alg, const uint8_t *in, uint32_t in_len, uint8_t *out) {
    if (hash_alg == RSA_HASH_SHA256) sha256(in, in_len, out);
    else if (hash_alg == RSA_HASH_SHA384) sha384(in, in_len, out);
    else if (hash_alg == RSA_HASH_SHA512) sha512(in, in_len, out);
}

bool rsa_verify_pkcs1_v15(const uint8_t *n, uint32_t n_len,
                          const uint8_t *e, uint32_t e_len,
                          int hash_alg, const uint8_t *digest, uint32_t digest_len,
                          const uint8_t *sig, uint32_t sig_len) {
    uint32_t hlen = hash_len_for(hash_alg);
    if (hlen == 0 || digest_len != hlen) return false;

    static const uint8_t DI_SHA256[] = {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03,
        0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
    };
    static const uint8_t DI_SHA384[] = {
        0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03,
        0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30
    };
    static const uint8_t DI_SHA512[] = {
        0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03,
        0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40
    };
    const uint8_t *di = (hash_alg == RSA_HASH_SHA256) ? DI_SHA256 :
                        (hash_alg == RSA_HASH_SHA384) ? DI_SHA384 : DI_SHA512;
    uint32_t di_len = 19u;

    if (n_len < 11u + di_len + hlen) return false;

    uint8_t em[RSA_MAX_BYTES];
    if (!rsa_modexp(em, sig, sig_len, n, n_len, e, e_len)) return false;

    uint32_t ps_len = n_len - 3u - di_len - hlen;
    if (em[0] != 0x00u || em[1] != 0x01u) return false;
    for (uint32_t i = 0; i < ps_len; i++) {
        if (em[2u + i] != 0xffu) return false;
    }
    if (em[2u + ps_len] != 0x00u) return false;
    if (memcmp(em + 3u + ps_len, di, di_len) != 0) return false;
    if (memcmp(em + 3u + ps_len + di_len, digest, hlen) != 0) return false;
    return true;
}

static void mgf1(int hash_alg, const uint8_t *seed, uint32_t seed_len,
                 uint8_t *mask, uint32_t mask_len) {
    uint32_t hlen = hash_len_for(hash_alg);
    uint8_t buf[64 + 4];
    uint8_t tmp[64];
    memcpy(buf, seed, seed_len);
    uint32_t off = 0;
    uint32_t counter = 0;
    while (off < mask_len) {
        buf[seed_len + 0u] = (uint8_t)(counter >> 24);
        buf[seed_len + 1u] = (uint8_t)(counter >> 16);
        buf[seed_len + 2u] = (uint8_t)(counter >> 8);
        buf[seed_len + 3u] = (uint8_t)(counter);
        hash_buf(hash_alg, buf, seed_len + 4u, tmp);
        uint32_t take = mask_len - off;
        if (take > hlen) take = hlen;
        memcpy(mask + off, tmp, take);
        off += take;
        counter++;
    }
}

bool rsa_verify_pss(const uint8_t *n, uint32_t n_len,
                    const uint8_t *e, uint32_t e_len,
                    int hash_alg, const uint8_t *digest, uint32_t digest_len,
                    const uint8_t *sig, uint32_t sig_len) {
    uint32_t hlen = hash_len_for(hash_alg);
    if (hlen == 0 || digest_len != hlen || n_len == 0 || n_len > RSA_MAX_BYTES) return false;

    uint32_t slen = hlen;
    if (n_len < hlen + slen + 2u) return false;

    uint8_t em[RSA_MAX_BYTES];
    if (!rsa_modexp(em, sig, sig_len, n, n_len, e, e_len)) return false;

    /* Determine top-bit mask for modBits - 1 */
    uint8_t top = n[0];
    int msb = 7;
    while (msb > 0 && ((top >> msb) & 1u) == 0) msb--;
    uint32_t mod_bits = (n_len - 1u) * 8u + (uint32_t)msb + 1u;
    uint32_t em_bits = mod_bits - 1u;
    uint32_t em_len = (em_bits + 7u) / 8u;

    const uint8_t *emp = em;
    if (em_len < n_len) {
        if (em[0] != 0x00u) return false;
        emp = em + 1;
    }
    if (em_len < hlen + slen + 2u) return false;
    if (emp[em_len - 1u] != 0xbcu) return false;

    uint32_t unused_bits = 8u * em_len - em_bits;
    uint8_t top_mask = (uint8_t)(0xffu << (8u - unused_bits));
    if (unused_bits > 0 && (emp[0] & top_mask) != 0) return false;

    uint32_t db_len = em_len - hlen - 1u;
    const uint8_t *h = emp + db_len;

    uint8_t db[RSA_MAX_BYTES];
    mgf1(hash_alg, h, hlen, db, db_len);
    for (uint32_t i = 0; i < db_len; i++) db[i] ^= emp[i];
    if (unused_bits > 0) db[0] &= (uint8_t)(~top_mask);

    uint32_t ps_len = db_len - slen - 1u;
    for (uint32_t i = 0; i < ps_len; i++) {
        if (db[i] != 0x00u) return false;
    }
    if (db[ps_len] != 0x01u) return false;

    const uint8_t *salt = db + ps_len + 1u;
    uint8_t mprime[8 + 64 + 64];
    memset(mprime, 0, 8);
    memcpy(mprime + 8, digest, hlen);
    memcpy(mprime + 8 + hlen, salt, slen);

    uint8_t hprime[64];
    hash_buf(hash_alg, mprime, 8u + hlen + slen, hprime);
    return memcmp(h, hprime, hlen) == 0;
}
