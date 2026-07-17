# #26 — Wire per-tx contextual block checks into the live c23 reducer (spec)

> **Status: LANDED (reference, not a TODO).** Implementation in
> `app/jobs/src/script_validate_contextual.c`; disposition in
> `docs/work/security-audit-response-2026-06-09.md`. This doc is kept for the
> fork-safety analysis behind the gating conditions, which both the live code
> comment and the audit-response link here for.

Fork-safety-reviewed spec for audit finding #26 (the root cause of #2/#4): the
per-tx contextual rules — Overwinter expiry, network-upgrade version gating,
Sapling/Overwinter structural, per-tx finality (`bad-txns-nonfinal`), BIP34
`bad-cb-height` — run **nowhere** on the live reducer path.
`contextual_check_block()` (lib/validation/src/check_block.c:390) has zero
production callers.

## Verification corrections (do NOT skip — the raw synthesis had one false blocker)

- **BLOCKER-1 (synthesis claim "finality still uses MTP") is FALSE.**
  `contextual_check_block` line 411 already uses `block_header_get_time(&block->header)`
  (our FR-2). The synthesis agent misread the *comment* (which names "median-time-past"
  to explain why NOT to use it) as code. No finality change is needed; keep header time.
- **BLOCKER-2 (IBD granularity) is CONFIRMED against source.** zclassicd
  `ContextualCheckBlock` (zclassic-cpp/src/main.cpp:4070-4108) is itself NOT IBD-gated:
  it runs `ContextualCheckTransaction` (A), `IsFinalTx`/`bad-txns-nonfinal` (B), and BIP34
  `bad-cb-height` (C) for every block. The IBD short-circuit is INSIDE
  `ContextualCheckTransaction` (`if (isInitBlockDownload()) return true;`, main.cpp:941).
  ⇒ thread `is_ibd` into `contextual_check_block` and gate **only** the per-tx
  `contextual_check_transaction` call; **(B) finality + (C) BIP34 run unconditionally**.
- **Tip-proximity guard (BLOCKER-3) ADOPTED.** A restored datadir can replay historical
  heights with the IBD latch already false; gating the per-tx rules on the IBD latch
  alone could then re-reject a 2-year-old block (e.g. an "expired" tx zclassicd accepted
  during its own IBD). Add `next_h >= finalized_tip - K` so the gate can only fire near
  the live tip — structurally reproducing zclassicd's "ContextualCheckBlock runs at the
  contemporaneous tip" condition.

## History-safety argument (airtight)

During cold-sync/replay/reindex/snapshot, `is_initial_block_download(ms)` is true (tip
below `nMinimumChainWork` or older than `nMaxTipAge`) AND historical heights are below the
tip-proximity window ⇒ the per-tx rules are skipped — byte-identical to zclassicd's
`ContextualCheckTransaction` IBD short-circuit, so no "expired/wrong-version" historical tx
is newly rejected. Finality + BIP34 run during replay too, but every historical block
passes them (zclassicd enforced them unconditionally, so the chain has no violator). At the
tip, all rules run, matching zclassicd's `AcceptBlock → ContextualCheckBlock`.

## Chosen strategy: gate at the head of `script_validate` stage

`step_validate` in `app/jobs/src/script_validate_stage.c`, **after** the block body read and
**before** script verification — the c23 analogue of zclassicd `AcceptBlock →
ContextualCheckBlock(block, state, pindex->pprev)` (main.cpp:4203), which runs *before*
`ConnectBlock`/coins commit.

- **Strategy A (call from reducer_ingest_block after the drain) is mechanically broken:**
  `reducer_drain_to_convergence()` runs the full 8-stage pipeline first, so `utxo_apply`
  has already committed coins (`coins_kv_set_applied_height`) before that point. Marking the
  block failed there tears `coins_applied_height` vs the active-chain tip — the exact
  must-never-fork corruption. The contextual check MUST precede the coins commit.
- `script_validate` is the earliest live point where block body + `bi->pprev` + canonical
  height (`next_h`) + `g_ms` are all in scope, runs once per height on a forward-only
  cursor, and already owns the fail-closed `ok=0` mechanism.

## Reference implementation (as landed)

The source is `app/jobs/src/script_validate_contextual.c` (`contextual_check_block`
takes `is_ibd` and gates only the per-tx call) and the 3-part gate in
`script_validate_stage.c step_validate` (after body read, before script verify:
`next_h >= tip_h - CTX_TIP_WINDOW` with `CTX_TIP_WINDOW=16 >=` max legit pipeline
depth; on reject, hash-stamped `ok=0` row + cursor advance + `JOB_ADVANCED`,
touching no coins). The audit found zero *production* callers of the old path —
reindex uses `connect_block`, not this.

## Rule → zclassicd map (all verified against main.cpp)

| c23 rule | zclassicd | reject | DoS | IBD-gated |
|---|---|---|---|---|
| Overwinter expiry (contextual_check_tx expiry) | main.cpp:1010-1015 | tx-overwinter-expired | 100 (0 if just expired) | yes |
| version gating (sapling_structural) | main.cpp:949-999 | tx-overwinter-not-active / tx-overwintered-flag-not-set / bad-{sapling,overwinter}-tx-version-group-id / bad-tx-*-version-too-{low,high} / tx-overwinter-active | 100 | yes |
| pre-Sapling oversize | main.cpp:1018-1024 | bad-txns-oversize | 100 | yes |
| per-tx finality | main.cpp:4083-4090 (header time) | bad-txns-nonfinal | 10 | **no** |
| BIP34 coinbase height | main.cpp:4097-4102 | bad-cb-height | 100 | **no** |

## Reject blocks finalization without corrupting the cursor

`script_validate` writes `ok=0` (hash-stamped), advances its OWN cursor, JOB_ADVANCED, touches
no coins → `proof_validate` reads ok=0, writes ok=0 upstream_failed, advances → `utxo_apply`
reads proof ok=0 → `block_apply_failure` → **JOB_BLOCKED**, holds its cursor at H, writes no
coins, `coins_applied_height` unchanged → `tip_finalize` gated on the utxo_apply cursor →
stays JOB_IDLE, never publishes H. This is the existing, proven `spend_unknown`/`value_overflow`
reject shape; the hash-stamp lets a legitimate replacement block at H get a fresh verdict.

Test coverage: `test_script_validate_contextual_gate.c`.
