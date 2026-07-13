# LB-1 — Wiring the parallel verify engine into the hot path

**Status:** design (no production code in this branch). **Scope:** Phase-1 items
1.2–1.6 of `docs/work/archive/architecture-execution-plan.md` (lines 158–214). **Goal:**
the shipped-but-unwired engine (`lib/validation/{thread_pool,verify_queue}.{c,h}`)
owns the per-input / per-proof crypto hot path, gated behind `-par` (default-on,
`-par=1` is the bit-for-bit serial oracle), with the per-block atomic co-commit
and every concurrency invariant preserved.

This doc is file:line-grounded against the `wf/lb1-design` worktree as of the
recon. **All four "drift" line numbers the execution plan warned about have been
re-verified here and are stated in their corrected form** (see "Verified drift
corrections" below). An implementer should follow this without re-deriving.

---

## Executive summary (the wiring approach)

1. **One global pool, started once at boot.** `app_init` reads
   `ctx->par_workers` (`config/include/config/boot.h:91`, currently consumed
   nowhere) and calls `thread_pool_start(&g_verify_pool, par_workers)` in
   `config/src/boot_services.c` *before* the stages register
   (before line 1457). `-par=1` ⇒ zero worker threads ⇒ `verify_queue` runs
   inline = the serial oracle.
2. **1.3 — submit, don't loop.** `script_validate_stage.c:251-326` and
   `proof_validate_stage.c:271-310` stop calling `verify_script` /
   `default_verify_tx` inline; instead they fill a `verify_job[]` of
   self-contained items (one engine call per block) and AND-reduce.
3. **1.4 — three phases, shrink the lock.** Resolve prevouts SERIALLY under
   `progress_store_tx_lock` (the FULLMUTEX/freshness contract), **release the
   lock**, run crypto on the pool with **no lock and no DB**, **re-take** the
   lock for the serial write (the `*_log` row + cursor UPSERT + `nStatus`).
   The atomic co-commit in `lib/util/src/stage.c:412-456` is untouched.
4. **1.5 — invert the driver.** Make the refold/drain driver the default IBD
   engine and demote `staged_sync_supervisor` to a pure observer
   (heartbeats + named stalls), keeping the `EV_OPERATOR_NEEDED` named-block
   latch and the `reducer_drive_active()` guard at
   `staged_sync_supervisor.c:212`.
5. **1.2 — subsume + delete two ad-hoc pools.** `vh_pool`
   (`validate_headers_stage.c`) and the per-batch spawn/join in
   `bg_validation_scripts.c` both become `verify_job` submitters on the one
   pool; the duplicate pthread machinery is deleted.
6. **1.6 — the gate.** `-par=1` vs `-par=N` must produce identical verdicts and
   identical, **non-zero**, **equal** `*_verified_total` counters over a height
   range (proves crypto ran in both); a planted bad sig/proof must be rejected
   **and the block named** in both modes; full-history replay must include
   **fork-bearing ranges**, which a single-threaded fork-free test cannot reach.
7. **Rollback is structural, not a revert:** every step keeps `-par=1` as a
   live, in-binary serial path. If a parallel run ever disagrees with the
   serial oracle, the operator drops to `-par=1` and the node is provably
   back on the reference order.

---

## Verified drift corrections (cite these, not the plan's stale numbers)

| Thing | Plan said | **Verified actual** |
|---|---|---|
| `reducer_drive_enter/exit` | `reducer_ingest_service.c:153,176` | **`:361` (enter), `:419` + `:456` (exit)**; `153/176` is now `reducer_pending_body_is_accepted` |
| Supervisor drive-guard | `staged_sync_supervisor.c:205`, path `app/services/src/` | **`reducer_drive_active()` at `app/supervisors/src/staged_sync_supervisor.c:212`**; the `period_secs=2` gate is `:249` |
| Window-mutation fn | `move_window_tip/chainstate.c:379` | **`active_chain_move_window_tip` at `lib/validation/src/chainstate.c:393`**; read accessor `active_chain_at` at `:281` |
| `-par` consumption | implied wired | **parsed at `src/main.c:1698` → `ctx.par_workers` (`boot.h:91`), consumed NOWHERE** |

Verified-accurate (already corrected in the plan): `nStatus` RMW at
`script_validate_stage.c:514`; co-commit at `stage.c:412-456`;
`script_check_item` builder at `bg_validation_service.c:359-379`; the
`g_verified_total` counters at `proof_validate_stage.c:71` /
`script_validate_stage.c:87`. Also confirmed: **no Sapling batch-verifier
symbol exists** in `lib/sapling/include/sapling/sapling_prover.h` (grep empty),
so any "BatchValidator semantics" is net-new crypto, not a wrapper — out of
scope for LB-1, which wraps the existing per-proof verifiers as both the serial
fallback and the per-job `fn`.

---

## Step 0 — Boot the one global pool (prerequisite for 1.2–1.6)

**Files:** `config/src/boot_services.c`, `config/include/config/boot_internal.h`
(for the `g_verify_pool` extern), `src/main.c` (no change — `-par` already
parsed at `:1698`).

**Change:**
- Add a file-scope (or `boot_internal`) `static struct thread_pool g_verify_pool;`
  and an accessor `struct thread_pool *verify_pool(void)` so the stages and
  the subsumed pools all fan across the **same** pool.
- In `boot_services.c`, *before* the stage registration block (the
  `staged_sync_supervisor_register(svc->state)` at `:1457` is the latest safe
  point; do it earlier in the same function, after datadir/params but before
  any stage `_init`), call:
  ```c
  if (!thread_pool_start(&g_verify_pool, ctx->par_workers))
      LOG_FAIL("boot", "verify pool start failed");
  ```
  `par_workers == 0` (default) ⇒ `GetNumCores()-1` workers; `== 1` ⇒ zero
  workers ⇒ inline. The `thread_pool_start` contract is `thread_pool.h:71-76`.
- In teardown, after all stages are quiesced (the existing shutdown block
  `boot_services.c:1677-1704` tears stages down in dependency order), call
  `thread_pool_stop(&g_verify_pool)` **after** the last stage shutdown so no
  in-flight `submit_batch` outlives the pool.

**Ordering:** pool start **before** any stage `_init`; pool stop **after** every
stage shutdown. Workers are spawned once for process life — never per block,
never per batch.

**Invariant preserved:** single-submitter. `verify_queue_submit_batch`
serializes submits (`thread_pool.h:28-30`), and the staged reducer runs exactly
one `step_once` at a time under `progress_store_tx_lock`, so only one stage ever
submits at once. The subsumed `vh_pool`/`bg_validation` paths (1.2) also submit
through `verify_queue`, so they inherit the same serialization — but a header
batch and a script batch must never be in flight simultaneously; today they are
not (validate_headers runs in its own stage step). Add an assertion in
`thread_pool_run_batch` if a second submitter is ever observed (`p->n_done < p->n_tasks` on entry).

**Rollback:** `-par=1` ⇒ zero workers ⇒ every `verify_queue_submit_batch` call
in every step below runs inline on the calling thread, in array order. The pool
object still exists (zero threads), so no call site needs a NULL guard.

**Failure mode a fork-free single-thread test misses:** that the pool is
actually *started* (a typo making `par_workers` always 1 would silently keep
the whole feature serial and still pass every test). The 1.6 gate's
"counters equal AND `worker_count>0` asserted in `-par=N`" closes this.

---

## 1.3 — script_validate + proof_validate SUBMIT instead of looping

### 1.3a — script_validate (`app/jobs/src/script_validate_stage.c`)

**Today (serial):** `validate_block_scripts_with_prevout()` (`:251-326`) has an
outer per-tx loop (`:251`), an inner per-vin loop (`:260`), resolves the prevout
at `:267`, and calls `verify_script(...)` **inline** at `:294-296`, bumping
`g_inputs_verified_total` at `:299`. The whole thing runs under
`progress_store_tx_lock` (taken by `STAGE_STEP_ONCE_SIMPLE` at
`stage_helpers.h:200`).

**Change — collect then submit:**
1. Lift `struct script_check_item` (`bg_validation_internal.h:21-29`) into a
   shared header (e.g. `validation/script_check_item.h` or reuse the engine's
   `verify_queue.h` neighborhood) so both this stage and `bg_validation` share
   one definition. It is already exactly the self-contained shape the engine's
   `arg` wants (tx ptr, `input_index`, `amount`, `branch_id`, `txdata`,
   `script_pub_key`, `flags`). This is the plan's 1.3 sub-step.
2. Reuse `bg_validation_scripts.c:28-41`'s `verify_script_item(void*)` verbatim
   as the per-job `fn` — it is already a pure self-contained wrapper over
   `verify_script`. Move it to the shared TU alongside `script_check_item`.
3. Rewrite `validate_block_scripts_with_prevout` so the per-vin body **fills a
   `script_check_item` + a `verify_job{ kind=VERIFY_JOB_SCRIPT_CHECK, fn=verify_script_item, arg=&item[k] }`** into per-block arrays instead of calling
   `verify_script`. Keep the prevout resolve (`:267`) and `precompute_tx_data`
   (`:258`) exactly where they are — they stay serial (1.4 makes this explicit).
   `txdata` is per-tx; each item must **own a copy** (the struct already embeds
   `struct precomputed_tx_data txdata` by value, not a pointer — good).
4. After the collect loops, **one** `verify_queue_submit_batch(verify_pool(),
   jobs, n)` per block. The return is the block's script verdict.
5. On `false`, find the first failing job (`jobs[i].result == false`) to set
   `out->first_failure_txid` / `first_failure_vin` / `first_failure_serror`.
   **Determinism note:** the serial path returns on the *first* failure in tx/vin
   order; the parallel path verifies all jobs, so to name the **same** block-local
   failure the reducer must pick the **lowest-index** failing job, not the first
   one a worker reports. Carry `txid`/`vin`/`serror` *in the item* (extend
   `script_check_item` with out-fields, or keep a parallel out-array indexed
   identically) so the lowest-index failing item names the block identically in
   both modes. This is load-bearing for 1.6's "same block named in both modes".

**Counters:** bump `g_inputs_verified_total` (`:91`/`:299`) by the count of jobs
that passed, and `g_inputs_failed_total` by the failing count, **after** the
batch — keep them atomic. `g_verified_total` (`:87`) still bumps once per
fully-verified block at `:506`. Workers must **not** touch any shared counter
(`thread_pool.h:44-48` forbids it); the per-job verdict slot is the only write.

### 1.3b — proof_validate (`app/jobs/src/proof_validate_stage.c`)

**Today (serial):** `validate_block_proofs()` (`:271-310`) loops per-tx (`:281`),
calling `default_verify_tx()` (`:102-229`) inline, which runs ed25519
(`:141`), the Sapling spend loop (`:157-169`), output loop (`:171-182`), final
binding check (`:184`), and the Sprout Groth16/PHGR loop (`:194-226`).

**Change — one job per shielded tx:**
- The natural parallel grain is **per-tx** (each `default_verify_tx` is already
  self-contained over one `tx` + `height` + `branch_id`). Build a
  `verify_job[]` where each shielded tx (`tx_has_shielded_proofs`, `:95-100`)
  becomes one `verify_job{ kind=VERIFY_JOB_GROTH16_PROOF, fn=verify_tx_item,
  arg=&item[k] }`. Skip non-shielded tx entirely (they short-circuit at
  `:115-116` today — keep that filter when building the batch so the batch is
  small and the verdict matches).
- `verify_tx_item(void*)` is a new thin wrapper that calls the existing
  `default_verify_tx` (or `g_tx_verifier` if set) over a self-contained
  `proof_check_item { const struct transaction *tx; int height; uint32_t branch_id; }`
  and writes pass/fail + a `first_failure_proof_type` out-field into the item.
  **Do not** share the `g_tx_verifier_user` pointer across threads unless it is
  provably reentrant; for the default path it is `NULL` (`:286`), which is safe.
- One `verify_queue_submit_batch` per block. On failure pick the **lowest-index**
  failing item to set `out->first_failure_txid` / `first_failure_proof_type`
  (same determinism rule as 1.3a), then route to `internal_error` vs
  `proof_invalid` exactly as `step_validate` does at `:397-410`.
- **Per-primitive counters** (`g_sapling_spends_verified_total` etc.,
  `proof_validate_stage.c:75-83`, summed in `add_success_counters` `:238-254`)
  must still be bumped, and only for tx that **passed**. Because the worker
  cannot touch shared counters, each item must return its per-primitive tallies
  (spends/outputs/joinsplits, already on `proof_validate_tx_report`) and the
  serial post-batch pass folds them into the atomics. This keeps the per-primitive
  proof-of-execution that 1.6 checks.

**Sapling context caveat:** `zclassic_sapling_verification_ctx_init/free`
(`:149`/`:164` etc.) is **per-tx, allocated and freed inside `default_verify_tx`**
— so each job already owns its own sctx; there is no shared Sapling verification
state across jobs. Confirm `sapling_params_loaded()` (the global params, gated
serially at `step_validate` `:376-381` *before* fan-out) — params load once at
boot and are read-only during verification, safe to read from N workers.

**Invariant preserved (both 1.3a/b):** verdict equals the serial AND-reduce
because every `fn` is a pure function of its `arg` (engine determinism contract,
`verify_queue.h:23-25`); the failing block is named identically because the
reducer selects the lowest-index failing job, matching serial first-failure order.

**Rollback:** `-par=1` ⇒ `verify_queue_submit_batch` runs the same jobs inline in
index order = the exact pre-LB-1 order.

**Failure modes a fork-free single-thread test misses:**
- A data race on a *shared* `txdata`/sctx (if an implementer passes a pointer
  instead of a value copy) — invisible at `-par=1`, only fires at `-par=N` under
  TSan or load. Mitigation: the item owns `txdata` by value; add a TSan CI lane
  over `-par=N` on a small height range.
- Non-deterministic failure naming (first-reported vs lowest-index) — invisible
  unless a *planted bad sig* is present, which fork-free history never has. 1.6's
  planted-fault test in **both** modes is the only thing that catches it.

---

## 1.4 — the three-phase pipeline (shrink the tx-lock; crypto OUT, write IN)

**Today:** `STAGE_STEP_ONCE_SIMPLE` (`stage_helpers.h:194-204`) takes
`progress_store_tx_lock()` at `:200`, runs the **entire** `step_validate`
(block read + all crypto + log insert) under it, unlocks at `:202`. The crypto
holds the global progress.kv tx lock for the whole block.

**Target — three phases inside one step:**

**(a) SERIAL prevout-resolve (lock held).** On the drive thread, under
`progress_store_tx_lock`, resolve every input's prevout into self-contained
`script_check_item` buffers. The resolver `created_index_prevout`
(`script_validate_stage.c:118-189`) reads the `SQLITE_OPEN_FULLMUTEX`
progress.kv handle and `coins_kv`; **`coins_kv` forbids per-thread
connections**, and there is a **same-txn freshness contract**: a coin created by
an earlier block in the batch must be visible to a later block's resolve on the
**same connection** (verbatim at `utxo_apply_stage.c` FRESHNESS CONTRACT comment
~`:537`). Therefore prevout resolution **must stay serial on the drive thread's
connection** — it cannot move to a worker. `precompute_tx_data` (`:258`) also
stays here (cheap, and keeps `txdata` ownership local).

**(b) PARALLEL crypto (NO lock, NO DB).** Release nothing yet inside the macro —
instead the *step itself* does the fan-out: after phase (a) has built the
`verify_job[]` from fully self-contained items (no `block_index`, no DB handle,
no `active_chain`), call `verify_queue_submit_batch`. The workers touch only
their `arg`. **The lock can be dropped around this call**: the items are copies,
the only shared reads are the read-only Sapling params and chain_params. The
cleanest seam is to **narrow `STAGE_STEP_ONCE_SIMPLE`'s lock to phases (a) and
(c)** for the two validate stages — i.e. give script/proof a *bespoke*
`*_stage_step_once` that does `lock → resolve → unlock → submit_batch → lock →
write → unlock`, rather than the shared macro. The macro stays for the other six
simple stages. This is the "crypto moves OUT, write moves back IN" shrink.

**(c) SERIAL write (lock re-taken).** Re-take `progress_store_tx_lock`, then do
the cursor-stamped write under the **existing** `stage_run_once` →
`stage.c:412-456` co-commit: open `BEGIN IMMEDIATE` (or `SAVEPOINT` when a drain
batch is open, `:412-414`), write the `*_log` row, `cursor_write_locked`
(`:441`), `stage_step_commit` (`:447`), set `s->cursor` (`:456`). The `nStatus`
RMW (`script_validate_stage.c:514`, the **non-atomic** read-modify-write of a
shared `block_index` validity LEVEL in the low `BLOCK_VALID_MASK` bits) and the
`block_index_emit_header_event` (`:516`) happen **here, in phase (c), serially**
— never on a worker, because two stages mutating the same node's `nStatus`
concurrently would corrupt the level. This is inviolable.

**Atomic co-commit preserved:** phase (c) is the *only* phase that writes the DB,
and it is still wrapped by `stage.c`'s single BEGIN/COMMIT (or SAVEPOINT)
envelope. A crash between (b) and (c) leaves the cursor un-advanced — the block
is re-resolved and re-verified next boot. A crash inside (c) rolls back via
`stage_step_rollback` (`:436/:451/:466`). **The cursor never advances past
unverified work**, which is the 1.4 invariant verbatim.

**active_chain read-only-for-the-batch invariant:** `active_chain_at`
(`chainstate.c:281`) is read **once, serially, in phase (a)** (it already is, at
`script_validate_stage.c:422` / `proof_validate_stage.c:360`). Workers never
index `active_chain`. The window-mutation fn `active_chain_move_window_tip`
(`chainstate.c:393`) must not run concurrently with a batch — it does not, because
window extension (`reducer_extend_window_to_candidate`, `stage_helpers.h:199`)
runs serially before the lock is taken, on the same drive thread, between steps.
Keep that ordering.

**Lock-order invariant:** the reducer drive holds `coins_kv`; **never** take
`csr->lock` or run evidence machinery from the drive (live ABBA deadlock, per the
LOCK-ORDER LAW memory). Phase (b) holds **no** lock, phase (a)/(c) hold only the
progress.kv tx lock — neither touches `csr->lock`. Preserved by construction.

**Ordering within the step:** `(a) lock+resolve+precompute` → `unlock` →
`(b) submit_batch` → `lock` → `(c) write+nStatus+emit+co-commit` → `unlock`.

**Rollback:** `-par=1` ⇒ phase (b) is inline; the lock-drop/re-take still happens
but is a no-op for correctness (single thread). To get *byte-identical* behavior
to pre-LB-1 you may keep the lock held across (b) when `worker_count==0` (skip the
drop) — but it is not required, since nothing else contends in serial mode.

**Failure modes a fork-free single-thread test misses:**
- A second writer sneaking into phase (b) — invisible serially. Guard: assert in
  phase (c) that `progress_store` is held and that `g_batch_open` state is
  consistent before the co-commit.
- The freshness contract broken by accidentally moving prevout resolve into a
  worker (a "tidy-up" refactor) — would corrupt the within-batch coin visibility.
  Add a comment + a debug assert that `created_index_prevout` is only ever called
  on the drive thread (e.g. `assert(thread_is_drive())`).

---

## 1.5 — invert the driver (refold = default IBD; supervisor = observer)

**Files:** `app/services/src/reducer_ingest_service.c` (the drive envelope),
`app/supervisors/src/staged_sync_supervisor.c` (the guard + the 2s gate),
`lib/util/include/util/reducer_drive_guard.h` (the API).

**Today:** the `reducer_drive_enter()` / `reducer_drive_exit()` envelope is at
`reducer_ingest_service.c:361` (enter), `:419` and `:456` (exit). The supervisor
checks `reducer_drive_active()` at `staged_sync_supervisor.c:212` and, when a
drive is active, **returns early after a heartbeat** (`:213-215`) — i.e. the
drive already suppresses supervisor stage execution. The supervisor's own
`period_secs=2` gate is at `:249`.

**Change — make the drain/refold driver the default IBD engine:**
- Today the supervisor *can* step stages itself; the drive guard makes it skip
  when a drive is active. **Invert the default:** start the refold/drain driver
  (the thing that calls `reducer_drive_enter`) as the standard IBD path, so the
  supervisor is *always* in the guarded "drive active" branch during sync.
- Demote `staged_sync_supervisor` to a **pure observer**: it emits heartbeats,
  watches cursor progress, and raises **named stalls** — but it never executes a
  stage step. Concretely, in the `:212` guard, the early-return-after-heartbeat
  becomes the *only* path during IBD; the stage-stepping fallback below `:212`
  is reached only when no driver is registered (degenerate / tests).
- **Do not just drop the 2s timer** (`:249`) — that is the heartbeat cadence the
  supervisor uses to detect a stalled driver. Keep the timer; change only *what
  it does* (observe, not step).

**EV_OPERATOR_NEEDED named-block latch preserved:** the "never halt silently"
contract is that a stall is always a named blocker. The supervisor-as-observer
must still latch `EV_OPERATOR_NEEDED` with the exact `height` + `reason` when the
driver makes no forward progress within its deadline. Route the driver's
per-block failure (the `EV_BLOCK_REJECTED` events the validate stages already
emit at `script_validate_stage.c:484/499` and `proof_validate_stage.c:401/408`,
carrying height + txid + reason) into the same latch. The supervisor's
liveness-tree registration (`zcl_state subsystem=supervisor`) stays; it now
reports the **driver** as the child whose `last_tick_age_us` / `progress_marker`
it watches.

**Inviolable invariant:** the `reducer_drive_enter/exit` envelope around the
default driver is **what makes the supervisor skip stage execution** — it is the
mutual-exclusion between "driver owns the stages" and "supervisor owns the
stages". Never run both. Preserved by keeping the guard at `:212` and only
inverting which side is the default.

**Ordering:** land 1.3 + 1.4 first (so the driver's per-block step is already the
three-phase parallel one); then flip the default in 1.5. Flipping the driver
before the hot path is parallel just changes who calls a serial step — harmless
but pointless.

**Rollback:** keep a flag (e.g. `-driver=supervisor` or reuse the existing
profile) that restores the supervisor-steps-stages path. The guard logic is
unchanged; only the *default* registration flips.

**Failure modes a fork-free single-thread test misses:** a **wedge** where the
driver stops making progress but the supervisor, now an observer, fails to latch
`EV_OPERATOR_NEEDED` — the node would halt silently, the exact thing the North
Star forbids. A single-threaded green test never exercises the stall path. Gate:
a fault-injection test that freezes the driver mid-batch and asserts
`EV_OPERATOR_NEEDED` fires with the correct named height within the deadline.

---

## 1.2 — subsume vh_pool + bg_validation_scripts into the one engine (delete duplicates)

**Two ad-hoc pools to delete after wiring:**

**(1) `vh_pool` — Equihash PoW (`app/jobs/src/validate_headers_stage.c`).**
`struct vh_pool` (`:60-75`) is a fixed 4-thread greedy-steal pool
(`VH_POOL_SIZE=4`, `validate_headers_stage.h:78`) with `worker_entry`
(`:121-151`) and `pool_run_batch` (`:156`). The worker calls `g_validator(...)`
(`:139-141`) which runs the Equihash-from-nSolution + PoW-target check.
- **Subsume:** each `vh_job` (`:50-58`) becomes a `verify_job{
  kind=VERIFY_JOB_EQUIHASH_POW, fn=vh_validate_item, arg=&item }` submitted via
  `verify_queue_submit_batch(verify_pool(), ...)`. `vh_validate_item(void*)` wraps
  the existing `g_validator`. The greedy-steal cursor, `mu`/`cv_take`/`cv_done`,
  `threads[]`, `worker_entry`, `pool_run_batch`, and the pool start/stop are then
  **deleted** — the one global pool owns the threads.
- **Inviolable:** validate_headers must **still verify every Equihash solution**.
  The `EQUIHASH_POW` job kind names exactly this; the `fn` is the same validator,
  so coverage is unchanged. After the swap, assert the headers stage still
  produces identical pass/fail for a known-good and a known-bad header batch.

**(2) `bg_validation_scripts.c` per-batch spawn/join.**
`bg_validation_verify_scripts_parallel` (`:67-125`) does `pthread_create`
(`:112`) / `pthread_join` (`:117`) **per flush** — created and joined every batch
(the inefficiency the persistent pool fixes), with a serial fallback for
`count<=4 || num_workers<=1` (`:74`). Its `verify_script_item(void*)` (`:28-41`)
is already a pure `verify_job.fn`.
- **Subsume:** replace the body of `bg_validation_verify_scripts_parallel` with
  "build `verify_job[]` from the `items[]`, call
  `verify_queue_submit_batch(verify_pool(), ...)`". The builder at
  `bg_validation_service.c:359-379` is unchanged (it already fills
  `script_check_item`). Delete `struct worker_ctx` (`:45-50`), `worker_thread`
  (`:52-63`), and the per-batch pthread create/join.
- Since 1.3a already lifted `script_check_item` + `verify_script_item` into a
  shared TU, `bg_validation` and `script_validate` now share **one** definition
  and **one** verifier wrapper — the subtraction the plan wants.

**Ordering:** do 1.2 **after** Step 0 (the global pool exists) and after 1.3a
(the shared `script_check_item`/`verify_script_item` TU exists). Then delete the
duplicates in one commit each, with the equivalence test green.

**Rollback:** `-par=1` ⇒ both subsumed paths run inline (the engine's serial
fallback) = behaviorally identical to the old `num_workers<=1` / `count<=4`
serial branches. Keep the deletions in a separate commit so a revert is a clean
`git revert` without touching the wiring.

**Failure modes a fork-free single-thread test misses:** the headers pool dropped
from 4 threads to the global pool's (nproc-1) — a *throughput* change, not a
correctness one, but a test that only checks verdicts won't notice if the new
path accidentally **skipped** a header (e.g. an off-by-one in the batch build).
Gate: a header batch with a planted bad solution must still be rejected and the
exact height named, in both `-par=1` and `-par=N`.

---

## 1.6 — THE GATE (the only thing that proves the wiring is consensus-safe)

`test_consensus_parity` is **golden constants only and does NOT execute the
crypto path** (confirmed: `lib/test/src/test_consensus_parity.c:39` asserts
`powLimit`/genesis/utxo-sha3 golden values, no `verify_script` call). So it
cannot be the gate. The gate is a **new** test/replay harness:

**G1 — verdict equivalence over a height range.** Replay the same contiguous
height range twice: once `-par=1`, once `-par=N`. Assert **every** block's
accept/reject verdict and the **named failing block (height+txid+vin/proof_type)**
are identical. The cursor reached must be identical.

**G2 — proof-of-execution counters (the anti-golden check).** After each run,
read the `*_verified_total` accessors and assert they are **non-zero AND equal**
between the two modes:
- `script_validate_stage_verified_total()` (`script_validate_stage.c:627-630`,
  backing `g_verified_total` `:87`/`:506`) and
  `script_validate_stage_inputs_verified_total()` (`:91`/`:299`).
- `proof_validate_stage_verified_total()` (`:523-526`, `g_verified_total`
  `:71`/`:412`) and **every** per-primitive total:
  `..._sapling_spends_verified_total`, `..._sapling_outputs_verified_total`,
  `..._sprout_groth16_verified_total`, `..._sprout_phgr13_verified_total`,
  `..._binding_sig_verified_total` (`proof_validate_stage.c:75-83`).
- **Non-zero** proves the crypto actually ran (a golden-constant test that never
  calls the verifier would leave these at 0). **Equal** proves `-par=N` ran the
  *same* crypto as `-par=1`, not fewer jobs. Pick a range that contains shielded
  tx so the Sapling/Sprout counters are non-zero.

**G3 — planted-fault rejection, both modes.** Plant (a) a bad ECDSA sig and (b) a
bad Groth16/Sapling proof into a block. Assert **both** `-par=1` and `-par=N`
reject the block, increment the matching `*_failed_total`, and **name the same
block** (height + txid + vin/proof_type) in the `EV_BLOCK_REJECTED` event. This
is the test that catches the parallel "lowest-index vs first-reported" failure-
naming hazard (1.3) that fork-free history cannot.

**G4 — full-history replay WITH fork-bearing ranges.** Replay the real chain
end-to-end in `-par=N`, and crucially include **fork-bearing height ranges**
(reorg windows) — a single-threaded fork-free range exercises neither the
window-mutation serialization (`active_chain_move_window_tip`, `chainstate.c:393`)
nor the reorg-unwind path that runs `nStatus` RMW under contention. The consensus
rule "validate against the CHAIN, not the reference text" (e.g. the 125,811-byte
tx at h=478544) means the gate's range **must** include the known oddball heights.
Assert tip parity vs the live `zclassicd` oracle at the checkpoints the cold-sync
recipe already proves (SHA3 anchor h=3,056,758).

**G5 — TSan lane.** Run G1 over a small range under ThreadSanitizer in `-par=N`
to catch the shared-`txdata`/shared-sctx data race that is invisible at `-par=1`
and produces correct verdicts under light load.

**Inviolable:** the gate is the **only** evidence that LB-1 is consensus-safe.
Do not merge any of 1.3–1.5 to the default-on `-par` until G1–G4 are green on a
real-history replay. `-par=1` remaining bit-identical to pre-LB-1 is the floor.

**Failure modes the gate is specifically built to catch (that a fork-free
single-thread test misses):** non-deterministic failure naming (G3),
shared-state data races (G5), skipped jobs / off-by-one batch builds (G2 equal-
counters), reorg/window-mutation contention (G4 fork ranges), and a silently-
serial pool from a `par_workers` typo (G2 non-zero + assert `worker_count>0`).

---

## Ordered, owner-reviewable checklist

1. **[Step 0] Boot one pool.** Add `g_verify_pool` + `verify_pool()`; call
   `thread_pool_start(&g_verify_pool, ctx->par_workers)` in
   `config/src/boot_services.c` before stage `_init` (before `:1457`); call
   `thread_pool_stop` after the last stage shutdown (after `:1704`). **DoD:**
   `-par=N` boots N-1 workers (assert `thread_pool_worker_count>0`); `-par=1`
   boots zero workers; full suite green.
2. **[1.3a] Lift `script_check_item` + `verify_script_item`** to a shared TU
   from `bg_validation_internal.h:21-29` / `bg_validation_scripts.c:28-41`.
   Rewrite `validate_block_scripts_with_prevout` (`script_validate_stage.c:251-326`)
   to build a `verify_job[]` and submit once per block; pick lowest-index failure
   for naming. **DoD:** G1+G2 green on a transparent-only range, `-par=1` byte-
   identical.
3. **[1.3b] proof_validate batch.** Add `proof_check_item` + `verify_tx_item`;
   rewrite `validate_block_proofs` (`proof_validate_stage.c:271-310`) to submit
   one job per shielded tx; fold per-primitive tallies serially. **DoD:** G1+G2
   green on a **shielded** range (Sapling/Sprout counters non-zero & equal).
4. **[1.4] Three-phase step.** Give script/proof bespoke `*_stage_step_once`:
   `lock→resolve+precompute→unlock→submit_batch→lock→write(+nStatus@514+emit)→
   unlock`, keeping `stage.c:412-456` co-commit. Assert prevout-resolve only on
   the drive thread. **DoD:** crash-mid-batch leaves cursor un-advanced; G1–G3
   green; lock-held time per block drops measurably.
5. **[1.2] Subsume + delete vh_pool** (`validate_headers_stage.c:60-75,121-156`)
   and **bg_validation spawn/join** (`bg_validation_scripts.c:45-63,82-125`) onto
   the one pool; delete the duplicate pthread machinery. **DoD:** headers still
   reject a bad-solution block (named height); G3 green for EQUIHASH_POW; LOC down.
6. **[1.5] Invert the driver.** Make refold/drain the default IBD driver;
   `staged_sync_supervisor` becomes a pure observer (keep `:212` guard, `:249`
   timer, `EV_OPERATOR_NEEDED` latch). **DoD:** frozen-driver fault-injection
   latches `EV_OPERATOR_NEEDED` with the correct named height; no silent halt.
7. **[1.6] Run the gate.** G1 (verdict equiv), G2 (non-zero + equal counters),
   G3 (planted fault named in both modes), G4 (full-history replay incl. fork
   ranges + h=478544 + tip parity vs zclassicd@3,056,758), G5 (TSan lane).
   **DoD:** all green → flip `-par` default-on; `-par=1` retained as the live
   serial oracle and the rollback.

**Global rollback at any step:** `-par=1` forces zero workers ⇒ every
`verify_queue_submit_batch` runs inline in index order ⇒ provably the pre-LB-1
serial reference. No revert needed to get a known-good node; revert only to drop
the wiring code itself.
