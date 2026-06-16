# Benchmark Log — measured progress over time

Append-only ledger of the benchmarks defined in
[`USER_BENCHMARKS.md`](./USER_BENCHMARKS.md) (the spec). One row per
measurement. This is the source for the "now" column and the bars on the board
— **measured values, never estimates.**

## How to add a measurement (from Claude Code)

1. Pull live gauges: `zcl_status` (RSS, height, peers, uptime) and `zcl_metrics`.
2. For timing benchmarks, run the harness (only meaningful on a *healthy* node):
   - `#1 cold`  → `build/bin/zclassic23 -bench-coldstart`
   - `#2 warm`  → `build/bin/zclassic23 -bench-warmstart`
   - `#4 thru`  → `zcl_validationstatus` `blocks_per_sec` during bg-verify
   - `#6 kill-9`→ `build/bin/zclassic23 -bench-kill9`
3. Append rows below with today's date + `git rev-parse --short HEAD`.
4. Leave a metric out rather than guess. `—` = not measured this run.
5. Commit. Trend for any metric: `grep "RSS" docs/BENCHMARKS_LOG.md`.

Format: `date | commit | benchmark | value | how measured / notes`

## Measurements

| date | commit | benchmark | value | how / notes |
|---|---|---|---|---|
| 2026-05-24 | be5e90b05 | #3 RSS | 1532 MB | live `zcl_status.memory_rss_mb` (target ≤1000) |
| 2026-05-24 19:15 | 4ea5f5063 | #3 RSS | 1587 MB | RSS creeping while stuck |
| 2026-05-24 | be5e90b05 | #9 binary size | **14.6 MB** | `ls` of built binary (target stay small) |
| 2026-05-24 | be5e90b05 | #1 cold sync | — | not measured (node stuck, no clean restart this session) |
| 2026-05-24 | be5e90b05 | #2 warm restart | — | not measured |
| 2026-05-24 | be5e90b05 | #4 throughput | 0 blk/s | `zcl_validationstatus` — bg-verify idle/complete at local tip |
| 2026-05-24 | be5e90b05 | #6 kill-9 recovery | — | not measured |
| 2026-05-24 | 941b9803d | #1 cold sync (LDB→bootable tip) | ~180s | live cold-import recovery from wedge. **blk*.dat marking = 101s (THE bottleneck), single-threaded**; then wallet backfill + utxo + mmb ~80s. RPC up at tip 3,123,688. Path PR-3 (parallel io_uring import) must replace; cf. 5.6s rebuild_recent prototype (reformat only, no validation, not bootable). |
| 2026-05-24 | 6e0f6a82c | #1 cold import identity, serial worker | 48.9s | `ZCL_BLOCK_SCAN_WORKERS=1 ZCL_COLD_IMPORT_DEBUG_WINDOW=3028 build/bin/zclassic23 -datadir=/tmp/zcl-cold-serial -cold-import=/home/rhett/.zclassic -nofilesync -nobgvalidation`. Imported `legacy_tip=3123726`, `block_index=3124929`, `utxos=1345066`, `blk_files=50`; pending anchor published via CSR. `utxo_sha3=981b7bbceb522f816e29e4adccf7f80fdcab75cd392ee7b438b55787385031f1`, `coins_best_block=acad56115a58a82ff18395591263a7ec881bd13603ec31e1e72adb12ea010000`, UTXO stats `(1345066, min_h=1, max_h=3123726, sum_value=1038775293114532)`. No `Block file scan` line: cold-import bulk-copies the legacy block index and bypasses `scan_block_files_mark_data`. |
| 2026-05-24 | 6e0f6a82c | #1 cold import identity, default workers | 57.3s | `ZCL_COLD_IMPORT_DEBUG_WINDOW=3028 build/bin/zclassic23 -datadir=/tmp/zcl-cold-parallel -cold-import=/home/rhett/.zclassic -nofilesync -nobgvalidation`. Identical `utxo_sha3`/`coins_best_block`/UTXO-stats as serial; also bypasses `scan_block_files_mark_data`. The historical 101s `blk*.dat` marking baseline applies to normal/file-sync boot scanning, not this cold-import path. |
| 2026-05-24 | 941b9803d | #6 wedge recovery (cold-import) | ~180s + restart | a single stale BLOCK_FAILED_VALID wedged the tip; restart did NOT clear it; cold-import did. Target <60s via PR-0 in-place snapshot recovery. |
| 2026-05-24 | 319596f0d | #3 RSS | 2.02 GB | `/proc/PID/VmRSS` after cold-import + warm restart, up ~10min, at tip. Regression vs 1.5GB earlier today — post-cold-import RSS higher. Target ≤1.0GB. |
| 2026-05-24 | 319596f0d | #5 stay-at-tip | 0-block gap | `getblockchaininfo`: blocks==headers==3,123,688. Node fully synced. Latency-to-new-block not yet harnessed. |
| 2026-05-24 | be5e90b05 | #7 MTBF | — | uptime only 990s (recent restart); needs a soak to measure |
| 2026-05-24 | be5e90b05 | #8 operator pages | 6/10 conditions active, 1 critical failing | `zcl_conditions` — self-heal degraded right now |
| 2026-05-24 | 078667266 | #1 cold sync PR-3 serial-vs-parallel | serial 194.856s; parallel 295.263s | `tools/bench_cold_import_equivalence.sh` against `/tmp/zcl-legacy-snapshot`; both reached h=3,123,688, tip `00000f027587b4eeb3f4890f77659c7057f9ea0512f761295c294d1000f9d462`, `utxo_sha3=3160565aba65ef205ba54886a57d39fccd1dade2ec709de1eff9c1d1307ffc48`, `utxos=1,345,067`. **⚠ parallel was SLOWER (+100s) — scanner integration regressed cold-import.** |
| 2026-05-24 21:30 | e4b5528ea | #2 warm restart | **37.7s** | real-world `systemctl stop`→`start`, timed to first `getblockcount` at tip 3,123,688 (poll @0.25s). Target 10s; ~prior 33s. Wall-clock incl. systemd + Tor bootstrap, not the `-bench-warmstart` harness. |
| 2026-05-24 21:30 | e4b5528ea | #3 RSS (fresh boot) | **1805 MB** | `/proc/PID/VmRSS` at uptime 46s, right after warm restart. vs 2082 MB pre-restart (uptime ~13min) → RSS creeps with uptime; fresh-boot floor ~1.8 GB. Target ≤1.0 GB. |
| 2026-05-24 21:30 | e4b5528ea | #7 MTBF | soak STARTED | fresh boot = soak baseline; sampler /tmp/zcl_soak.csv logs uptime/RSS/throughput @60s. |
| 2026-05-24 21:35 | e4b5528ea | #4 throughput | **~107 blk/s** | `validationstatus.blocks_per_sec` during bg-verify; verified_height 114,144→133,544 over 180s = 107.8 blk/s (samples report 97–112). Full re-verify of 3.12M blocks ≈ 8h. |
| 2026-05-24 21:35 | e4b5528ea | #3 RSS (creep slope) | 1532→1901 MB in 4.5min | soak CSV: uptime 89s=1532MB, 149s=1813, 209s=1883, 269s=1901. **~+370 MB in first 3min of bg-verify**, then leveling. Creep toward 2.0GB is bg-validation buffers, not a steady leak; needs longer soak to confirm plateau. |
| 2026-05-24 21:40 | dc3a5f773 | #3 RSS (CORRECTION) | **climbs to ~2.39 GB, not bounded** | longer soak: not a plateau. Full curve (uptime→RSS, val_h): 89s=1.53GB → 510s=1.93GB (looked flat) → 750s=2.37GB → 1050s=2.39GB @ val_h=205,852 (**6.6% of bg-validation**). RSS **stair-steps with validation depth** — each step = more buffered. Still creeping ~0.1 MB/s at 17min; full ~8h run needed to find the real ceiling. ~2.4× the 1 GB target. Lever: Phase-3 monolith dissolve + a bg-verify buffer cap. |
| 2026-05-25 08:01 | 2ce361fe6 | #5 stay-at-tip / #6 wedge recovery | **WEDGED, not deployed** | Read-only post-fix live check: `tools/scoreboard.sh` exit 3, live z23=3,123,688 vs legacy=3,124,208 (gap 520), tip not advancing. Running systemd binary `/home/rhett/zclassic23/zclassic23` mtime `00:29` / SHA256 `b8a6450f...` differs from current worktree binary mtime `03:22` / SHA256 `c887c311...`; unwedge commit `dbf4845a1` is in git but not live. `node.log` still shows `STALL: h=3123688 entries_at_3123689=1`. |
| 2026-06-04 | 671fd79e3 | #7 kill-9 full-binary harness (`make test-crash-bootstrap`) | PASS — 2/2 cycles recovered, 0 regress/overshoot | isolated /tmp regtest node, 39030-33 ports; 2 SIGKILL-process-group → restart cycles, asserts height-monotone + zero-UTXO-above-tip on `node.db`. DEGRADED genesis-only mode (regtest `generate` mines no valid Equihash block on this build, so `over=-1` not-applicable); boot-recovery still exercised. No orphan/datadir left. |
| 2026-06-04 | 671fd79e3 | #6 soak-ci proxy machinery | spawn/sample/RSS/verdict OK; verdict reflects no-load | isolated /tmp regtest node; soak runner samples its OWN child pid (rss_max~161 MiB, not the live node), threads ZCL_DATADIR+ZCL_RPCPORT on every rpc. Verdict path correct (`FAIL_TOO_SHORT`/`FAIL_TIP_STALL` as designed). `make soak-ci` (180s, `--ci-proxy`) goes RED with `tip_hwm=0` because regtest `generate` advances no tip on this build (node-miner issue, not harness) — runner names it explicitly. |

## Native rebuild benchmark (`rebuild_recent` tool)

| date | commit | N blocks | rebuild ms | blocks/s | bytes | notes |
|---|---|---|---|---|---|---|
| 2026-05-24 | (tool) | 10 | 339 | 29 | 14,590 | v1, durable event_log appender. **fsync-bound** (fsync×2/event). |
| 2026-05-24 | (tool) | ALL (3,123,618) | 34,180 | 91,387 | 11.25 GB | **io_uring** bulk writer (8 buffers in flight, 1 fsync at end). 5.1M tx, 11.4M utxo-adds, 27.7M events, short_writes=0. 329 MB/s. setup +5.4s. ~3000× the v1 write path. |
| 2026-05-24 | (tool) | ALL (3,123,618) | 17,990 | 173,611 | 11.25 GB | + hardware CRC32C (SSE4.2), verified == software table. 625 MB/s. Software CRC was ~half the runtime (34→18s). SHA256 already SHA-NI. |
| 2026-05-24 | (tool) | ALL (3,123,618) | 5,570 | 560,693 | 11.25 GB | **+ parallel sharding** (32 threads, 64 independent io_uring segments, dynamic schedule). **2.0 GB/s — at the NVMe write floor.** All 64 segments byte-valid, 27.7M events, short_writes=0. 6× over single-thread io_uring; **34s→5.6s overall (~10× / fsync-v1 ~astronomical)**. ~5.4s setup (snapshot+index) on top. NOTE: output is a 64-segment event log (each a standalone valid log), not one file — matches Phase 8 segmentation; single-file needs an offset-fixup concat pass. **Kept version.** |

**Why parallel works now but failed before:** the first attempt used one shared `ordered` io_uring writer → the serial 11 GB memcpy + offset patch was the bottleneck (Amdahl). Sharding gives each thread its *own* io_uring ring + segment file — zero coordination, near-linear until the disk saturates. Hardware CRC was the prerequisite (software CRC would have re-become the per-thread bottleneck).

### Parallelization experiment (NEGATIVE result — reverted)

Tried OpenMP parallel parse+CRC-framing with a single `ordered` writer feeding io_uring. Result: **no speedup; threads hurt.** Whole-chain REBUILD by thread count: 1t=34.2s, 4t=34.4s (flat), 8t=79.8s, 32t=64.8s. Output stayed byte-valid. Conclusion: the rebuild is **not CPU-parse-bound** — it's bound by the serial write path (11 GB memcpy + crc + io in the ordered region) and malloc-arena contention in `block_deserialize` across threads (Amdahl: large serial fraction). Reverted to single-threaded io_uring. Real levers to go below 34s: zero-copy submit of worker buffers (drop a memcpy) + a per-thread block-parse arena/pool (kill malloc contention) — bigger surgery, deferred.

## Operational snapshot (context for the above, not a benchmark)

| date | commit | height-behind | peers | uptime | errors_total | note |
|---|---|---|---|---|---|---|
| 2026-05-24 | be5e90b05 | 1,905 | 2 | 990s | 16,396 | node stuck: chain-advance blocked, `block_failed_mask_at_tip` failing |
