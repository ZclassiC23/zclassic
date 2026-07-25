# Benchmark Log — measured progress over time

Append-only ledger of the benchmarks defined in
[`USER_BENCHMARKS.md`](./USER_BENCHMARKS.md) (the spec). One row per
measurement. This is the source for the "now" column and the bars on the board
— **measured values, never estimates.**

## How to add a measurement (from Claude Code)

1. Pull live gauges: `zclassic23 core status` (RSS, height, peers, uptime) and
   `zclassic23 ops metrics`.
2. For timing benchmarks, run the harness (only meaningful on a *healthy* node):
   - `#1 cold`  → `build/bin/zclassic23 -bench-coldstart`
   - `#2 warm`  → `build/bin/zclassic23 -bench-warmstart`
   - `#4 thru`  → `zclassic23 core sync validation` `blocks_per_sec` during bg-verify
   - `#6 kill-9`→ `build/bin/zclassic23 -bench-kill9`
3. Append rows below with today's date + `git rev-parse --short HEAD`.
4. Leave a metric out rather than guess. `—` = not measured this run.
5. Commit. Trend for any metric: `grep "RSS" docs/BENCHMARKS_LOG.md`.

Format: `date | commit | benchmark | value | how measured / notes`

## Measurements

| date | commit | benchmark | value | how / notes |
|---|---|---|---|---|
| 2026-05-24 | be5e90b05 | #9 binary size | **14.6 MB** | `ls` of built binary (target stay small) | <!-- stale-ok: dated benchmark measurement, not a present-tense claim -->
| 2026-05-24 | 6e0f6a82c | #1 cold import identity | serial 48.9s / default-workers 57.3s | `ZCL_COLD_IMPORT_DEBUG_WINDOW=3028 build/bin/zclassic23 -datadir=/tmp/zcl-cold -cold-import=~/.zclassic -nofilesync -nobgvalidation` (serial adds `ZCL_BLOCK_SCAN_WORKERS=1`). Both: `utxo_sha3=981b7bbceb522f816e29e4adccf7f80fdcab75cd392ee7b438b55787385031f1`, `coins_best_block=acad56115a58a82ff18395591263a7ec881bd13603ec31e1e72adb12ea010000`, `utxos=1345066` (min_h=1, max_h=3123726, sum_value=1038775293114532). Cold-import bulk-copies the legacy block index and **bypasses `scan_block_files_mark_data`** — the 101s `blk*.dat` marking baseline is a normal/file-sync boot cost, not this path. |
| 2026-05-24 | 078667266 | #1 cold sync PR-3 serial-vs-parallel | serial 194.9s; parallel 295.3s | `tools/bench_cold_import_equivalence.sh` vs `/tmp/zcl-legacy-snapshot`; both h=3,123,688, tip `00000f027587b4eeb3f4890f77659c7057f9ea0512f761295c294d1000f9d462`, `utxo_sha3=3160565aba65ef205ba54886a57d39fccd1dade2ec709de1eff9c1d1307ffc48`, `utxos=1,345,067`. **⚠ parallel SLOWER (+100s) — scanner integration regressed cold-import.** |
| 2026-05-24 | e4b5528ea | #2 warm restart | **37.7s** | `systemctl stop`→`start` to first `getblockcount` at tip 3,123,688 (poll @0.25s). Target 10s. Wall-clock incl. systemd + Tor bootstrap, not the `-bench-warmstart` harness. |
| 2026-05-24 | e4b5528ea | #4 throughput | **~107 blk/s** | `validationstatus.blocks_per_sec` during bg-verify (97–112). Full re-verify of 3.12M blocks ≈ 8h. |
| 2026-05-24 | dc3a5f773 | #3 RSS under bg-verify | **stair-steps to ~2.4 GB, unbounded** | soak curve: 1.53GB@89s → 1.93GB@510s → 2.39GB@1050s (val_h=205,852, 6.6% of bg-validation), still creeping ~0.1 MB/s at 17min. RSS stair-steps with validation depth (buffers, not a steady leak). ~2.4× the 1 GB target. `-nobgvalidation` = lean baseline. Lever: Phase-3 monolith dissolve + bg-verify buffer cap. |
| 2026-06-04 | 671fd79e3 | #7 kill-9 harness (`make test-crash-bootstrap`) | PASS — 2/2 cycles, 0 regress/overshoot | isolated /tmp regtest, ports 39030-33; 2 SIGKILL-process-group → restart cycles assert height-monotone + zero-UTXO-above-tip on `node.db`. DEGRADED genesis-only (regtest `generate` mines no valid Equihash block on this build → `over=-1` N/A); boot-recovery still exercised. |
| 2026-06-04 | 671fd79e3 | #6 soak-ci proxy (`make soak-ci`, 180s `--ci-proxy`) | machinery OK; verdict reflects no-load | soak runner samples its OWN child pid (rss_max~161 MiB), threads ZCL_DATADIR+ZCL_RPCPORT per rpc. Verdict path correct (`FAIL_TOO_SHORT`/`FAIL_TIP_STALL`). Goes RED with `tip_hwm=0` because regtest `generate` advances no tip on this build (node-miner, not harness). |

> RSS / cold-sync / warm-restart rows above are dated snapshots against a
> specific tip height. Re-measure on the current binary before quoting; see
> `HANDOFF.md` for current live state.

## Consensus-verify microbenchmark (`make bench-crypto-verify`)

The two dominant per-block consensus-VERIFY costs, timed in isolation with
the mandated monotonic clock (`clock_now_monotonic_ns`; `gettimeofday` is
banned). `make bench-crypto-verify` appends `ns/op` rows to
`docs/bench-history.csv`; `make bench-regress` (run in `make ci`) fails if a
new run is >20% slower than the prior recorded run for that primitive (ns/op
is lower-is-better). The numbers are **HOST-RELATIVE** — re-baseline on your
host. The benchmark is protected against going hollow-fast by the
`verify_bench_selftest` test group AND an in-harness teeth check: each
primitive must return TRUE on a valid fixture and FALSE on a one-bit-flipped
copy before any number is recorded, so a broken/always-true/no-op verifier
cannot "get fast" and pass the gate.

| date | commit | primitive | value | how / notes |
|---|---|---|---|---|
| 2026-07-10 | de89ee8d4 | Equihash 200,9 solution verify | **~120.6 µs/op** (~8,300 ops/s) | `check_equihash_solution` on a baked real (200,9) witness (`lib/test/include/test/verify_bench_fixture.h`); AMD Ryzen 9 7950X3D. |
| 2026-07-10 | de89ee8d4 | Groth16 BLS12-381 output-proof verify | **~7.85 ms/op** (~127 ops/s) | `sapling_check_output` (full pure-C23 BLS12-381 pairing) on a real prover output proof; needs `~/.zcash-params`; AMD Ryzen 9 7950X3D. |

## Crypto-vs-Rust standing invariant (`make check-crypto-perf`)

The above two-primitive bench is subsumed by the **standing "beat Rust"
invariant** — see [`CRYPTO_PERF.md`](./CRYPTO_PERF.md). `make bench-crypto-vs-rust`
times **every** consensus-path C crypto primitive (Equihash verify,
Groth16/BLS12-381 output verify, BLS12-381 pairing + Fp mul, secp256k1 ECDSA
verify, ed25519 verify, SHA256, SHA3-256, BLAKE2b) as a flake-resistant **median
of N** ns/op and appends the rows here. `make check-crypto-perf`
(`tools/scripts/check_crypto_perf.sh`, NOT in the default `make lint` aggregate —
timing flakes under load) then gates against `tools/crypto_perf_baseline.csv`:
a **ratchet** (each C primitive may only get faster; the baseline is a ceiling
that only shrinks) plus a **ratio-vs-Rust** rule (primitives that beat Rust must
stay ahead — hard fail on a lost lead; primitives behind Rust, e.g. Groth16
today, print a loud `BEHIND RUST — optimize` line but do not fail). Hollow-fast
is forbidden by the `crypto_perf_selftest` test group + in-harness teeth. We
beat Rust on Equihash verify, ECDSA verify, and BLAKE2b; the
elliptic-curve/pairing/Groth16 stack, ed25519, and SHA256 (no SHA-NI) are the
tracked optimize targets. Run `make check-crypto-perf` for the current
per-primitive standing.

### Shipped flags vs host flags — what the shipped binary leaves on the table

Two builds of the same commit, benched on the same host: the shipped default
(`-march=x86-64-v3` — AVX2/FMA/BMI2, **SHA-NI and AVX-512 compiled out**) and
`ZCL_NATIVE=1` (`-march=native` — AVX-512 + SHA-NI compiled in).

Method matters here: the build machine is shared, and a neighbour's build
inflates a whole median-of-N run, so median-of-N is *not* enough protection.
The two binaries were stashed side by side and run **interleaved, four
alternating rounds each**, and the table reports the **minimum** per primitive —
the least-contended observation. A first pass that did not interleave produced
a 1.71x "Groth16 regression" that vanished under A/B; that number was
contention, not instruction set.

| primitive | shipped `-v3` | host ISA | host / shipped |
|---|---|---|---|
| Equihash 200,9 verify | 123,553 ns | 124,923 ns | 1.01 |
| BLS12-381 `fp_mul` | 60.2 ns | 65.1 ns | **1.08 (slower)** |
| BLS12-381 Ate pairing | 1,908,834 ns | 2,045,707 ns | **1.07 (slower)** |
| secp256k1 ECDSA verify | 52,462 ns | 53,096 ns | 1.01 |
| ed25519 verify | 1,618,294 ns | 1,838,567 ns | **1.14 (slower)** |
| SHA-256 (1 KiB) | 2,115.8 ns | 516.4 ns | **0.24 (4.10x faster)** |
| SHA3-256 (1 KiB) | 1,861.7 ns | 1,841.2 ns | 0.99 |
| BLAKE2b-512 (1 KiB) | 908.2 ns | 888.8 ns | 0.98 |
| Groth16 output verify | 6,399,453 ns | 6,987,349 ns | **1.09 (slower)** |

**SHA-256 is the only instruction-set path worth compiling in — it is not the
first of several, it is the only one.** SHA-NI is guarded by `#ifdef __SHA__`
(`lib/crypto/src/sha256.c`), which `-march=x86-64-v3` does not define, so the
shipped binary runs the portable transform on a CPU that has the instruction.
That is a real 4.1x left on the table, and it is worth a targeted fix — a
per-file `-msha` on `sha256.c` plus the runtime CPUID self-test that already
exists there, not a global flag change.

A global `ZCL_NATIVE=1` would be a **net loss on the consensus path**:
everything else is flat (within ±2%) or regresses, and the whole BLS12-381
stack goes 7-9% the wrong way. A cross-check narrows where that comes from —
building the *same* BLS12-381 sources standalone at `-O2` without LTO shows no
ISA sensitivity at all (6.61 ms `-v3` vs 6.39 ms native), so the regression is
produced by `-O3 -march=native -flto` on the whole program, not by the field
arithmetic being AVX-512-hostile in itself. Auto-vectorising 6-limb carry
chains that want scalar `mulx/adcx/adox` is the obvious suspect; it is a lead,
not yet a diagnosis.

Row naming: host-ISA runs land under `crypto-vs-rust [host-isa] <key>`, never
under the shipped name. `-bench-regress` gates the last two rows sharing a
name at ±20% and SHA-256 moves 4x between the builds, so one untagged
host-flags row would red the next shipped run for a change nobody made.
`tools/crypto_perf_baseline.csv` pins the **shipped** build and must not be
re-baselined from a `ZCL_NATIVE=1` run.

### Where the 7.7 ms of a Groth16 verify actually goes

`bash lib/test/differential/run_parity_oracle.sh profile` — exact Fp-multiply
counts per phase (linker-interposed `fp_mont_mul_accel`, which every multiply
in `bls12_381.c` reaches through) plus per-phase wall time, decomposed with
the public multi-pairing API at n=0/1/4. No edit to the frozen verifier.

A naive "4 Miller loops + 1 final exponentiation" operation count predicts
~2 ms. At the measured `fp_mul` cost of 60.2 ns that model is implicitly
budgeting ~33,000 field multiplies. The real number is **76,658**, and the
gap is fully enumerable:

| phase | Fp muls | share | note |
|---|---|---|---|
| 4 x Miller loop | 32,644 | 43% | 8,161 each; **1,245 of each (15%) is the two to-affine Fermat inversions**, redone every pairing on points that are VK constants |
| final exponentiation | 14,486 | 19% | vs ~8k textbook: `fp12_inv` bottoms out in `fp_inv` = `fp_pow(q-2)` = 613 muls, and `fp12_sq` is the generic one, not a cyclotomic squaring |
| **public-input MSM + negations** | **29,312** | **38%** | 5 public inputs, ~5,862 muls each. **Absent from the naive model entirely** |
| 4 x `fp12_mul` (accumulate) | 216 | <1% | |
| **total `groth16_verify`** | **76,658** | | |

So the ~3.6x splits into two independent factors, both measured:

* **2.32x — more multiplies than the model counted.** The public-input MSM is
  38% of the verify and the naive model does not count it at all; the final
  exponentiation is ~1.8x textbook because inversion is Fermat exponentiation.
* **1.39x — work that is not a multiply.** 76,658 x 60.2 ns = 4.61 ms
  predicted vs 6.40 ms measured: Fp multiplies are only **70%** of the wall
  time. The other 30% is `fp_add`/`fp_sub` (6-limb add + conditional subtract
  each) and by-value struct copies through the fp2/fp6/fp12 tower.

2.32 x 1.39 = 3.2x, i.e. ~2 ms -> 6.4 ms on the quiet build machine. The older
7.85 ms row was measured on the live-node host under load; the algorithmic
explanation stops at 6.4 ms and the rest is host.

Use the multiply-count column, not the wall-time column, for the phase split.
The time decomposition is subtractive (`T(1) - T(0)`, `T(4)` vs the whole) and
its three shares sum to ~107% — each subtraction carries the noise of both
terms. The counts are exact and sum to 100%, and the two agree inside that
noise.

**Consequence for anyone optimizing:** the four Miller loops are 43% of the
verify and the pairing as a whole (Miller + final exponentiation) is 62%.
Restructuring pairing arithmetic cannot touch the other 38%, which is a
public-input MSM. The
already-shipped fixed-base comb tables (`groth16_vk_build_combs`, wired at
`lib/sapling/src/params_init.c:176`) take that MSM down and are worth **1.40x
on OUTPUT (k=5) and 1.54x on SPEND (k=7)** measured end to end —
`run_parity_oracle.sh bench`. The cheapest remaining wins are outside the
pairing restructure: hoist the constant-point to-affine inversions out of the
per-pairing path, and replace Fermat inversion with a binary/extended-Euclid
inverse (613 multiplies per inversion today).

## Native rebuild benchmark (`rebuild_recent` tool)

| date | commit | N blocks | rebuild ms | blocks/s | bytes | notes |
|---|---|---|---|---|---|---|
| 2026-05-24 | (tool) | ALL (3,123,618) | 5,570 | 560,693 | 11.25 GB | Parallel-sharded `io_uring` writer: 32 threads, 64 independent segments, dynamic schedule, hardware CRC32C (SSE4.2). **2.0 GB/s — at the NVMe write floor.** All 64 segments byte-valid, 27.7M events, short_writes=0. ~5.4s setup (snapshot+index) additional. Output is a 64-segment event log (each a standalone valid log), not one file; a single-file need requires an offset-fixup concat pass. |

Design: one `io_uring` ring per thread, one segment file per thread — zero
cross-thread coordination, near-linear scaling until the disk saturates.
Hardware CRC32C is required; software CRC is the per-thread bottleneck at
this throughput. A shared single-writer `io_uring` design serializes on the
in-memory buffer and offset bookkeeping and does not scale past a few
threads — keep the per-thread-segment design. Remaining lever: zero-copy
submit of worker buffers + a per-thread block-parse arena to remove
`block_deserialize` malloc contention.

## 2026-07-25 — developer inner-loop baseline (build/test, not node runtime)

Host: 32 core / 93 GB, HEAD `7e28252b5`, gcc, ccache enabled.
First build-time measurements ever recorded here; `tools/scripts/timings.sh`
still reads only lint/test/dev-loop artifacts and says outright that build wall
time is unrecorded. These are the before-numbers for the inner-loop work.

| Action | Wall |
|---|---|
| `make -j32 build-only`, no change | 6.0s |
| `make -j32 build-only`, one .c edited | 9.0s |
| `make -j32 test_parallel`, no change | 10.7s |
| `make -j32 test_parallel`, one .c edited | 31.6s |
| `make -j32 zclassic23`, one .c edited | 67.0s (whole-program LTO, uncacheable by design) |
| `make lint`, 103 gates, 8 jobs | 16.6s |
| full suite, cold, 32 workers | ~157s |

### Bare link, test-strict lane, measured directly

Same object set both runs — 1883 objects in, one test-runner binary out
(quote `tools/scripts/binary_size.sh` if you need the size; it is not the
variable under test here, the wall time is):

| Linker | Wall | maxrss |
|---|---|---|
| `ld.bfd` (what the gate uses today) | 0.90s | 423 MB |
| `ld.gold` | 0.58s | 446 MB |

**The link is not the bottleneck.** A pre-measurement hypothesis held that
`ld.bfd` accounted for most of the 21s one-file delta, because
`ZCL_DEV_LINKER` (Makefile:457) resolves to empty on this host — mold and lld
are both absent — and `TEST_REL_LDFLAGS` (Makefile:906) never references it
anyway. Both facts are true, and both are worth fixing, but the measurement
prices the fix at ~0.3s, not ~20s.

The one-file delta is therefore the compile-epoch churn: the object directory is
keyed on a whole-tree content-and-stat hash, so a single edit relocates all 1883
objects to a new `epochs/<hash>/` directory and Make re-invokes the compile
recipe for every one, each spawning ~13 processes before reaching ccache.
