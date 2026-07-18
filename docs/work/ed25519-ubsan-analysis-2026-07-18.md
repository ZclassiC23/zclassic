# UBSan left-shift findings in lib/crypto/src/ed25519.c — analysis (no code change)

Date: 2026-07-18. Lane: read-only analysis. Trigger: ASan/UBSan profile run
(`make t-asan`, see docs/BUILD.md "Sanitizer profiles") flagged UBSan
"left shift" reports at `lib/crypto/src/ed25519.c:81` and `:304`.
Ed25519 is consensus-ADJACENT (JoinSplit signature verification), so per
`docs/CONSENSUS_PARITY_DOCTRINE.md` this lane is ANALYSIS ONLY — nothing
ships without the validation bar at the bottom.

## 1. The exact flagged expressions

`lib/crypto/src/ed25519.c:75-83` (TweetNaCl carry):

```c
static void car25519(gf o)              /* typedef int64_t gf[16]; (line 42) */
{
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;                /* line 81 — FLAGGED */
    }
}
```

`lib/crypto/src/ed25519.c:296-308` (TweetNaCl scalar reduction mod L):

```c
x[j] += carry - 16 * x[i] * (int64_t)L[j - (i - 32)];
carry = (x[j] + 128) >> 8;
x[j] -= carry << 8;                     /* line 304 — FLAGGED */
```

Provenance: the file header (lines 1-35) describes a TweetNaCl-style
16-limb GF(2^255-19) implementation, verify-only. This is NOT the ref10
`<< 24`-into-the-sign-bit pattern hypothesized in the lane brief; both
flags are the **TweetNaCl carry idiom**: `int64_t << {8,16}` where the
left operand can be **negative**.

## 2. Type-level cause

- Operand types: `c` and `carry` are `int64_t`. No integer-promotion
  subtlety applies (promotion only matters for types narrower than int);
  the operative rule is the signed left-shift rule itself.
- Why the operands go negative: gf limbs are signed by design. `Z()`
  (line 131-134) produces negative limbs; `M()` accumulates products of
  signed limbs; `car25519` is called on exactly those values (M calls it
  twice per multiply, pack25519 three times per pack, on field elements
  straight out of point_add's `Z(e, b, a)`). So `o[i] < 0` ⇒ `c < 0` ⇒
  `c << 16` left-shifts a negative signed value. In modL, line 302's
  `- 16 * x[i] * L[...]` term drives x[j] negative, so
  `carry = (x[j] + 128) >> 8` goes negative ⇒ `carry << 8` same class.
- Why it's UB: C11/C17 §6.5.7¶4 — left shift of a negative signed value
  is undefined behavior. UBSan's shift-base check diagnoses it.
- C23 wrinkle: C23 §6.5.8 redefines signed left shift as modular
  (`a·2^b mod 2^N`, the C++20 rule), and the project builds with
  `-std=c23` (Makefile:343), so at the language level the behavior is
  now *defined* (wrapping). gcc 14.2's UBSan, however, still instruments
  negative signed left shifts under the pre-C23 rule — hence the report.
  The finding is real as a sanitizer/portability defect even though the
  C23 text no longer calls it UB.
- Overflow direction (positive) is NOT reachable: `c = o[i] >> 16` on
  int64_t bounds |c| ≤ 2^47, so `c << 16` always fits int64_t; modL
  carries are far smaller (|carry| ≪ 2^24). The only defect class at
  these lines is negative-base left shift — there is no
  shift-into/past-sign-bit overflow case here.
- Why benign in practice on gcc x86-64: both expressions lower to a
  plain `shlq`; hardware shift is modular two's-complement, identical to
  the C23-defined result. No gcc −O3 pass exploits negative-left-shift
  "UB" the way signed-overflow UB is exploited (and in C23 mode there is
  no UB to exploit). Production codegen = wrapping = the intended
  TweetNaCl carry math. The report is noise we should silence properly,
  not a live miscompilation.

## 3. Upstream idiom — cast to unsigned before shift

- ref10 as vendored in tor (`vendor/tor/src/ext/ed25519/ref10/`, a
  submodule not covered by workspace grep) never left-shifts a signed
  type: every byte-load shift casts first, e.g.
  `result |= ((crypto_uint64) in[1]) << 8;`
  (fe_frombytes.c:9-21, sc_reduce.c:10-21, sc_muladd.c:10-21).
  libsodium and ed25519-donna use the same unsigned-load idiom.
- TweetNaCl upstream leaves `c << 16` as-is (targets "real compilers"),
  which is why every TweetNaCl-derived file trips this UBSan class.
- Accepted embedder fix, and the one proposed here:
  `(int64_t)((uint64_t)c << n)`.
- Bit-exactness proof for ALL inputs, not just reachable ones:
  1. int64→uint64 conversion is defined mod 2^64 and preserves the bit
     pattern (C23 mandates two's complement);
  2. unsigned `<< n` is defined mod 2^64 — a pure bit shift;
  3. uint64→int64 of an out-of-range value is implementation-defined
     (not UB); gcc on x86-64 defines it as modular wrap.
  Composite = `c·2^n mod 2^64` for every one of the 2^64 inputs =
  exactly the C23-defined value of `c << n` = exactly what gcc emits
  today. For the reachable range (|c| ≤ 2^47 / |carry| ≪ 2^24) the
  result is the plain mathematical product with bits unchanged. Codegen:
  one `shlq` either way — instruction-identical (verifiable by objdump
  diff of the object file, listed in the validation bar).

## 4. Other instances of the pattern (counts only)

- Signed-negative left shift (the actual flagged class): 3 sites total
  in lib/crypto — ed25519.c:81, ed25519.c:304, and the identical
  TweetNaCl twin `lib/crypto/src/curve25519.c:50` (`o[i] -= c << 16;`
  inside its own car25519). A fix naturally covers the twin; curve25519
  will flag the moment an X25519 path runs under the profile.
- `<< 24` in lib/crypto: 8 hits in 4 files — ALL already on unsigned
  operands (`(uint32_t)` casts in aes256.c:34/40/59,
  chacha20poly1305.c:19, blake2s.c:33; unsigned params in common.h
  bswap32/bswap64; `1u << 24` literal). The hypothesized ref10 sign-bit
  pattern does not exist in our tree.
- `<< 16` in lib/crypto: 8 hits in 5 files; signed ones are exactly the
  two car25519 sites above (the rest are `(int64_t)uint8 << 8`-style
  loads of nonnegatives or uint32-cast).
- vendor (non-submodule): 0 hits for `<< 24`. tor ref10/donna are
  systematically unsigned as shown above.
- Right shifts of possibly-negative int64_t (e.g. `o[i] >> 16`) are
  implementation-defined pre-C23 / defined in C23, gcc-arithmetic in
  practice; UBSan does not flag them. Out of scope.

## 5. Minimal fix sketch (NOT applied — analysis lane)

Exact lines, exact changes, nothing else:

```c
/* lib/crypto/src/ed25519.c:81 */
-    o[i] -= c << 16;
+    o[i] -= (int64_t)((uint64_t)c << 16);

/* lib/crypto/src/ed25519.c:304 */
-    x[j] -= carry << 8;
+    x[j] -= (int64_t)((uint64_t)carry << 8);

/* natural twin, same class — include in the same lane or explicitly defer */
/* lib/crypto/src/curve25519.c:50 */
-    o[i] -= c << 16;
+    o[i] -= (int64_t)((uint64_t)c << 16);
```

No signature, struct, constant, or control-flow changes. Comment in the
file header's audit block may note "shifts via unsigned casts: UBSan
shift-base clean, C23-modular semantics" if desired.

## 6. Validation bar before any merge (doctrine-mandated)

1. UBSan clean: `make t-asan ONLY=test_crypto` (group contains the
   RFC 8032 vectors calling ed25519_verify at test_crypto.c:114/128/142
   plus negative tests :157/:166) with `UBSAN_OPTIONS=halt_on_error=1`;
   then `make asan-ci` green.
2. Bit-exactness: existing ed25519/curve25519 test vectors pass
   unchanged; objdump diff of ed25519.o (and curve25519.o if the twin is
   included) pre/post patch shows identical instructions — decisive
   codegen-identical evidence, near-zero cost.
3. `make lint` green, including the `check-consensus-parity` E13 gate —
   this file is consensus-adjacent; the gate decides what shape is
   permissible.
4. `make test_parallel` green (regression floor).
5. Full-history parity replay per
   `docs/CONSENSUS_PARITY_DOCTRINE.md` (replay-canary surfaces:
   `make replay-canary-anchor` / `make replay-canary-genesis`).
   Ed25519 verifies JoinSplit signatures; the doctrine's replay bar is
   unconditional for this file class even when codegen is provably
   identical. `make tsan-ci` unaffected (no threading change) but run
   for completeness.
6. Why fix at all given C23 defines it: (a) silence shift-base noise so
   real UBSan findings don't drown in the asan-ci lane; (b) portability
   for any pre-C23 consumer of lib/crypto; (c) zero cost — provably
   identical codegen.
