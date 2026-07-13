# FUTURE-CLAUDE — historical 2026-06-23 work list (superseded)

> **2026-07-12 correction:** canonical is wedged at H*=3,176,325 on incomplete
> shielded anchors/nullifiers. `ab512d577` fixed only the earlier transparent
> loader. Use `HANDOFF.md`, `FORWARD_PLAN.md`, and
> `SOVEREIGN-NETWORK-ROADMAP.md`; the numbered narrative below is historical.

Grounded in four FRESH analyses done this session (NOT recycled narrative — the
owner purged 55 story files because recycled narratives caused ~103 re-halts).
Trust ONLY what you re-derive from the code THIS minute + the live node. Every
file:line below was read this session; specifics ROT — re-verify before acting.
Gate every fix on **H\* CLIMB** (`reducer_frontier_compute_hstar` advancing past
the wedge to the moving tip), NEVER "booted without FATAL." Copy-prove on a
`cp -a` datadir COPY before any live change.

---

## 1. TL;DR (read this, then §2)

1. HISTORICAL: the older transparent wedge at `blocks=3,156,170` was repaired and the node
   temporarily reached the network tip (>3,156,900, `verificationprogress=1`),
   fixed by commit `ab512d577` (extends the active-chain WINDOW above coins-best in
   `config/src/boot_refold_staged.c:568`) PLUS loading a borrowed transparent
   snapshot whose body SHA3 was verified
   at h=3,156,809 (`utxo-seed-3156809.snapshot`, count 1,344,918) that folds FORWARD
   and never re-touches block 3,156,171. RESTART now SELF-HEALS: it re-seeds 3,156,809
   + folds forward (~13 min). STILL BORROWED — the 3,156,809 snapshot is minted from
   the zclassicd oracle; its height/hash matched a validated header but its state
   contents were not consensus-bound or re-derived from genesis.
2. Historical root (the OLD wedge): the torn seed `utxo-stopgap-3151901.snapshot`
   (count 1,344,817) was MISSING prevout `21876e8b…` that block 3,156,171 legitimately
   spends; `script_validate_log[3156171].ok=0 prevout_unresolved` capped the MIN-fold,
   H\* pinned at 3,156,169, and `coin_backfill` refusal was CORRECTLY terminal (a
   UTXO-set membership disagreement, not a fetchable body). The live loader NO LONGER
   uses that torn seed, so this specific hole is gone.
3. The ONLY cure is SUBTRACTION: re-seed `coins_kv` by FOLDING from a SHA3-verified
   anchor at h=3,056,758 via `-refold-from-anchor`, then DELETE the ~715 LOC
   borrowed-seed loader (`-load-snapshot-at-own-height`).
4. The from-genesis MINT (the sole cure-path artifact, no real anchor snapshot
   exists on disk yet) is the gate FOR SOVEREIGNTY (not for liveness — the node
   already reaches tip via the borrowed 3,156,809 snapshot), and it is on VOLATILE
   storage — it needs durability (the one owner action). No SOVEREIGN anchor exists
   today; `/tmp/utxo-anchor-3056758.snapshot` is MISNAMED (byte-identical to the
   3,151,901 stopgap on disk).
5. Everything else (climb-proof test, cutover-defect fixes, regression gates,
   lint hardening, the full backlog) is PARALLEL-ABLE NOW with no owner dependency.

---

## 2. THE ONE OWNER ACTION — make the mint DURABLE

**This is the only step future-Claude cannot do alone.** The node ALREADY serves the
network tip (via the borrowed 3,156,809 snapshot) — the mint is NOT a liveness gate;
it is the SOVEREIGNTY gate that lets us DELETE the borrowed loader. The from-genesis
mint (the mint process, datadir `/dev/shm/fmram`, flags `-mint-anchor
-mint-anchor-fast -nobgvalidation`) is ~10% done (h ~308,993 / 3,056,758) and
writes to VOLATILE `/dev/shm` + a `/tmp` output path (`ZCL_MINT_ANCHOR_OUT`). A
host reboot loses ALL progress. It is the sole artifact on the cure path and runs
ONCE.

Owner: EITHER commit to no host reboot until it finishes, OR restart it ONCE onto
DURABLE disk with a durable `-mint-anchor` / `ZCL_MINT_ANCHOR_OUT` output path.

Hard rules (owner directives — DO NOT violate):
- Do NOT touch, optimize, or restart the mint process yourself.
- Do NOT optimize the one-time fold for speed — the requirement is that it
  TERMINATES with a self-verified SHA3==compiled-checkpoint artifact, not that it
  is fast. The mint hard-asserts `count==1,354,771` + `SHA3==00e95dbd54a791…`
  (`lib/chain/src/checkpoints.c:86-104`) and `_exit(FAILURE)`+unlinks on mismatch
  (`config/src/boot_mint_anchor.c:126-180`), so a wrong set can never publish.

---

## 3. CRITICAL PATH

Each step: file:line + how it is PROVEN + tag.
Tags: `[blocks-daily-driver]` `[blocks-sovereignty]` `[nice-to-have]`.

### CP-1 — [OWNER, blocks-sovereignty] Make the mint durable
See §2. The node already serves tip via the borrowed 3,156,809 snapshot, so this
unblocks SOVEREIGNTY (deleting the borrowed loader), not liveness. Proof:
`[mint-anchor] SUCCESS`; then `uss_open(<path>, expected_sha3=cp->sha3_hash)` returns
non-NULL with `count==1,354,771 height==3,056,758`.

### CP-2 — [I-OWN, blocks-sovereignty, gated on nothing] Make the fold observable
**PARTIAL 2026-07-01.** The utxo_apply stage already emits a 10k-block
`[utxo_apply] fold progress: applied_height=...` heartbeat, and the
`-mint-anchor` driver now uses the same 10k cadence for its
`applied-through=... / anchor` operator line. This was landed without touching
the running mint; the active PID keeps its old binary, while the next run /
fixture gets the new line. Follow-up on 2026-07-01 replaced the old source-grep
guard with the compiled predicate `boot_mint_anchor_should_log_progress()` and a
focused `mint_skip_crypto` test that proves 10k heartbeats at 10000/20000 and
the final-anchor tail. A full copied-chain sidecar run was attempted from
`$HOME/.zclassic-c23-anchor-mint-cp2-proof` but did not reach the mint loop
before being stopped, so it is NOT closing evidence. Remaining proof before
closing CP-2: run a fixture or next mint until two increasing 10k heartbeats are
observed and record the sample-twice delta as evidence.

### CP-3 — [I-OWN, blocks-sovereignty] Land the H\* CLIMB proof + wire the 3 orphans
A COMPLETE worktree fixture already mints a verified mini-anchor, runs the
production `-refold-from-anchor` reset, folds M→N over on-disk stage logs, and
asserts `reducer_frontier_compute_hstar` CLIMBS M→N + retro commitment-equality.
Do NOT rewrite it — read, cherry-pick into main, register in
`lib/test/src/test_parallel.c` (~`:193`, alongside `refold_progress_floor`).
File: `.claude/worktrees/wf_70f4640d-454-2/lib/test/src/test_refold_retro_validate.c`.
The 3 formerly-ORPHANED tests are NOW WIRED [LANDED — `test_parallel.c:194`]:
`X(refold_from_anchor_fatal)` `X(refold_auto_arm)` `X(anchor_selfmint)` (the retro
climb gate `refold_retro_validate` + `refold_body_span_contiguous` are wired at
`:196`). Remaining: confirm the stale claim at `test_coins_kv.c:151` is corrected now
that they run. Proof: named tests green in test_parallel; the climb gate the 3 prior
refold tests did NOT cover.

### CP-4 — [I-OWN, blocks-sovereignty] Close BOTH cutover defects (or the wedge relocates)
Both REAL; copy-prove each with a named test.
- **Defect (1) FROZEN resume_target.** [LANDED 584789165] `refold_progress_bump_target`
  (`app/jobs/src/refold_progress.c:230`) now raises the resume target to the LIVE tip
  each tick (never lowers it, `:253`), so a multi-hour fold no longer "completes at
  boot-time tip" and hands the accrued gap back to the forward path that wedged. The
  prior bug: `resume_target = (int32_t)active_chain_height(...)` was captured ONCE at
  boot and `refold_progress_clear_if_reached` cleared the moment the cursor reached
  that frozen target. Keep the named test that asserts `clear_if_reached` does NOT fire
  while the live tip is ahead by > `ZCL_FINALITY_DEPTH`.
- **Defect (2) body-span contiguity precheck — [LANDED; auto-recovery still open].**
  `boot_refold_body_span_contiguous` (`config/src/boot_refold_staged.c:928`, called
  at `:1056` before `boot_refold_from_anchor_reset` at `:1080`) asserts body_persist
  holds a CONTIGUOUS body span 3,056,759..tip and raises the NAMED blocker
  `refold.body_gap at h=<x>` on a hole — instead of pinning utxo_apply mid-fold (the
  same `prevout_unresolved` wedge relocated). Proven by `test_refold_body_span_contiguous`
  (wired `test_parallel.c:196`). STILL OPEN: a gap only HALTS-with-a-name; it does not
  yet trigger peer re-fetch to auto-heal. Files for the re-fetch: body_persist
  lower-bound query; `block_pruning_service` (`lib/storage/src/disk_block_io.c:101`),
  `seal_kv.c:309 seal_prune_below_in_tx`.
- Also register a supervisor liveness child for the fold ("cannot halt without
  saying so" applies to the cure): assert `utxo_apply_cursor` strictly increases
  during an active from-anchor refold with a deadline; on stall raise
  `refold.stalled at h=<cursor>`.

### CP-5 — [I-OWN, blocks-daily-driver] Regression gates for the 5 deployed fixes + restart-stays-synced
None of these can ship a revert green today. Each: add a named test in
test_parallel.
- **FIX 1 loader_owns_seed** (`app/services/src/boot_services.c:1512-1521`) — [LANDED]
  covered by `test_loader_owns_seed_gate.c` (wired `test_parallel.c:195`): with
  `load_snapshot_at_own_height` set, `armed_from_anchor==true` and
  `block_index_loader_seed_stages_from_cold_import` is NOT called; trusted base stays
  at `seed_h`, not the checkpoint, with `loader_owns_seed=false` as the negative
  control.
- **FIX 3 anchor-hash FATAL** (`config/src/boot_refold_staged.c:1054-1066`
  `boot_snapshot_anchor_hash_matches(...)` → `_exit(FAILURE)` on mismatch) —
  PARTIAL coverage: `test_boot_refold_window_extend` (wired `test_parallel.c:195`)
  pins the window-extend seam + the no-index-block NULL→FATAL trigger (Cases
  A/B), and `test_loader_owns_seed_gate` (wired `test_parallel.c:196`) pins the
  pure anchor-hash predicate itself: identical 32-byte hashes pass; first-byte
  flip, last-byte flip, zero-vs-real, and NULL inputs refuse. The existing
  `test_refold_from_anchor_fatal` covers a DIFFERENT FATAL (the
  `-refold-from-anchor` reset). STILL OPEN if you want full end-to-end teeth: a
  forked-child loader test that builds a self-SHA3-valid snapshot, gives `ms` a
  PoW-proven block at `seed_h`, flips one byte in `hdr.anchor_block_hash`, and
  asserts the actual `anchor_hash_mismatch` FATAL; byte-equal is the accept
  control.
- **FIX 5 block_parse_cache** (`lib/storage/src/block_parse_cache.c:122-191`) —
  [LANDED] covered by `test_block_parse_cache.c` (wired `test_parallel.c:257`): (a)
  `bpc_encode`↔`bpc_decode` round-trip byte-identical for a multi-tx block; (b) HIT
  byte-equals `read_block_from_disk_pread`; (c) SAME height + DIFFERENT hash is a MISS
  (no stale body across a reorg); (d) LRU eviction at `BPC_CAPACITY` does not corrupt
  survivors. This guards a consensus-critical fold input.
- **FIX 2 header forward-only** — already VERIFIED COVERED
  (`test_chain_state_repo.c:720-775`, fix at `chain_state_service.c:453`). Keep; no
  action.
- **FIX 4 node.db-before-wallet** — COVERED but fragile
  (`test_wallet_persistence_cycle.c:430-456` source-grep; PASSes silently outside a
  repo tree). Optional: make the in-repo path mandatory under `make ci`.
- **NEW restart-stays-synced gate** — no `make ci` gate proves the node stays synced
  ACROSS A PROCESS RESTART (the in-process forward-progress gate never reopens the
  db; `crash_recovery_test`/`two_node_peer_tip.sh` are opt-in, excluded from ci, and
  soft-green without `ZCL_CRASH_REQUIRE_TEETH`). Add a hermetic gate: mine N regtest
  blocks through the reducer front door, CLOSE `coins_kv`+`node.db`, REOPEN, assert
  durable tip+H\* survive at N, ingest N+1..N+M, assert monotonic climb post-reopen.
- **Add an orphan-test drift lint gate**: grep `lib/test/src` for `^int
  test_[a-z_]+(void)` and FAIL if any defined entry point is absent from the
  `test_parallel.c` TEST_LIST (closes the silent-skip class the file's own header
  warns about).

### CP-6 — [I-OWN, blocks-daily-driver] Close hollow lint gates
The deployed-fix gates above ship a revert green BECAUSE the lint surface is hollow
in places. Fix the proven-hollow ones (inject-verify each: clean→exit 0,
injected→exit 1, emptied/renamed scan→exit 2):
- `check-before-save-hooks` (`Makefile:1709-1715`) greps the token `before_save`
  not the `ar_register_before_save(` call — inject-proven hollow. Change to
  `grep -qE 'ar_register_before_save[[:space:]]*\('`.
- `check_one_write_path.sh:53` omits `domain/` (where consensus writers live) and
  is fail-silent on a zero-file find. Add `domain` to roots + a scan-count floor.
- `check_raw_sqlite.sh` (sole AR-lifecycle gate) `2>/dev/null … || true` swallows
  grep exit ≥2. Add a non-empty-scan preflight + explicit grep-exit handling
  (mirror `check_consensus_parity.sh:62-70`).
- **Pre-push local-CI gate — LANDED 2026-07-01.** `make install-hooks` now arms
  this clone with `core.hooksPath=tools/githooks`, and `make lint` / `make ci`
  run `check-git-hooks-installed`, which fails loud unless the tracked
  `tools/githooks/pre-push` gate is active. The checker also verifies the
  tracked hook still defaults to and invokes the local `make ci` gate, so a
  no-op executable hook cannot pass. `test_make_lint_gates` injects `.git/hooks`
  (must fail), `tools/githooks` (must pass), and a temporary no-op
  `tools/githooks/pre-push` body (must fail, then restore). Limit: Git cannot
  invoke an uninstalled hook on the first raw push from an unarmed clone; the
  enforced path is `make install-hooks` plus `make lint` / `make ci`.

### CP-7 — [AT MINT COMPLETION, blocks-sovereignty] Copy-prove the sovereign cutover, then deploy
> Operational runbook with the exact staging command + the durability fork:
> [`sovereign-cutover-runbook.md`](./sovereign-cutover-runbook.md). The mint writes its output to
> `/tmp/anchor-ram.snapshot` (ZCL_MINT_ANCHOR_OUT), NOT the mislabeled `/tmp/utxo-anchor-3056758.snapshot`.
> The RANK-1 parity blocker (over-counting unspendable outputs) is **RESOLVED** (`9fe9a8ee6`,
> `utxo_apply_delta.c:381`), so the mint reaches `count==1,354,771` and will not FATAL.
1. Verify the artifact read-only: `uss_open(<path>, expected_sha3=cp->sha3_hash)`
   non-NULL, `count==1,354,771`, `height==3,056,758`.
2. Atomic-place at the loader's expected path `<datadir>/utxo-anchor.snapshot`
   (.tmp on the SAME fs + fsync + rename; a torn cp yields SHA3 mismatch →
   `uss_open` NULL → contaminated node.db reseed fallback → FATAL). UNSET
   `ZCL_MINT_ANCHOR_OUT` in the systemd unit AND confirm it is absent from the live
   service environment — `mint_snapshot_path` reads the env FIRST
   (`config/src/boot_refold_staged.c:141-158`, env-first at `:143`). Pre-verify via
   `anchor_snapshot_verified_reachable()` (`:169-180`).
3. Reconcile the systemd unit: the LIVE process diverges from its unit (launched
   with `-load-snapshot-at-own-height` + `-nobgvalidation` not in ExecStart). After
   cutover the unit carries `-refold-from-anchor` (one-shot; no-ops once
   `coins_kv_is_proven_authority`, `:852`) and DROPS `-load-snapshot-at-own-height`.
4. On a `cp -a` COPY: drop `-load-snapshot-at-own-height`, boot `-refold-from-anchor`
   (`boot_refold_from_anchor_reset`, `:305-468`), GATE ON H\* climbing 3,056,758 past
   3,156,171 to the (moving) tip — NEVER "booted without FATAL." Confirm the
   wedged-height prevout NOW resolves because coins_kv is FOLDED not copied;
   `invariant_sentinel I4.3` (utxo_apply log hole) clears. THEN deploy live; confirm
   H\*=tip + I4.3 clear.
- BELT-AND-SUSPENDERS (do before relying on the refold): cross-check `21876e8b…`
  / `getblock 3156171` against the running zclassicd (RPC 8232, NEVER stop it) once
  its reindex finishes, to 100% confirm seed-hole (refold cures it) vs
  consensus-invalid (chain genuinely ends at 3,156,170, H\* is correct, nothing
  wedged). Peers carry block 3,156,171 on the main chain, so seed-hole is
  near-certain.

### CP-8 — [AFTER cutover proven live, blocks-sovereignty] DELETE the borrowed loader
After the cutover reaches tip AND soak accrues (do NOT count the deletion as
progress before then): write an ADR (`docs/adr/000X-from-anchor-refold-supersedes-own-height-loader.md`)
recording that the from-anchor refold supersedes `-load-snapshot-at-own-height` AND
that the daily-driver guarantees a contiguous body span 3,056,759..tip. Then DELETE
`boot_load_snapshot_at_own_height_reset` (~336 LOC), the flag + call site
(`config/src/boot.c:3320-3322`, flag parse in `src/main.c`), the stopgap snapshot,
and the node.db reseed fallback (`coins_kv_seed_from_node_db` branch at
`boot_refold_staged.c:344-353`) — ~715 LOC total, plus ~1,928 LOC of borrowed-seed
machinery (`utxo_recovery_restore.c` 800, `utxo_recovery_frontier_gate.c` 480,
`block_index_loader_torn_gate.c` 234, `utxo_recovery_torn_anchor.c` 223,
`utxo_recovery_seed_provenance.c` 111, `utxo_recovery_mirror_walk.c` 80). Proof:
ADR merged; `make build-only` + test_parallel green; live boots WITHOUT the flag;
H\* stays at tip across a restart; grep confirms zero `load_snapshot_at_own_height`
references. The subtraction that ends the wedge class permanently.

---

## 4. FULL BACKLOG (deduped, prioritized)

P0 = halt/nudge/fork right now. P1 = blocks daily-driver / sovereignty. P2 =
correctness/robustness, off the critical path. Each: problem · evidence · fix ·
proof. Items already on the CRITICAL PATH are referenced, not repeated.

### P0 — halt / nudge / fork (all collapse onto the §3 cutover)

> **CORRECTION 2026-07-12:** `ab512d577` resolved the specific transparent
> prevout halt below and temporarily reached tip from the borrowed 3,156,809
> artifact. It did not prove complete shielded state. Canonical is currently
> held at H*=3,176,325 by incomplete anchor/nullifier history. These entries
> remain the historical root and sovereignty rationale, not current health.

- **forward-sync HARD HALT — borrowed seed incomplete (HISTORICAL, now RESOLVED).**
  See TL;DR + CP-7.
  `app/jobs/src/utxo_apply_stage.c:358` (upstream.ok==0 → JOB_BLOCKED holds cursor);
  `script_validate_log[3156171]=0/prevout_unresolved`. Fix = the cutover (subtraction);
  do NOT add an in-place repair branch (coin_backfill correctly refuses terminal).
  Proof: after cutover `script_validate_log[3156171].ok=1` (or a UNIFORM consensus
  reject the from-genesis mint reproduces) + getblockcount climbs past 3,156,170.
- **UNRESOLVED consensus question gating the fix.** Not yet proven `21876e8b…` is a
  seed hole vs a real consensus-invalid block. Cluster of 3 holes (3156171, 3156255,
  3151412) suggests systematic seed/index gap. Cross-check zclassicd RPC 8232 (read
  only). See CP-7 belt-and-suspenders.
- **sovereign cutover BLOCKED ON MINT.** No SHA3-verified anchor on disk; the only
  candidate is byte-identical to the stopgap (h=3,151,901, misnamed). See §2 + CP-1.
- **every restart wipes the UTXO set + re-seeds (no idempotency guard).**
  `boot_load_snapshot_at_own_height_reset` unconditionally `DELETE FROM coins`
  (1.34M rows) + re-seeds + forces cursors down EVERY boot (`config/src/boot.c:3320`,
  `boot_refold_staged.c:585`, `coins_kv_reset_for_reseed`). Under the OLD torn seed
  this re-seeded @3,151,901 + re-wedged; under the current COMPLETE 3,156,809 seed it
  re-seeds + folds FORWARD to tip and self-heals (~13 min), so the consequence is
  slow-but-recovering, not a wedge. Contrast the guarded `do_from_anchor`
  (`boot.c:3336-3339`, `coins_kv_is_proven_authority`). Fix = the cutover (CP-8);
  interim subtraction: add the `coins_kv_is_proven_authority` short-circuit at the
  top of the loader so a folded-forward node is never reset + re-seeded. Proof:
  restart twice, getblockcount stays at tip, no re-seed on the 2nd boot.
- **restart was NOT a recovery path for the tip wedge (HISTORICAL — under the OLD
  torn seed).** It re-seeded + re-folded straight back into the same
  `not_script_valid` stall (`tip_finalize_stage.c:249`, `script_validate_stage.c:283`);
  WATCHDOG cleared BLOCK_FAILED every ~313s and re-failed. With the current COMPLETE
  3,156,809 seed, restart re-seeds + folds FORWARD to tip and self-heals. Fix toward
  sovereignty = the cutover.

### P1 — blocks daily-driver / sovereignty

Forward-sync / reducer:
- **PEER-GATE FALSE SUPPRESSION (latent, masked).**
  `app/conditions/src/reducer_frontier_reconcile_light.c:484-494` gates the L1
  healer on `peer_lag_allows_repair()` (false unless a peer is strictly AHEAD).
  Peers at 3,155,947 < local 3,156,170, so every actionable detect is suppressed
  (`suppressed_ticks` climbing). Fix: widen the peer-independent bypass to treat a
  non-terminal `coin_backfill_attempted` (SCANNING/REPAIRED) like `refused_coin_tear`
  at `:485` (durable internal-store evidence, not peer staleness). Proof: inject a
  repairable hole on a fixture with peers behind; assert the remedy runs,
  suppressed_ticks stops climbing, H\* climbs.
- **OWNER-ACK GATE = second manual nudge.** `stage_repair_coin_backfill.c:606-608`
  refuses with `owner_ack_missing` every ~5s tick BEFORE the txindex resolve at
  `:629`. Fix: for chain-bound, txindex-provable backfills default SCAN/insert to
  auto; reserve owner ack ONLY for genuinely-unprovable mints. Pin current behavior
  in a lock-in test first.

P2P / net:
- **`sync_violation_lag` MISATTRIBUTES a downstream connect wedge to bad peers.**
  `gap = connman_max_peer_height - local_tip` where `connman_max_peer_height`
  returns `node->starting_height` (HANDSHAKE-STATIC, never updated;
  `lib/net/src/connman.c:1962-1995`, `:1969`). On a non-network pin it force-rotates
  every outbound peer + sets SYNC_IDLE (fights gap_fill/activation) + pages forever
  (`sync_violation_lag.c:42-45,84-92`). Fix (subtraction): gate the remedy on
  evidence the LOCAL pipeline is progressing — when
  `g_last_precondition_height==local_tip` with `reason=not_script_valid`, suppress
  the rotation + SYNC_IDLE reset (reuse the `reconcile_light.c:405` "no peer ahead"
  predicate); at minimum do NOT set SYNC_IDLE in the remedy (`:91`). Proof:
  copy-prove the pin → 0 rotations over 1h, stops paging, H\* does not regress.
- **`connman_max_peer_height` reads ONLY `starting_height`.** Same root as above;
  on a cured node at tip it false-negatives "no peer ahead" for every long-lived
  peer whose tip advanced after handshake → permanently suppresses self-heal. Fix:
  track a live per-peer best-advertised-header height; return
  `MAX(starting_height, live_best_header)` (update in `lib/net/src/msg_headers.c`).
  Proof: `test_connman_live_peer_height` — handshake at H, advertise H+N, assert
  `connman_max_peer_height==H+N`.

Wallet / Sapling:
- **In-memory keystore (201 keys) diverges from on-disk wallet_keys (100 rows)
  RIGHT NOW** (`getwalletinfo.persistence healthy:false mismatch:true`). The 101
  unflushed keys VANISH on restart; change sent to an unflushed key
  (`wallet_shielded_send.c:228, 236-245`) and the ZSLP genesis key
  (`zslp_command_service.c:133`) become unspendable. Fix (subtraction): make
  `wallet_generate_new_key` itself persist via the wallet_db handle so NO caller can
  grow the keystore without a row (mirror the getnewaddress flush-or-rollback,
  `wallet_controller.c:67-86`). Proof: z_sendmany with change → kill -9 → reboot →
  `mismatch==false` AND the change UTXO is spendable.
- **Sapling commitment tree replays from activation (h=476,969) on EVERY boot under
  the stopgap loader.** `boot_refold_staged.c:667-669` unconditionally clears
  `node_state["sapling_tree"]`/rescan/rebuild then `:673` rebuilds (~5 min,
  2,676,852 blocks). Fix (subtraction): do NOT clear when the persisted tree root
  already matches the chain tip's hashFinalSaplingRoot; disappears with the cutover.
  Proof: 2nd warm boot logs `Sapling tree loaded` with NO `replaying h=476969…`.
- **The flat-file Sapling checkpoint (`sapling_tree_ckpt.dat`) is DEAD**
  (`sapling_tree_flush_checkpoint`, `incremental_merkle_tree.c:432`, has ZERO
  production callers) — the missing "bound" the boot comment relies on. Fix: pick ONE
  tree-persistence authority — either wire the flush every
  `SAPLING_CHECKPOINT_BLOCK_INTERVAL` blocks, OR (subtraction) DELETE the flat-file
  scaffolding and make node_state resume work (next item). Proof: warm boot resumes
  with no genesis replay.
- **node_state-backed Sapling resume is effectively dead too** — requires an exact
  root match OR a `(ckpt_h-476969)%100000==0` boundary
  (`sync_controller_sapling_tree.c:91-132`). Fix: persist the tree root alongside the
  blob, verify against `active_chain_at(ckpt_h)->hashFinalSaplingRoot` for ANY
  ckpt_h (drop the 100000-boundary special case). Proof: 2nd boot at an arbitrary tip
  replays 0 blocks.

Mempool / reorg:
- **`active_chain_height` (getblockcount RPC + P2P start_height) reads the raw
  finalize tip `g_last_advance_height`, NOT H\*.** On a reorg rewind,
  `rewind_cursor_if_active_chain_reorged` lowers the cursor + refreshes
  `g_provable_tip` but never lowers `g_last_advance_height` (raise-only;
  `chainstate.c:626-650`, `tip_finalize_stage.c:315/330/799`,
  `msgprocessor.c:1136-1139`). Across the rewind→re-finalize window getblockcount +
  the P2P version advertise just-disconnected blocks. Fix (subtraction): delete
  `g_last_advance_height` as a second authority and serve H\* directly (repoint
  `get_height` to `reducer_frontier_provable_tip_get()`). Proof: extend
  `test_waitforheight_provable.c` to assert `active_chain_height() ==
  reducer_frontier_provable_tip_get()` after a synthetic reorg rewind.
- **Hard reorg-depth cap (10) diverges from zclassicd's most-work rule.**
  `zcl_reorg_allowed` REFUSES any reorg deeper than `ZCL_FINALITY_DEPTH=10` when
  in_ibd=false; the sole caller hardcodes in_ibd=false so the
  `MAX_IBD_REORG_LENGTH=1000` allowance is dead (`sync_evidence_policy.c:41-65`,
  `checkpoint.c:6-10`, `utxo_apply_delta_reorg.c:418`). A legal >10 reorg → zcl23
  pins on the lighter chain while zclassicd follows the heavier → permanent fork
  (R2-L3). REPLAY-GATED: replay full history (assert zero >10 reorgs occurred) before
  relaxing the steady-state cap; plumb in_ibd to close the dead-code half.
- **Mempool admission has no IsFinalTx/BIP113 check, and the miner can self-mine a
  self-rejected block.** `accept_to_mempool.c` never calls `is_final_tx`; `miner.c`
  template loop (`:110-146`) never checks it either (`is_final_tx` enforced only at
  `check_block.c:465`). A non-final tx → mined template → block the node's own
  check_block rejects. Fix: add an `is_final_tx` gate to BOTH mempool admission
  (`accept_to_mempool.c` ~`:132`) and the miner template loop (`miner.c:128`); the
  miner gate is the must-fix. Proof: cross-check `is_final_tx` parity vs zclassicd
  CheckFinalTx.
- **The two stale-fork-at-tip reorg self-healers depend on a running local
  zclassicd.** `tip_fork_stale.c:81,98` + `tip_stall_oracle_rebuild.c:81` remedy via
  `rebuild_recent_repair` hardwired to RPC 127.0.0.1:8232 with NO P2P fallback. Blocks
  the sovereign goal. Fix: add a P2P body-fetch fallback (reuse `body_fetch_stage`),
  or raise a typed PERMANENT blocker when the only block source is unreachable. Proof:
  H\* CLIMB on a copy with zclassicd unreachable.

Storage / corruption self-heal:
- **progress.kv has NO integrity check on open** (`progress_store.c:102-131` —
  `sqlite3_open_v2` + apply_pragmas only, NO `PRAGMA quick_check`, NO quarantine),
  unlike node.db (`database.c:448-457`). A torn coins_kv/stage-cursor/seal-ring page
  is undetected at open → SQLITE_CORRUPT at fold time → JOB_FATAL → H\* pins forever.
  Fix (subtraction-aligned): reuse the node.db pattern — `PRAGMA quick_check(1)`, on
  non-'ok' quarantine to `progress.kv.corrupt.<ts>` (shape at `database.c:372-388`),
  reopen fresh; coins_kv re-seeds + stages re-fold. Proof: corrupt an interior page
  on a copy, boot, confirm quarantine + H\* CLIMB.
- **Failed `progress_store_open` → permanent 5s crash-loop with no quarantine**
  (`boot.c:1495-1498` continues → `progress_store_db()` NULL → loader FATAL `_exit`
  at `boot_refold_staged.c:494-501`; unit is Restart=always RestartSec=5; the unit
  comment claiming "never auto-restarted" is stale). Closed by the quick_check +
  quarantine above (recoverable before the loader runs).
- **deployed loader has NO idempotency guard** — duplicate of the P0 reseed item;
  fix = the cutover (CP-8).

Tor / onion / explorer:
- **PERSISTENT .ONION IS NOT IMPLEMENTED.** `tor_write_torrc()` writes no
  HiddenServiceDir; vendored dynhost forces `is_ephemeral=1` and mints a fresh key
  every boot (`tor_integration.c:115-119`; `vendor/tor/src/feature/dynhost/dynhost.c:53-54,230,246`;
  5 distinct .onions on 2026-06-22). `read_onion_from_hostname_file` reads a file Tor
  never writes (DEAD). Structurally breaks persistent peer discovery. Fix (preferred,
  subtraction): switch `tor_write_torrc` to a standard `HiddenServiceDir` +
  `HiddenServicePort`, drop dynhost's ephemeral path → activates the existing-but-dead
  hostname-file code. Proof: restart twice, last two .onions byte-identical;
  `tor_data/onion_service/hostname` equals `onion_service_get_address()`.

MCP / RPC surface:
- **`zcl_getblockcount` MCP tool LIES vs H\*.** Returns
  `chain_projection_best_block_height()` (`SELECT MAX(height) FROM blocks WHERE
  status>=3`, = 3,156,170) not H\* (`chain_controller.c:31`,
  `chain_projection.c:33-38`), a 1-block divergence RIGHT NOW vs getblockcount RPC.
  Fix (subtraction): delete the projection fast-path in `h_zcl_getblockcount` and
  fall through to `mcp_node_rpc("getblockcount")` so the ONE H\* authority is the only
  source. Proof: `zcl_getblockcount == getblockcount RPC == waitforheight provable`.
- **`zcl_rebuild_recent` + auto-remedy callers HARD-DEPEND on local zclassicd
  8232.** `rebuild_recent_run()` returns "Cannot reach zclassicd…" if it is gone;
  the SAME `rebuild_recent_repair` is the auto-remedy for `tip_stall_oracle_rebuild`
  + `tip_fork_stale` (`repair_controller_rebuild.c:7-11,322-326,563-568`). Forward-sync
  auto-recovery breaks the moment the operator deletes zclassicd (the sovereign
  goal). Fix: P2P body-fetch / bodies-only refold; until then surface a typed
  PERMANENT blocker, not a quiet error string.
- **MCP event-push channel can SILENTLY DROP operator events under load.** Fixed
  256-event poll window every 750ms, de-dups only on seq watermark
  (`mcp_notify.c:191/212/323`); >256 events between polls → `g_high_water` jumps to
  newest, skipping the gap (an `oracle.chain_halted`/`chain.utxo_drift_detected`
  dropped exactly when busiest). Also a cookie/datadir mismatch returns an
  error-envelope and the channel emits nothing forever with NO log (`:237-241`). Fix:
  use the in-process `event_observe()`/ring subscription the file's own comment
  promises; until then detect seq discontinuity + emit a synthetic `events.dropped`,
  and log the error-envelope once. Proof: inject >256 events incl one operator event;
  assert delivery (or an explicit gap marker).

Resource / RAM:
- **From-genesis mint writes its log to RAM-backed tmpfs (`/dev/shm/fmram`);
  projected to exhaust 47GB before the anchor.** event_log.dat grows ~6.6KB/block
  (~18GB projected for the log alone + node.db ~9GB; tmpfs already 19.5GB used at
  ~1/10 target). ENOSPC kills the mint mid-fold, losing the work — THIS is the owner
  action in §2. Fix: next mint run points `-datadir` at REAL disk; do NOT touch the
  running mint. The growing event_log.dat is the load-bearing per-height H\*
  success-log — fix is location, not deletion.

Build / lint:
- **Pre-push local-CI gate NOT INSTALLED** — see CP-6.
- **`check-before-save-hooks` / `check_one_write_path.sh` / `check_raw_sqlite.sh`
  hollow** — see CP-6.

### P2 — correctness / robustness (off the critical path)

- **getblockcount serves H\* but getbestblockhash serves active_chain_tip
  (inconsistent by 1)** (`blockchain_controller_blocks.c:45` vs `:59`). Fix: serve the
  hash AT H\* (one line). Proof: `getblockhash(getblockcount)==getbestblockhash`.
- **Boot total ~447s + a ~4,300-block re-fold each restart** (utxo_import 51s is the
  legacy LDB import the genesis-folded path skips, `boot.c:2494/2505`). Disappears
  after cutover with `-nolegacyimport`.
- **Sapling tree fully rebuilt (1,056,485 commitments) every reseed boot** — see the
  P1 Sapling items; disappears with the cutover.
- **Block-index sidecar corruption with an unhandled verdict is a hard FATAL →
  crash-loop** (`boot.c:2468-2484`, `block_index_sidecar_integrity.c:274`). Route the
  quarantine through the bounded `boot_auto_reindex` budget so it pages once + serves
  degraded.
- **Several boot FATAL/`_exit` paths abort with NO bounded retry budget** (coins-view
  `boot.c:1694`, progress.kv `:1774`, wallet D/E/F `:1328/:1380/:1406`, schema
  downgrade `:1220`). Leave the data-loss guards FATAL; the gap is observability —
  ensure each emits `EV_OPERATOR_NEEDED` so `zcl_blockers` names a permanent blocker.
- **STATE F FATAL on keystore count mismatch** (`boot.c:1392-1404`) — fixed by always
  flushing new keys (P1 wallet item); STATE F then becomes a true invariant.
- **Post-fold detection of a SILENTLY-corrupt coins_kv is absent** — seal_kv carries
  per-grid `coins_sha3` but nothing cross-checks the live commitment against it
  (`coins_kv.c:345-385`, `seal_kv.c:58`). Wire one cross-check at each seal grid;
  mismatch raises `seal.coins_mismatch.<G>` (the keystone the MEMORY note flags).
- **auto-reindex self-heal does NOT heal the deployed coins_kv** — the loader
  re-DELETEs+re-seeds AFTER the reindex (`boot.c:2622` then `:3321`). Cured by deleting
  the loader (CP-8).
- **No mempool resurrection on block DISCONNECT** (`tip_finalize_post_step.c:164-171`;
  reorg unwind has zero mempool calls). Re-submit disconnected non-coinbase txs,
  bounded by finality depth.
- **No conflict (double-spend) eviction on block connect** (`txmempool.c:458-461`
  hash-match only). Walk each confirmed tx's vin + evict conflicting entries via the
  existing next_tx index.
- **at-tip inv→getdata bypasses the download manager** (`msg_tx.c:133-168` — no
  `dl_mark_in_flight`/`dl_assign_to_peer`); a dropped body can stall new-block ingest
  up to 10 min. Route through the download manager so `dl_check_timeouts` reassigns.
- **Fresh-node DNS seeds degraded** (`chainparams.c:156-197`; zclassic.org resolves to
  a GitHub Pages IP, dnsseed/mainnet return nothing). Drop the dead DNS seeds; keep
  the 10 fixed + onion seed.
- **peer_directory accumulates DEAD self rows every restart** (`onion_service.c:478-492`
  INSERT OR REPLACE, no DELETE of prior self) — cured by the persistent .onion;
  interim `UPDATE peer_directory SET self=0 WHERE self=1` first.
- **HTTPS explorer deferred-start has a SINGLE trigger** (`https_deferred_check` called
  only from `msg_blocks.c:361`); if forward-sync wedges during IBD the explorer never
  starts + no named blocker. Register it as a supervisor child tick.
- **Explorer compute-thread can wedge on "Loading…" forever (flag-leak class,
  `explorer_controller.c:181-197`)** — same class as the just-fixed tokens bug
  (09248a0b4). Replace the bare `_computing` int with a (state, started_at) pair that
  re-arms on staleness.
- **L1 — no serialized-block-byte-size cap** (`check_block.c:131-139` bounds only
  num_vtx). REPLAY-GATED. Add a >MAX_BLOCK_SIZE byte check after a full-history replay
  shows zero false-rejects.
- **D2 — coinbase-maturity reject DEFAULT-OFF on the live fold**
  (`utxo_apply_delta.c:60`, accepts a premature coinbase spend zclassicd rejects).
  REPLAY-GATED via `ZCL_REPLAY_COUNT_ONLY` over the full chain INCLUDING 3056758..tip,
  then flip the default; lock-in test pins current behavior first. NEVER enabled on
  the cutover fold itself.
- **Sapling-root mismatch reject DEFAULT-OFF + frontier not wired**
  (`connect_block.c:97/707-735`; NULL tree → returns true → cannot reject). Wire
  `g_sapling_tree` (couples to L7 + the cure), then flip the default. REPLAY-GATED.
- **L7 — shielded anchor-membership is an accept-all stub**
  (`coins_view.c:468-477 return true`; `sapling_anchors` table created-never-read). A
  valid zk-proof against a FORGED root is the attack. NOT a deletion — maintain a real
  anchor set on the fold-forward path; couples to the cure; owner-gated;
  parity-RESTORING. Mitigated today because Groth16 proof_validate rejects proofs
  against a non-existent anchor.
- **L2 — `process_block_should_skip_contextual_header()` skips difficulty/MTP/
  checkpoint binding for deep-history headers** (`process_block_contextual_header.c:75`;
  the comment claiming connect_block re-validates is FALSE). REPLAY-GATED. Replace the
  >1000-below-tip skip with the narrower sparse-window skip.
- **Checkpoint table is 65 ALL-ZERO-hash entries with checkpoints_enabled=true** —
  dead defense + latent halt at the first checkpoint height if L2 is tightened
  (`chainparams.c:4-58`, `checkpoints.c:53-64`, `boot.c:427`). Populate real hashes OR
  delete the entries / disable checkpoints. Interlocked with L2.
- **RAM-aware bg-validation batch cap only engages below 8GB RAM**
  (`bg_validation_service.c:662` → unbounded on real hosts; the RSS climb that forced
  `-nobgvalidation`). Set an absolute upper cap (~50000) regardless of total RAM
  before re-enabling validation.
- **IBD turbo sets `wal_autocheckpoint=0`; the only bound is a periodic checkpointer
  the live node logs as repeatedly failing** (`database_modes.c:152-156`,
  `db_service.c:297-300`). Keep a large-but-nonzero autocheckpoint in turbo, or
  escalate N consecutive failures.
- **systemd MemoryMax/SwapMax-vs-physical-RAM invariant enforced only by a hand-edited
  drop-in** (6 overlapping drop-ins; HANDOFF P1-3 OPEN). Add a boot-time guard that
  FATALs if `memory.max + swap.max >= MemTotal`; collapse to ONE authoritative file.
- **Out-of-process `-mcp` (documented default) fails EVERY RPC tool on a
  datadir/cookie mismatch while in-process state tools PASS** (mixed self_test
  signal; `mcp_server.c:181-182`, `rpc_client.c:29`). Make in-process MCP the default;
  reserve HTTP for the detached-proxy case.
- **`mcp_node_rpc_http` builds the request into a fixed `char body[8192]`** — silently
  truncates params >8KB; a 250KB-hex sendrawtransaction (the h=478544 class) fails
  (`rpc_client.c:136`). Size the body to the params length, or land the in-process
  default.
- **`MCP_ROUTER_MAX_ROUTES`=128, register silently returns false past the cap** (107
  live, 21 left; `router.c:21/102-103`). Raise to 256 + log-FATAL on a dropped route.
- **`zcl_sql` returns `interrupted:true` with ZERO rows on a 2s timeout** — looks like
  an empty result (`diagnostics_controller.c:39-53`). Set an MCP error body
  (`MCP_ERR_TOOL_TIMEOUT`) on interrupted+empty.
- **`getdataintegrity` took 53s synchronously on the RPC thread** — make
  `zcl_dataintegrity` async/cached or rate-limited.
- **ZCL_AR_ENFORCE is a DEAD `-D` macro** (`Makefile:104`, zero consumers) — the
  "compiler-enforced AR" claim is false. Wire `#error` guards in `activerecord.h`, or
  subtract the dead `-D` and correct the docs.
- **No build-twice byte-identity gate exists** — add a hermetic `make repro-check`
  (build twice with identical SOURCE_DATE_EPOCH, `cmp -s`); gate in `make ci`.
- **node-binary build false-greens on header-only edits** (no depfile tracking on the
  whole-program rule; `Makefile:291/1339`). Make `zclassic23` depend on `$(ALL_OBJS)`
  (depfile-tracked) and link those — kills the stale-binary trap + the deploy-only
  `rm -f`.
- **Five lint gates remain GREEN-build-hollow** (`check_no_secret_printf.sh`,
  `gate_stage_log_reorg_unsafe_ratchet.sh`, `check_projections_pure.sh`,
  `check_supervisor_*registration.sh`, `check_stage_advances_or_blocks.sh`) — retrofit
  the shared `gate_lib.sh` (`gate_require_scanned` + grep-exit-aware wrapper);
  discover by naming convention with a pinned floor, not a bare glob. Inject-verify.
- **`soak_assert.sh` uses python3 in 5 places** (violates the NEVER-python rule;
  silently no-ops on a python-less box). Rewrite with the awk/grep extractors from
  `soak_evidence.sh`.
- **C6 reporters disagree on whether a watchdog self-recycle breaks the 168h window**
  (`soak_harness.c:88-94` FAIL_CRASH vs `soak_evidence.sh` AUTONOMOUS). Pin the C6
  definition in `docs/MVP.md` + add a too-many-recycles NOT_MET selftest fixture.
- **FULL operator-claim proofs (C1/C2/C4/C7) + `ci-coldstart` (C3) are driven by
  NOTHING automated** (`mvp-verify`/`ci-install-linger`/`ci-stress`/`soak-ci` live
  outside `make ci`). Add a LOCAL recurring driver (`make ci-nightly` or a
  `systemd --user .timer`) that runs them on a cadence; keep them OUT of hermetic
  `make ci`.
- **`mvp` scoreboard inside `make ci` is advisory-only** (`mvp_scoreboard.sh`/
  `mvp_gate.sh` exit 0 unless `--strict`, which nothing invokes). Add a
  `make ci-mvp-strict` against a live node, wired into the nightly driver only.

### Recorded — NOT divergences, do NOT "fix" (they would re-introduce a halt)

- **L4 oversize-tx grandfather** (`tx_structural.c:108-133`) REPRODUCES live zclassicd
  (which false-rejects its own chain at h=478544). Fixture
  `domain_consensus_tx_oversize_grandfathered` covers the 413 txids.
- **L8 nullifier-gap below the activation cursor** surfaces as the PERMANENT blocker
  `utxo_apply.nullifier_backfill_gap` (halts HONESTLY); closed by a from-genesis
  replay (cursor==0). Do NOT suppress the blocker.
- **D1 difficulty-retarget** is mainnet BYTE-IDENTICAL (`pow.c:109-115`); the R3
  extra-paren only changes testnet/regtest.
- **TIP_FINALIZE one-block lookahead** structurally pins H\* one below utxo_apply at
  steady state (`reducer_frontier.c:147-151`) — intentional pipeline depth, not a
  stall.
- **STAGE_DRAIN_IMPL reorg-unwind durability** is CLOSED today (transitively, via the
  cursor setters marking dirty); protected by `test_reducer_forward_progress_gate`
  PART 2 — keep that floor.

---

## 5. WHAT NOT TO DO

- Do NOT optimize, touch, or restart the one-time MINT — it runs ONCE; the
  requirement is that it TERMINATES with a self-verified SHA3==checkpoint artifact,
  not that it is fast (owner hard-rule).
- Do NOT treat the borrowed stopgap (`-load-snapshot-at-own-height`) as the cure or
  harden it as a permanent loader — it is a borrowed crutch DELETED after the fold
  owns the seed + an ADR documents the body-span guarantee it provided.
- Do NOT loosen the `coin_backfill` refused-marker latch to re-insert the missing
  coin — the marker is CORRECT; loosening it admits a forged anchor (R2/L7 risk). The
  FOLD, not a backfill, resolves it.
- Do NOT count dead/orphaned code as coverage (3 orphaned `test_refold_*` tests; the
  dead `sapling_tree_ckpt.dat` path; the dead `ZCL_AR_ENFORCE` macro). Verify a test
  is in the `test_parallel.c` TEST_LIST before claiming it guards anything.
- Do NOT count the ~715/~1,928 LOC borrowed-seed DELETION as progress until the
  from-anchor fold has reached tip AND soaked.
- PREFER SUBTRACTION (deleting the borrowed loader, deleting `g_last_advance_height` as
  a 2nd authority, deleting the dead Sapling/AR scaffolding) over adding code.
- Do NOT enable ANY parity-tightening default (coinbase-maturity, L7
  anchor-membership, Sapling-root, the reorg cap, block-byte cap) on the cutover fold
  OR without a FULL real-chain replay INCLUDING the post-anchor 3,056,758..tip window
  proving ZERO false-rejects (the h=478,544 lesson — the chain has a 125,811-byte tx a
  text-copied cap false-rejects). The cure fold runs with CURRENT loose defaults so it
  is never gated on an un-replayed predicate.
- Do NOT regress any external height surface (getblockcount, getblockchaininfo.blocks,
  P2P version.start_height, `zcl_getblockcount`) back to `active_chain_height` — keep
  `reducer_frontier_provable_tip_cached` as the single authority.
- Do NOT chase the flat header rate / SLOW-ADVANCE peer re-sends — the limiter is the
  LOCAL wedge, not upstream pull.
- Do NOT do live surgery: copy the datadir, repro there, prove the fix FIRES on the
  copy, gate on H\* CLIMB, THEN deploy. NEVER stop the local zclassicd (RPC 8232 — the
  cross-check oracle).
