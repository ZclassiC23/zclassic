# Sync-Strength Fix — Implementation Design (SLATED FOR DELETION)

> **Status (robustness-audit findings, now folded into
> [`archive/stability-improvements-2026-06-16.md`](./archive/stability-improvements-2026-06-16.md),
> + tenacity-roadmap item 4):** this whole repair ladder — `coin_backfill`, ~1798
> LOC — is a runtime compensator for the exact import-skip the project decided to
> prevent at **write time** via the SHA3 import gate (shipped — the bless-time
> torn-import gate now lives in `block_index_loader_torn_gate.c` + its test
> `test_seed_torn_import_gate.c`). It is slated for **leaf-first deletion**
> after the sovereign `-refold-from-anchor` cutover makes copied-seed coin tears
> unreachable. The current delete/keep split is in
> [`archive/architecture-deletion-plan.md`](./archive/architecture-deletion-plan.md): delete the
> coin-backfill TUs, their public/private headers, the boot torn gate, and the
> `coin_backfill` diagnostics together; remove or rename the
> `script_validate.pending_prevout` producer; keep/re-home the reducer-frontier
> refill core, and treat tip-finalize row backfill as prune/re-home rather than
> blanket delete. `make lint` now includes `check-no-new-coin-backfill-caller`,
> so the public repair entry point cannot gain a second production caller before
> the subtraction. The exhaustive guard-ladder / scan-protocol / test-plan detail
> (the original §2–§5) has been archived; what remains is a summary plus the
> **consensus-safety argument**, which is load-bearing for as long as the code is
> still compiled. **Do not delete the safety reasoning while the code ships.**

**Scope:** (A) guarded repair for `prevout_unresolved` frontier holes (any old
missing coin); (B) peer-retention + body-download robustness so the node keeps
peers and bodies flowing while repairing.

---

## Mechanism

**Guarded multi-coin backfill ("frontier coin backfill"), NOT rewind/reapply.**
Rewind to before the creator height is structurally impossible on the wedged
datadir:

1. **Delta coverage.** `utxo_apply_delta` covers only a narrow recent window;
   creators tens of thousands of heights below it have no inverse images, and
   `rewindable_utxo_row` correctly refuses on the first missing `utxo_apply_log`
   row.
2. **Anchor floor.** H* floors at the durable trusted anchor; a rewind below it
   drops the public tip below served finality — violates never-regress doctrine.
3. **The hole is a state gap, not a verdict.** The missing coin was lost before
   the delta window began; no replay of logged history re-derives it. The raw
   creating block (present, byte-verified) is the only remaining authority.

The backfill re-derives the coin from the raw creating block, hash-verified
against the active chain, then proves no-spend. It refuses loudly on anything
unprovable, mints nothing, and touches only the coin insert — no cursor, no
`*_log`, never `tip_finalize_log`. After insert, the existing stale-script
replay re-runs on the next tick and performs the already-proven cursor rewind.

The repair keys on the **lowest reducer hole** when its status is
`prevout_unresolved`, enumerates **all** unresolved prevouts of the failing
block in one pass. The delta horizon is retained as observability for old-hole
geometry, but it is not a consensus refusal boundary: coins created inside the
recent delta window are repairable when the creating block is active-chain
hash-verified and the terminal-bound no-spend scan proves the coin was unspent
at the frontier.

`node.db` txindex is only a locator hint. If it misses a creator, the job scans a
bounded recent active-chain window (32K blocks) before accepting
`txindex_miss` as terminal; legacy unversioned `txindex_miss` markers are
deleted and re-proven. Newly proven terminal misses are written as
`txindex_miss:v2`. **Refusals are whole-set** (no partial inserts). Every
outpoint is one-shot, so any future lost coin heals through the same gate.

## Consensus-safety argument (load-bearing)

The no-spend proof scans every applied active-chain block in `(creator,
frontier)`, and the entire scanned segment is **hash-chain-bound to the hole
block's own chain**: a persisted running last-scanned hash prev-links every
chunk and resume boundary; terminal linkage extends the chain through
`[frontier..H]` to require block H's hash equals the hole row's hash; and the
insert transaction re-binds the proof to the *insert-time* active chain. By
second-preimage resistance of double-SHA256, every scanned block is provably the
ancestor, at its height, of the chain block H extends — no reorg/oscillation
interleaving can stitch branches into the proof.

The repair is structurally incapable of minting:
- **a spent coin** — the scan finds the spend → refuse-permanent;
- **an off-chain coin** — the creating tx must hash-match inside an active-chain
  block, either via the txindex locator or the bounded active-chain fallback;
- **a cross-branch coin** — the chain binding above.

**Owner-gated:** `ZCL_REDUCER_COIN_BACKFILL_ACK=1` is required (it mints state →
strictest gate). Every refusal status pages directly from the Job (typed blocker
+ `EV_OPERATOR_NEEDED`, once-latched), independent of condition-engine attempt
exhaustion, per the silent-halt doctrine.

The single write transaction (`coins_kv_add` + outpoint marker + replay-marker
delete), under `progress_store_tx_lock()` + `BEGIN IMMEDIATE`, is all-or-nothing
and re-checks active-chain binding inside the tx; coins_kv shares the progress.kv
handle, so a kill-9 anywhere is atomic.

## Peer-retention / body-download fixes

Minimal surgical subset, all safe vs malicious peers, that keep peers and bodies
flowing while repairing:
- **P1** — credit the header-usefulness counter on **new-to-index** headers only
  (never `accepted`, which includes known headers a withholding peer could
  replay).
- **P2** — frontier-parity gate on the stale-header rules: at frontier parity
  getheaders cannot be useful, so disconnecting is pure churn.
- **P3** — trusted-peer exemption (localhost / whitelisted) on the stall rules;
  protects the loopback zclassicd lifeline.
- **P4** — fix tip_watchdog backpressure accounting so the OOM-guard is
  structurally reachable in both regimes and stops false-triggering on queued
  hashes; add re-arm hysteresis.
- **P5 (optional)** — only reset addnode backoff for addnodes that ever
  handshook.

Combined: peers survive, headers track the network tip, bodies flow to disk
under a sane bound — so the moment the repair clears, catch-up is immediate.

## Tests / gate

Build/lint/test gate:
`make -j"$(nproc)" build-only && make lint && make test_parallel`
(read the "N passed, M failed" line). Inner loop:
`make t ONLY=stage_repair_coin_backfill` (never `test_zcl`).

The archived spec defined the full adversarial test matrix (spent-mid-range,
off-chain creator, delta-window creator, scan gap, present-but-unusable,
coinbase immaturity, multi-coin, resume integrity, owner gate, chain rebind on
kill-9+reorg, terminal linkage) plus the net-fix tests (P1–P4). Recover from git
history if the code survives past the import gate.
