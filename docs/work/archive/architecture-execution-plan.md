> **ARCHIVED / SUPERSEDED.** Superseded by docs/REFACTOR_STATUS.md (debt board) — the checklist below is historical. See `docs/work/ROADMAPS.md` for the live roadmap index. Kept for history — do not act on this as current.

# Architecture Execution Plan — checklist

> Companion to [architecture-roadmap.md](./architecture-roadmap.md) (the WHAT/WHY).
> This is the HOW: an ordered, checkbox checklist a fresh Claude can execute cold.
> Built 2026-06-20. Verify every file:line against the code before acting — specifics rot.
> Machine: **32 cores** (`nproc`=32) — the parallelism target. Single ~15MB C23 binary.

## How to use this (future Claude, read first)
1. `cat docs/HANDOFF.md` + `docs/work/architecture-roadmap.md` + this file.
2. Work top-down. Do NOT start a later phase before its dependency is checked off.
3. Each item: do the **action**, then the **verify**, then tick `[x]` and note the commit hash.
4. Every change is behind a flag or copy-proven on a fixture. The node must stay bootable
   and consensus-parity-identical to zclassicd at EVERY commit. No big-bang rewrite.
5. When stuck on the 2nd–3rd rebuild with no gain: STOP, re-read the roadmap, re-question
   the approach (see [[feedback_question_the_frame_reason_from_invariants]]).

## Objective (judge every change against THIS)
Fully-sovereign node: verifies EVERYTHING itself (Equihash 200,9 PoW, ECDSA, Groth16/PHGR);
FAST by saturating all 32 cores; cold-starts in seconds from a SELF-MINTED checkpoint; never
serves an unproven value; never halts without naming the exact block+reason; **bit-for-bit
consensus-compatible with zclassicd (INVIOLABLE)**; operated by AI via typed MCP tools.

## INVIOLABLE constraints
- **Consensus parity.** Never ship a parity-tightening predicate without a full real-chain
  replay first (the h=478544 / 125,811-byte-tx lesson — the source text is a lossy proxy;
  validate against the CHAIN). Lint gate: `check-consensus-parity` (E13) + `test_consensus_parity`.
- **Speed from parallelism, NEVER from skipping validation.** No "trust the snapshot forever".
  The assumeutxo bargain = relaxed briefly THEN mandatory background full re-validation.
- **Copy-prove before live.** Repro/prove every recovery+seed change on a COPY first
  (frozen wedge fixture `~/.zclassic-c23-postrestore-wedge-20260611`), never live surgery.
- **Never stop zclassicd.** Services run under linger. `test_parallel`, never `test_zcl`.

## Hard-won gotchas (cost me hours this session — don't repeat)
- **NEVER run `build/bin/zclassic23` without `-datadir=`** — no `--version`/`--help` exists,
  unknown flags are silently ignored, and it boots a full node against the LIVE `~/.zclassic-c23`.
  Bit me twice. See [[feedback_never_run_bare_node_binary]]. Kill strays with `pkill -x zclassic23`
  (exact comm; `pkill -f build/bin/zclassic23` self-matches the shell → exit 144).
- **The node takes up to 90s to shut down** (a long drain pass ignores SIGTERM). After `pkill`,
  POLL `pgrep -x zclassic23` until empty before relaunching, or the next boot hits "datadir locked".
- **Green tests are NOT live truth.** A heap buffer-overflow (fork-on-real-chain) passed the full
  suite because test chains are fork-free + single-threaded. ALWAYS confirm on a live fold run.
- **File-size cap (E1, 800 LOC) bites constantly.** boot.c=3625, boot_services.c=1768,
  utxo_apply_stage.c=798 are AT baseline. Any added line trips it. (FIX-5 removes this gate — do
  it early so it stops costing time.) lib/ + domain/ are exempt today.
- **Profiling is locked** (`/proc/sys/kernel/yama/ptrace_scope`=1; perf+gdb refuse). To profile,
  ask the owner to run `! echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope`. Else instrument
  in-code (and gate the trace on the ACTUAL batch size, not a stale constant — that bug cost a run).

## Current state (2026-06-20)
- **main** (`1b79f4876`): has the -refold-staged enabler (`5cbc16b88`, floor→0 + suspend
  self-repair during refold, self-clears at anchor) + the reset verb (`9db6ade8e`, resets 8
  cursors+coins_kv to genesis) + the deletion-plan doc. Normal boot byte-identical, 433/433 green.
- **branch `round1/io-speedups-clean`** (3 commits on main): `429706f87` io-speedups (batch
  progress.kv commits + synchronous=OFF in IBD, with the reorg savepoint-nesting fix folded in);
  `c22baa278` #3 (skip the O(n) mirror rebuild during refold); `f311482ae` #1 (stop retracting the
  active-chain window every block during refold — removed the 3.1M pprev walk, CPU 76%→53%).
  Full suite green at each.
- **UNCOMMITTED** on the branch: the continuous `refold_driver` thread (reducer_ingest_service.c/.h,
  boot_services.c) — lifts the fold 3→~14 blk/s by removing the 2s cadence. STILL HAS TEMP
  INSTRUMENTATION (per-pass + per-stage timing in `reducer_drain_all_stages` and `refold_driver_main`);
  the per-stage trace is gated `>=1000` but batch is 256 so it never fires — fix or strip before merge.
- **The single-thread driver is a STEPPING STONE to LB-1**, not the destination: it removes the
  timer; LB-1 adds the cores. Promote it to the default IBD driver under LB-1; the engine is the
  real fix.

## Verification toolkit
- **Fold fixture** (throwaway, never the live datadir): `~/.zclassic-c23-foldmeasure` (17G, snapshot
  excluded). Rebuild from `~/.zclassic-c23` with rsync `--exclude='consensus_snapshot.db*'`. Has full
  block bodies to ~3,153,108. The `-refold-staged` boot resets coins_kv+cursors so the fixture is reusable.
- **Launch a refold (always with -datadir!):**
  `build/bin/zclassic23 -datadir=$FIX -refold-staged -nolegacyimport -nobgvalidation -allow-degraded
   -port=18933 -rpcport=28932 -httpsport=28943 -fsport=28974 -connect=127.0.0.1:1`
- **Sample cursors:** `build/bin/sqlq $FIX/progress.kv "SELECT name,cursor FROM stage_cursor ORDER BY cursor"`.
- **Checkpoint golden** (lib/chain/src/checkpoints.c): h=**3,056,758**, SHA3
  **00e95dbd54a791a51433d68127f9975a3b1d6f8e9002b109647343ba0c83c3e0**, count **1,354,771**.
  (NOTE: this value's provenance is "verified against zclassicd" — LB-2 re-mints it from our own
  fold. A self-fold that reproduces it confirms the value; a mismatch is a BUG to find, never a fork.)
- **Build:** `rm -f build/bin/zclassic23 && make -j$(nproc) build/bin/zclassic23 sqlq zcl-rpc`
  (stale-.o trap: always `rm` the binary first). Compile-check loop: `make build-only -j$(nproc)`.
- **Test:** `ulimit -s unlimited; build/bin/test_parallel` (433 groups; verdict line "ALL TESTS PASSED").

---

## CHECKLIST

> **SHIPPED 2026-06-20 (parallel-worktree workflow batch, all gated build + test_parallel 434/434):**
> 0.2+0.2b provable-tip H* → external readers + catchup refold-guard (`e75b5c62c`);
> 0.3+0.5 E1 file-size cap → WARN + lib-layering ratchet→HARD (`9a56188a6`);
> 0.4 consensus-dedup INVESTIGATED → no-op, not collapsible (`7fbf8eeb2`);
> 0.5 new HARD domain-purity lint gate (`098605865`);
> 0.6 shutdown-aware wait RPCs waitforheight/halt/blocker (`a1ace9df3`);
> 1.1 parallel verify engine thread_pool+verify_queue behind -par, +determinism test, NOT wired (`9c5357930`).
> REMAINING Phase-0: only 0.1 (refold-driver cleanup, experimental WIP). NEXT BIG STEP = LB-1 wiring
> (1.2–1.6) — OWNER-GATED: it touches the consensus hot path and needs the full-history replay + the
> concurrency invariants before any push. Do not auto-ship it.

### Phase 0 — independent, ship now (no LB dependency, can't regress consensus)
- [ ] **0.1 Finish + merge the refold work-in-flight.** Strip/repair the TEMP instrumentation in
  `reducer_ingest_service.c` (`reducer_drain_all_stages` per-stage trace + `refold_driver_main` per-pass
  trace). Decide: keep a clean `zcl_state subsystem=refold` dump instead of stderr spam. Commit the
  driver. Run full `test_parallel`. Merge `round1/io-speedups-clean` → main (FF). **Verify:** 433/433;
  a `-refold-staged` live run still folds (≥10 blk/s) and DOES NOT crash (heap-safe). Then a NORMAL
  boot is unaffected (driver only spawns under refold). ⚠ REVIEW #8: this heap-safety/throughput proof is
  REFOLD-SCOPED ONLY (`refold_driver_start_if_active` early-returns on normal boot,
  reducer_ingest_service.c:184); it does NOT transfer to the normal-boot path that 1.5 promotes the driver
  to — 1.5 MUST re-verify on normal forward sync WITH reorgs.
- [x] **0.2 FIX-1: wire H\* (provable tip) to served APIs.** ✓ DONE 2026-06-20 (e75b5c62c): `getblockcount` now serves `reducer_frontier_provable_tip_cached` (`blockchain_controller_blocks.c:45`) and P2P `start_height` serves H\* (`msg_version.c:155`); cached atomic as specified, internal window callers untouched. Original notes below. `getblockcount` read `active_chain_height`
  at `app/controllers/src/blockchain_controller_blocks.c:39`; P2P `start_height` at
  `lib/net/src/msg_version.c:148`. ⚠ **REVIEW CORRECTION:** the provable value is
  `reducer_frontier_compute_hstar` (`reducer_frontier.h:90` — MIN over ALL stage logs incl. tip_finalize),
  **NOT** `reducer_frontier_derive_coins_best` (`reducer_frontier.c:652` = `coins_applied-1`, which can
  LEAD H\* by pipeline depth → would still serve an unproven height). ⚠ Do NOT call the derive per RPC —
  it takes `progress_store_tx_lock` (`reducer_frontier.c:778`) and would block behind a multi-second fold
  commit; instead serve a CACHED atomic snapshot the reducer refreshes when it advances. ⚠ Build the
  caller table FIRST: only the EXTERNAL serving readers move to H\* (blockchain_controller_blocks.c:39,
  msg_version.c:148); the ~30+ internal `active_chain_height` callers (process_block_invalidate/revalidate,
  msgprocessor_snapshot.c sync decisions, msg_headers.c:270 `pre_tip_height`, conditions, sync_monitor)
  legitimately need the window/lookahead tip — KEEP. (`accept_block_header.c:228` derives height from the
  parent link, not the label — unaffected.) **Verify:** at tip, `getblockcount` == zclassicd; mid-fold it
  reports LOWER (correct). Replay-check vs a zclassicd handshake (a lower advertised `start_height` may make
  peers deprioritize us — confirm acceptable). Update tests asserting the old value. **Gate:** serving value,
  not an acceptance predicate — no consensus-rule change.
- [ ] **0.2b Gate `node_db_catchup_service` off during refold** (REVIEW gap #9). During `-refold-staged` it
  fires every 5s, indexes zero readable blocks, hits "final commit missing tip hash"
  (`node_db_catchup_service.c:659`), rolls back, retries — log spam + node.db writer-lock contention (no
  wedge risk: bounded retry, no coins_kv/tx-lock touch). Mirror the proven guard
  `if (refold_in_progress()) return 0;` from `utxo_mirror_sync_service.c:319` at
  `boot_background_workers.c:369` (the re-trigger) and/or the top of `node_db_catchup_service_run`.
  **Verify:** no catchup spam during a refold soak run.
- [ ] **0.3 FIX-5: replace the 800-line hard cap with a cohesion gate.** `tools/scripts/check_file_size_ceiling.sh:28`
  (`CEILING=800`) + baseline. Drop the hard cap → WARN; KEEP `check-long-functions` (≤500 LOC/fn) and
  apply uniformly to lib/+domain/. **Verify:** `make lint` green; the cap no longer blocks edits.
  Remove the `*_accessors.c` shared-static splinter headers created only to dodge the cap (separate commit).
- [x] **0.4 FIX-6: collapse duplicate consensus defs — INVESTIGATED 2026-06-20 → NO-OP (correct).** A
  workflow (parallel recon + adversarial verify, `consensus_parity` green) proved NONE of the candidates is
  a behaviour-preserving collapse, so the right action is to make **no edit** — recorded here so it is not
  re-attempted:
  - **upgrades** is a LAYERED pair, not duplicate defs. `domain/consensus/src/upgrades.c` exports the pure
    `domain_consensus_*` (`zcl_result`) verifiers; `lib/consensus/src/upgrades.c` exports DIFFERENT symbols —
    the 3 const tables (`NetworkUpgradeInfo`/`SPROUT_BRANCH_ID`/`EquihashUpgradeInfo`) + 8 legacy `consensus_*`
    bool/int wrappers that add `assert()`+`LOG_FAIL`/`LOG_ERR` side-effects the domain funcs lack (and the
    domain TU link-depends on the lib table). Merging changes signatures, logging, and the table home →
    observable behaviour change. NOT a collapse.
  - **equihash** is 3 genuinely distinct layers (header→state verifier; the Wagner-tree primitive; the legacy
    bool shim), not 3 copies. The only removal candidate, the `lib/chain` shim `check_equihash_solution`, has
    TWO production callers needing its legacy `bool(header, params)` signature — `lib/mining/src/gen.c:90`
    and `app/jobs/src/validate_headers_validator.c:393` (plus 3 parity-seal tests). Removal = a real
    caller-migration refactor with a full-history replay gate, NOT behaviour-preserving. Out of scope here.
  - **base58/bech32** already single-homed in `domain/encoding/`; no lib wrapper exists. Nothing to do.
  - **Lesson for the plan:** "duplicate-name" ≠ "duplicate definition". The earlier review (#7) and roadmap
    FIX-6 over-counted; consensus de-dup here would have been a fork risk, not a tidy. Leave as-is.
- [x] **0.5 Promote domain-purity + lib-layering lint from RATCHET to HARD** ✓ DONE 2026-06-20
  (9a56188a6 lib-layering→HARD; 098605865 new `check_domain_purity.sh` HARD gate). `make lint` green;
  injected app-include into domain/ trips it.
- [x] **0.6 FIX-3 (reads): add `zcl_wait_*` long-poll RPCs** ✓ DONE 2026-06-20 (a1ace9df3):
  waitforheight/waitforhalt/waitforblocker, 200ms poll, shutdown-aware, 9000ms cap under the 10s RPC
  watchdog; +3 MCP wrappers. Returns on event and on shutdown; no busy-loop.

### Phase 1 — LB-1: parallel verification engine (behind default-on `-par`, `-par=1` = serial oracle)
> ⚠ **PHASE-1 INVIOLABLE CONCURRENCY INVARIANTS (REVIEW #1,#2,#3 — read before 1.1).**
> A parallelism data race is a correctness break that fork-free single-threaded tests do NOT catch
> (the heap-overflow gotcha). These are load-bearing:
> - **Three-phase pipeline, only the middle is parallel:** (a) SERIAL prevout-resolve into self-contained
>   `script_check_item` buffers (copy `amount` + `script_pub_key` out — `bg_validation_service.c:359-379`
>   already does this) on the drive thread — the resolver `created_index_prevout`
>   (`script_validate_stage.c:118`) reads the `SQLITE_OPEN_FULLMUTEX` progress.kv handle and `coins_kv`
>   FORBIDS per-thread connections (`coins_kv.c:11`) with a same-txn FRESHNESS contract
>   (`utxo_apply_stage.c:537` — a coin created by an earlier block in the batch must be visible to a later
>   block's resolve on the SAME connection); → (b) PARALLEL `verify_script`/Groth16 over the self-contained
>   items (no DB, no shared index); → (c) SERIAL write phase under `progress_store_tx_lock` (the `*_log` row,
>   cursor UPSERT, `nStatus` mutation, header-event emit).
> - **active_chain window is READ-ONLY for the duration of a batch.** Any `active_chain_at`/`_height` read
>   happens once, serially, before fan-out (or under `cs_main` like `bg_hash_verification_service.c:194`);
>   window mutation (`move_window_tip`/extend/install, `chainstate.c:379`) is serialized vs the batch.
>   Workers hold only self-contained items, never index `active_chain`. `nStatus` writes
>   (`script_validate_stage.c:514`, a non-atomic RMW on a shared node) stay in the serial write phase.
> - **Preserve the `reducer_drive_enter/exit` envelope** (`reducer_ingest_service.c:153,176`) around the
>   default driver — it's what makes the supervisor skip stage execution (`staged_sync_supervisor.c:205`,
>   the drive-guard). Demote the supervisor to a PURE observer (no stage execution), never just "drop the
>   timer." VERIFY: an injected concurrent supervisor stage-tick during a drive is a no-op.
- [ ] **1.1 Build the engine.** New `lib/validation/thread_pool.c` + `verify_queue.c`: a bounded work
  queue of `{script_check | groth16_proof | equihash_pow}` items, `(nproc-1)` PERSISTENT workers (created
  once at boot, not spawn/join per batch), per-job result slots (no shared counters → no race), single
  AND-reduce of verdicts. `-par=N` flag (default `nproc-1`; `-par=1` forces serial). **Verify:** a unit
  test: pool result == serial result over a crafted mixed batch; `-par=1` path is bit-identical to today.
- [ ] **1.2 Subsume the two ad-hoc pools** into the one engine: the 4-thread `vh_pool`
  (`validate_headers_stage.c:60-75`, `VH_POOL_SIZE 4`) and the per-batch spawn/join in
  `bg_validation_scripts.c:67-125`. DELETE the duplicates (subtraction). **Verify:** validate_headers still
  verifies every Equihash; full suite green.
- [ ] **1.3 Make `script_validate_stage` + `proof_validate_stage` SUBMIT, not loop.** Collect all
  `(tx,input,prevout,branch_id)` for a block into a `script_check_item[]` (the bg path already builds this
  at `bg_validation_service.c:370-378`) → one engine call/block; the driver thread joins the pool.
  Collect all Groth16/PHGR proofs → BATCH-verify (Zebra/librustzcash BatchValidator semantics) WITH a
  per-proof serial fallback to name the failing block. **Verify:** verdict-equivalence vs serial over a
  height range; a planted bad sig/proof is still rejected AND the bad block is named.
- [ ] **1.4 Implement the three-phase pipeline** (see the Phase-1 invariants above; NOT "verify lock-free"):
  (a) serial prevout-resolve → self-contained items; (b) parallel crypto on the pool; (c) serial write under
  `progress_store_tx_lock` — the `*_log` row + cursor UPSERT + `nStatus`. The atomic co-commit lives in the
  shared stage kernel `lib/util/src/stage.c:414-447` (a SAVEPOINT nested in the batch txn — NOT
  utxo_apply_stage.c:507 as an earlier draft said); preserve it: a crash mid-batch must not advance a cursor
  past unverified work. **Verify:** kill-9 mid-batch test; the suite's atomicity tests; the per-block
  co-commit invariant holds; the freshness contract (later block sees earlier-block coins in the same batch).
- [ ] **1.5 Invert the driver.** Promote `refold_driver_main` (reducer_ingest_service.c:142) to the default
  IBD driver; demote the supervisor from throughput-driver to OBSERVER (keep liveness heartbeats + named
  stalls; drop `period_secs=2` as the gate at `staged_sync_supervisor.c:249`). **CRITICAL:** keep surfacing
  per-block failures through the same `EV_OPERATOR_NEEDED`/FATAL latch (the "never halt without naming the
  block" invariant). `utxo_apply` stays serial. **Verify:** live fold saturates ~31 cores (`%CPU`>3000);
  rate is orders of magnitude up; a planted bad block still pages OPERATOR_NEEDED with the height.
- [ ] **1.6 GATE (parity + equivalence):** full-history replay against the REAL chain (h=478544 lesson),
  **run WITH reorg/fork-bearing ranges present** (a parallelism race only shows on forks — the suite is
  fork-free, REVIEW #2/#10). Zero false-rejects of the genuine chain. Equivalence proof = the
  `*_verified_total` atomic counters (`proof_validate_stage.c:71`, script `g_verified_total`) are NON-ZERO
  and EQUAL between `-par=1` and `-par=N` over the same range (this proves crypto actually RAN, not stubbed —
  `test_consensus_parity` is golden-constants only and does NOT execute the crypto path). A planted bad
  sig/proof must be rejected AND name the block in BOTH modes. **Only then** is LB-1 merge-ready.

### Phase 2 — LB-2: self-mint the sovereign checkpoint (copy-proven before live)
- [ ] **2.1 Run the parallel from-genesis FULL validation** (all crypto on) on the fixture to the anchor.
  **Verify:** reproduces SHA3 `00e95dbd…` + count 1,354,771 at h=3,056,758. A mismatch is a BUG to find,
  NEVER a fork — bisect with `-par=1`.
- [ ] **2.2 Compile the self-minted constant in** (replace checkpoints.c:73's borrowed-provenance value with
  the self-minted one; update the provenance comment to "minted by this node's full from-genesis validation").
- [ ] **2.3a Tighten the SHA3-UTXO snapshot path (only-tightening, do this one first).** `snapshot_verify.c:132`
  trusts the peer's `offered_utxo_root`; assert it against the in-binary `g_sha3_checkpoint.sha3_hash`
  (`checkpoints.c:86`) at h=3,056,758. This is genuinely only-tightening at that one anchor height →
  replay-confirm ZERO false-rejects of the genuine chain first.
- [ ] **2.3b Break the circular FlyClient/MMB root — NOT a one-liner (REVIEW #4).** `snapshot_offer.c:597,667`
  → `flyclient.c:151,212` verify samples against the PEER's offered `mmb_root`, and there is **NO in-binary
  MMB root** anywhere (the block-hash checkpoint array `chainparams.c:19-81` is all-zero placeholders). So
  this REQUIRES first MINTING + compiling an in-binary MMB root (or populating the block-hash checkpoints and
  pinning each sample's `block_hash` against them) — new work + surface, not only-tightening. Sequence it
  explicitly; don't let future Claude hit the dead end.
- [ ] **2.3c Retire weak `fc_check_pow` → real Equihash.** `fc_check_pow` (`flyclient.c:21`) skips powLimit +
  the solution. Real `CheckProofOfWork` needs the full header (the sample doesn't carry the Equihash
  solution today) — so this depends on widening the sample shape to include the solution. Note the dependency.
- [ ] **2.4 Make background genesis→anchor re-validation MANDATORY + visible** via `zcl_validationstatus`
  (the assumeutxo "converges to full security" property). Additive. **Verify:** a fresh snapshot-load node
  re-validates in the background and the status reflects progress.

### Phase 3 — subtraction (LAST; copy-proven on the frozen wedge fixture)
- [ ] **3.1 PROVE the self-minted seed cold-starts to tip hash-identical to zclassicd at ≥2 heights**
  on a COPY of `~/.zclassic-c23-postrestore-wedge-20260611`. This is the gate for ALL deletion below.
- [ ] **3.2 Delete the recovery/repair fabric** (~12,051 → ~2–3k LOC): `utxo_recovery_*`, `stage_repair_*`
  (keep rewind/header_solution/body_fetch if they have independent live callers — verify), `chain_restore_*`,
  the LevelDB `cp -a` copy path, `utxo_mirror_sync_service.c`, `utxo_projection.db`, `consensus_snapshot.db`
  export, the cold-import seed/provenance/torn-gate, the L1/L2 reconcile tree. Cross-check against
  `docs/work/architecture-deletion-plan.md`. **Verify after each file:** `test_parallel` green; a fresh
  cold-start still reaches tip on the fixture; never live.

### Phase 4 — UX + organization (parity-inert)
- [ ] **4.1 In-process MCP transport** — host the MCP loop on a node thread (handlers call dumpers/controllers
  directly instead of `mcp_node_rpc` re-marshaling JSON twice / `Connection: close` socket per call,
  `tools/mcp/rpc_client.c:142`). Batch the aggregators (`zcl_self_test`/`zcl_admin`/`zcl_status`) server-side.
- [ ] **4.2 Push out-channel** — wire the `EV_*` stream to an MCP `notifications/*` channel (event-driven
  operator UX, not poll-driven).
- [ ] **4.3 Onion-exposed `/mcp` behind `-mcp-onion`** — bearer-gated (reuse `ZCL_MCP_BEARER_TOKEN`),
  destructive-rate-limited, DEFAULT READ-ONLY. This is the only change that widens the attack surface
  (local-stdio → remote-Tor) — opt-in only.
- [ ] **4.4 RETHINK-3: domain-axis reorg** — make `domain/` the primary partition (vertical
  `consensus/<concern>/`); keep `*_stage.c`/`*_service.c` suffixes for grep. Mechanical, lint-checkable,
  large diff — stage it. **Verify:** one UTXO-apply concern reads in one place; parity tests green.
- [ ] **4.5 RETHINK-4: gate the default WebKit GUI launch** (`app/views/src/wallet_gui.c`) behind an explicit
  flag; make headless/MCP the default boot. Reclaims binary size toward the 15MB target.

---

## PLAN REVIEW (independent adversarial pass, 2026-06-20)

**VERDICT: NOT safe to execute as written.** The strategic shape (parallelize-don't-omit, self-mint,
delete-last) is correct and the KEEP list protects the right things; the failures are in the HOW of the
two hardest steps. The CRITICAL/HIGH fixes are now folded into the checklist items above (marked ⚠).
**Phase 0 is safe to start** (0.2 with the corrections, 0.2b, 0.3, 0.4 per-rule, 0.5, 0.6).
**Phase 1 must NOT start until** items #1–#3 below are in — they're the two load-bearing concurrency
invariants a green suite will NOT catch. **Phase 2 step 2.3** is not executable as originally written
(no in-binary MMB root exists — now split into 2.3a/b/c).

Ranked issues (fix folded inline where marked ⚠; full evidence in the review transcript):
1. **CRITICAL [1.4]** "verify lock-free on the pool" was WRONG — prevout resolve is FULLMUTEX SQLite +
   a same-txn freshness contract. → three-phase serial-resolve / parallel-crypto / serial-write. (Fixed: 1.4 + Phase-1 invariants.)
2. **CRITICAL [Phase 1]** active_chain window read-only-during-batch + `nStatus` race invariants were missing.
   (Fixed: Phase-1 invariants block. Gate: an `active_chain_at` mid-batch swap must not be observed by a worker.)
3. **HIGH [1.5]** the `reducer_drive_enter/exit` interlock (what makes the supervisor skip stage execution)
   must be preserved when demoting the supervisor to observer. (Fixed: Phase-1 invariants + 1.5.)
4. **HIGH [2.3]** no in-binary MMB root exists — "verify against the in-binary root" is a dead end as a
   one-liner. Only the SHA3-UTXO path at the anchor is only-tightening. (Fixed: 2.3a/b/c split.)
5. **HIGH [0.2]** named the wrong frontier fn — `derive_coins_best` (=coins_applied-1) can LEAD H\*; use
   `compute_hstar`, and serve a CACHED atomic (the derive takes the fold's lock). (Fixed: 0.2.)
6. **MEDIUM [0.2]** ~30+ `active_chain_height` callers — only external serving readers move to H\*; build
   the caller table first. (Fixed: 0.2.)
7. **MEDIUM [0.4]** dedup is per-rule: equihash = 3 layers (keep crypto primitive + domain verifier),
   base58/bech32 already single. (Fixed: 0.4.)
8. **MEDIUM [0.1]** driver soak is refold-only; 1.5 re-verifies on the normal/reorg path. (Fixed: 0.1.)
9. **MEDIUM [catchup]** gate `node_db_catchup_service` off during refold (low severity; one-line guard exists).
   (Fixed: 0.2b.)
10. **MEDIUM [gates]** `test_consensus_parity` is golden-constants only (never runs crypto) → equivalence
    must use the `*_verified_total` counters + the real-chain replay WITH forks. (Fixed: 1.6.)
11. **LOW** file:line drift corrected: the atomic co-commit is in `lib/util/src/stage.c:414-447` (not
    utxo_apply_stage.c:507); `script_check_item` is in a services-internal header — lifting it to a shared
    header is a sub-step of 1.3.
12. **LOW [0.3]** "apply uniformly to lib/+domain/" is a scope EXPANSION (subjects dense crypto fns to the
    ≤500-LOC/fn gate) — baseline pre-existing long fns or WARN-only; do NOT split consensus functions
    (the roadmap NON-GOAL).

**The one structural lesson for future Claude:** the two hardest steps (parallel engine, self-mint) fail
in their concurrency/trust DETAILS, not their direction. Do NOT treat "move crypto off the lock" or "verify
against our root" as one-liners — both have a hidden serial dependency (FULLMUTEX SQLite resolve; a
not-yet-minted MMB root). Resolve those dependencies explicitly before writing code.
