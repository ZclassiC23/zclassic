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
      Verify: next anchor-canary run should COMPLETE inside the band with
      crossnode_height green.

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
