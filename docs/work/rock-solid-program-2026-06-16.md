# Rock-Solid Program — 2026-06-16

Program of record for the 2026-06-16 recovery. Read-only engineering pass
against HEAD `69efe2de4`. Inputs: phase-1 verification (areas A–D), root-cause
memories `project_recurring_anchor_collapse_wedge_2026-06-13` +
`project_coldimport_restart_fragility_2026-06-15`, `docs/HANDOFF.md`,
`docs/work/robustness-audit-2026-06-15.md`, `docs/work/FORWARD_PLAN.md`,
`docs/MVP.md`. Line citations in the Layer-1 specs are verified against HEAD.

---

## 1. Diagnosis

The node wedged ~2.85 days for two compounding reasons — **both datadir/deploy,
not software**.

1. **Coin tear.** The live binary (`6934ad512`) is 18 commits stale and predates
   HEAD's torn-import detection gate. Its cold-imported datadir developed a
   durable `ok=0 prevout_unresolved` hole at h=3,145,595 (missing canonical
   coinbase `60fc6f43a630b5b7:0`), with the trusted SHA3 anchor collapsed to
   checkpoint+1 (`hstar_cursor=3,056,759`). The stale binary can only surface it
   as an opaque `I4.3 window.consistency` HOLD; the watchdog fired 1,138
   fruitless recoveries. The block index is hash-identical to zclassicd at the
   wedge block, the spending region, and the tip (`zcl_probe_zclassicd
   match:true`) — the damage is purely in the coin set, invisible to hash
   probes (the defining trait of this class).

2. **Restart fragility.** Cold import is a non-self-sufficient "borrow from
   zclassicd" bootstrap. It skips SHA3 verify at non-checkpoint heights and, on
   a clean restart of a node that forward-synced past zclassicd, commits its tip
   *down* to zclassicd's index-best (a 10,984-block backward commit observed
   live on the dev lane).

**The software is sound:** the dev lane (HEAD code, fresh cold-import) held the
network tip, advancing, consensus-correct vs zclassicd, no crash/flap/SEGV in
9h. Remaining defects: (a) live datadir torn, stale binary can't name it;
(b) two HEAD health signals false-latch (`replay_canary_failed` from a 3-day-old
cross-binary sentinel, `contradiction_frozen` from the restart-fragility
backward commit); (c) a data-race SEGV in the `chain_restore_finalize` recovery
cascade; (d) the restart-fragility bug itself.

---

## 2. Layer 0 — Restore the live node — DONE

The destructive restore (snapshot wedged datadir → deploy HEAD → wipe + two-step
cold re-import → verify) is **largely complete; the node recovered** — see
`docs/HANDOFF.md` for live state. The two-step recipe is in CLAUDE.md (cold-sync
recipe) and the verification probes there. The forensic fixture
`~/.zclassic-c23-cointear-fixture-20260616` (missing coinbase `60fc6f43`, hole
h=3,145,595) is the load-bearing Layer-1 / §4.5-seal repro — **keep it.**

**Residual risk until Layer-1 (b) lands:** a freshly re-imported node that
forward-syncs past zclassicd and is then cleanly restarted can re-trigger the
`zclassicd_import_best` backward commit and leave a `contradiction_frozen`
latch. Until (b) ships, **avoid unnecessary restarts of the live node once it
forward-syncs past zclassicd**; a `contradiction_frozen` page after such a
restart is benign once liveness/consensus probes are green. A re-import also
re-runs the write-boundary defect (SHA3-skip at non-checkpoint heights), so
Layer-0 is a band-aid; Layer-1 (b) + the §5 fork are the cure.

---

## 3. Layer 1 — Autonomous durability engineering (copy-prove-gated, no live mutation)

Ordered by stability impact and dependency. **None touches the live node or live
datadir.** Each is gated on a copy-prove (datadir copy on isolated ports) and/or
a test, run against the running build gate's successor — do not start a competing
`make` in this worktree.

### (a) Commit the verified-green WIP (consensus-parity coinbase encoding + reducer test hooks)
- **What:** the uncommitted working-tree diff. `domain/consensus/src/coinbase.c`
  replaces the fixed 3-byte BIP34 height push with a faithful `CScriptNum`
  minimal-push (`dcb_scriptnum_serialize` / `dcb_push_height` /
  `dcb_push_scriptnum`) that byte-for-byte matches zclassicd's
  `CScript() << nHeight` at *every* height, not just where the magnitude is 3
  bytes wide. Plus `app/jobs/src/reducer_frontier.c` test-only
  `g_test_compiled_anchor_override` so the hermetic regtest can drive the real
  reorg re-bind path below the SHA3 checkpoint, the three expanded test files,
  and the `Makefile` CI flake-tolerance retry.
- **Why:** a consensus-parity fix on the coinbase builder removes a latent
  fork-risk (a mined block with a non-3-byte height encoding would diverge from
  zclassicd); the reducer test hooks unblock coverage of the reorg path
  otherwise only exercisable at mainnet height; the CI retry keeps the gate
  armable.
- **Touchpoints:** `domain/consensus/src/coinbase.c:19-90` (new encoders +
  `DCB_HEIGHT_MAX`); `app/jobs/src/reducer_frontier.c:26-60`
  (`reducer_frontier_test_set_compiled_anchor` + `reducer_frontier_compiled_anchor`);
  `lib/test/src/test_domain_consensus_coinbase.c`,
  `test_reducer_block_ingest_gate.c`, `test_reducer_forward_progress_gate.c`;
  `Makefile:1681-1696` (`ci:` retry block).
- **Gate:** running build+test gate (test_parallel green) + `make lint`,
  specifically `check-consensus-parity` (E13) and the `test_consensus_parity`
  golden group. Coinbase change is consensus-adjacent — confirm golden values
  still match and a regtest mine→zclassicd-accept round-trip passes if available.
  Branch off `main`, do not commit on a dirty detour.
- **Effort:** S.

### (b) Cold-import restart-fragility — OPTION 1 fix
- **What:** stop the destructive `zclassicd_import_best` backward commit (and the
  clamped flat re-save) when the node's own derived frontier is **strictly above**
  zclassicd's index tip. Full spec is Task C's; load-bearing points:
  1. At the top of the `need_zcd` block, derive the node's own frontier once via
     `boot_derive_coins_best(&ndcb)` (helper at `config/src/boot.c:820`, callable
     here — `progress_store_open` ran at boot.c:1482), and compute
     `zcd_best_h = best ? best->nHeight : -1` after the existing best-chain scan.
  2. **Suppress** `boot_promote_tip_via_csr(best, "zclassicd_import_best",
     false)` (`config/src/boot.c:2110-2113`) ONLY when
     `have_ndcb && ndcb.height > zcd_best_h` (**strictly** `>`). Emit a loud WARN
     + `EV_RECOVERY_ACTION` (`zcd_import_tip_suppressed derived=… zcd_best=…`).
     Keep the index import + blk-file linking **unconditional** — only the tip
     lowering is suppressed, so ancestry still roots the detached coins-best
     block.
  3. **Skip** `save_block_index_flat(ctx->datadir, &g_state)`
     (`config/src/boot.c:2122`) when the promotion was suppressed — re-saving the
     clamped map below the coins frontier is the across-restart amplifier.
  4. Key **strictly** `>`, never `>=`: a node at exactly zcd's tip or trailing it
     MUST still promote (`lag=0` was the live dev state during the audit).
  5. Do **not** touch the detached-island guard (`boot_index.c:576-599`), do
     **not** raise `REBUILD_RECENT_MAX_RANGE` (`repair_controller_rebuild.c:44`),
     do **not** force-link the `pprev=NULL` placeholder — never weaken a gate
     (TENACITY I3).
  - Companion **OPTION 3** (defense-in-depth, lower priority): in
    `save_block_index_flat` itself (`app/services/src/block_index_loader.c:183-227`)
    refuse to overwrite a good flat whose top `>=` derived coins-best with one
    whose top is below it.
- **Why:** **root cause of the live `contradiction_frozen` latch** and the 2nd
  documented cold-import failure class — preventing the harmful backward commit
  prevents the frozen-contradiction page (half of "node reports healthy at tip")
  and stops the partial-flat amplification that degrades every subsequent
  restart.
- **Touchpoints:** `config/src/boot.c:1975-1987` (need_zcd gate), `:2081-2107`
  (best-chain scan), `:2110-2117` (backward commit), `:2122` (flat re-save),
  `:737-774` (`boot_promote_tip_via_csr` def with `max_depth=INT64_MAX`/`POLICY_ALLOW`),
  `:808-826` (`boot_derive_coins_best`); `app/jobs/src/reducer_frontier.c:725-748`
  (`reducer_frontier_derive_coins_best_now`);
  `app/services/src/chain_state_service.c` csr_validate_locked (backward-move
  guards compare vs `chain_active`/sqlite, NOT the derived coins-best frontier —
  why the rollback lands even without the auth bypass).
- **Gate (copy-prove, test-first, never live):**
  `systemctl --user stop zcl23-dev`; `cp -a --reflink=auto` the dev datadir to a
  fixture preserving hardlinks; run a `/tmp` copy on **isolated** ports (NOT
  8053/18252 dev, NOT 8043/18242 soak, NOT live 8033/18232).
  - **Baseline (current binary):** confirm the bug — `Loaded NNN block index
    entries from zclassicd`, `csr: tip committed from=<derived> to=<zcd tip>
    reason=zclassicd_import_best`, `detached non-genesis root`, low-height
    getheaders re-chase.
  - **Positive (patched, fresh copy):** zclassicd index import STILL runs (no
    `zclassicd_import_best` commit below derived coins-best — the new
    `tip_suppressed` WARN appears), flat NOT re-saved below the frontier, chain
    forward-syncs from zcd tip.
  - **Negative (regression floor, separate fixture with bodies genuinely absent
    / real torn set, derived ≤ zcd):** the node STILL promotes/refuses exactly
    as today (guard not weakened); a legit fresh-import-at-zcd-tip fixture still
    promotes normally.
  - Tear down `/tmp` copies; keep the fixture. Effects are **preventive** for
    future datadirs (the live forward-bridge bodies are gone; heals only via P2P
    crawl or a fresh fixed-binary import).
- **Effort:** M (small surgical patch; the cost is the disciplined copy-prove
  matrix).

### (c) chain_restore_finalize SEGV — serialize the recovery cascade
- **What:** make the `chain_restore_finalize` rebuild cascade hold `ms->cs_main`
  (the lock bg-hash-verify snapshots under) for the duration of the
  block_index/active_chain mutation. Today the cascade
  (`chain_restore_rebuild_active_chain` /
  `chain_restore_rebuild_active_chain_from_block_files`) mutates shared
  block_index node fields and the `active_chain` slot array under `c->write_lock`
  **only** — never `cs_main` (`write_lock` at chain_restore_repair.c:50,52,231,242
  and ZERO `cs_main` in the file) — while the bg-hash-verify worker and
  bg-validation walk those same fields under `cs_main`/lock-free. The two locks
  don't serialize, so the remedy thread tears state out from under concurrent
  readers → heap corruption tripping later in a worker's `free()`
  (`malloc_trim+0x122`). Same class as the documented `phashBlock`/`block_map_grow`
  UAF.
  - **Defense-in-depth:** gate the runtime self-heal invocation so the heavy
    rebuild does not run concurrently with bg workers (pause/quiesce bg-validation
    + bg-hash around it, or only run the rebuild at boot before workers spawn).
- **Why:** a recovery path that **crashes the node** and corrupts memory is the
  worst kind of instability. The runtime trigger (`chain_integrity_failed` remedy)
  is **not currently armed on the live wedge** (`currently_active=false,
  attempts=0`), so this is presently a canary/anchor-replay hazard plus a latent
  boot/recovery-path hazard — but it WILL arm whenever `chain_integrity_failed`
  fires with bg workers live, and it already crashed the canary run.
- **Touchpoints:** `app/services/src/chain_restore_repair.c:606`
  (`chain_restore_finalize` entry), `:615` (calls rebuild), `:88-267` (rebuild
  sweep; writes `c->chain[h]` under write_lock `:231-242`, mutates
  `cur->pprev/pskip` `:258-267`), `chain_restore_disk_repair.c:419-490`
  (`_from_block_files` producer that ran per the crash log);
  `app/services/src/bg_hash_verification_service.c:194-222` (snapshots under
  `cs_main`, then `read_block_from_disk_pread` + `block_free` on a worker);
  `app/conditions/src/chain_integrity_failed.c:54-73` (remedy → finalize at `:67`);
  `app/supervisors/src/self_heal.c` (5s tick runs the remedy on a supervisor
  thread, concurrent with bg workers); `config/src/boot.c:3360-3368` (boot-path
  call); `lib/validation/src/chainstate.c:128-145` (`block_map_grow` free under
  wrlock — the realloc the bg-hash comment warns about).
- **Gate (sanitizer repro on an ISOLATED copy):** build a debug+ASan/UBSan binary
  (`-g -O1 -fsanitize=address,undefined`) and a separate TSan binary; point at a
  `/tmp` COPY of an anchor-replay datadir that exhibits `tip_window_holes` (the
  canary `--from=anchor` state: `--importblockindex` into a scratch dir, boot
  with bg-validation ON so bg-hash-verify runs while `chain_integrity_failed`
  re-fires the remedy). **Never the live datadir.** `ulimit -c unlimited`; make
  the canary exit-trap copy `node.log` + any core to the verdict dir before
  `rm -rf` (it currently auto-deletes, which is why no core survived). Expected:
  ASan heap-corruption/UAF or TSan data-race on a block_index field
  (`pprev/nFile/nDataPos/nStatus`) or `active_chain c->chain[]` between the
  self_heal thread and a bg worker. Confirm the fix (cs_main unification) makes
  both sanitizers clean and the canary survives.
- **Effort:** M–L (the repro + sanitizer build is the bulk; the fix is a lock
  change but must be reviewed for new lock-order hazards vs the LOCK-ORDER LAW —
  do NOT take `cs_main` while holding a leaf lock the drive path holds).

### (d) Health-signal de-staling — make HEAD report healthy when it IS healthy
The *other half* (with (b)) of "deploy HEAD + fresh import = healthy node that
stops paging." Both sub-items are observability-correctness, not liveness.

- **(d1) Scope the replay-canary FAIL sentinel.** `canary_sentinel_watch.c` reads
  the sentinel `verdict` and latches `fail` on `"FAIL"` but does **not** filter by
  `build_commit` (vs the running build) or `started_ts` (vs process start) — even
  though both fields are present in the sentinel JSON
  (`app/services/src/canary_sentinel_watch.c:191-205` reads only `verdict`; the
  JSON carries `build_commit`/`started_ts` per `test_canary_sentinel_watch.c:63`).
  A 3-day-old FAIL file from the OLD wedged binary (`build_commit 6934ad512,
  tip:0, reason=rpc_unreachable_getsyncdiag`) in the **shared**
  `~/.local/state/zclassic23-canary/` dir latches `replay_canary_failed` forever
  on an otherwise-healthy node.
  - **Fix:** in `fold_sentinel` ignore any sentinel whose `build_commit` != the
    running build or whose `started_ts` predates the current process start (treat
    as stale/not-mine, not a FAIL). Touchpoint
    `app/services/src/canary_sentinel_watch.c:140-206`.
  - **Gate:** unit test (`test_canary_sentinel_watch.c` already constructs
    sentinels with `build_commit`/`started_ts` — add stale-build + stale-ts cases
    asserting no FAIL latch) + a dev-lane restart showing the condition clears.
  - **Effort:** S.

- **(d2) Make `contradiction_frozen` self-clearing once reconciled.** The freeze
  is raised on a transient `active_tip_hash != csr_tip_hash` at boot
  (`app/services/src/chain_evidence_reconstruct.c:178-184`), but the remedy
  returns `COND_REMEDY_FAILED` with "no auto-repair; operator follow-up required"
  and **never clears** (`app/conditions/src/contradiction_frozen.c:38`), even
  though the live snapshot already shows `active_tip_hash_mismatch:false` once the
  chain reconciles forward. The coins-best cursor mismatch is *already* correctly
  treated as recoverable lag, not a freeze (`chain_evidence_reconstruct.c:185-201`)
  — the self-clearing precedent is in the same function.
  - **Fix:** once `active_tip_hash == csr_tip_hash` again (and the persisted
    `cec.contradiction_reason` no longer reflects a live mismatch), clear the
    `cec.*` keys and let the witness mark the condition resolved. The *root*
    trigger is (b) — landing (b) prevents the contradiction from being raised at
    all; (d2) is belt-and-suspenders for any datadir already carrying the latch.
  - **Secondary:** eliminate the `database is locked` write of
    `cec.contradiction_reason` at boot (a contention symptom in the boot log).
  - **Touchpoints:** `app/services/src/chain_evidence_authority_service.c:197-252`
    (loads/persists `cec.contradiction_reason`, clear path `:206-224`);
    `chain_evidence_snapshot.c:150-169` (mismatch computation + health_reason).
  - **Gate:** copy-prove on a dev-datadir copy that carries the frozen latch: boot
    the patched binary, confirm the latch clears once `active_tip_hash` matches
    and the node reports `healthy:true`; confirm a node with a GENUINE persistent
    mismatch still freezes (negative case).
  - **Effort:** S–M.

### (e) Lower-priority hardening surfaced by the findings
- The `rebuild_recent range > 10000 setup error` during boot reconcile
  (`repair_controller_rebuild.c:446`) is benign noise but indicates the boot
  reconcile asks for a range it will always reject; once (b) removes the 11k
  backward bridge this should disappear — verify, don't patch the cap (raising
  `REBUILD_RECENT_MAX_RANGE` is forbidden).
- `tip_finalize_stage.c:221` "have_data_missing" warnings climbing on the soak
  lane are the OLD `body_fetch_missing_have_data` wedge class — keep the soak lane
  as standing evidence that class is real and distinct; do not conflate with the
  dev lane's health-latch issues.

---

## 4. Layer 2 — Continuous proof / make instability impossible to miss

All LOCAL, no GitHub Actions (cost rule). Goal: every regression and stall is
caught automatically, locally, before reaching the live node, and live drift is
continuously measured.

### 4.1 Local pre-push enforcement (git hook via core.hooksPath)
- **Current state:** `core.hooksPath` is the default `.git/hooks`; no `.githooks`
  dir → **no enforced pre-push gate** today.
- **Action:** add a tracked `.githooks/pre-push` that runs `make lint` (the 33
  gates incl. `check-consensus-parity` E13) and a fast subset of `test_parallel`
  (or the full gate if affordable on the dev box), and set
  `git config core.hooksPath .githooks` (per-clone; document it in the worktree
  README so wt2/wt3 set it too). A push failing lint or the consensus-parity
  golden group is **blocked locally** — no consensus-breaker leaves the machine.
  Keep it fast enough to not tempt `--no-verify`.

### 4.2 systemd --user timer for the heavy gate + soak accrual
- A `--user` timer running the full `make ci` (lint + bench-regress +
  test_parallel + ci-mvp-gates) nightly against HEAD, writing a dated verdict, is
  the local CI. Pair with the dev lane staying up to **accrue soak time** toward
  MVP-C (7-day soak) — the dev lane is the soak candidate now that it holds tip;
  once (b)+(d) land it should also report `healthy:true`, the real soak start.

### 4.3 Get the replay-canary GREEN and scheduled
- The canary masking fix is **already landed** (the poll loop does `kill -0
  ISO_NODE_PID` and fails fast with `node_crashed_signal_N`, per
  `tools/scripts/replay_canary.sh`), so part (a) of the 2026-06-13 fix is done.
  Two things remain to make it green and trustworthy:
  1. Land (c) so an anchor-replay run no longer SEGVs (the canary's current FAIL
     is the masked SEGV).
  2. Land (d1) so a stale cross-binary sentinel can't latch a false FAIL.
  Then schedule the canary on the §4.2 timer (or its own) and require a PASS
  sentinel for the running build_commit before declaring the node soak-healthy.
- **Clean up the stale sentinel** in `~/.local/state/zclassic23-canary/` (the
  `2026-06-12` `tip:0` FAIL) as part of (d1)'s rollout, or scope the dir per-build
  so cross-run contamination is structurally impossible.

### 4.4 MVP-C8 parity oracle — keep it continuous
- The MVP-C8 parity oracle (zclassic23 tip hash == zclassicd at every probed
  height) is the inviolable consensus check. Make it a continuous probe on the
  live + soak lanes (a small timer calling `zcl_probe_zclassicd` and alerting on
  `match:false`), so a future tear is caught the moment it diverges, not 2.85 days
  later. The live node had NO continuous set-parity gate when the tear formed —
  this closes that hole.

### 4.5 Long-term: rolling-commitment seal for self-healing a future tear
- **Owner-gated, designed not built.** The seal ring is currently `open, head=-1,
  slots empty` — no rolling commitment to self-heal from. A rolling per-window
  SHA3 UTXO commitment, persisted, would let a node detect AND repair a coin tear
  from its own recent history instead of a full re-import — the structural cure
  for "a tear becomes permanent." Consensus-adjacent; must be designed against the
  parity doctrine first — **do not build speculatively**. Capture the design; ship
  only after the §5 fork is decided (it may be subsumed by self-sufficient sync).
  Overlaps `tenacity-roadmap` §5 (seal).

---

## 5. The strategic fork — cold-import vs self-sufficient P2P/trustless sync

This is the **owner's call**. Both Layer-0 wedge classes (the coin tear AND the
restart fragility) share one root: **cold import is a non-self-sufficient "borrow
from zclassicd" bootstrap.** It skips SHA3 verification at non-checkpoint heights,
depends on a co-located zclassicd, and commits state derived from zclassicd's
index — so it can install a torn or backward-clamped chain that becomes "our"
chain. Layer-1 (b) and a write-boundary import-correctness gate *harden* this
path, but do not change its nature.

**Option A — Keep + harden cold import (the current flagship path).**
- *Pros:* fastest cold sync (~25 min to tip, headers in ~60–74s); already wired;
  Layer-1 (b) + the write-time import-correctness gate close the two known failure
  classes; MVP cold-sync criterion met by hand today.
- *Cons:* structurally depends on a trusted local zclassicd (not trustless, not
  what "personal sovereignty stack" promises for a fresh user with no oracle);
  every new cold-import defect is a new way to install a bad chain; the import is a
  privileged write path that bypasses the normal validated ingest, so its
  correctness must be proven separately from the consensus engine.

**Option B — Move the flagship to self-sufficient sync (FlyClient + SHA3 UTXO
snapshot + delta P2P).**
- *Pros:* trustless and oracle-free — the real "30-second cold sync to tip"
  vision; every block enters through the *same* validated reducer path, so no
  privileged import writer to tear the coin set; eliminates both Layer-0 wedge
  classes at the source (no `need_zcd`, no `zclassicd_import_best` commit, no
  SHA3-skip import). FlyClient + MMB proofs are already partly built.
- *Cons:* the FlyClient/MMB/snapshot path is not yet proven end-to-end to tip in
  production; more code to make rock-solid; a larger investment than hardening
  cold import.

**Recommendation: B is the destination; A is the bridge.** Ship Layer-0 (A,
hardened by Layer-1 b) to restore the live node *now*, because B is not yet
production-proven and the operator needs a working node today. In parallel, treat
**self-sufficient sync as the v1.x flagship** — the only path that makes the two
wedge classes *structurally impossible* rather than *individually patched*, and
what the sovereignty-stack vision promises. **The owner's decision: how much to
invest in B before declaring cold import "good enough for v1."** If v1 ships on A,
fund B immediately after; do not let A's "works on my box with zclassicd next to
it" hide that a fresh user with no oracle cannot bootstrap trustlessly.

---

## 6. Sequencing — the critical path

**Owner-gated (destructive / deploy):**
1. **Layer 0** — restore the live node (§2). DONE; residual risk until L1(b) lands.

**Autonomous (copy-prove-gated, no live mutation) — in this order:**
2. **L1 (a)** — commit the verified-green WIP (§3a).
3. **L1 (b)** — cold-import restart-fragility OPTION 1 (§3b). Keystone.
4. **L1 (d1)** — scope the replay-canary sentinel (§3d1).
5. **L1 (c)** — chain_restore_finalize SEGV (§3c). Can run in parallel with 3–4.
6. **L1 (d2)** — contradiction_frozen self-clear (§3d2). Lower urgency once (b) ships.

**Continuous (Layer 2, stand up as the L1 items land):**
7. **L2 4.1/4.2** — pre-push hook + nightly `make ci` timer. Stand up immediately.
8. **L2 4.3** — schedule the replay canary, green once (c)+(d1) land.
9. **L2 4.4** — continuous MVP-C8 parity oracle on live+soak. Stand up immediately.
10. **L2 4.5** — rolling-commitment seal: owner-gated, after §5 is decided.

**Strategic (owner decision, parallel to everything):**
11. **§5 fork** — how much to invest in self-sufficient sync (B) before v1.

**What unblocks what:** Layer-0 gives a working node now; L1(a) ships finished
consensus-correctness; L1(b) is the keystone that makes restarts safe and removes
a re-tear vector; L1(c)+(d1) make the replay canary a trustworthy green gate; L2
makes every future regression and drift impossible to miss locally; §5 decides
whether the *next* class of wedge is patched or made structurally impossible.
