# Crypto performance — the standing "beat Rust" invariant

**The invariant:** every C crypto primitive on the ZClassic23 consensus path
must stay **at least as fast as its Rust/blst counterpart**, and may only ever
get **faster** — never regress. "Beat Rust" is a durable, gated property, not a
one-time push. This document is the standing contract; the mechanism is the
`check-crypto-perf` gate.

Consensus verify **logic is frozen** (see
[`CONSENSUS_PARITY_DOCTRINE.md`](./CONSENSUS_PARITY_DOCTRINE.md)). This whole
surface is **measurement + gating only** — it calls the production verify/hash
predicates and touches no validity predicate. **No Rust is linked into the
shipped binary** (the no-external-deps rule); the comparison is against pinned /
cited Rust numbers, never a live Rust link.

## The three pieces

| piece | path | role |
|---|---|---|
| Benchmark | `build/bin/zclassic23 -bench-crypto-vs-rust` (`src/main.c`) — `make bench-crypto-vs-rust` | Times every consensus-path C primitive as a **median of N** ns/op, prints machine-readable `CRYPTOPERF <key> <ns> <ops/s>` lines, appends medians to `docs/bench-history.csv`. |
| Baseline | `tools/crypto_perf_baseline.csv` | Per primitive: `c_ns_baseline` (a **ceiling that may only shrink**), `rust_ns_baseline`, `gate_mode`, `rust_source`. |
| Gate | `tools/scripts/check_crypto_perf.sh` — `make check-crypto-perf` | Measures C live and enforces the ratchet + the ratio-vs-Rust rule below. |

The benchmark cannot go **hollow-fast**: every primitive is TEETH-checked
(valid → true, perturbed → false / avalanche / square-consistency) before any
number is recorded, and the same teeth run independently in the fast test pool
as the `crypto_perf_selftest` group (`lib/test/src/test_crypto_perf_selftest.c`).
A no-op hash / always-true verify / operand-returning multiply fails there
before the gate can ratchet a broken primitive.

## The gate rules

Run `make check-crypto-perf` in a **quiet context** (it is deliberately **NOT**
in the default `make lint` aggregate — microbench timing flakes under CI load).
Margin default **20%** (`ZCL_CRYPTO_PERF_MARGIN`).

1. **RATCHET (always, hard).** `measured_c_ns <= c_ns_baseline * (1 + margin)`.
   The baseline is a **ceiling**; as we optimise we **shrink** it (never raise
   it). A self-regression beyond the margin **FAILS**. This is the core
   protection — our C crypto can only get faster.

2. **RATIO vs Rust.** `ratio = measured_c_ns / rust_ns_baseline`.
   - `gate_mode=beat` — we are ahead. **Hard-FAIL if `measured_c_ns >=
     rust_ns_baseline`** (we lost the lead). This is **flake-proof by
     construction**: a `beat` row is only valid when
     `rust_ns_baseline >= c_ns_baseline * (1 + margin)`, so any run that passes
     the ratchet is necessarily below Rust. The gate asserts that headroom
     (`FAIL_CONFIG` if a `beat` row is mis-pinned).
   - `gate_mode=behind` — we are behind (or only slimly ahead, kept in this
     bucket for flake safety). **No hard fail** (that would red main today);
     the gate prints a loud **`BEHIND RUST — optimize <primitive>`** line and
     relies on the ratchet for monotonic improvement toward parity. This is the
     target list for the crypto-beat-rust workflow.

## Current standing (baseline 2026-07-13, AMD Ryzen 9 7950X3D, pure-C23 `-v3`)

Median-of-5 × 120 ms, pinned slightly above max-observed for ratchet headroom.
Rust numbers are **CITED** published reference points (no Rust is linked); see
`tools/crypto_perf_baseline.csv` for the source of each.

| primitive | C ns/op | Rust ns/op | ratio | verdict |
|---|--:|--:|--:|---|
| equihash-200-9 verify | ~122k | 160k (zcashd C++ ref) | 0.72 | **BEAT** |
| secp256k1 ECDSA verify | ~49k | 90k (RustCrypto k256) | 0.55 | **BEAT** |
| BLAKE2b-512 (1 KiB) | ~0.85k | 1.1k (blake2b scalar) | 0.78 | **BEAT** |
| SHA256 (1 KiB) | ~0.47k | 0.68k (SHA-NI) | 0.70 | **BEAT** |
| SHA3-256 (1 KiB) | ~1.7k | 2.0k (keccak scalar) | 0.83 | behind* (slim) |
| BLS12-381 Fp mul | ~56 | 45 (blst asm) | 1.23 | behind |
| groth16 BLS12-381 output verify | ~7.7M | 3.0M (librustzcash) | 2.57 | behind |
| BLS12-381 Ate pairing | ~1.83M | 0.6M (blst) | 3.05 | behind |
| ed25519 verify | ~1.49M | 55k (ed25519-dalek) | 27.0 | behind (no windowing) |

\* `sha3-256` is marginally ahead but pinned `behind` so its hard-fail can't
flake near the boundary; optimise it past a 25% margin to promote it to `beat`.

**Where we beat Rust:** Equihash verify, ECDSA verify (vendored libsecp256k1 vs
RustCrypto), BLAKE2b (AVX2 path), SHA256 (SHA-NI path). **Optimize targets
(behind):** the pairing / Fp-mul / Groth16 elliptic-curve stack (blst
hand-asm) and ed25519 (needs wNAF + precompute). Groth16 being behind is
expected today and must NOT red main — the loud line keeps it visible.

### SHA256 ISA dispatch — runtime only, never compile-time

SHA-256 selects between a portable C transform and an Intel SHA-NI transform at
**runtime**, via CPUID plus a known-answer test against the portable reference
(`detect_sha_ni`). The hardware transform is emitted from the baseline
translation unit by a per-function `__attribute__((target("sha,sse4.1")))` —
the same shape `blake2b_avx2.c`, `keccak_avx512.c` and `sha3_256_x4.c` use.

Two properties depend on that being a runtime decision, and a compile-time
guard breaks both:

* The shipped `-march=x86-64-v3` does **not** define `__SHA__`. A
  `#ifdef __SHA__` around the transform deletes it from every released binary
  while `/proc/cpuinfo` still reports `sha_ni` — silently, since the portable
  fallback stays correct, merely ~4× slower on every block hash, merkle root,
  txid and sighash. This was the state of the tree until the guard was removed.
* `tools/scripts/check_reproducible_build.sh` deliberately does not set
  `ZCL_NATIVE`, so the binary must be byte-identical across build hosts.
  Runtime dispatch keeps it so; compile-time selection would make the artifact
  depend on the builder's CPU.

Measured at the shipped `-march=x86-64-v3` on the baseline host, portable vs
SHA-NI within the same binary:

| workload | portable | SHA-NI | speedup |
|---|--:|--:|--:|
| SHA-256, 1 KiB | 1934.2 ns | 470.5 ns | 4.11× |
| double-SHA-256, 80 B (block hash) | 364.3 ns | 91.4 ns | 3.99× |
| double-SHA-256, 64 B (merkle combine) | 362.2 ns | 89.3 ns | 4.06× |

`ZCL_NATIVE=1` (`-march=native`) buys nothing further — 474.9 ns for the 1 KiB
case, i.e. within noise of the portable-march build — because the target
attribute, not the `-march`, is what emits the SHA-NI opcodes. The dispatch
stays a runtime decision precisely so the binary remains byte-identical across
build hosts for `tools/scripts/check_reproducible_build.sh`.

The `test_sha256_isa_parity` group is the differential oracle for the two
transforms and the standing regression guard: on a CPU that advertises SHA-NI
it FAILS if the node does not actually install SHA-NI, so a re-introduced
compile-time guard cannot go unnoticed again.

## Optimizing safely

Any optimization to a consensus crypto primitive must stay **bit-identical** to
the frozen verify logic. The differential parity oracle
(`lib/test/differential/`, run with `make check-groth16-parity`) proves an
optimized implementation returns the exact same accept/reject verdict as the
frozen reference on adversarial inputs. It compiles
`lib/sapling/src/bls12_381.c` straight from source and replays a frozen corpus
of point encodings (canonical + non-canonical infinity, out-of-field x,
on-curve non-subgroup), malformed proofs, and crafted single/batch
verifications against `groth16_parity_golden.bin` +
`groth16_decode_corpus.bin`. A single verdict flip fails the gate.

Parity is defined **against our own frozen behavior**, not against
librustzcash: the corpus deliberately pins the quirks this chain accepts —
notably the BLS12-381 non-canonical infinity encoding (infinity flag set with
dirty trailing bytes), which librustzcash rejects and we ACCEPT. Never
"fix" a pinned quirk; that is a consensus break.

Flow: optimise → `make check-groth16-parity` → prove bit-identity via the
`crypto_perf_selftest` teeth → re-run `make check-crypto-perf` → **shrink** the
baseline in `tools/crypto_perf_baseline.csv` (and flip `behind`→`beat` once you
clear the margin). The ratchet then holds the new line forever.
`run_parity_oracle.sh record` re-freezes the golden and is legitimate ONLY
after a deliberate, full-history-replay-approved consensus change.

## Landed: fixed-base public-input scalar-mul

`vk_x = IC[0] + sum(input[i]*IC[i+1])` multiplies bases that are CONSTANT for
the life of a verifying key, yet the naive path paid a full 256-bit
double-and-add per non-zero input on every verify. `groth16_vk_build_combs()`
precomputes a windowed table per IC point once at param load
(`lib/sapling/src/params_init.c`), and each per-verify scalar-mul becomes 64
table lookups plus adds with zero doublings. Tables are read-only after the
build, so one VK stays shareable across verify threads.

Measured with `make bench-groth16-comb ITERS=40` (7950X3D, `-O2 -march=x86-64-v3`,
median of three runs — both paths timed in one process against the same key):

| circuit | inputs | naive | fixed-base | speedup |
|---|--:|--:|--:|--:|
| sapling OUTPUT verify | 5 | 6.98 ms | 4.93 ms | 1.41x (-29%) |
| sapling SPEND verify | 7 | 7.74 ms | 5.05 ms | 1.53x (-35%) |

Cost: 144 KiB of table per IC point — ≈3.0 MB resident for all three consensus
keys (SPEND 7 inputs, OUTPUT 5, sprout-groth16 9), built once in ~25 ms total
at param load. On allocation failure the naive path is kept: same verdicts,
original speed. The remaining verify time is the four Miller loops plus the
final exponentiation, which is where the next optimization has to go.

The `tools/crypto_perf_baseline.csv` ratchet still carries the pre-optimization
`groth16 output verify` number: shrinking it requires a `make
check-crypto-perf` run against a full build with real params, not this
micro-bench.
