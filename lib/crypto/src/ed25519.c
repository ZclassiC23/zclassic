/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Ed25519 signature verification — pure C23 implementation.
 * Twisted Edwards curve: -x^2 + y^2 = 1 + d*x^2*y^2
 * Field: GF(2^255-19), TweetNaCl-style 16-limb representation.
 *
 * ── Constant-time audit ────────────────────────────────────
 *
 * This file is **verify-only**. There is no `ed25519_sign` in the tree
 * (consensus paths only need verify; JoinSplit/Sapling signing happens
 * via RedJubjub in lib/sapling). All inputs to ed25519_verify are
 * public — signature, message, public key — so the threat model that
 * makes Curve25519 DH and jub_scalar_mul timing-critical does not
 * apply here. The CT properties below are belt-and-suspenders.
 *
 * Properties confirmed:
 *   - sel25519: branchless mask cswap (same as curve25519.c)
 *   - scalarmult: cswap-driven Montgomery-like ladder; every iteration
 *     runs the full point_add(q,p) + point_add(p,p) sequence, no
 *     conditional adds keyed on bits
 *   - Final compare: `diff |= t[i] ^ sig[i]` (XOR-OR accumulator), NOT
 *     a memcmp early-exit
 * - S<L canonical-S check: byte-walked accumulator with mask
 *     selection; rejects malleable signatures pre-scalarmult
 *
 * Branches on data (acceptable, public values only):
 *   - unpackneg: `if (neq25519(chk, num)) ...` — operates on the
 *     decompressed public key; leak is OK (pubkey is public)
 *   - LOG_FAIL early-returns: branches on verify outcomes (a public
 *     signature is either valid or not — observable from the result)
 *
 * **If a sign function is ever added** it MUST keep the secret nonce
 * out of timing: branch-free clamp, ladder/comb scalarmult, no
 * data-dependent table lookups, ed25519-donna or ref10 patterns. See
 * vendor/tor/src/ext/ed25519/{donna,ref10}/ for known-CT references. */

#include "crypto/ed25519.h"
#include "crypto/sha512.h"
#include "util/log_macros.h"
#include <string.h>

typedef int64_t gf[16];

static const gf gf0 = {0};
static const gf gf1 = {1};

static const gf D = {
    0x78a3, 0x1359, 0x4dca, 0x75eb,
    0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7,
    0xfe73, 0x2b6f, 0x6cee, 0x5203
};

static const gf D2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6,
    0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e,
    0xfce7, 0x56df, 0xd9dc, 0x2406
};

static const gf I = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee,
    0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d,
    0xdf0b, 0x4fc1, 0x2480, 0x2b83
};

static const uint8_t BASE_POINT[32] = {
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66
};

static void car25519(gf o)
{
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b)
{
    int64_t c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t o[32], const gf n)
{
    gf m, t;
    memcpy(t, n, sizeof(gf));
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int64_t b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, (int)(1 - b));
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void unpack25519(gf o, const uint8_t n[32])
{
    for (int i = 0; i < 16; i++)
        o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff; /* mask sign bit (bit 255) */
}

static void A(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void Z(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void M(gf o, const gf a, const gf b)
{
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++)
        t[i] += 38 * t[i + 16];
    memcpy(o, t, sizeof(gf));
    car25519(o);
    car25519(o);
}

static void S(gf o, const gf a) { M(o, a, a); }

static void inv25519(gf o, const gf a)
{
    gf c;
    memcpy(c, a, sizeof(gf));
    for (int i = 253; i >= 0; i--) {
        S(c, c);
        if (i != 2 && i != 4) M(c, c, a);
    }
    memcpy(o, c, sizeof(gf));
}

static void pow2523(gf o, const gf a)
{
    gf c;
    memcpy(c, a, sizeof(gf));
    for (int i = 250; i >= 0; i--) {
        S(c, c);
        if (i != 1) M(c, c, a);
    }
    memcpy(o, c, sizeof(gf));
}

static int par25519(const gf a)
{
    uint8_t d[32];
    pack25519(d, a);
    return d[0] & 1;
}

static int neq25519(const gf a, const gf b)
{
    uint8_t c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    int v = 0;
    for (int i = 0; i < 32; i++) v |= c[i] ^ d[i];
    return v != 0;
}

/* Extended coordinates: (X, Y, Z, T) where x=X/Z, y=Y/Z, T=X*Y/Z */
typedef gf gep[4];

static void set_identity(gep p)
{
    memset(p[0], 0, sizeof(gf));
    memcpy(p[1], gf1, sizeof(gf));
    memcpy(p[2], gf1, sizeof(gf));
    memset(p[3], 0, sizeof(gf));
}

static void point_add(gep p, const gep q)
{
    gf a, b, c, d, e, f, g, h, t;
    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, D2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);
    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

static void cswap(gep p, gep q, int b)
{
    for (int i = 0; i < 4; i++)
        sel25519(p[i], q[i], b);
}

static void pack_point(uint8_t r[32], const gep p)
{
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= (uint8_t)(par25519(tx) << 7);
}

static int unpackneg(gep r, const uint8_t p[32])
{
    gf t, chk, num, den, den2, den4, den6;
    set_identity(r);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);

    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);

    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) M(r[0], r[0], I);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) return -1;

    if (par25519(r[0]) == (p[31] >> 7))
        Z(r[0], gf0, r[0]);

    M(r[3], r[0], r[1]);
    return 0;
}

static void scalarmult(gep p, gep q, const uint8_t s[32])
{
    set_identity(p);
    for (int i = 255; i >= 0; i--) {
        int b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        point_add(q, p);
        point_add(p, p);
        cswap(p, q, b);
    }
}

static const uint64_t L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0x10
};

static void modL(uint8_t r[32], int64_t x[64])
{
    for (int i = 63; i >= 32; i--) {
        int64_t carry = 0;
        int j;
        for (j = i - 32; j < i - 12; j++) {
            x[j] += carry - 16 * x[i] * (int64_t)L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    int64_t carry = 0;
    for (int j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * (int64_t)L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; j++)
        x[j] -= carry * (int64_t)L[j];
    for (int i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

static void reduce(uint8_t r[64])
{
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = (int64_t)(uint64_t)r[i];
    memset(r, 0, 64);
    modL(r, x);
}

/* Constant-time check that S (sig[32..63], 32 LE bytes) is canonical:
 *   S < L, where L is the Ed25519 group order. Required by RFC 8032
 *   §5.1.7 and by Zcash consensus (malleable S-values split consensus
 *   from zcashd). Compare from most-significant byte down; accumulate
 *   "less-than" only on bytes where all higher bytes are still equal. */
static bool ed25519_S_is_canonical(const uint8_t S[32])
{
    uint32_t lt = 0;
    uint32_t eq = 1;
    for (int i = 31; i >= 0; i--) {
        uint32_t sb = S[i];
        uint32_t lb = (uint32_t)L[i]; /* L[i] fits in one byte */
        /* sb < lb  iff  (sb - lb) underflows to a value with bit 31 set */
        uint32_t is_lt = ((sb - lb) >> 31) & 1u;
        /* sb == lb iff  (sb ^ lb) == 0 */
        uint32_t is_eq = (((sb ^ lb) - 1u) >> 31) & 1u;
        lt |= eq & is_lt;
        eq &= is_eq;
    }
    (void)eq;
    return lt != 0u;
}

bool ed25519_verify(const uint8_t sig[64],
                    const uint8_t *msg, size_t msg_len,
                    const uint8_t pk[32])
{
    gep q;

    /* Reject the identity point (all-zero pubkey) */
    {
        uint8_t zero[32] = {0};
        if (memcmp(pk, zero, 32) == 0)
            LOG_FAIL("ed25519", "pk is identity (all zero)");
    }

    /* Canonical-S check (RFC 8032 §5.1.7, Zcash consensus). Must happen
     * BEFORE the scalar mul so malleable signatures are rejected even if
     * the decompression/point math would otherwise accept them. */
    if (!ed25519_S_is_canonical(sig + 32))
        LOG_FAIL("ed25519", "S >= L (non-canonical signature scalar)");

    /* Decompress -A from public key */
    if (unpackneg(q, pk) != 0)
        LOG_FAIL("ed25519", "pubkey decompression (unpackneg) failed");

    /* h = SHA-512(R || pk || msg) mod L */
    uint8_t h[64];
    struct sha512_ctx hs;
    sha512_init(&hs);
    sha512_write(&hs, sig, 32);
    sha512_write(&hs, pk, 32);
    sha512_write(&hs, msg, msg_len);
    sha512_finalize(&hs, h);
    reduce(h);

    /* Compute [S]B + [h](-A) and check == R */
    gep sb;
    {
        gep bp;
        if (unpackneg(bp, BASE_POINT) != 0)
            LOG_FAIL("ed25519", "base point decompression failed");
        Z(bp[0], gf0, bp[0]);
        Z(bp[3], gf0, bp[3]);
        scalarmult(sb, bp, sig + 32);
    }

    gep ha;
    {
        gep q2;
        memcpy(q2, q, sizeof(gep));
        scalarmult(ha, q2, h);
    }

    point_add(sb, ha);

    uint8_t t[32];
    pack_point(t, sb);

    int diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= t[i] ^ sig[i];

    if (diff != 0)
        LOG_FAIL("ed25519",
                 "signature verify: [S]B - [h]A != R (signature mismatch, msg_len=%zu)",
                 msg_len);
    return true;
}
