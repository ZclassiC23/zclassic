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
| 2026-05-24 | be5e90b05 | #9 binary size | **14.6 MB** | `ls` of built binary (target stay small) |
| 2026-05-24 | 6e0f6a82c | #1 cold import identity | serial 48.9s / default-workers 57.3s | `ZCL_COLD_IMPORT_DEBUG_WINDOW=3028 build/bin/zclassic23 -datadir=/tmp/zcl-cold -cold-import=~/.zclassic -nofilesync -nobgvalidation` (serial adds `ZCL_BLOCK_SCAN_WORKERS=1`). Both: `utxo_sha3=981b7bbceb522f816e29e4adccf7f80fdcab75cd392ee7b438b55787385031f1`, `coins_best_block=acad56115a58a82ff18395591263a7ec881bd13603ec31e1e72adb12ea010000`, `utxos=1345066` (min_h=1, max_h=3123726, sum_value=1038775293114532). Cold-import bulk-copies the legacy block index and **bypasses `scan_block_files_mark_data`** — the 101s `blk*.dat` marking baseline is a normal/file-sync boot cost, not this path. |
| 2026-05-24 | 078667266 | #1 cold sync PR-3 serial-vs-parallel | serial 194.9s; parallel 295.3s | `tools/bench_cold_import_equivalence.sh` vs `/tmp/zcl-legacy-snapshot`; both h=3,123,688, tip `00000f027587b4eeb3f4890f77659c7057f9ea0512f761295c294d1000f9d462`, `utxo_sha3=3160565aba65ef205ba54886a57d39fccd1dade2ec709de1eff9c1d1307ffc48`, `utxos=1,345,067`. **⚠ parallel SLOWER (+100s) — scanner integration regressed cold-import.** |
| 2026-05-24 | e4b5528ea | #2 warm restart | **37.7s** | `systemctl stop`→`start` to first `getblockcount` at tip 3,123,688 (poll @0.25s). Target 10s. Wall-clock incl. systemd + Tor bootstrap, not the `-bench-warmstart` harness. |
| 2026-05-24 | e4b5528ea | #4 throughput | **~107 blk/s** | `validationstatus.blocks_per_sec` during bg-verify (97–112). Full re-verify of 3.12M blocks ≈ 8h. |
| 2026-05-24 | dc3a5f773 | #3 RSS under bg-verify | **stair-steps to ~2.4 GB, unbounded** | soak curve: 1.53GB@89s → 1.93GB@510s → 2.39GB@1050s (val_h=205,852, 6.6% of bg-validation), still creeping ~0.1 MB/s at 17min. RSS stair-steps with validation depth (buffers, not a steady leak). ~2.4× the 1 GB target. `-nobgvalidation` = lean baseline. Lever: Phase-3 monolith dissolve + bg-verify buffer cap. |
| 2026-06-04 | 671fd79e3 | #7 kill-9 harness (`make test-crash-bootstrap`) | PASS — 2/2 cycles, 0 regress/overshoot | isolated /tmp regtest, ports 39030-33; 2 SIGKILL-process-group → restart cycles assert height-monotone + zero-UTXO-above-tip on `node.db`. DEGRADED genesis-only (regtest `generate` mines no valid Equihash block on this build → `over=-1` N/A); boot-recovery still exercised. |
| 2026-06-04 | 671fd79e3 | #6 soak-ci proxy (`make soak-ci`, 180s `--ci-proxy`) | machinery OK; verdict reflects no-load | soak runner samples its OWN child pid (rss_max~161 MiB), threads ZCL_DATADIR+ZCL_RPCPORT per rpc. Verdict path correct (`FAIL_TOO_SHORT`/`FAIL_TIP_STALL`). Goes RED with `tip_hwm=0` because regtest `generate` advances no tip on this build (node-miner, not harness). |

> RSS / cold-sync / warm-restart rows above are 2026-05-24 snapshots against tip 3,123,688; the node has been recovered/rebuilt several times since (see `HANDOFF.md`). Treat as history, re-measure on the current binary before quoting.

## Native rebuild benchmark (`rebuild_recent` tool)

| date | commit | N blocks | rebuild ms | blocks/s | bytes | notes |
|---|---|---|---|---|---|---|
| 2026-05-24 | (tool) | 10 | 339 | 29 | 14,590 | v1, durable event_log appender. **fsync-bound** (fsync×2/event). |
| 2026-05-24 | (tool) | ALL (3,123,618) | 34,180 | 91,387 | 11.25 GB | **io_uring** bulk writer (8 buffers in flight, 1 fsync at end). 5.1M tx, 11.4M utxo-adds, 27.7M events, short_writes=0. 329 MB/s. setup +5.4s. ~3000× the v1 write path. |
| 2026-05-24 | (tool) | ALL (3,123,618) | 17,990 | 173,611 | 11.25 GB | + hardware CRC32C (SSE4.2), verified == software table. 625 MB/s. Software CRC was ~half the runtime (34→18s). SHA256 already SHA-NI. |
| 2026-05-24 | (tool) | ALL (3,123,618) | 5,570 | 560,693 | 11.25 GB | **+ parallel sharding** (32 threads, 64 independent io_uring segments, dynamic schedule). **2.0 GB/s — at the NVMe write floor.** All 64 segments byte-valid, 27.7M events, short_writes=0. 6× over single-thread io_uring; **34s→5.6s overall (~10× / fsync-v1 ~astronomical)**. ~5.4s setup (snapshot+index) on top. NOTE: output is a 64-segment event log (each a standalone valid log), not one file — matches Phase 8 segmentation; single-file needs an offset-fixup concat pass. **Kept version.** |

**Why parallel works (kept version) but the earlier `ordered`-writer attempt failed:** per-thread io_uring ring + segment file = zero coordination, near-linear until the disk saturates. Hardware CRC is the prerequisite — software CRC would re-become the per-thread bottleneck. The first attempt shared one `ordered` io_uring writer, so the serial 11 GB memcpy + offset patch was the Amdahl bottleneck (1t=34.2s, 4t=34.4s flat, 8t=79.8s, 32t=64.8s; output byte-valid throughout). Levers to go below 34s single-thread: zero-copy submit of worker buffers + per-thread block-parse arena (kill `block_deserialize` malloc contention) — deferred.
