## ONE ordered fix plan — collapse the second authority, match zclassicd's one-writer-per-block model

> **STATUS (2026-06-22): the live node is NO LONGER wedged** — the consolidated
> daily-driver loader reaches tip via a consensus-bound stopgap
> (`docs/HANDOFF.md`). STEP 0/1 (the `-refold-from-anchor` un-wedge runbook + the
> loader-owns-seed / consensus cross-check) are **DONE / superseded** by the
> deployed fixes. This doc is **retained as the subtraction worksheet** for the
> sovereign cure — the file:line decomposition of the ~715-LOC permanent deletion
> (HANDOFF P2-3). **Re-verify every line number against the current
> `deploy/daily-driver` tree before deleting**, and re-frame STEP 0 from
> "un-wedge" to "cut the live node over from the stopgap to the self-folded
> foundation."

**Root (verified):** cold import bulk-copies zclassicd's `node.db.utxos` into `coins_kv`, stamps `COINS_KV_MIGRATION_COMPLETE_KEY` (so `coins_kv_is_proven_authority` at `lib/storage/src/coins_kv.c:396-427` reads it as a proven authority), and stamps the 8 stage cursors + `coins_applied_height` forward over a span with NO per-height log rows. `reducer_frontier` then pins H* at the compiled checkpoint (3,056,758) because the contiguous ok=1 log prefix has a hole at anchor+1, and forward finalization halts. The cure mechanism already exists and is proven: `boot_refold_from_anchor_reset` / `refold_from_anchor_active` (`config/src/boot_refold_staged.c:419-465`) forces the 8 cursors to the SHA3-verified anchor + sets `coins_applied_height=anchor+1` in one txn, then the live single fold (`app/jobs/src/utxo_apply_stage.c:520-563`) re-derives forward, co-committing coins + cursor + ok=1 log row per height.

---

### STEP 0 — Migrate the already-wedged live datadir FIRST (READ-ONLY today; the owner runs this after the build lands)
This is NOT a code change — it un-wedges the existing `~/.zclassic-c23` without re-investigation. The proven un-wedge runbook (MEMORY `project_unwedge_runbook_verified_2026-06-21`):
1. Ensure a SHA3-verified anchor snapshot is reachable at `<datadir>/utxo-anchor.snapshot` (or `$ZCL_MINT_ANCHOR_OUT`). If absent, mint it: `build/bin/zclassic23 -mint-anchor-fast` (writes the snapshot bound to the compiled checkpoint root, count 1,354,771). Have one at `/tmp/utxo-anchor-3056758.snapshot` per MEMORY — copy it into the datadir.
2. Un-wedge with `build/bin/zclassic23 -refold-from-anchor` (NOT `-load-verify-boot`, which no-ops on the stamped contaminated `coins_kv` = false-green per the verified runbook). This calls `boot_refold_from_anchor_reset` → forces cursors to anchor, clears `COINS_KV_MIGRATION_COMPLETE_KEY` trust (via `utxo_recovery_clear_cold_import_seed`, `boot_refold_staged.c:324`), re-seeds from the verified snapshot, folds anchor+1..tip.
3. Gate success on H* CLIMBING past the anchor (`zcl_state subsystem=reducer` / `reducer_frontier_compute_hstar`), NOT merely "booted without FATAL". Cross-check the resulting tip hash vs zclassicd at the same height before declaring done.

Copy-prove on the frozen wedge fixture `~/.zclassic-c23-postrestore-wedge-20260611` (CLAUDE.md Tenacity rule: never live surgery) before touching `~/.zclassic-c23`. **Note:** a from-genesis fold is currently running on a copy — do not touch any `~/.zclassic-c23*` until it finishes (prompt constraint).

After STEP 6 lands, a *fresh* `-refold-from-anchor` is unnecessary because the cold-start default itself never plants the contaminated seed (STEP 6) — but the migration above remains the recovery path for any datadir already carrying the stamped seed.

---

### STEP 1 — Make bodies-only refold-from-anchor the DEFAULT cold-start (the load-bearing flip)
`config/src/boot_services.c:1496-1502` is THE single normal-boot seed-vs-refold decision site. Today: `armed_from_anchor = refold_from_anchor_active() || boot_refold_from_anchor_arm_if_torn(...)`; if NOT armed → `block_index_loader_seed_stages_from_cold_import(...)`. The auto-arm only fires on a DURABLE TEAR (`block_index_loader_torn_import_detect` finds a `prevout_unresolved` hole + `coin_backfill.refused` marker), so a fresh-but-not-yet-wedged cold import still seeds and re-creates the wedge.

**Edit `config/src/boot_services.c:1496-1502`:** drop the torn-detect gate and arm the from-anchor refold whenever a SHA3-verified snapshot is reachable. Concretely, add a default-true arm:
```c
bool armed_from_anchor =
    refold_from_anchor_active() ||
    boot_refold_from_anchor_arm_default(svc->state, boot_node_db(svc),
                                        progress_store_db());
if (!armed_from_anchor)
    (void)block_index_loader_seed_stages_from_cold_import(
        svc->state, boot_node_db(svc), progress_store_db());
```
Add `boot_refold_from_anchor_arm_default` in `config/src/boot_refold_staged.c` as a thin variant of `boot_refold_from_anchor_arm_if_torn` (`:552-613`): it arms whenever `anchor_snapshot_verified_reachable(ndb, cp)` (`boot_refold_staged.c:166-183`) returns true — WITHOUT requiring `block_index_loader_torn_import_detect` to fire — and KEEPS the `anchor_snapshot_verified_reachable == false` DECLINE (`:586-594`) so a no-snapshot datadir still falls through to the honest `operator_needed` halt (`block_index_loader_torn_import_gate_fires`) rather than FATALing on the contaminated node.db reseed. The seed path becomes the fallback that runs ONLY when no verified snapshot exists. (Simplest: extend `boot_refold_from_anchor_arm_if_torn` itself to arm on reachable-snapshot regardless of torn-detect, reusing its existing DECLINE + reset machinery — fewer new symbols.)

**Reconcile the integrity-abort skip:** `config/src/boot.c:3469-3480` downgrades the post-restore `chain_restore_finalize` integrity abort to a warning only when `mint_or_refold = ctx->mint_anchor || ctx->refold_from_anchor || ctx->refold_staged`. The default-flip arms the refold WITHOUT setting any ctx flag, so this skip would NOT engage and a fresh boot could FATAL on the integrity gate before the fold runs. **Fix:** extend the `mint_or_refold` condition to also include `refold_from_anchor_active()` (the durable signal the default arm sets), so the auto-armed default refold gets the same warning-not-FATAL treatment.

---

### STEP 2 — DELETE the cold-import bulk-copy seed call (FIX a, producer 1)
`app/services/src/utxo_recovery_restore.c:357-394`, the `imported_count > 100000` accepted gate (the path the normal `--importblockindex + boot` takes). DELETE:
- `:369-371` the `coins_kv_seed_from_node_db(progress_store_db(), ...)` call (the bulk-copy into `coins_kv` with zero log rows).
- `:381-384` the `utxo_recovery_write_cold_import_seed(...)` call (records `cold_import_seed_anchor_*` keys the wedge-heal consumer later stamps cursors from).
- KEEP `:359-363` the `leveldb_utxo_migrated` stamp and `:391-393` `seed_integrity_stamp_utxo_sha3` ONLY IF the LDB→node.db mirror read itself is retained as a one-time chainstate read (see STEP 8). The COINS_KV seed + the cursor-forward seed keys are the defect; remove those two calls.

**~3 LOC of calls deleted here** (the surrounding SHA3-verify + count logic stays as the one-time chainstate read or dies in STEP 8).

---

### STEP 3 — DELETE the borrowed restore stamp (FIX a, the frontier_exempt install)
`app/services/src/utxo_recovery_restore.c:234-255`, the `ldb_import_found` branch. DELETE the seed machinery:
- `:240-242` the `utxo_recovery_commit_tip(ctx, &commit_blk, "ldb_import_found", true, true)` — the 5th arg `frontier_exempt=true` is the ONLY production call site passing true (all 7 other `utxo_recovery_commit_tip` callers — `utxo_recovery_service.c:332,:601,:363` and `restore.c:548,:698` — pass false). It bypasses the INVARIANT-A header-frontier derivability gate at `utxo_recovery_service.c:74-93` and installs `coins_best` at the import height with no per-height derivability check.
- `:252-254` the paired `utxo_recovery_write_cold_import_seed(...)`.

**Consensus-parity flag (STEP 3 + STEP 7):** removing the `frontier_exempt=true` install RESTORES the INVARIANT-A gate for every tip install. This is parity-RESTORING toward zclassicd's single-authority-per-block model (zclassicd installs a tip only via `ConnectTip`, gated). It does NOT alter a block/tx validity predicate — it changes which tip gets *committed to coins_best*, never what blocks are *accepted*. Safety check: confirm via a replay on the wedge fixture that with the gate always-on, the genesis→anchor→tip fold still installs the same tip the proven runbook reaches (H* climbs to the real tip, hash-matches zclassicd). **~3 LOC deleted.**

---

### STEP 4 — DELETE the wedge-heal STAMP consumer + its boot wiring
- DELETE `block_index_loader_seed_stages_from_cold_import` (`app/services/src/block_index_loader_rebuild.c:534-768`) — the function that ADVANCES `coins_applied_height` to H+1 (`cold_import_set_applied_if_behind` at `:728`) and stamps the 7 upstream cursors + tip_finalize to H over a log-less span (`tip_finalize_stage_seed_anchor(H, trusted=true)` at `:743`). This is the proximate cause of H* pinning. Once STEP 2/3 stop writing the `cold_import_seed_anchor_*` keys, it early-returns at `:544-545` on every datadir — but delete it outright to keep the invariant total.
- DELETE its only helper `cold_import_set_applied_if_behind` (`:500-532`).
- DELETE the `COLD_IMPORT_SEED_*` key `#define`s (`:486-488`).
- DELETE the boot call site fallback at `config/src/boot_services.c:1500-1502` (already collapsed in STEP 1 — the `else` branch is gone).
- DELETE the prototype in `app/services/include/services/block_index_loader.h:206` (+ the doc comment `:184`).

**~270 LOC deleted** (`block_index_loader_rebuild.c` function ~235 + helper ~32 + defines/proto).

---

### STEP 5 — DELETE the cold-import seed-provenance lifecycle (now unreferenced)
`app/services/src/utxo_recovery_seed_provenance.c` (111 LOC):
- DELETE `utxo_recovery_write_cold_import_seed` (`:42`) — both producers (restore.c:252, :382) are gone.
- DELETE `utxo_recovery_read_cold_import_seed` (`:71`) and `utxo_recovery_set_cold_import_trust_anchor` (`utxo_recovery_frontier_gate.c:127`) — called only from the PART-B/C trust-anchor block `utxo_recovery_restore.c:629-695` (read at `:638`, set at `:645`); DELETE that whole block (it becomes a no-op once nothing writes the keys — `seed_anchor_hash_p` stays NULL on every datadir).
- **KEEP `utxo_recovery_clear_cold_import_seed`** (`:94`) — still called by the wipe path (`utxo_recovery_service.c:248,:271`) and the KEEP refold path (`boot_refold_staged.c:58,:324`) to neutralize any stale seed.
- DELETE the now-dead prototypes in `app/services/src/utxo_recovery_internal.h:135,:146,:158`.

**~90 LOC deleted** (provenance functions ~70 + restore.c:629-695 block ~30, minus the kept clear).

---

### STEP 6 — KEEP the seed_exempt parameter; KEEP coins_kv_seed_from_node_db; DO NOT touch stage_anchor.c:153
Critical correction to the naive "delete the bypass" reading: `seed_exempt` (`app/jobs/src/stage_anchor.c:138,:153`) is fed by `trusted_seed` from 6+ LEGITIMATE callers via `tip_finalize_stage_seed_anchor` — reindex_epilogue (KEEP), boot_refold_staged (KEEP), snapshot_apply (KEEP, `:91`), reducer_ingest (KEEP, always false). Those paths co-commit real log rows, so the exemption is correct there. The invariant is enforced UPSTREAM by deleting the cold-import producer (STEP 2/3/4), NOT by removing the exemption. **KEEP `stage_anchor.c:153`, KEEP the `seed_exempt` parameter, KEEP `coins_kv_seed_from_node_db` (`coins_kv_boot_rebuild.c:123-185`)** — it is the shared reseed primitive for reindex_epilogue.c:98 and boot_refold_staged.c:348. After STEP 2/3/4, NO cold-import caller reaches `seed_exempt=true` over a no-log span.

---

### STEP 7 — ADD the invariant guard (FIX c — the structural enforcement)
Add a debug-build assertion at the two forward-advance choke points so a regression that re-plants a seed is caught by construction, matching zclassicd's "one writer, one cursor, one log row per block" model:
- `lib/util/src/stage.c` `cursor_write_locked` (`:457`): on a FORWARD advance (`ctx.cursor_out > cursor_in`, the existing `:444` invariant), assert the stage's own log row exists at every height in `(cursor_in, cursor_out]`. In practice every legit forward step advances by exactly one (`JOB_ADVANCED`), so this is a single point-probe for `cursor_out`.
- `lib/storage/src/coins_kv.c` `coins_kv_set_applied_height_in_tx` (`:358`): on a RAISE (`height > current`), assert an ok=1 `utxo_apply_log` row exists at `height-1` in the same txn. Downward sets (reorg/repair) are exempt — they delete rows above the target in the same txn (this is why the function has NO monotonic floor, comment `:362-363`).

Guard behind `#ifndef NDEBUG` / a `ZCL_INVARIANT_CHECKS` flag so it is an assertion in test/CI, not a hot-path cost in production (the per-block fold already opens the txn; a point-probe is cheap, but keep it gated to honor the profile-first doctrine). Pair with two grep lint gates per `docs/work/never-stuck-plan.md:357,359`: `check-no-rowcount-install` (forbid `INSERT...SELECT FROM coinssrc.utxos` outside the kept reseed primitive's two legit callers) and `check-no-seed-exempt-stamp` (forbid a NEW `frontier_exempt=true` / `trusted_seed=true` call site outside the allow-list). **NOTE:** the existing `make lint` gates are fail-silent/hollow (MEMORY `project_lint_gates_fail_silent_2026-06-19`) — write these with a non-empty scan set and a non-swallowed grep exit, and inject-verify each fires RED on a re-added violation.

---

### STEP 8 — INVESTIGATE (do deliberately, don't bulk-delete): the LDB body import + reindex_epilogue assertion
- `config/src/boot.c:2480-2499` runs `utxo_recovery_import_ldb` on every non-log-rebuild boot when `!ctx->no_legacy_auto_import`. The dissection wants the bulk-copy SEED gone, not necessarily the LDB chainstate READ. Decide: (a) if the bodies-only refold fully replaces the need to read `~/.zclassic` chainstate, delete this call and the LDB read machinery (`utxo_recovery_ldb_copy.c` becomes dead — STEP 9); (b) if a one-time chainstate read into `node.db` is retained as the body source, keep the read but ensure it NEVER seeds `coins_kv`/cursors (STEP 2/3 already removed the seeds). Bias toward (a) given the refold folds on-disk bodies and the snapshot supplies the anchor coin set.
- reindex_epilogue (`app/services/src/reindex_epilogue.c`, KEEP, 236 LOC): it legitimately co-commits — a from-genesis replay through `connect_block` writes every log row, THEN reseeds (`:98`) + raises `coins_applied_height` (`:156`) + stamps cursors via `tip_finalize_stage_seed_anchor(trusted=true)` (`:178`). Under the strict STEP 7 invariant, ADD an explicit assertion before the `:178` stamp that every height in `(0, tip_h]` has an ok=1 `utxo_apply_log` row (it does — the replay wrote them), so the trusted-seed stamp is provably backed. This keeps the path but proves it satisfies the invariant rather than relying on the exemption blindly. Investigate whether reindex can instead end by folding through `utxo_apply_stage` so cursors advance the legit way (larger refactor — defer).

---

### STEP 9 — DELETE the two whole subsystems with no remaining trigger (after STEP 2-6 land + replay-gate clears)
Once the bulk-copy seed is gone, there is no foreign coin set to be incomplete, so no `prevout_unresolved`-from-tear ever arises. These lose their only input:

**(A) The seed-tear coin-backfill engine — CONSENSUS-PARITY FLAGGED:**
- `app/jobs/src/stage_repair_coin_backfill.c` (795) + `_scan.c` (539) + `_util.c` (483) = **1817 LOC**. Its ONLY consensus mutation is a `coins_kv` INSERT (mints a UTXO outside the single fold writer — a second writer into the consensus UTXO set). Deleting it is parity-RESTORING (zclassicd has no re-derivation path), BUT the deletion touches the consensus UTXO surface → **MUST be replay-gated** against the real chain (h=478544 doctrine): prove a genesis→anchor→tip fold reproduces count==1,354,771 + the SHA3 root with this engine absent, on the real chain, before deleting.
- Tests: `test_stage_repair_coin_backfill.c` (2246 LOC).
- `app/jobs/src/stage_repair_reducer_frontier_coin.c` (786 LOC) — the coin-rewind half of the L2 tear handling; seed-tear-only, deletable.

**(B) The torn-import detect/refuse + LDB tear-avoidance copy:**
- `app/services/src/block_index_loader_torn_gate.c` (234 LOC) — torn-import DETECT + REFUSE. NUANCE: `block_index_loader_torn_import_detect` is consumed by the auto-arm (`boot_refold_staged.c:571`); once STEP 1 makes refold the default (armed on reachable-snapshot, not on torn-detect), this consumer's torn precondition is replaced by "refold is default" — delete AFTER STEP 1 inverts the polarity. Tests: `test_seed_torn_import_gate.c` (769) + `test_torn_index_blocks_tip.c` (356).
- `app/services/src/utxo_recovery_ldb_copy.c` (93 LOC) — point-in-time copy of a live zclassicd LevelDB (tear-avoidance for the borrowed copy). Fully dead with no borrowed copy. Delete iff STEP 8 chooses (a).

**PRUNE-not-delete (mark, don't fully remove):** `reducer_frontier_reconcile_light.c` (677, prune the `refused_coin_tear` + tear-bypass ~150 LOC, KEEP the crash-recovery cursor-clamp role), `stage_repair_reducer_frontier.c/_refill.c/_tipfin.c/_purge.c` (~2526, keep a thin partial-crash cursor-skew slice). These retain a legitimate non-seed role (clamping a cursor a partial-crash left ahead of its own log).

**MUST KEEP (other reason to exist):** `reducer_frontier.c` (833, the North Star L0 fold authority), the `legacy_mirror_*` drift-monitor family (1642, standalone consensus-parity drift detection vs zclassicd), the generic `prevout_unresolved` status in `script_validate_stage.c:282-496` (a legitimate validation outcome for ALL prevout failures — CONSENSUS surface, do not touch; only its seed-tear CONSUMERS are dead).

---

### STEP 10 — Proving tests (two new deterministic tests + one regression-flip invariant)

**PROOF #1 — POSITIVE (extend `lib/test/src/test_reducer_forward_progress_gate.c`):** after PART-1 reaches tip N (the existing convergence loop, asserts at `:527-535`), ADD after `:535`:
```c
int32_t hstar=-1, served=-1; bool ok=reducer_frontier_compute_hstar(progress_store_db(),&hstar,&served);
int32_t applied=-1; bool found=false; coins_kv_get_applied_height(progress_store_db(),&applied,&found);
RFP_CHECK("H*==tip==coins_applied-1 (zclassicd one-cursor invariant)",
          ok && found && hstar==RFP_N && applied==RFP_N+1);
```
This is the literal "coins-applied == finalizable == provable tip, no rowless span" condition — FALSE on the live wedge (hstar pinned at 3,056,758 while coins_applied stamped to 3,150,489), TRUE post-fix. Already wired: `make mvp-forward-progress` (Makefile:850,892, selector `reducer_forward`, sentinel `=== reducer-forward subset complete:`), `ZCL_TEST_ONLY=reducer_forward` (test.c:124-127), opt-in via `ZCL_STRESS_TESTS=1`.

**PROOF #2 — NEGATIVE (new `lib/test/src/test_seed_no_log_hole.c`, fork-child harness mirroring `test_refold_auto_arm.c:399-417 raa_run_child`):**
- Reuse `raa_seed_torn_progress` / `raa_set_applied` (`test_refold_auto_arm.c:144-198`) to stamp `coins_applied_height` to a frontier above a rowless span (the exact "coins ahead of log" state the deleted seed created).
- `T_pinned`: assert `reducer_frontier_compute_hstar` pins H* at the anchor, NOT at the stamped frontier (engine refuses the hole + names the height — the honest-halt property).
- `T_heal`: with a matching SHA3 snapshot present (`raa_build_matching_snapshot`, `:225`), call the DEFAULT cold-start trigger (the STEP-1 arm function), assert it resets the 8 cursors to anchor + `coins_applied_height=anchor+1` (`boot_refold_staged.c:419-426`) and `refold_from_anchor_active()==true`.
- `T_invariant` (the decisive pin): after draining the fold to tip, for the 5 upstream success-checked logs assert `reducer_frontier_log_frontier(db, log_table, cursor_name, &prefix)` yields `prefix == coins_applied_height-1` (use `utxo_apply_stage_succeeded_at(H)` as the per-height ok=1 oracle, already used at `test_reducer_forward_progress_gate.c:533`). NO height H where `coins_applied_height` advanced but `utxo_apply_log` has no ok=1 row.
- Register in `test.c` (mirror the `:786` line + a `reducer_forward`-style selector), add a Makefile sentinel + `mvp_gate` (Makefile:835 macro, false-green guarded) so CI enforces it.

**Replay-gate the parity claim:** the change alters HOW the committed UTXO set is constructed (re-derived vs copied) — consensus-adjacent. The decisive gate is the mint: a genesis→anchor (+anchor→tip) fold must reproduce count==1,354,771 + the SHA3 root (`checkpoints.c`). Cite the OPEN parity blocker the fold must clear first: `utxo_apply_delta.c` over-counts unspendable outputs (missing the `coins.c:86 script_is_unspendable` exclusion per never-stuck-plan.md:227-244) — the fold FATALs on a count mismatch until that one-line parity fix lands. Run PROOF #2 under `ZCL_STRESS_TESTS`, do NOT assert exact production counts in the hermetic test — the EXACT-count assertion belongs in the live/copy mint replay on the wedge fixture.

---

### Consensus-parity safety check per consensus-adjacent edit
- **STEP 3/7 (frontier_exempt removal + INVARIANT-A always-on):** parity-RESTORING, not a validity-predicate change. No block/tx is newly accepted or rejected; only tip-install gating changes. Verify on the wedge fixture that the always-on gate installs the same tip the proven runbook reaches (hash-match zclassicd at the anchor + at tip).
- **STEP 9(A) (delete stage_repair_coin_backfill):** the ONLY out-of-band `coins_kv` consensus MUTATION. Deleting it is parity-RESTORING but touches the consensus UTXO surface → full-history replay against the REAL chain (NOT the reference text — CLAUDE.md rule, the chain has the 125,811-byte tx at h=478544) MUST show the fold reproduces the exact UTXO set + SHA3 root with the engine absent, before the delete merges. Gate behind E13 `check-consensus-parity` + the `test_consensus_parity` golden group.
- **All other STEPs (1,2,4,5,6,8,10):** cursor/log/seed plumbing only — no consensus predicate touched. STEP 1's default-flip changes which state seeds `coins_kv` at boot (snapshot-verified anchor vs node.db copy); the from-anchor path then runs the REAL script/proof/utxo_apply/tip_finalize stages forward, so the committed set is fully validated, identical to the fold.

---

### Deletable LOC (conservative)
- Production fully deletable now (STEP 2-5): restore.c seed/stamp calls ~6, `block_index_loader_rebuild.c` consumer + helper + defines ~270, `utxo_recovery_seed_provenance` net ~90, restore.c:629-695 block ~30 ≈ **~400 LOC**.
- Production deletable after replay-gate + polarity inversion (STEP 9): coin-backfill engine 1817 + `_frontier_coin.c` 786 + torn_gate 234 + ldb_copy 93 ≈ **~2930 LOC**.
- Tests deletable (STEP 9): 2246 + 769 + 356 ≈ **~3371 LOC**.
- **Production total fully deletable ≈ 3330 LOC; with tests ≈ 6700 LOC.** A further ~3200 LOC (reconcile_light + stage_repair_reducer_frontier family) is PRUNE-not-delete (~150-800 LOC of tear paths removable, the crash-recovery slice retained).
