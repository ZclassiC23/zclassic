# #26 — Wire per-tx contextual block checks into the live c23 reducer (spec)

Implementation-ready, fork-safety-reviewed spec for audit finding #26 (the root
cause of #2/#4): the per-tx contextual rules — Overwinter expiry, network-upgrade
version gating, Sapling/Overwinter structural, per-tx finality (`bad-txns-nonfinal`),
BIP34 `bad-cb-height` — run **nowhere** on the live reducer path. `contextual_check_block()`
(lib/validation/src/check_block.c:390) has zero production callers.

Produced by a 19-agent design+adversarial-verify workflow (3 strategies × 5
fork-safety dimensions + synthesis), then **every load-bearing claim verified by
hand against the zclassicd source** before trusting it. Corrections below.

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

## Code (apply only after re-verifying each symbol — see "verify before implementing")

### `contextual_check_block` — add `is_ibd`, gate only the per-tx call
```c
bool contextual_check_block(const struct block *block, struct validation_state *state,
                            const struct chain_params *params,
                            const struct block_index *pindex_prev, bool is_ibd)
{
    int nHeight = pindex_prev == NULL ? 0 : pindex_prev->nHeight + 1;
    for (size_t i = 0; i < block->num_vtx; i++) {
        if (!is_ibd) {                       /* zclassicd main.cpp:941 IBD short-circuit */
            if (!contextual_check_transaction(&block->vtx[i], state,
                                              &params->consensus, nHeight, 100))
                LOG_FAIL("check_block", "contextual_check_transaction tx[%zu] h=%d", i, nHeight);
        }
        /* finality cutoff = header time (already FR-2); runs ALWAYS, incl. IBD */
        int64_t cutoff = block_header_get_time(&block->header);
        REJECT_UNLESS(is_final_tx(&block->vtx[i], nHeight, cutoff), state, 10, "bad-txns-nonfinal");
    }
    if (nHeight > 0)
        REJECT_UNLESS(bip34_check_coinbase_height(&block->vtx[0], nHeight), state, 100, "bad-cb-height");
    return true;
}
```
Callers to update with `is_ibd=false`: any boot-reindex/mempool caller + the test callers
(`test_chain.c`, `test_bip113_bip65.c`, `test_validation.c`). (Confirm the real caller set —
the audit found zero *production* callers; reindex uses `connect_block`, not this.)

### `script_validate_stage.c step_validate` — the 3-part gate, before script verify
```c
const struct chain_params *cp = chain_params_get();
bool ibd = is_initial_block_download(ms);
int  tip_h = tip_finalize_stage_last_height();        /* finalized tip */
const int CTX_TIP_WINDOW = 16;                         /* >= max legit pipeline depth */
if (cp && bi->pprev && next_h >= tip_h - CTX_TIP_WINDOW &&
    !process_block_should_skip_contextual_header(ms, bi->pprev, &cp->consensus)) {
    struct validation_state cstate; validation_state_init(&cstate);
    if (!contextual_check_block(&blk, &cstate, cp, bi->pprev, ibd)) {
        const char *reason = cstate.reject_reason[0] ? cstate.reject_reason : "contextual-invalid";
        block_free(&blk);
        atomic_fetch_add(&g_contextual_reject_total, 1);
        event_emitf(EV_BLOCK_REJECTED, 0, "script_validate contextual_invalid h=%d reason=%s", next_h, reason);
        /* hash-stamped ok=0 row -> proof_validate ok=0 -> utxo_apply JOB_BLOCKED:
         * coins never mutate, tip_finalize stays IDLE. Reorg-safe via phashBlock. */
        if (!script_validate_log_insert(db, next_h, reason, false, 0, 0, NULL, -1,
                                        SCRIPT_ERR_OK, bi->phashBlock)) return JOB_FATAL;
        c->cursor_out = c->cursor_in + 1;
        return JOB_ADVANCED;
    }
}
```
Surface `g_contextual_reject_total` in `script_validate_dump_state_json` + reset on shutdown.

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

## Test plan (new `test_script_validate_contextual_gate.c`, harness default CHAIN_MAIN)

1. expired-at-tip → ok=0, tx-overwinter-expired, BLOCK_VALID_SCRIPTS unset, counter++.
2. non-final-at-tip → bad-txns-nonfinal. **Discriminator:** a tx with nLockTime between MTP
   and header time is ACCEPTED (proves header-time cutoff, not MTP).
3. wrong-version-for-height → tx-overwintered-flag-not-set / bad-sapling-tx-version-group-id.
4. bad-cb-height (reuse test_chain.c BIP34 wrong-height fixture).
5. NEGATIVE: each invalid block with IBD=true → per-tx gate skipped; BUT finality + BIP34
   STILL fire under IBD (proves BLOCKER-2 + parity).
6. NEGATIVE: IBD=false but finalized tip far above next_h (next_h = tip-1000) → gate SKIPPED
   (proves the tip-proximity guard prevents history re-rejection).
7. snapshot-tail: sparse pprev → `process_block_should_skip_contextual_header` true → skipped.
8. valid-at-tip → no reject, BLOCK_VALID_SCRIPTS set, counter unchanged.
9. cursor cascade: script+proof advanced, utxo_apply JOB_BLOCKED cursor held at H,
   coins_applied_height unchanged, tip_finalize IDLE, no JOB_FATAL.
10. self-heal-benign: drive the utxo_apply blocker escape past retry_budget for a
    contextually-invalid block → NO anchor rewind / height wipe / reindex / BLOCK_FAILED_MASK.
11. regression: test_script_validate_stage, test_reducer_ingest_e2e, test_chain (BIP34),
    test_bip113_bip65, test_validation — green; update their contextual_check_block calls.
12. lint guard (test_make_lint_gates): script_validate_stage references all four gate symbols.

## Verify-before-implementing (symbols the spec assumes; confirm signatures/lines first)

`tip_finalize_stage_last_height()`, `script_validate_log_insert(...)` exact arg list,
`REJECT_UNLESS` / `validation_state_init` availability in this TU, `bip34_check_coinbase_height`,
`process_block_should_skip_contextual_header`, the exact step_validate body-read/insert lines,
and whether `contextual_check_transaction` runs shielded proofs at tip (see coordination note).

## ⚠ COORDINATION + merge-gating (read before implementing)

- **Codex collision:** `script_validate_stage.c` (the chosen insertion file) AND
  `proof_validate_stage.c` are Codex's ACTIVE files — the forward-sync stall (prevout_unresolved)
  is in `script_validate`, and #3 (JoinSplit Ed25519 sig) is in `proof_validate`. Implementing
  the #26 wiring in `script_validate` in parallel will hard-conflict on merge. **Sequence #26
  AFTER Codex's script_validate forward-sync work lands, or assign the wiring to Codex.**
- **#3 overlap:** if c23's `contextual_check_transaction` runs the JoinSplit Ed25519 check, the
  at-tip contextual call would enforce #3 too (defense-in-depth with Codex's proof_validate fix,
  not a conflict — both reject the same forged-sig block). Confirm and de-dup if desired.
- **Merge-blocking datadir-copy proves (owner-gated, on a COPY never live):**
  1. Historical-replay ZERO-reject: with IBD latched false, drive the reducer across the
     post-IBD historical window, assert zero new `ok=0` contextual_reject rows below the
     tip-proximity window. (The must-never-fork proof.)
  2. utxo_apply blocker-escape: force a contextual reject at a synthetic tip, exhaust
     retry_budget, confirm a benign indefinite hold — NO destructive self-heal / BLOCK_FAILED_MASK.
  3. IBD-latch timing parity: c23 `nMinimumChainWork` / `nMaxTipAge` match the deployed
     zclassicd (same chainparams) so the latch flips at the same chain state.
