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
| SHA3-256 (1 KiB) | ~1.7k | 2.0k (keccak scalar) | 0.83 | behind* (slim) |
| BLS12-381 Fp mul | ~56 | 45 (blst asm) | 1.23 | behind |
| SHA256 (1 KiB) | ~2.0k | 0.68k (SHA-NI) | 2.86 | behind (no SHA-NI) |
| groth16 BLS12-381 output verify | ~7.7M | 3.0M (librustzcash) | 2.57 | behind |
| BLS12-381 Ate pairing | ~1.83M | 0.6M (blst) | 3.05 | behind |
| ed25519 verify | ~1.49M | 55k (ed25519-dalek) | 27.0 | behind (no windowing) |

\* `sha3-256` is marginally ahead but pinned `behind` so its hard-fail can't
flake near the boundary; optimise it past a 25% margin to promote it to `beat`.

**Where we beat Rust:** Equihash verify, ECDSA verify (vendored libsecp256k1 vs
RustCrypto), BLAKE2b (AVX2 path). **Optimize targets (behind):** the pairing /
Fp-mul / Groth16 elliptic-curve stack (blst hand-asm), ed25519 (needs wNAF +
precompute), and SHA256 (needs SHA-NI). Groth16 being behind is expected today
and must NOT red main — the loud line keeps it visible.

## Optimizing safely

Any optimization to a consensus crypto primitive must stay **bit-identical** to
the frozen verify logic. The differential parity oracle
(`lib/test/differential/`, on the `wf/groth16-beat-rust` workflow) exists to
prove an optimized implementation returns the exact same accept/reject verdict
as the reference on adversarial inputs. Flow: optimise → prove bit-identity via
the differential oracle + the `crypto_perf_selftest` teeth → re-run
`make check-crypto-perf` → **shrink** the baseline in
`tools/crypto_perf_baseline.csv` (and flip `behind`→`beat` once you clear the
margin). The ratchet then holds the new line forever.
