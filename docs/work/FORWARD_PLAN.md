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

## #1 PRIORITY — hold the at-tip sovereign state through a clean soak, then canonical deploy

> **The sovereign shielded-state cure landed and passed the wedge.** The
> serve node is AT NETWORK TIP on self-verified state, past the historical
> shielded-anchor wedge this section used to track — read
> [`../HANDOFF.md`](../HANDOFF.md) §0-LATEST fresh before acting; this file
> carries no live height/soak numbers. Of the two cure tracks this section
> used to run in parallel, **the sovereign consensus-bundle install**
> (`docs/work/sovereign-cutover-runbook.md`) is the one that actually passed
> the wedge live; the operational import-path track
> ([`fast-sync-to-tip-plan-2026-07-16.md`](./fast-sync-to-tip-plan-2026-07-16.md))
> documents currently-shipped importer code but was not cut over. Cure design
> record (now PROVEN, not an open plan):
> [`self-verified-tip-plan.md`](./self-verified-tip-plan.md). The durable
> Phase 0–6 hierarchy and promotion gates remain
> [`SOVEREIGN-NETWORK-ROADMAP.md`](./SOVEREIGN-NETWORK-ROADMAP.md).

**The #1 work now, in order:**

1. **Soak (C6)** — hold the serve node at tip through a clean 168h window:
   zero manual restarts, gap ≤1, exact same-height hash vs `zclassicd` (C8),
   continuous evidence. `tools/mvp_gate.sh` reports live soak-accrual;
   `make soak-evidence-report` is the authoritative JSONL judge. Do not churn
   the serve datadir during the window — copy-prove any change on a fixture,
   never on the soaking node.
2. **Canonical deploy gate** — after a clean soak, cutting the canonical lane
   over to the proven serve binary/datadir is the owner's lever (`make
   deploy`, owner-gated, never automatic). Post-deploy cleanup named in
   `../HANDOFF.md` §0-LATEST: reclaim the pre-flip backup and old candidate
   datadirs once the deploy is confirmed durable.
3. **Fresh-machine-to-tip gap (C3)** — the at-tip proof so far is on the ONE
   node that ran the cure; C3's actual claim is a genuinely FRESH zclassic23
   node reaching tip from a serving zclassic23 peer in <10 min. Both halves of
   that path have landed on `main`: the **seed** side (`rom_seed`) serves
   content-verified chunks, and the **fetch** side (`lib/net/src/rom_fetch.c` +
   `app/controllers/src/rom_fetch_controller.c`, operator-invocable via
   `ops.debug.rom_fetch.bundle`) does multi-seeder verify-by-content download
   with durable resume (`rom_fetch_download_verified_parallel`). What remains
   for C3 is the end-to-end timed proof itself: sync a second, genuinely fresh
   zclassic23 node against the serve node (not `zclassicd`) and confirm the
   <10 min bar.
4. **Hardening backlog** — ranked by the Wave-N first-principles audit (4
   parallel read-only auditors covering integrity/trust/repair-consolidation/
   pipeline-simplification, memory
   `project_wave_n_first_principles_backlog_2026-07-19`, HEAD ~b2eb1393b):
   1. **[in-flight]** Blocks-table row repair: per-row skip+purge in
      `load_block_index_from_blocks_table` instead of whole-table refusal;
      quarantine via the existing unwired `db_block_delete`
      (`app/models/src/block.c`), drop `HAVE_DATA` → `body_fetch` refetch →
      revalidate; typed blocker. This is the §0-LATEST-named follow-up
      (poisoned solution-row quarantine/refetch).
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
   9. Mechanical: the `cac_`/`chain_advance_coordinator` →
      `block_source_policy` rename mostly landed (Wave N,
      `7855fcb57`/`47877391d`); `git grep 'cac_\|chain_advance_coordinator'`
      still surfaces ~13 files — verify each is a legitimate compatibility
      name (e.g. a stable `dumpstate` subsystem key) before treating the
      rename as fully closed.

**Standing method (never skip):** copy-prove on a fixture before live; NEVER
delete `tip_finalize_log` rows; NEVER lower the public tip below `coins_best`;
NEVER ship a consensus-adjacent change without a copy proof.

**MRS scoreboard:** see **[`docs/MVP.md`](../MVP.md)** (scoreboard of record).
Re-run `tools/mvp_gate.sh` for the current MRS — do not trust a pinned number
here (`do not bump without proof`, `../HANDOFF.md` §4).

---

## Critical path — AUTONOMOUS / OWNER-GATED / OPERATIONAL

Ordering principle: **the sovereign state cure is LANDED → prove sustained
exact parity and liveness through the soak window → canonical deploy → close
the fresh-machine-to-tip gap (C3) → work the Wave-N hardening backlog → finish
native transactional hot swap / native-command consolidation → build the
sandboxed App and publishing planes.** Refactor debt does not block a working
sovereign node and must not jump the queue.

### A. AUTONOMOUS (do now — no live mutation, no owner gate)
- [x] **Criterion tests are real CI gates (hermetic slices)** — DONE, guarded by
      `make ci` → `ci-mvp-gates` (hermetic #3/#5/#7 slices) + `ci-stress`/`mvp-stress`
      (non-hermetic #2/#4); ◐ not ✅ until promoted to full-scope (see next item).
- [ ] **Promote slice-gates to full ✅ gates** — replace #3/#5/#7's slice tests
      with full-scope tests (real sync / real shielded buy / full-binary
      restart-to-peer-tip) and add net-new CI jobs for #1 (clean-container
      install) and #8 (parity). Only then does the CI-verified MRS move.
      Regression-armor landed 2026-06-17 (raises the floor, NOT a ✅ promotion):
      C8 `test_parity_slice.c` now also covers the COARSE `exact=false` production
      branch the live `zclassicd` oracle hits (C1 match / C2 skip-on-skew / C3
      clears a stale exact drift); the C3 seed-authority proof
      (`mvp-coldstart-local`, preferring the `block_index.bin` +
      `utxo-seed-*.snapshot` operator bundle through
      `-load-snapshot-at-own-height`) is green locally, while the full C3
      bundle-to-peer-tip command is `make mvp-coldstart-to-tip-local`. The full
      Groth16 send+receive proof (C4 `test-shielded-payment`) is de-orphaned into
      `make mvp-verify`. C1's portability floor is enforced
      WITHOUT docker (docker is never used in this project) by the hermetic
      `make ci-symbol-floor` (`tools/scripts/ci_symbol_floor_gate.sh`, in
      `make ci`): max GLIBC/GLIBCXX/CXXABI symbol ≤ the documented triple floor.
      The retired docker-based clean-OS gate is replaced by a planned
      linger-service install proof (`make ci-install-linger`) exercising the real
      `make install` + `systemctl --user start`. Audit of record: the 8-criterion
      gap scoreboard (workflow `mvp8-gap-audit-and-close`, 2026-06-17).
      Best next coverage multiplier: a high-throughput deterministic simulation
      harness inside `build/bin/test_zcl` / `test_parallel` (not an operator
      binary) that drives reducer stages, fake peers, fake clocks, temp
      datadirs, and invariant checks by seed, then prints the 64-bit replay seed
      for every failure. Use it to turn current slice tests into broad scenario
      sweeps while keeping full-binary/live-network proofs owner-gated.
- [x] **Consensus-parity-diff service (C8)** — DONE
      (`app/services/src/utxo_parity_service.c`, wired via
      `config/src/boot_utxo_parity.c`, default-ON), guarded by `mvp-parity-slice`
      (`test_parity_slice.c`, in `make ci-mvp-gates`). REMAINING ◐→✅: 0
      mismatches over the 7-day soak + an exact byte reference.
- [x] **Regtest on-demand mining `generate N`** — DONE (`f83101b81`), guarded by
      `test_reducer_ondemand_genesis_seed` and `make test-two-node-peer-tip`
      (both opt-in, spawn real nodes — hermetic-CI promotion remains open).
- [x] **Regtest kill-9 RESTART-DURABILITY (the C7 single-node blocker)** — DONE
      (`341020c05`, owner-gated, copy-proven), guarded by `make lint` (E13),
      `test_parallel` (`bil`/`seedfin` cases), and `make test-crash-bootstrap`.
      Live deploy owner-gated (new binary on the live datadir = wipe+cold-import).
- [x] **C5 store gate → real ivk-decrypt purchase (Slice 1, additive+hermetic)** —
      DONE (`store_e2e_shielded`, wired into `ci-mvp-gates` alongside the old
      `store_e2e` gate), guarded by
      `ZCL_STRESS_TESTS=1 ZCL_TEST_ONLY=store_e2e_shielded build/bin/test_zcl`.
- [ ] **Cleanup** — comment STRIP/REWORD pass + doc-pointer fixes; gate with
      `make lint && make test_parallel`.
- [x] **Code-review remediation** (secondary hardening lane; must not displace
      the #1 spine) — DONE for the autonomous subset of the 2026-06-27 audit
      (archived: recover with
      `git log --follow -- docs/work/archive/code-review-remediation-2026-06-30.md`);
      continue MVP work from the sovereign cure and fresh soak window.

### B. OWNER-GATED (consensus-critical; explicit owner go + repro-on-copy)
> NOTE (2026-06-17): the C7 **restart-durability** blocker is now handled by the
> §A forward-seed keystone (`341020c05`). The coins-commitment-persist item below
> is a SEPARATE self-heal hardening (a durable SHA3 anchor for stale-coins
> reconciliation), no longer C7-blocking.
- [ ] **Coins-commitment-persist keystone** — write the 76-byte anchored
      `utxo_sha3` record inside `coins_view_sqlite_batch_write_ex`'s txn
      (`lib/storage/src/coins_view_sqlite.c`), table-derived height/count,
      + `_save_anchored`/`_load_anchor` in `lib/coins/src/utxo_commitment.{c,h}`,
      + re-validating heal in `coins_reconcile_stale_anchor`. Design-of-record
      `coins-commitment-persist-plan.md` (adversary-vetted; original verdict
      DO_NOT_APPLY → corrected design at top), removed from the tree —
      recover with `git log --follow -- docs/work/archive/coins-commitment-persist-plan.md`.
      **Do NOT apply live without owner go.**
- [ ] Persist `utxo_sha3` at finalized-tip so the self-heal has a fresh input.
- [ ] **Reducer shielded-consensus enforcement** — nullifier double-spend gate
      landed (`app/jobs/src/utxo_apply_nullifiers.c`, C-3); REMAINING = anchor
      membership + ZIP-209 turnstile (design of record
      [`reducer-shielded-consensus-plan.md`](./reducer-shielded-consensus-plan.md)
      — DESIGN-only, all 3 reviewers returned `consensus_safe=false`; a
      refinement round is required before any code). Owner-gated + copy-prove.
- [ ] **Deferred consensus hazards** in
      [`concurrency-hazards-consensus-gated.md`](./concurrency-hazards-consensus-gated.md)
      (owner-gated + repro-on-copy; item 1 = a real bg_validation lock-free
      `chain_active` UAF, same class as the fixed phashBlock bug).
- [ ] MVP feature e2e proofs: C4 (receive shielded) + C5 (store sell) on a
      funded test wallet.
- [x] **C5 store gate Slice 2 (memo-bound reconcile + no fake address fallback)**
      — DONE: `store_process_payments` reconciles via the memo-bound finder
      `db_store_received_payment_for_memo` (the old amount/address finder has no
      app callers); `zslp_payment_generate_address` refuses to create an order
      without a seeded Sapling keystore. App-layer only; no consensus path touched.

### C. OPERATIONAL (network/config, not code; proves C3/C6/C7)
- [ ] **Prove C3 cold-sync end-to-end between zcl23 nodes** — a second zcl23 node
      EXISTS (`zclassic23-soak`, P2P 8043 / RPC 18242); remaining = prove the
      FlyClient + SHA3 snapshot SERVE path to a fresh peer.
- [x] **Peer floor restored** — DONE: 5 healthy peers / 5 groups; both units
      carry external addnodes (a localhost-only addnode set can NEVER converge
      a cold import). Do NOT lower the ≥3 floor.
- [x] **zclassicd oracle up** — DONE: RPC 8232 reachable; the C8 parity oracle
      runs against it continuously (read-only; per doctrine never stop
      `zclassicd`).
- [ ] **Run the 7-day soak (C6)** — the cure landed and the window is running
      on the serve node (`../HANDOFF.md` §0-LATEST); require gap ≤1, exact
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

**Gating summary:** C6/C8's shielded-state-cure precondition is CLEARED — the
next promotion is a clean 168-hour soak window on the at-tip sovereign state,
then canonical deploy. C3's fresh-machine-to-tip proof is a separate,
still-open gap (see #1 PRIORITY item 3). CI promotion (A) gates honest
measurement; the boot refactor gates nothing v1.

---

## Off the v1 path (reference — do NOT start until v1 buckets clear)

Architecture axis (~90% done): [`../FRAMEWORK.md`](../FRAMEWORK.md) §9 (the
open-item debt board). The only remaining size debt is the three
`config/` boot files (`boot.c`, `boot_services.c`, `boot_index.c`), frozen
shrink-only by the size gate; seam plan was in `boot-decomposition-seams.md`,
removed from the tree — recover with
`git log --follow -- docs/work/archive/boot-decomposition-seams.md`.
Safe-execution method for any consensus-critical change: [`fast-path.md`](./fast-path.md).
