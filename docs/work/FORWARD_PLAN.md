# zclassic23 — V1 PLAN (MVP-anchored)

> **READ THIS FIRST.** This is THE finishing plan. The v1 bar is the 8
> acceptance criteria in **[`docs/MVP.md`](../MVP.md)** (v1 = MRS 8/8).
> Everything below is sequenced to move MRS toward 8/8.
>
> The framework/architecture refactor ([`docs/FRAMEWORK.md`](../FRAMEWORK.md)
> §9 is the open-item debt board; boot decomposition) is **~90% done and
> OFF the v1 path. Do NOT jump the queue into it.** It is reference, not the
> mission.

---

## #1 PRIORITY — win Q1 on a COPY; keep canonical recovery owner-gated

> Read [`../HANDOFF.md`](../HANDOFF.md) §0-LATEST before acting. The
> autonomous task is the architecture-board Q1 proof on a datadir COPY; the
> canonical lane is protected and any recovery/deploy remains the owner's
> lever. Cure design record:
> [`self-verified-tip-plan.md`](./self-verified-tip-plan.md). Durable Phase 0–6
> promotion gates: [`SOVEREIGN-NETWORK-ROADMAP.md`](./SOVEREIGN-NETWORK-ROADMAP.md).

**The #1 work now, in order:**

1. **Q1 / fresh-machine-to-tip (C3)** — use the immutable serving fixture and
   a fresh datadir COPY. Run the exact
   `make mvp-coldstart-to-tip-stopwatch` proof; a climb or an `at_tip` FSM label
   is not enough. The ledger must PASS within 600 seconds with final H\* equal
   to the captured true peer tip. Then confirm `make arch-score` rises and run
   the full lint/test gates.
2. **Canonical lane diagnosis/recovery** — read-only diagnosis is autonomous;
   restart, deploy, or datadir repair is owner-gated. Never use the canonical
   lane to test a Q1 change. A previously at-tip observation does not override
   current typed status.
3. **Q4 soak and disruption proof** — after Q1, run the dedicated fixture
   net-disruption proof, then accrue a clean 168-hour C6 window only on a
   sovereign, exact-parity candidate with gap ≤1 and zero manual restarts.
   `make soak-evidence-report` is the judge.
4. **C3 transport substrate** — the **seed** side (`rom_seed`) serves
   content-verified chunks, and the **fetch** side (`lib/net/src/rom_fetch.c`
   + `app/controllers/src/rom_fetch_controller.c`, operator-invocable via
   `ops.debug.rom_fetch.bundle`) does multi-seeder verify-by-content download
   with durable resume (`rom_fetch_download_verified_parallel`). Do not build
   another transport; what remains is the exact end-to-end timed proof in
   item 1, against zclassic23 rather than `zclassicd`.
5. **Hardening backlog** — ranked open items:
   1. Blocks-table row repair: per-row skip+purge in
      `load_block_index_from_blocks_table` instead of whole-table refusal;
      quarantine via the existing unwired `db_block_delete`
      (`app/models/src/block.c`), drop `HAVE_DATA` → `body_fetch` refetch →
      revalidate; typed blocker.
   2. Persisted FAILED-bit trust at boot made consistent: below the ROM
      checkpoint, re-derive, never trust a persisted status bit; above it,
      treat a persisted FAILED bit as a revalidate candidate, never let it
      gate `promote_best_header` (today only the blocks-hydrate rung drops
      FAILED bits; the flat/SQLite loaders trust `n_status` verbatim).
   3. `block_index_cache` integrity envelope: the flat file has an embedded
      SHA3 (`bii_verify_embedded`); the SQLite cache has only `COUNT>1000`.
      Add the same envelope or demote the SQLite cache to a pure
      re-derivable cache.
   4. Import-time per-row verify: `--importblockindex`'s bulk memcpy trusts
      legacy bytes; hash-bind+PoW-verify per row at import time and
      quarantine bad rows instead of the hydrate rung refusing the whole
      batch on one bad row.
   5. Consolidation: fold `recovery_coordinator` R1–R3 into condition-engine
      scheduling (keep R4, ~250 LOC); replace the boot loader's 6-rung
      if/else ladder with a dispatch table + dumpstate-visible counters;
      build `window_rebuild` once and reuse it for both
      `mirror_divergence_located` and `state_window_inconsistent`;
      standardize every remedy-loop WARN on `log_throttle_should_emit`.
      **Guards that must survive any consolidation verbatim:** the
      `coin_backfill` owner-ack env gate, the durable-vs-transient refusal
      marker distinction, `cooldown_max_rearms=0` retry-forever semantics,
      the `poison_rewind` served_floor bound, segment chmod-before-unlink,
      and any DEFAULT-OFF consensus-tightening flag (replay-gated — never
      flip without a full-history replay, the h=478544 lesson).
   6. Boundary-root ladder sparse coverage: `mmb_utxo_root` rows at
      non-rung heights (rungs stride 100k) are never cross-checked —
      densify rung coverage or self-hash the rows.
   7. `coins_applied_height` unbound, and `sapling_anchors` read-failure
      silently skipping instead of naming a typed blocker — small
      hardening pair.
   8. Non-anchor peer snapshot staging must refuse without
      checkpoint/PoW binding.
   9. Mechanical: verify every remaining `cac_`/`chain_advance_coordinator`
      hit (`git grep 'cac_\|chain_advance_coordinator'`) is a legitimate
      compatibility name (e.g. a stable `dumpstate` subsystem key) before
      treating the `block_source_policy` rename as fully closed.
   10. Bundle-seeded node: full-history block swarm starves the tip fold.
      Observed 2026-08-01 on the stopwatch-fixture rebuild (fresh datadir,
      bundle installed at 3056758, H* pinned there): the swarm plans
      50030 pieces from h=1, low-height pieces saturate the block-intake
      queue ("waiting for timeout retry after local payload intake
      backpressure"), and the reducer's needed range (3056759+) never
      enters the queue — H* frozen while 256 pieces stay inflight with
      zero completions. This contradicts `gap_fill`'s design comment
      ("deliberately rate-limited so it cannot starve live sync"). Fix
      direction: intake/scheduling priority for the reducer's next-needed
      range over census backfill, or cap below-floor backfill inflight
      while the fold is behind. Repro: `zcl-stopwatch-peer` unit journal
      2026-08-01 21:55-22:00 UTC; datadir archive
      `~/.zclassic-c23-fixture-serve.broken-aug01`.
      **FIXED 2026-08-02 (this commit):** root cause — the bundle install
      advances the stage cursors (body_fetch = seed+1) but never moves
      `chainactive`, and both body planners keyed their window bottom on
      `active_chain_height` alone. Two seed-floor raises, each a no-op on
      an unseeded node: (a) the block swarm's completion seed at
      `msgprocessor_snapshot.c` floors `our_h` at
      `reducer_frontier_provable_tip_cached()` (H*), so the first open
      piece contains the fold's next-needed height instead of all ~50k
      pieces tying rarest-first to the lowest indexes and flooding the
      128-slot intake ring through the 256-deep pipeline; (b)
      `gap_fill_service`'s window bottom floors `tip_h` at
      `body_fetch_stage_cursor()−1`, so dl_queue stops filling with
      below-floor heights and the height-sorted keep-lowest eviction
      stops refusing the fold-needed successors above the seed. The S2.4
      validate_headers floor is untouched (it only ever lowers the
      window as a backstop, never behind the fold). Regression floor:
      existing gap_fill/swarm/fast_sync groups green; live copy-prove on
      the aug01 fixture archive is the acceptance gate before the C3
      stopwatch rerun.
   11. ~~Anchor replay-canary degraded to a genesis-scale bg-validation
      walk~~ **FIXED 2026-08-01** (two commits): `make replay-canary-anchor`
      FAILed `budget_exceeded` at the 5400 s hard budget — bg-validation
      always started its fresh walk at genesis, and the Equihash-serial
      per-block walk (~47 blocks/s) needs ~19 h for 3.2M blocks. The
      canary's 45-min band only fits anchor→tip (~145k). First fix
      (4c8f4a208) started the fresh walk at the durable trusted base — but
      the rerun FAILed `crossnode_height` in 306 s: `tip_finalize_anchor`
      keeps RAISING the trusted base toward tip as anchors finalize, so by
      bg start it sat at ~tip and the walk completed instantly (just over
      the 300 s too-fast floor), and the anchor variant's dead
      `-connect=127.0.0.1:39999` sink froze the node at boot-tip while
      zclassicd kept mining (~22-block skew at verdict), so the
      crossnode equality gate and the byte-exact
      `--legacy-utxo-commitment` tier were structurally unreachable.
      Second fix (this commit): new `REDUCER_SEED_FLOOR_HEIGHT_KEY`
      (progress_meta, 8-byte LE), written ONCE (absent-guarded) only by a
      genuine external-seed path — cold-import wedge heal
      (`block_index_loader_rebuild.c`) and bundle install
      (`consensus_state_snapshot_install_activate.c`) — never advanced;
      bg-validation's fresh walk starts at seed_floor+1 when declared and
      at genesis otherwise (from-genesis/reindexed datadirs declare no
      floor, so the `--from=genesis` exact tier still walks full history).
      The anchor variant now dials `-connect=127.0.0.1:8034` (the
      read-only co-located zclassicd the genesis track already sanctions)
      so the node tracks tip to verdict.
      Third rerun FAILed `rpc_unreachable_getutxocommitment` at 383 s —
      a false unreachable: the seed-floor fix worked (`[bg-valid] seed
      floor at height 3202072` fired; the floor is the UTXO-snapshot
      height, NOT the 3056758 checkpoint — the walk extent above a
      cold-import seed is genuinely small), but `getutxocommitment`
      recomputes the SHA3 fold over ~1.35M UTXOs per call (~10.5 s under
      fold+walk load) and the RPC server's per-request watchdog
      (`ZCL_RPC_TIMEOUT_MS`, default 10000 ms) shutdown()s the socket at
      the deadline, so the client read an empty reply while the worker
      logged its span OK 0.5 s later. Third fix: canary nodes run with
      `ZCL_RPC_TIMEOUT_MS=120000` (production default untouched), and
      run_live gained a budget-bounded tip-convergence wait after the
      bg-COMPLETE loop — with a live oracle dialed, bg COMPLETE can
      arrive while the node is still folding post-seed bodies, and the
      crossnode gate demands exact tip equality.
      Fourth and fifth reruns surfaced a REAL seed-boot wedge (not a
      harness defect): the node healed via the seed, folded to the first
      post-seed block carrying a Sapling output, and stalled permanently
      below the oracle tip (fail-closed is consensus-correct). Three
      defects compounded. (1) ROUTING: the cold-import heal leaves BOTH
      utxo_apply.anchor_backfill_gap and utxo_apply.nullifier_backfill_gap
      set, and the sapling_anchor_frontier_unavailable remedy dispatched
      to the named-remedy ladder whenever the nullifier blocker was
      present, so the tier1 checkpoint-verified seed — the only real cure
      for the EMPTY_TABLE class — never ran (the ladder's Rung A anchor
      path only arms a refold respawn the escalator's permanent-blocker
      hold parks); detect already recorded these dual episodes as
      birth-defect, so the routing contradicted its own episode kind.
      (2) LATCH: the remedy's 5 attempts exhausted in ~4 min while the
      deferred sapling-tree rebuild that makes the flat-file checkpoint
      root-verified takes ~10 min, and with cooldown_secs=0 the engine
      latched at max_attempts forever — the checkpoint became valid
      (root == hashFinalSaplingRoot) minutes AFTER operator_needed.
      (3) HOLD MEMO: with (1) fixed, tier1b borrowed a header-verified
      frontier at H*=3202121 (root == PoW-committed hashFinalSaplingRoot) <!-- stale-ok: dated 2026-08-02 incident narrative (FAIL log), not a present-tense tip claim -->
      25 minutes before the kill, yet the fold never resumed: the first
      GATE_HOLD records an in-memory history-hold memo
      (utxo_apply_history_hold_record) that parks every later utxo_apply
      step while the anchor gap BLOCKER exists, the blocker legitimately
      outlives the seed (activation cursor stays >0), and only a process
      restart dropped the memo. node.logs archived at
      ~/.local/state/zclassic23-canary/lastfail_anchor_node_{wedge_20260801,holdmemo_20260802}.log.
      Fourth fix (this commit): remedy attempts tier1/tier1b FIRST
      whenever classify is EMPTY_TABLE; cooldown_secs=300 re-arms the
      remedy instead of latching (runs are idempotent — tier1 fail-closed,
      tier1b capped per-process, Rung C a no-op log); and a successful
      tier1/tier1b seed calls utxo_apply_history_hold_clear() so the
      parked fold re-runs the shielded gate (still fail-closed if the
      frontier were absent). Regression test: dual-gap engine case in
      test_sapling_anchor_frontier_condition.c (tier1 attempted before the
      ladder, birth-defect episode kind kept, no fake resolve, refused
      seed writes nothing, hold memo survives a refused seed).
      Fifth rerun (2026-08-02): the FAIL#4 fixes are PROVEN live —
      tier1b borrowed the header-verified frontier at H*=3202121 and the <!-- stale-ok: dated 2026-08-02 incident narrative (FAIL log), not a present-tense tip claim -->
      fold resumed IN-PROCESS (batch_commit 3202122..3202145, rows=24) —
      then the fold held permanently at 3202146, the first post-seed
      block carrying a shielded SPEND. Root cause: the shielded preflight
      (app/jobs/src/utxo_apply_nullifiers.c shielded_history_preflight)
      fail-closed-HOLDs any spend block while the shielded ACTIVATION
      cursors are >0, and the cold-import seed never flips them — only
      `-import-complete-shielded` or a from-genesis fold does. No
      auto-remedy can cure this BY DESIGN: the cursors are the node's own
      "my shielded history below the seed is unproven" marker, and
      holding (not forging a remedy) is the consensus-correct behavior.
      node.log archived at
      ~/.local/state/zclassic23-canary/lastfail_anchor_node_spendhold_20260802.log.
      A dry-run of the importer against the live zd then showed the
      shipped verb COULD NOT have cured it: (1) a fresh datadir refuses
      at shi_read_boundaries ("cursor(s) absent") — the import needs one
      boot first; (2) ldb_snapshot_make copies only .ldb SSTs + metadata
      and DROPS the .log WAL, so the 'B'/'z' pointers read the last
      compacted state (empirically: SST 'B'=3183455 vs coin records at
      3202110 vs zd live tip ~3202160 — every on-disk artifact lags <!-- stale-ok: dated 2026-08-02 incident narrative (FAIL log), not a present-tense tip claim -->
      differently), while utxo_recovery_copy_chainstate_stable (the full
      cp -a + dir-signature point-in-time proof the UTXO import and
      tier1b already trust) sees the fresh WAL state. Fifth fix (three
      parts): (a) the verb snapshots through
      utxo_recovery_copy_chainstate_stable and derives the tip bind
      TARGET-side — reducer seed floor when declared, else coins_best,
      with the sapling root taken from the target's own header at that
      height — refusing "boot it once first" when neither exists; (b) the
      service's tip verify is now BY-ROOT
      (chainstate_legacy_get_sapling_anchor at the expected tip root must
      return FOUND, plus an explicit incremental_tree_root Pedersen
      re-verify), so a stale 'z' pointer no longer refuses a good import
      while a forged tree still fails closed; (c) the bind guard anchors
      at the seed floor when declared, else coins_best, and documents the
      partially-folded-target rule (import owes history <= floor; the
      fold's own rows cover above; the preflight guarantees the folded
      span consumed no spends). Tests: scenario G (stale 'z' pointer
      re-pointed at an older root — import SUCCEEDS via the by-root
      bind), scenario F comment refresh (the refusal now fires at the
      up-front by-root verify); scenarios A-F stay green. The canary's
      anchor track gained the import interlude: wait for the staged-seed
      floor in node.log, wait for header coverage past H*, settle, stop
      the node, run -import-complete-shielded (one retry), respawn — and
      the anchor budget/elapsed ceiling grew 5400 -> 7200 s (a 6 h
      silent degrade still blows the band).
      Sixth rerun (2026-08-02): FAILed `shielded_import_failed` at 396 s —
      the import interlude ran exactly as scripted (seed H*=3202110 landed, <!-- stale-ok: dated 2026-08-02 incident narrative (FAIL log), not a present-tense tip claim -->
      header coverage 3202121, clean stop, import attempted twice), but the
      verb refused "tip bind SOURCE is all-zero" BOTH times: the first
      cut's root source was node.db `blocks.sapling_root`, and blocks rows
      are only written on body fold / lean sync / block import — a seeded
      boot's fold-resume anchor is a header-only height, so there was no
      usable row at 3202110 (not projection lag; the 60 s retry was
      identical). Sixth fix: the expected root now comes from the
      block_index projection (`block_index_projection_get_by_height`),
      which materializes `hashFinalSaplingRoot` for EVERY persisted header
      (`accept_block_header` copies the wire field into the index entry at
      lib/validation/src/accept_block_header.c:163; `block_index_db`
      serializes it), including header-only heights. A stale-branch
      sibling at the same height fails CLOSED at the service's by-root
      source lookup (a sibling never connected has no anchor record in the
      source chainstate), never a misbind.
      Seventh rerun (2026-08-02): FAILed `shielded_import_failed` again —
      the event-log block_index projection opened with `events_total=0`:
      a bulk P2P header sync emits no EV_BLOCK_HEADER events, so the
      projection is EMPTY on a fresh cold-import datadir (the live node's
      46k projection rows come from the tip-emitting path only). Seventh
      fix (two parts): (a) new `block_index_flat_sapling_root_at()`
      (app/services/src/block_index_loader.c) — the block_index.bin flat
      IS the store that carries `hashFinalSaplingRoot` for every header
      including header-only heights; the point reader mmaps it, verifies
      the embedded BIIE SHA3 (legacy sidecar-only files refused — no
      unverified bytes), and binary-searches the height-sorted 172-byte
      rows (format math proven byte-exact vs the zd oracle — hash AND
      finalsaplingroot at h=3202041); (b) the interlude's stop now waits
      120 s for TERM (was 10 s — the SIGKILL preempted the graceful
      shutdown's ~550 MB flat rewrite, which is what freshens the flat
      past the header-sync tip) and FAILs named
      `shutdown_flat_save_missing` when the save never landed.
      Eighth rerun (2026-08-02): FAILed `shutdown_flat_save_missing` at
      340 s — a HARNESS bug, not a node defect: the interlude copied
      node.log to node.boot1.log BEFORE delivering the TERM, so the
      shutdown lines never landed in the copy the new gate greps. The
      preserved lastfail log showed the node shutting down perfectly
      ("Saving block index flat file (3204761 entries)... 525MB (5s)",
      "fast restart state persisted", clean exit 0) — the flat including
      the P2P topup past the seed floor, which also proves the FAIL#7
      flat read has its data when the stop is graceful. Fix: the copy
      now runs AFTER the stop completes.
      Ninth rerun (2026-08-02): the canary reached a fully healthy end
      state — IMPORT COMPLETE, fold at live tip, bg COMPLETE — then could
      never prove it. Three probe-layer defects, all in how the harness
      SAMPLES truth, none in the node:

      (a) Convergence was structurally impossible. The tip-convergence wait
      compared `getblockcount` on both sides, but the c23 public tip is
      finalize-lagged — it reports exactly one block below the coins tip
      persistently (a block is only exposed after the next block's finalize).
      Against a still-mining oracle, node_getblockcount >= zd_getblockcount can
      never hold. Live evidence: node getblockcount=3202299 vs zd=3202300 for
      60+s while gettxoutsetinfo on BOTH reported height=3202300 with identical
      bestblock — the node WAS at tip; the probe was lying. Fix: both sides now
      sample `gettxoutsetinfo` height (the coins tip, == reducer_frontier H*),
      which is commitment-cached at ~1 s/call, so polling is cheap.

      (b) Probe binding. The verdict's crossnode/exact tiers compare TX (node
      gettxoutsetinfo), ZD (oracle), and UC (node commitment), but the old probe
      order (SD,DIAG,UC,TX,ZD,LEG) spread them seconds apart — a fresh 36-137 s
      oracle block could land between UC and TX/ZD and false-FAIL the height
      binds. Fix: TX→ZD→UC→LEG are sampled back-to-back in a retry loop until
      all three heights agree (the ~3 s window fits inside any single block
      gap); SD/DIAG are diagnostic-only and taken after.

      (c) The LEG verb was stale by design. `--legacy-utxo-commitment` snapshotted
      the oracle chainstate with ldb_snapshot_make, which copies only compacted
      .ldb SSTs and drops the WAL — measured live: snapshot max_height=3183455 /
      vouts=1345257 vs the daemon's own gettxoutsetinfo height=3202300 /
      txouts=1345637. leg_best could never equal tx_best, so the byte-exact tier
      SKIPped every run — the exact byte-diff only ever ran against the
      in-process fixture, which is precisely reviewer criticism #1. Fix: the verb
      now uses the same WAL-inclusive signature-proven stable copy as the
      shielded importer (utxo_recovery_copy_chainstate_stable), shared by
      --gen-utxo-snapshot too since both stream a live zd.
      Tenth rerun (2026-08-02): **VERDICT=PASS from=anchor tip=3202314
      verified=3202314 elapsed=378s** — and the byte-exact tier ran
      against the LIVE oracle for the first time: exact_tier=match, UTXO
      SHA3 34fddecce7aa…8850590a identical over the zclassicd chainstate
      and the c23 coins set at best block
      00000bb67bc2ad55df785fe1542478136cd983fee3c3fc4772b71993fd9c7a90,
      txouts=1345651 and supply=10412321.61252558 identical on both
      sides, consensus_rejects=0, bg_state=complete. Sentinel:
      ~/.local/state/zclassic23-canary/replay_canary_anchor.json. The C8
      gate is now the accumulating byte-exact replay the reviewer asked
      for — the `--from=genesis` track extends the same exact tier over
      full history.

**Standing method (never skip):** copy-prove on a fixture before live; NEVER
delete `tip_finalize_log` rows; NEVER lower the public tip below `coins_best`;
NEVER ship a consensus-adjacent change without a copy proof.

**MRS scoreboard:** see **[`docs/MVP.md`](../MVP.md)** (scoreboard of record).
Re-run `tools/mvp_gate.sh` for the current MRS — do not trust a pinned number
here (`do not bump without proof`, `../HANDOFF.md` §4).

---

## Critical path — AUTONOMOUS / OWNER-GATED / OPERATIONAL

Ordering principle: **win Q1/C3 on a COPY → diagnose canonical state without
mutating it → owner-approved recovery/deploy if required → Q4/C6/C8 disruption,
soak, and parity evidence → Wave-N hardening → transactional hot swap and
sandboxed publishing.** Refactor debt must not jump the queue.

### A. AUTONOMOUS (do now — no live mutation, no owner gate)
- [ ] **Promote slice-gates to full ✅ gates** — replace #3/#5/#7's slice tests
      with full-scope tests (real sync / real shielded buy / full-binary
      restart-to-peer-tip) and add net-new CI jobs for #1 (clean-container
      install) and #8 (parity). Only then does the CI-verified MRS move.
      C1's portability floor is enforced WITHOUT docker (docker is never used
      in this project) by the hermetic `make ci-symbol-floor`
      (`tools/scripts/ci_symbol_floor_gate.sh`, in `make ci`): max
      GLIBC/GLIBCXX/CXXABI symbol ≤ the documented triple floor. The
      clean-OS install gate is a planned linger-service install proof
      (`make ci-install-linger`) exercising the real `make install` +
      `systemctl --user start`.
      Best next coverage multiplier: a high-throughput deterministic simulation
      harness inside `build/bin/test_zcl` / `test_parallel` (not an operator
      binary) that drives reducer stages, fake peers, fake clocks, temp
      datadirs, and invariant checks by seed, then prints the 64-bit replay seed
      for every failure. Use it to turn current slice tests into broad scenario
      sweeps while keeping full-binary/live-network proofs owner-gated.
- [ ] **Cleanup** — comment STRIP/REWORD pass + doc-pointer fixes; gate with
      `make lint && make test-parallel`.

### B. OWNER-GATED (consensus-critical; explicit owner go + repro-on-copy)
- [ ] **Coins-commitment-persist keystone** — write the 76-byte anchored
      `utxo_sha3` record inside `coins_view_sqlite_batch_write_ex`'s txn
      (`lib/storage/src/coins_view_sqlite.c`), table-derived height/count,
      + `_save_anchored`/`_load_anchor` in `lib/coins/src/utxo_commitment.{c,h}`,
      + re-validating heal in `coins_reconcile_stale_anchor`.
      **Do NOT apply live without owner go.**
- [ ] Persist `utxo_sha3` at finalized-tip so the self-heal has a fresh input.
- [ ] **Reducer shielded-consensus enforcement** — anchor membership + ZIP-209
      turnstile remain unbuilt (nullifier double-spend gate is already live in
      `app/jobs/src/utxo_apply_nullifiers.c`). Design of record
      [`reducer-shielded-consensus-plan.md`](./reducer-shielded-consensus-plan.md)
      — DESIGN-only; a refinement round closing its §8 gaps is required before
      any code. Owner-gated + copy-prove.
- [ ] **Deferred consensus hazards** in
      [`concurrency-hazards-consensus-gated.md`](./concurrency-hazards-consensus-gated.md)
      (owner-gated + repro-on-copy; item 1 = a real bg_validation lock-free
      `chain_active` UAF, same class as the fixed phashBlock bug).
- [ ] MVP feature e2e proofs: C4 (receive shielded) + C5 (store sell) on a
      funded test wallet.

### C. OPERATIONAL (network/config, not code; proves C3/C6/C7)
- [ ] **Prove C3 cold-sync end-to-end between zcl23 nodes** — use the immutable
      serving fixture named in `../HANDOFF.md`; remaining = the exact Q1 timed
      proof to a fresh peer.
- [ ] **Resolve canonical peer-floor state (owner-gated)** — diagnose
      read-only; do not restart or mutate the lane. Do not lower the ≥3 floor.
- [ ] **Run the 7-day soak (C6)** — start/restart evidence only after the
      candidate is healthy (`../HANDOFF.md` §0-LATEST); require gap ≤1, exact
      same-height hash, complete security posture, continuous evidence, RSS
      plateau, and zero manual restarts for the full 168h — measure against
      [`../USER_BENCHMARKS.md`](../USER_BENCHMARKS.md) /
      [`../BENCHMARKS_LOG.md`](../BENCHMARKS_LOG.md). Judge with
      `make soak-evidence-report`; re-read live accrual with
      `tools/mvp_gate.sh`, do not trust a pinned percentage here.
- [ ] **Full-binary kill-9 (C7)** — extend `make chaos`
      ([`../CHAOS_HARNESS.md`](../CHAOS_HARNESS.md)) from the SQLite-atomicity
      slice to restart-to-peer-tip (opt-in `make test-two-node-peer-tip` already
      proves it; remaining = hermetic-CI promotion). Operator coverage:
      [`../RUNBOOK.md`](../RUNBOOK.md).

**Gating summary:** Q1/C3's fresh-machine-to-tip proof is first (see #1 item
1). Q4/C6/C8 evidence follows only on a healthy sovereign candidate; canonical
recovery/deploy remains owner-gated. CI promotion (A) gates honest measurement;
the boot refactor gates nothing v1.

---

## Off the v1 path (reference — do NOT start until v1 buckets clear)

Architecture axis (~90% done): [`../FRAMEWORK.md`](../FRAMEWORK.md) §9 (the
open-item debt board). The only remaining size debt is the three
`config/` boot files (`boot.c`, `boot_services.c`, `boot_index.c`), frozen
shrink-only by the size gate.
Safe-execution method for any consensus-critical change: [`fast-path.md`](./fast-path.md).
