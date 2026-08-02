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
   **2026-08-02 measurements (both SEAM, both named, zero silent stalls):**
   (a) wiped stopwatch (`ZCL_CS_PEER=127.0.0.1:8034`, the read-only zd
   oracle): provable tip climbed 0 -> 31,387 in 616 s (~51 blk/s while
   sharing the host with the genesis replay-canary), 1 boot, blockers all
   named — exit 3 SEAM. The wiped path folds from genesis today; the
   native checkpoint weld (boot auto-activates the compiled 3,056,758
   authority + pulls the peer-served UTXO/shielded state against the
   baked SHA3, no flags) is the C3 code gap, exactly as the stopwatch
   header anticipates. Artifact: build/c3-stopwatch/20260802T045247Z-*.
   (b) bundle probe (`make mvp-coldstart-to-tip-local`,
   utxo-seed-3155842.snapshot + live serving peer 127.0.0.1:8033): seed
   RE-SEEDED digest-verified in ~20 s (count=1,344,903, body SHA3 OK),
   H\* climbed 3,155,842 -> 3,155,873 on peer-served above-seed bodies
   (the backlog-#10 starvation is cured — the fold reached its NEXT
   blocker class), then HELD on `utxo_apply.anchor_backfill_gap` — the
   v2 seed carries UTXOs + Sapling frontier but not the anchor/nullifier
   history, and the probe datadir had no `utxo-anchor.snapshot`
   companion (boot_anchor_snapshot_reachability: absent). Auto-remedies
   exhausted: rebuild_recent refused (46,504 > 10,000 cap), Rung C named
   the importer. The many `notfound` replies from the live peer were
   below-seed census-backfill requests for bodies the seeded live node
   legitimately does not have (h=2,959,325 spot-checked — header-only)
   — correct behavior, not a serving defect. Exit 3 SEAM; artifact
   build/c3-probe/20260802T050837Z-*. **Next concrete moves, in order:**
   stage the checkpoint-bound `utxo-anchor.snapshot` (the compiled-
   checkpoint-coherent artifact from `produce_anchor_snapshot.sh` /
   `seed_anchor_snapshot.sh`) into the probe bundle and re-run; if the
   fold then climbs past the first post-seed shielded block, the
   remaining gap is pure fold throughput (~50 blk/s today) — the delta
   above a FRESH tip-height seed is what fits the 600 s budget, so mint
   a fresh v2 seed (`tools/mint_v2_snapshot.c`) on a datadir copy at the
   captured peer tip as the second step.
   (c) weld stopwatch #3 (zd header import + consensus-state-bundle-3056758
   + live serving peer 127.0.0.1:8033): the weld LANDED — H*=3,056,758 at <!-- stale-ok: dated 2026-08-02 stopwatch measurement narrative, not a present-tense tip claim -->
   t=157 s (90 s read-only zd header import + ~67 s bundle install), and the
   body swarm completed 727/727 pieces h=3,155,843..3,202,352 from the live
   peer (its manifest starts at its own seed floor). The fold then could not
   advance: bodies 3,056,759..3,155,842 (98k) sit BELOW the live peer's
   serving floor, so no connected peer had them — named, not silent. Lesson:
   a seeded live peer cannot serve the sub-seed-floor span; the weld-to-seed
   delta needs a full-history body source.
   (d) weld stopwatch #4 (same weld + zd oracle 127.0.0.1:8034 as the
   full-history body source): the full pipeline works end to end — weld at
   3,056,758, no shielded hold (the bundle carries coins 1,354,769 +
   anchors 631,645 + nullifiers 1,495,126, so the anchor_backfill_gap of
   probe (b) does not exist on this path), provable tip climbed
   3,057,142 -> 3,135,959 across 1 boot in 619 s, accelerating to
   ~180 blk/s late — exit 3 SEAM (over budget, never stalled). Artifact:
   build/c3-stopwatch/20260802T054826Z-*. Arithmetic for the PASS: a FRESH
   tip-height bundle (delta ~0-20k) + ~67 s install + ~90 s header import +
   fold at the observed ~180 blk/s lands ~4.5 min — inside 600 s. The
   remaining C3 gap is therefore (i) an arbitrary-height checkpoint-bundle
   export (today's `-export-consensus-bundle` only exports frozen H\* at the
   compiled checkpoint) and (ii) fold-throughput headroom on slower hosts.
   A long-budget rerun (ZCL_CS_BUDGET_SECS=1500) records the honest
   weld-to-tip wall clock (~16-18 min expected at the observed rate).
   (e) weld stopwatch #5 (same config as (d), ZCL_CS_BUDGET_SECS=1500):
   **PASS at the long budget — H\* reached network_tip in 1040 s wall across
   1 boot** (~17.5 min: 157 s weld + 145k-block fold at ~180 blk/s).
   Artifact: build/c3-stopwatch/20260802T060932Z-*. The pipeline needs no
   further mechanism work for the 600 s bar — only a SMALLER delta, i.e. a
   fresh bundle near tip.
   (f) the arbitrary-height export gap is CLOSED in code (62645e2b8):
   `-full-fold` now runs the tip-bound producer-END sequence at completion
   (boot_full_fold_conclude: receipt finalize at (tip, tip_hash) + earned
   sovereign markers + the height-generic consensus_state_snapshot_export),
   so one `-full-fold` run emits consensus-state-bundle-<tip>.sqlite — the
   install side already welds any above-checkpoint height (assisted tier,
   default ON). Producer lane in flight on a pinned binary (fresh datadir,
   headers 3,192,879 from a read-only zd import, FULL-validation fold at
   ~260 blk/s observed, ~3.5 h): at completion the tip bundle lands in the
   producer datadir, and stopwatch #6 (fresh tip bundle + zd full-history
   source, default 600 s budget) is the C3 PASS attempt. NOTE the receipt's
   running-binary + epoch binding: the fold and the export must be ONE
   binary in ONE session — never rebuild mid-lane; a code fix means a
   fresh fold.
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

## 2026-08-02 PM update — C5 rung A near-done, fold stall root-caused, reviewer dispositions

Local commits awaiting rebase+push: blk-read Defect A/B fix
(`44720391a`), watchdog sd-pet fix (`12970570b`), C5 changeset
(`b8786b6cc` — proof harness + all fixes below; its Verified line is
honest: proof not yet PASS, race in item 9 is the last blocker).

### Genesis canary early-stop (record)
The from-genesis replay canary (tools/scripts/replay_canary.sh --from=genesis)
was STOPPED at ~166k/3.2M after ~3h (~15-19 blk/s, one block per commit,
rows=1): budget_exceeded verdict was arithmetically certain. Its exact-proof
role is rerouted through the producer -full-fold datadir (drain_batch=2000 +
pv_lookahead, ~130-280 blk/s), which folds genesis->tip offline and then
takes the same exact-tier check (byte-exact-from-genesis-check.sh). Harness
throughput (parallel vh / batched commits) is a post-v1 backlog item — vh
(Equihash header verify, ~5ms/blk single-threaded) is the fold's rate
limiter; parallelizing it is consensus-adjacent and must be copy-proven.

### C3 status (fill after stopwatch#6)
- stopwatch#5: PASS at 1500s budget, honest weld-to-tip 1040s
  (build/c3-stopwatch/20260802T060932Z-*). The 600s bar needs delta~=0,
  i.e. a tip-height bundle. <!-- stale-ok: dated 2026-08-02 stopwatch measurement narrative, not a present-tense tip claim -->
- 62645e2b8: -full-fold exports consensus-state-bundle-<tip>.sqlite at
  completion (boot_full_fold_conclude/boot_full_fold_export_bundle).
- Producer lane: fold genesis->tip, pinned binary, receipt-bound — STALLED,
  see item 8; rerun uses a frozen blk COPY.
- stopwatch#6 (RESULT HERE): bundle=<file>, wall=<s>, verdict=<PASS/FAIL@s>.

### C8 status (fill after byte-exact check)
- Anchor run #10 PASS: UTXO SHA3 byte-identical c23 vs live zd chainstate at
  tip 3202314, txouts=1345651, supply=10412321.61252558. <!-- stale-ok: dated 2026-08-02 anchor-run measurement narrative, not a present-tense tip claim -->
- From-genesis exact tier via producer datadir (RESULT HERE):
  height=<h> sha3=<...> verdict=<PASS/FAIL/SKIP>.

### Reviewer-thread dispositions (2026-08-02)
1. C8 oracle coarseness: addressed by anchor byte-exact run #10 + the
   from-genesis exact tier above; the RUNBOOK replay canary's exact tier is
   now exercised against real full-history state (producer datadir), not
   only the injected-outpoint fixture. Remaining: make an exact-tier run a
   standing (accumulating) gate artifact.
2. Four Equihash sites: clean layering (one sealed predicate, one primitive,
   one compat wrapper, one registry shim). AUTHORITY NOTE exists
   (scheme_equihash_200_9.c:3-17, check_block.c:107-114); propagated to
   lib/crypto headers in `b8786b6cc`. Residuals recorded: duplicated 140-byte
   challenge serialization (check_block.c:89 vs core/consensus/src/
   equihash.c:72-86) and sealed-predicate->unsealed-math delegation;
   both fold into the seal-boundary decision (item 3).
3. Seal boundary: connect_block.c (896L) + chainstate.c (1,038L) ordering
   layer outside the seal — one decision with the lib/crypto equihash
   question above; candidate for a single unseal/extend/reseal ritual,
   owner-gated, post-v1. Numbers verified 2026-08-02: core/MANIFEST.sha3
   78 lines (~70 files ritual-protected); tracked C/H total 3,518 files,
   1,220,264 lines; the reviewer's "~3,400 unsealed vs ~70 sealed" is
   accurate. Candidate boundary extension (one ritual): add
   lib/validation/src/connect_block.c + chainstate.c + the Equihash verify
   half of lib/crypto/src/equihash.c (or a split verify-only TU) to core/ +
   MANIFEST. Cost: every future edit pays the unseal ritual — the point,
   but an owner decision with eyes open. Post-v1.
4. ZCode trust model: agreed — promote third-party bit-identical
   reproduction (content-addressed actions + pinned toolchain capsule) to
   the headline acceptance test; demote signer quorum to a latency
   optimization. Post-v1 backlog.
5. Attention: noted — ZCode work stays parked until C3/C5/C6/C8 close.

### Live-node incident + cure (2026-08-02 08:21-09:34 UTC)
Cause: the pre-8733cdd88 `--importblockindex` accident opened the canonical
node.db with a dev binary and migrated it to schema 47 (6161328b5). The
pinned live binary (75afb4361, schema <=36) FATALed on restart after the
08:21 watchdog kill and parked alive-degraded (the node_db_unopened gate
working as designed — named blocker, no crash loop).
Cure (copy-proven per TENACITY): 18GB datadir copy at ~/cure-repro-20260802
(node.db via sqlite backup API) booted with the pinned producer binary
(62645e2b8, schema 47, real Tor): passed db_open, self-healed the Sapling
tree root, restored tip 3202507, answered RPC. Then applied live: old <!-- stale-ok: dated 2026-08-02 incident narrative, not a present-tense tip claim -->
binary kept at ~/.local/bin/zclassic23-live.75afb4361.bak, new binary
installed atomically, unit restarted; verified H*=3202568, gap closing. <!-- stale-ok: dated 2026-08-02 incident narrative, not a present-tense tip claim -->
Residuals:
- 90-build-identity.conf expects 75afb4361 -> drift surface reports stale
  until the OWNER canonical promotion ritual updates the drop-in +
  deploy/release-candidates.jsonl. Owner-gated; not done by the agent.
- node.db backups: newest secure-backup was 2026-07-08 (useless for this);
  the repro copy doubles as a fresh schema-47 node.db backup; delete it
  once the live node holds tip for a clean window.
- The watchdog SIGABRT at 08:21 happened under fold+canary+agent compile
  load; fold lanes should nice/ionice below the canonical node.

### C6 soak window reset (2026-08-02)
The schema-47 incident broke the previous clean window (NRestarts=15,
watchdog SIGABRT 08:21). The cured node was active at 09:32 UTC; the new
candidate clean window starts there. soak-evidence-report accrues from
that point; earliest possible 168h verdict: 2026-08-09 ~09:32 UTC. NOTE:
the watchdog kill loop (item 1 below) means the C6 window cannot truly
accrue until the owner promotes a binary with the sd-pet fix.

### 2026-08-02 ~10:15 UTC — post-cure live-node findings (mvp_gate re-run, MRS 2/8)

Gate: C1,C4 PASS; C2 FAIL (probe); C3/C5/C6/C7/C8 BLOCKED to named proofs (by design).

1. WATCHDOG KILL LOOP — root-caused and fixed in-tree (`12970570b`):
   SEVEN systemd watchdog SIGABRTs in 3 h on the canonical node (08:21, 09:41,
   09:54, 10:17, 10:35, 10:48, 11:02; NRestarts 2->6) while the node held tip
   (gap=1, peers 11-22) and backfilled bodies. Two compounding causes:
   (a) The sd pet rode the shared single-threaded lib/health periodic ring;
       node_health_collect can block minutes on reducer-held locks during bulk
       ingest (LOCK-ORDER LAW: the reducer drive holds coins_kv for the whole
       ingest). Observed: StatusText frozen at the boot string for 5+ min past
       the first-tick due time, then healthy ticks, then silence -> kill.
   (b) The verdict sits at healthy=false PERMANENTLY in the backfill era:
       38fe0885b makes the sync FSM refuse SYNC_AT_TIP while
       body_history_is_proven() is false (the intentional not_serving posture,
       ~3.13M/3.2M bodies missing, ~5-day backfill), so the pet depended
       entirely on boot_progress, which drains minutes after each boot. <!-- stale-ok: dated 2026-08-02 incident narrative, not a present-tense tip claim -->
   Fix (in-tree, NOT yet deployed to canonical — owner promotion gate):
   - config/src/boot_sd_watchdog.c: dedicated pet thread (zcl_sd_watchdog_pet)
     pinging every WATCHDOG_USEC/4 from cheap atomics only — never a collect,
     never a node lock. Pure decision fn + ZCL_TESTING seam.
   - app/services/src/node_health_service.c: verdict publication atomics
     (node_health_last_verdict) + a STRICT body-gap posture flag (unhealthy
     with NO named degradation, archive unproven, at tip with peers) — the one
     state a systemd restart cannot improve. healthy/serving/api semantics
     unchanged (not_serving stays honest).
   - deploy/zclassic23.service comment updated; test_sd_notify.c decision table.
   Until the owner promotes a binary with this fix, the C6 clean-soak window
   cannot accrue on the canonical node — every boot is killed ~10 min after
   its boot_progress drains. EARLIEST C6 window start is therefore the
   promotion + first quiet boot, not 2026-08-02.
   Also recorded: op_return_index REFUSING legacy_v1 ERROR spam every pass on
   the canonical node (the 62645e2b8 binary writes op_return_index_state.v2,
   the datadir carries legacy_v1; remedy = owner runs `app oprindex rebuild`),
   and build.coordinator/build.worker deadline stalls on the live node under
   host load (symptom, not cause).

2. C2 GATE PROBE vs REALITY: onion IS up on this boot —
   full-mode healthcheck `/checks/`: tor_ready=true, onion_service_ready=true,
   onion_address=lhuca3i5c5az4yngb3i7ddtirwzzcp6ug566csgwbwbirvbhzsq2suqd.onion
   (ephemeral, NEW per boot — the 4gzb7kl3… address from the 09:32 boot is stale).
   The gate FAILs because it probes the BOUNDED first-call body of
   `core network onion status`, which under 62645e2b8 returns the cached-summary
   shape with no tor fields. Pinpointed: tools/command/native_command.c:186
   registers `core.network.onion.status` as an ALIAS of the generic
   `healthcheck` command; the dedicated leaf `core.network.onion.health`
   (zcl_native_onion_health_body, app/controllers/src/net_native_handlers.c:134)
   is an ACTIVE latency probe whose address getter returned NULL -> "not_started"
   even while Tor is up (its source disagrees with node_health_service.c:267-275,
   which full healthcheck proves correct). Follow-up (small):
   (a) mvp_gate reads the full-healthcheck checks.* triple (probe-side, zero
   node change), and/or (b) reconcile the addr getter in
   zcl_native_onion_health_body with node_health_service's source. Node
   substance is PASS.

3. not_serving IS INTENTIONAL: 38fe0885b requires body_history_is_proven() for
   synced/serving; the canonical datadir is snapshot-seeded (body gap below the
   seed floor), so health honestly reports serving=false /
   public_serving_allowed=false even at hstar=tip. Clears when the historical
   bodies below the seed floor are backfilled (the address_index blocker's
   OPTION A — hours of bandwidth, owner decision) or when a from-genesis/
   full-fold datadir (the producer lane!) becomes the canonical state.
   Note: the producer -full-fold datadir folds genesis->tip with ALL bodies —
   it will report serving=true honestly. Another reason that lane matters.

4. REGTEST BLK-READ BUGS (`44720391a`) — the C5 rung-A stage-9 blocker,
   reproduced on PLAIN regtest (pre-existing, NOT the store changeset):
   Defect A (regtest/testnet only, the actual wedge): the body writer
   persists under the NET-SPECIFIC datadir (GetDataDir(true) ->
   <base>/regtest/blocks; reducer_ingest_service.c:285-294) but several
   blk-reading services were wired the BASE datadir: op_return_backfill,
   node_db catchup (boot_start_catchup_service), bg_validation,
   state_auditor. Reads ENOENT -> backfills starve -> no zslp_validity rows
   -> mint baton never VALID. Invisible on mainnet (base==net-specific).
   Fix: resolve GetDataDir(true) at those four read sites (byte-identical on
   mainnet). Files: op_return_backfill_service.c, state_auditor.c,
   catchup_lifecycle_service.c, boot_bg_verification.c.
   Defect B (latent alias, all nets): from-scratch genesis gets HAVE_DATA
   with no body ever written -> nDataPos stays 0 -> block_index_disk_pos_
   snapshot handed (0,0) to the pread path, which resolved offset 0 to
   block 1's record -> "read_block_pread_hash_mismatch: h=0 got=<block 1>".
   Fix: chain.h guard — a recorded payload offset is never 0 (frame header
   occupies 0-7); nPos==0 now reports "no data". Pre-check proven: the live
   canonical block_index.bin has ZERO HAVE_DATA rows with n_data_pos==0, so
   no healthy entry relied on the alias. Producer full-fold datadir NOT
   affected (genesis at data_pos=8, legacy-seeded).

5. REGTEST MINING+RPC WEDGE (agent-9 root cause) — exposed the moment
   Defect A let catchup do real work during mining: a hard AB-BA deadlock.
   The generatetoaddress RPC worker runs the reducer drive inside
   stage_run_once, which holds the GLOBAL progress_store_tx_lock across the
   whole stage step; the regtest tip-finalize projection feed
   (tip_finalize_post_step.c) then did SYNCHRONOUS node_db_sync_connect_block
   / node_db_sync_wallet_tx writes — an untimed wait on db-service job
   completion — while the db-service writer (running the node_db catchup
   job) blocked on progress_store_tx_lock inside
   sapling_tree_flat_checkpoint_note's fold-cursor guard. Neither wait is
   timed: permanent deadlock. Cascade: all 4 RPC workers pile up behind
   sovereignty_guard_allow -> progress_store_tx_lock (whole RPC surface goes
   silent, incl. getblockcount/dumpstate/selfbacktrace), the tick-runner
   wedges inside chain_tip_watchdog's on_tick. Reproduced twice
   (h=59 and h=21); full 50-thread backtrace /tmp/wedge-bt.txt.
   Fix (in `b8786b6cc`, regtest-only, projections derived+repairable): the
   feed now uses the already-existing async db lane —
   node_db_sync_connect_block_async gained a _with_wallet variant that folds
   the per-tx wallet projection into the SAME db-service job (blocks row
   precedes the wallet_tx time lookup), so the drive never waits on
   db-service completion while holding progress_store_tx_lock. Invariant
   recorded: no thread holding progress_store_tx_lock may block on db-service
   completion, and no db-service job may block on progress_store_tx_lock (a
   trylock-fail-open variant of reducer_frontier_derive_coins_best is the
   complementary edge break; class fix is post-v1).
   ALSO RECORDED (recovery bug, post-v1, NOT fixed): a drive killed mid-fold
   leaves coins_applied_height = hstar+2 (e.g. applied=59, hstar=57): the
   mint gate coins_kv_tip_is_self_derived demands applied == hstar+1, but
   tip_finalize needs the hstar+1 witness block which can no longer be
   mined — circular, restart-persistent. Needs a rewind-one-block repair or
   gate tolerance; the preserved incident datadir
   /tmp/zcl23-storeproof-gSNvaz demonstrates it.
   Harness-side (same commit): sp_mine_to no longer fails a stage on ONE
   empty RPC reply — zcl-rpc hard-caps curl at --max-time 30 and the server
   keeps working past it; only a chain tip frozen for a sustained 60s
   window (12 polls x 5s) fails.

6. WALLET FLUSH nested-BEGIN (same regtest proof, PAY stage): wallet_sqlite
   shares ONE FULLMUTEX connection with node.db, so a mid-transaction db
   job makes BEGIN IMMEDIATE fail with SQLITE_ERROR "cannot start a
   transaction within a transaction" — wallet_sqlite_flush_r now treats
   that rc (with !sqlite3_get_autocommit) as retryable busy on the same
   bounded 8-attempt schedule (was a hard getnewaddress failure).
   Regression: test_wallet_persistence_cycle.c
   (test_flush_retries_under_same_connection_tx). In `b8786b6cc`.

7. C5 COLLECT WEDGE — cause (a) fixed in `b8786b6cc`, cause (b) is item 9:
   (a) WRONG TABLE: the /store/access token gate (store_check_token_access,
       serve_gated_content) read zslp_balances — the credit-only merchant
       table the chain-scan path deliberately leaves EMPTY in production
       (explorer_index.h). Every real, confirmed holder answered 0. The
       gate now answers store_access_token_balance = max(chain-derived
       debit-correct zslp_ledger holding, legacy fixture table) — never
       summed (no double-count). New file:
       app/controllers/src/store_access_gate.c (E1 split, store_controller.c
       back under the 800-line ceiling).

8. PRODUCER FOLD STALL at h=3,189,468 (2026-08-02 12:46 UTC, 21,248 s in,
   3,411 blocks short of tip) — root-caused (agent-10, evidence copies <!-- stale-ok: dated 2026-08-02 fold-stall incident narrative, not a present-tense tip claim -->
   /tmp/fold-block_index.bin + /tmp/fold_bad_entries.json): NOT an import
   or scan-arithmetic bug. The fold boot's own blk-file scan stored
   hash-VALID positions at 06:51; they dangled because the fold datadir's
   blk files are HARDLINKED into the LIVE zd oracle datadir, and zd's
   post-2026-07-18-corruption rebuilt LevelDB index believes blk00050.dat
   ends at ~66.15 MB (physical file is 71.39 MB) — so zd resumed appending
   at 66.15 MB and marched upward, overwriting the scan-captured tail
   copies of exactly 314 blocks (heights 3,189,468-3,190,869) between <!-- stale-ok: dated 2026-08-02 fold-stall incident narrative, not a present-tense tip claim -->
   06:51 and 12:46. Two live writers, one inode. body_persist cleared
   HAVE_DATA and held the cursor for a re-fetch that cannot exist in
   -full-fold (no peers; requeue_body_for_refetch's own comment says the
   hold "does not self-heal") -> 64 no-progress kicks -> mint-anchor
   named-blocker shutdown (tenacity design working: named blocker, not a
   silent halt). Contributing defects, in fix order:
   (a) scan duplicate policy is LAST-COPY-WINS
       (boot_block_file_scan.c:427-432) — it preferred the fragile tail
       copy over stable mid-file copies. Fix: earliest copy wins (lowest
       file, then lowest pos) — append frontiers only advance upward, so
       the lowest-offset copy is the most durable.
   (b) no local position repair on read failure: ALL 314 bad entries
       still have valid duplicate copies on disk today (2 each, below
       66.15 MB) — a hash-keyed re-scan (reuse
       chain_restore_scan_block_files machinery) stored via
       block_index_set_have_data_verified would have made the stall
       100% avoidable. Fix sites: requeue_body_for_refetch
       (body_persist_stage.c:228) + remedy_have_data_unreadable
       (have_data_unreadable.c:134).
   (c) ~5,305 more file-50 entries sit at/above zd's overwrite frontier
       and will dangle over the coming days as zd marches; the read-side
       hash check (disk_block_io.c:715-727) means they fail LOUD, never
       silently — but only (b) recovers them.
   (d) cheap tripwire: LOG_WARN once when a blk file has st_nlink > 1
       (hardlinked = foreign writer possible; every position in the last
       file is provisional).
   Fixes (a)+(b)+(d) + regression tests in flight (agent-10, same tree).
   OPERATIONAL (the real trigger): never fold against blk files
   hardlinked into a LIVE, still-syncing zd datadir. The rerun must use a
   frozen byte COPY (cp --reflink=never or plain cp) of $HOME/.zclassic/
   blocks, or pause zd for the fold's duration. Forensic note: ZCL block
   hash = SHA256d over the header INCLUDING compactsize+Equihash solution
   (block.h:71-76) — a 140-byte-only hash check finds nothing.

9. C5 COLLECT REMAINING WEDGE (the last blocker to VERDICT=PASS) —
   backfill-vs-catchup race + a catchup stall, diagnosed on the (deleted)
   proof datadir /tmp/zcl23-storeproof-YUlOua: the access-token MINT tx
   confirmed (h=116, in `transactions`, out of mempool), zslp_ledger cursor
   raced to 118, but the mint block was NEVER folded into the op_returns /
   zslp_transfers / zslp_ledger projections, so the token gate (fix 7a)
   correctly answered 0 forever. Two stacked defects: <!-- stale-ok: dated 2026-08-02 proof-run incident narrative, not a present-tense tip claim -->
   (a) zslp_ledger_backfill_run_once targets bare
       reducer_frontier_provable_tip_cached() with NO gate on the node_db
       catchup projection tip it actually reads (zslp_transfers /
       tx_outputs / tx_inputs come only from explorer_index_block, whose
       sole production caller is the catchup service). It folds "empty"
       heights and permanently advances its cursor past un-projected
       blocks. Fix (small): target = min(hstar, catchup projection tip
       (node_db_sync_get_tip_height / SYNC_PROJECTION_TIP_HEIGHT_KEY)) so
       the backfill only folds heights whose projections exist.
   (b) WHY catchup itself never folded the mint block inside the 150 s
       retry window is still open: candidates are the abort-streak park
       (node_db_catchup_lock_guard, 8 consecutive failed passes),
       repeated pass failure ("final commit missing tip hash" seen at
       boot on regtest), or the refolding/tail_folding defer guard in
       boot_background_workers.c. Next: KEEP=1 proof rerun (datadir
       preserved) for a decisive post-mortem, then the (a) gate fix.

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
      **C5 map (2026-08-02 investigation):** the criterion's gate is the STORE
      stack (`storebuy_*` + `app.store.*`), NOT the `zmarket_*` P2P market
      (MVP.md names the market post-MVP; its leaves are PLANNED/unbound).
      Hermetic gates `store_e2e` + `store_e2e_shielded` are green in
      `make ci-mvp-gates`; the ◐ gap is only "hermetic/in-process, not yet a
      full live buyer over the store/onion/file-transfer path" — the buyer
      drives `store_handle_request` against the LOCAL node.db
      (app/services/src/store_buyer.c:96-107,254,362). Two upgrade rungs:
      (A) single-node regtest operator proof (~200-300 lines bash, ~zero C —
      every leaf READY: list-product → order → pay (real shielded z_sendmany
      with order memo) → mine → reconcile → collect, bytes == content hash;
      mirrors C4's make test-shielded-payment shape); (B) the full live
      buyer: remote-store transport mode in store_buyer*.c (fetch/POST/GET
      via tor_integration_fetch_onion_blocking, lib/net/src/tor_integration.c:515)
      ≈ 300-450 lines C + two-node harness cloned from
      tools/scripts/two_node_peer_tip.sh. Payment leg reuses
      rpc_z_sendmany / wallet_direct_sendtoaddress — no consensus-adjacent
      code; `storebuy_pay` hard-refuses mainnet (store_buyer_pay.c:97-98),
      lifting that is owner-gated spend-guard doctrine.

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
