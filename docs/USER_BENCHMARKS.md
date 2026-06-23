# zclassic23 — User Benchmarks (the only metrics that matter)

> The user does not care about halts, BLOCK_FAILED_MASK, or stage cursors.
> The user wants: install → it works → stays working → doesn't eat the machine.
> Every architectural decision is judged against these five numbers.

## The five numbers

> **Live values are tracked in one place:** [`BENCHMARKS_LOG.md`](./BENCHMARKS_LOG.md)
> (append-only, measured, dated). This table is the *spec* — targets and how to
> measure. Never quote an estimate here; the ledger is the only source for
> measured numbers.

| # | Benchmark | Target | How measured |
|---|---|---|---|
| 1 | **Cold-start to operational** (empty datadir → tip current within 100 blocks, RPC + wallet ready) | ≤ **60 s** | `time build/bin/zclassic23 -bench-coldstart` |
| 2 | **Warm-start to operational** (restart with synced datadir → same tip, RPC ready) | ≤ **10 s** | `time build/bin/zclassic23 -bench-warmstart` |
| 3 | **Stay-in-sync MTBF** (mean time between unattended stalls > 60 s) | ≥ **30 days** | 30-day chaos soak (kill -9, net blip, peer churn) |
| 4 | **RAM budget steady-state** | ≤ **1 GB RSS** | `zcl_status.memory_rss_mb` over a soak |
| 5 | **Recovery from kill -9** | ≤ **60 s** | scripted kill loop, recovery histogram |

### Soak: hermetic proxy vs. operational acceptance (#3/#4)

`make soak-ci` is a **3–5 min hermetic PROXY**: it spawns an isolated
`/tmp` regtest node with synthetic `generate`-load and scores the run
through the *same* verdict math (`lib/test/src/soak_harness.c`) as the
real soak — crash → `FAIL_CRASH`, tip stall → `FAIL_TIP_STALL`, RSS walk
past the post-warmup baseline → `FAIL_RSS_WALK`. It gives a fast green/red
CI signal that the soak machinery and RSS-plateau logic are sound.

It is **not** the acceptance run. The real #3 (≥ 30-day MTBF) and #4
(≤ 1 GB steady-state RSS) require **168 h+ of live wall time under real
tx load** with zero operator restarts. The node now holds tip AND
finalizes forward (forward-sync wedge cleared by `ab512d577` via a
complete SHA3-verified snapshot at h=3,156,809 loaded above the old
wedge), so soak wall-time can finally accrue; the snapshot is still
borrowed from the zclassicd oracle (sovereign from-genesis cure
pending). Run that with `make soak-7day` against the installed node.

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

## How we know we hit them

`make bench` runs all 5 primaries against a clean checkout.
Results stream to `docs/bench-history.csv`.
CI fails any PR that regresses a primary > 20%.

By default the harness never touches the live service. Set
`ZCL_BENCH_LIVE_READONLY=1` to add read-only `/proc` samples for current RSS
and uptime when a live `zclassic23.pid` is available. Timing benchmarks that
restart or kill a node remain explicit subcommands.

## Architecture levers and sequencing

The architecture levers and wave sequencing that move each benchmark live in
[`docs/work/FORWARD_PLAN.md`](./work/FORWARD_PLAN.md) and
[`docs/REFACTOR_STATUS.md`](./REFACTOR_STATUS.md). Deferred waves (M, Z, T,
R-release) wait until the five are green.
