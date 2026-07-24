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
