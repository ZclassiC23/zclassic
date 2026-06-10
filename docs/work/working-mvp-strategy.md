# Working MVP strategy — the zclassic23 way (research-confirmed 2026-06-04)

> The decision doc. Supersedes the "drive the reducer from genesis / coins-lag"
> framing (that was a **wrong-marker misdiagnosis**). Code-confirmed by a
> read-only investigation of live `progress.kv` + `node.log`, and confirmed by 3
> adversarial reviewers.

## Diagnosis — why the app hasn't worked for days (CONFIRMED)

It is **not** the reducer being "stuck at genesis," and **not** a window-extender
oscillation. It is a **single-frontier-window solution-source desync that traps a
dishonest self-healer in an infinite, lying loop.**

Ground truth (live, verified):
- The **reducer IS the live tip authority** and is **at the tip**, not genesis.
  Stage cursors: `header_admit=3135424`, `validate_headers=3135368`,
  `body_*/script/proof/utxo_apply=3134304`, `tip_finalize=3134304`.
  `tip_finalize_log` has **637 finalized rows** to h=3134301. It reached the tip
  via anchor-seed + a ~1814-row recent-window fold (NOT a 3.1M-block replay).
- The **"C applied coins tip : 5"** that drove the old plan is
  `tools/diagnose_gap.sh:50` reading `node_state['cec.coins_best_block_height']`
  — a **dead legacy chain_evidence marker** nothing advances. It is NOT the
  reducer's cursor. The old plan diagnosed a corpse and prescribed a multi-day
  consensus replay for a problem that does not exist.
- **The wedge** (`validate_headers` frontier, h=3134304..~3134311): the forward
  validator (`app/jobs/src/validate_headers_validator.c:54-82`) sources the
  Equihash solution from `node.db blocks.solution`, which has **no row** at
  3134304. The recheck path (`validate_headers_stage.c:330`) calls
  `active_chain_at(h)` for h one **above** the active tip → **NULL** → `JOB_IDLE`,
  so it never reaches the frontier block. The **valid 400-byte solution sits
  unused** in `progress.kv header_solution_repair`. The block falls in a dead gap
  between two solution stores.
- **The self-healer lies** (`app/conditions/src/stale_validate_headers_repair.c:96-115`):
  its witness returns `ok` the instant the poison ROWS are deleted, even though
  `active_chain_height` never advanced. So `max_attempts=5` **never trips**, no
  `EV_OPERATOR_NEEDED` ever fires, and it rewinds 7 cursors + deletes ~1129 rows
  **every ~5s**. Live `node.log`: `cleared_count=9421` (and climbing) at h=3134304,
  tip frozen at 3134303. **This is the exact Law-7 violation the framework
  forbids** — a remedy reporting success without moving the symptom, strictly
  worse than a loud halt because it hid the halt for days.

## Strategic call (3 adversarial reviewers concur)

**SHIP THE MVP ON THE REDUCER.** It is the one blessed live engine, already at the
tip. Do **not** revert to the legacy `connect_block` engine (that path is the
offline reindex/recovery tool, reached only from `boot_index.c:252` — keep it,
don't bless it, don't delete it). Do **not** "drive the reducer from genesis"
(a phantom multi-day task — the reducer doesn't replay history; it's anchor-seeded
and the on-disk 1.3M-UTXO coins set serves all reads).

The Rails-like move is **not** "switch engines" — it's **"collapse the two
solution stores into one and make the self-healer honest."** Hours-to-a-day on a
datadir COPY, vs days of consensus-critical rewiring for the legacy path.

## "The zclassic23 way" — conventions that GUARANTEE a working node

1. **One engine, named honestly.** The reducer (log → projection → 8 Jobs) is THE
   engine; `reducer_is_authoritative()` is the single switch. Legacy
   `connect_block` is offline-reindex-only, never a runtime writer. Docs must
   match (the `check-doc-accuracy` gate must also catch stale runtime claims —
   today both `FRAMEWORK.md:165` "connect_block deleted" and FORWARD_PLAN's "C=5"
   passed it while being false).
2. **One fact, one source.** A stage validator resolves a fact (e.g. the header
   solution) through ONE ordered resolver that always covers the live frontier —
   never a `node.db` lookup that stops at the persisted tip plus a separate repair
   table the forward path can't see.
3. **Every write through the cursor; atomicity via SQLite.** progress.kv stage
   cursors under `progress_store_tx_lock`; node.db UTXO writes through the AR
   lifecycle. This is why kill-9 recovery works. Keep it.
4. **A witness checks the OUTCOME, not the side effect (Law 7).** Any self-heal
   witness must assert the operator-observable symptom moved — for a tip/finalize
   remedy that means `active_chain_height` advanced PAST target, never merely "the
   poison rows are gone." A remedy firing repeatedly without advancing the tip
   MUST trip `max_attempts` and page `EV_OPERATOR_NEEDED`. (New lint gate: any
   condition whose remedy mutates stage cursors must have a witness referencing
   tip advancement.)
5. **The end-to-end "it works" gate.** ONE hermetic CI test that boots the real
   binary on a snapshot-seeded /tmp datadir, ingests one real block through the
   reducer front door, and asserts `active_chain_height` incremented AND the UTXO
   commitment matches. Green must mean "a block advanced the tip on the real
   binary," or green proves nothing. Keep the ~33 hygiene lint gates — they catch
   real bugs.

## The next workflows (sequenced)

1. **W1 — Unwedge: collapse the two header-solution sources into one resolver**
   that covers the live frontier window (consult `progress.kv
   header_solution_repair` for h above the persisted tip). Autonomous to
   write + unit-test + copy-prove (`--full`, assert the tip ADVANCES past
   3134303); **owner-gated deploy** (consensus-adjacent: header PoW validation).
2. **W2 — Make the self-healer honest** (Law-7 witness asserts forward tip
   movement; trips `max_attempts` → pages). Autonomous.
3. **W3 — The end-to-end "it works" CI gate** on the real binary (boot →
   ingest one block → assert tip+1 + commitment). Autonomous.
4. **W4 — CI-lock the 8 MVP criteria** on the now-advancing engine (promote the
   ◐ slice-gates toward ✅; net-new C1 install + C8 parity-verdict jobs; the
   parity service already exists). Autonomous except the live soak/parity.
5. **W5 — Re-frame the reducer historical backfill as a POST-MVP migration**
   (docs + fix the regtest miner so `generate` produces blocks). Autonomous.

W1+W2 are the live forward-progress fix. W3 is the Rails-like confidence gate.

## What to FREEZE (so it stops blocking shipping)

- The **reducer historical backfill** ("re-fold 3.1M blocks / drive from genesis")
  — a phantom; the reducer is already at the live tip. Keep the anchor-seed
  bridge (`tip_finalize_stage_seed_anchor`) + the coins.db-seeded projection.
  Record the full backfill as a post-MVP migration whose acceptance gate is
  `SHA3(projection) == SHA3(coins.db)`.
- The broader **framework/architecture refactor** (`REFACTOR_STATUS.md` ~90%) and
  **boot decomposition** — both OFF the v1 path and overstating completion.
- Nothing is deleted; freezing = stop spending v1 cycles on it. Keep
  `connect_block.c` (offline reindex), the reducer stages, and
  `test_reducer_ingest_e2e.c` (the migration's acceptance harness).
