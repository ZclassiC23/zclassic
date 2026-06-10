# zclassic23 — User Benchmarks (the only metrics that matter)

> The user does not care about halts, BLOCK_FAILED_MASK, or stage cursors.
> The user wants: install → it works → stays working → doesn't eat the machine.
> Every architectural decision is judged against these five numbers.

## The five numbers

> **Live values are tracked in one place:** [`BENCHMARKS_LOG.md`](./BENCHMARKS_LOG.md)
> (append-only, measured, dated). This table is the *spec* — targets and how to
> measure. The "Latest" column is a pointer, not a source; never quote an
> estimate here. `—` = not measured this run.

| # | Benchmark | Target | Latest (see ledger) | How measured |
|---|---|---|---|---|
| 1 | **Cold-start to operational** (empty datadir → tip current within 100 blocks, RPC + wallet ready) | ≤ **60 s** | 180s (05-24, halt recovery path) | `time build/bin/zclassic23 -bench-coldstart` |
| 2 | **Warm-start to operational** (restart with synced datadir → same tip, RPC ready) | ≤ **10 s** | 37.7s (05-24, real restart→tip) | `time build/bin/zclassic23 -bench-warmstart` |
| 3 | **Stay-in-sync MTBF** (mean time between unattended stalls > 60 s) | ≥ **30 days** | soak in progress (started 05-24) | 30-day chaos soak (kill -9, net blip, peer churn) |
| 4 | **RAM budget steady-state** | ≤ **1 GB RSS** | ~2.4 GB & climbing (05-24 soak, 6.6% bg-verify) | `zcl_status.memory_rss_mb` over a soak |
| 5 | **Recovery from kill -9** | ≤ **60 s** | — (not measured this session) | scripted kill loop, recovery histogram |

### Soak: hermetic proxy vs. operational acceptance (#3/#4)

`make soak-ci` is a **3–5 min hermetic PROXY**: it spawns an isolated
`/tmp` regtest node with synthetic `generate`-load and scores the run
through the *same* verdict math (`lib/test/src/soak_harness.c`) as the
real soak — crash → `FAIL_CRASH`, tip stall → `FAIL_TIP_STALL`, RSS walk
past the post-warmup baseline → `FAIL_RSS_WALK`. It gives a fast green/red
CI signal that the soak machinery and RSS-plateau logic are sound.

It is **not** the acceptance run. The real #3 (≥ 30-day MTBF) and #4
(≤ 1 GB steady-state RSS) require **168 h+ of live wall time under real
tx load** with zero operator restarts, and depend on the live wedge being
cleared (node holds tip AND finalizes forward). Run that with
`make soak-7day` against the installed node.

> **Gap to call out:** `make soak-7day` defaults to a **7-day** window,
> while this table's #3 target is **30 days**. The 7-day target is the
> CI-affordable floor; the 30-day MTBF is the user-facing bar. Neither
> is met today (the proxy earns ◐, not ✅).

## Quality-of-life numbers

- Cold sync to last 10 blocks: ≤ 30 s (FlyClient + UTXO snapshot path)
- Wallet UI first paint: ≤ 1 s
- Send tx → broadcast acknowledged: ≤ 2 s
- Block-explorer page render: ≤ 200 ms (already met)
- Disk 30 days: ≤ 15 GB
- Network egress idle: ≤ 100 KB/s
- CPU idle: ≤ 5%

## The halt clause

Operator paging rate target: **0/month**.
The user is not an operator. If zclassic23 stalls, recovery is automatic or the node
is broken. There is no acceptable middle ground where "operator manually deletes a
sentinel from progress.kv" is the answer.

## How we hit each number — architecture levers

| # | Lever |
|---|---|
| 1 | **A2 peer snapshot bridge** — adapts S-4b's cursor-stamp pattern to a chainstate downloaded over Tor from any peer; SHA3-verified against baked-in `g_sha3_windows`; same atomic stamp at snapshot height. Genesis-walk becomes the always-works fallback, not the default. |
| 2 | **Mmap'd flat block_index** (file already exists from prior speed work) + **parallel startup** of wallet/mempool/RPC/supervisor instead of the current serial chain. |
| 3 | **Wave S staged pipeline** (cursor-on-disk per stage = halts impossible by construction). Lint gate #18 at S-10 makes it permanent. |
| 4 | **Paged block_index** (LRU keeps last N=10k hot; rest live in mmap'd file). UTXO via LSM (already on roadmap at S-8 → utxo.lsm). |
| 5 | **Stage primitive's BEGIN IMMEDIATE** atomic commit + sentinel pattern (already shipped F-2/S-1/S-4b). Each stage's crash-replay test enforces. |

## How we know we hit them

`make bench` runs all 5 primaries against a clean checkout.
Results stream to `docs/bench-history.csv`.
CI fails any PR that regresses a primary > 20%.

By default the harness never touches the live service. Set
`ZCL_BENCH_LIVE_READONLY=1` to add read-only `/proc` samples for current RSS
and uptime when a live `zclassic23.pid` is available. Timing benchmarks that
restart or kill a node remain explicit subcommands.

## Dream roadmap (sequenced so each wave moves a benchmark)

1. **Wave B — Benchmarks (1 session).** `build/bin/zclassic23 -bench` runs all 5 primaries. `docs/bench-history.csv`. CI regression gate. **Today's numbers become the baseline.** Without this, every other wave is unmeasurable.
2. **Wave S close (3–6 sessions).** S-5..S-12. Halts extinct. Locks benchmarks 3 + 5.
3. **Wave P — Peer Snapshot (~3 sessions).** A2 of the bridge. Locks benchmark 1 (cold-start over Tor).
4. **Wave W — Warm-start (1 session).** Mmap + parallel boot. Locks benchmark 2.
5. **Wave R — RAM diet (2 sessions).** Paged block_index + LSM UTXO. Locks benchmark 4.

After these, ALL five benchmarks hold green continuously. That is the dream architecture.

## What's deferred (do not touch until the five are green)

Wave M (Claude as participant), Wave Z (ZNAM as DID), Wave T (deterministic simulator),
Wave R-release (signed reproducible builds). They are all good ideas. They do not
make the node strong for the user. They come later.
