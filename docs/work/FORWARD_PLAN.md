# zclassic23 — V1 PLAN (MVP-anchored)

> **READ THIS FIRST.** This is THE finishing plan. The v1 bar is the 8
> acceptance criteria in **[`docs/MVP.md`](../MVP.md)** (v1 = MRS 8/8).
> Everything below is sequenced to move MRS toward 8/8.
>
> The framework/architecture refactor ([`docs/REFACTOR_STATUS.md`](../REFACTOR_STATUS.md),
> [`docs/FRAMEWORK.md`](../FRAMEWORK.md), boot decomposition) is **~90% done and
> OFF the v1 path. Do NOT jump the queue into it.** It is reference, not the
> mission.

---

## #1 PRIORITY — cure canonical shielded state, then earn clean evidence

> **Canonical is wedged now**, below tip, by incomplete historical shielded
> anchors and nullifiers (verify the live H\* via `zcl_status` / `dumpstate
> reducer_frontier`; `docs/HANDOFF.md` holds current state).
> `ab512d577` repaired an earlier transparent snapshot-loader failure; it did
> not prove or install complete shielded history. A snapshot payload digest and
> a matching validated anchor header authenticate the artifact and its chain
> location, but ZClassic headers do not commit UTXO, Sapling/Sprout, or
> nullifier roots. Those contents are therefore borrowed, not PoW-bound.
>
> Two complementary cure tracks are now in flight — current truth is
> [`../HANDOFF.md`](../HANDOFF.md), read it fresh before acting:
> **(A) operational import-path** — the complete-shielded-history import
> (`-import-complete-shielded`) is proven to clear this exact wedge on a
> throwaway datadir copy (`../HANDOFF.md` §0-NEWEST, 2026-07-16); it is
> release-assisted trust, not sovereign, and is NOT yet folded to the header
> tip, NOT yet pushed to `origin/main`, and NOT yet cut over on the live
> canonical datadir. **(B) sovereign producer/bundle** — a re-baked checkpoint
> fixed the root cause and a new producer is folding real blocks forward from
> it (`../HANDOFF.md` §0-NEW); production ACTIVATE stays contained
> (`VERIFIED_CONTAINED`) until an independent replay receipt lifts it. Track A
> reaches a verified tip fast; Track B later upgrades the same node to fully
> sovereign (`coins_kv_self_folded=true`). Do not feed a shielded v3 artifact
> through the current v1-oriented refold reset that can discard its shielded
> sections. Cure design details: [`self-verified-tip-plan.md`](./self-verified-tip-plan.md)
> (Track B) and [`fast-sync-to-tip-plan-2026-07-16.md`](./fast-sync-to-tip-plan-2026-07-16.md)
> (Track A). The durable Phase 0–6 hierarchy and promotion gates are in
> [`SOVEREIGN-NETWORK-ROADMAP.md`](./SOVEREIGN-NETWORK-ROADMAP.md).
> Copy-prove every recovery path on a datadir COPY before live; gate on H\* climb.

**The #1 work now:** finish folding Track A to the header tip and pass its full
copy-proof gate (or let Track B's producer complete and its bundle pass
independent replay verification), then copy-prove H\* climb on a canonical
datadir copy before any live cutover. CI-gate promotion may proceed in
parallel on isolated lanes, but canonical soak/canary/seal evidence does not
accrue while the node is wedged. Only after H\* climb, complete security
posture, and exact same-height parity are copy-proven may a fresh evidence
window begin. Root-cause on the preserved fixture, datadir COPY never live
([`fast-path.md`](./fast-path.md)); delete recovery ladders only after their
replacement and rollback are proven.

**Standing method (never skip):** copy-prove on a fixture before live; NEVER
delete `tip_finalize_log` rows; NEVER lower the public tip below `coins_best`;
NEVER ship a consensus-adjacent change without a copy proof.

**MRS scoreboard:** see **[`docs/MVP.md`](../MVP.md)** (scoreboard of record).
Current score is **4/8** by local operator proof; hermetic slices keep the
remaining criteria honest but do not promote them to ✅.

---

## Critical path — AUTONOMOUS / OWNER-GATED / OPERATIONAL

Ordering principle: **land the complete sovereign state cure → prove sustained
exact parity and liveness → make evidence commit-bound → finish native
transactional hot swap / zero-MCP → build the sandboxed App and publishing
planes.** Refactor debt does not block a working sovereign node and must not
jump the queue.

### A. AUTONOMOUS (do now — no live mutation, no owner gate)
- [x] **Criterion tests are real CI gates (hermetic slices)** — `make ci` chains
      `ci-mvp-gates` (#3 sync-FSM, #5 store-proxy, #7 kill-9 + `chain_advance_atomicity`),
      each FOCUSED via `ZCL_TEST_ONLY` and false-green-guarded; non-hermetic
      #2/#4 routed to `make ci-stress` + opt-in `mvp-stress`. These are ◐, not ✅.
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
- [x] **Consensus-parity-diff service (C8)** — exists at
      `app/services/src/utxo_parity_service.c` (wired at boot via
      `config/src/boot_utxo_parity.c`), default-ON when a zclassicd oracle
      resolves (since 2026-06-12), diffs the reducer UTXO set against a reference
      (`utxo_reference_source_{fixture,zclassicd}.c`), emitting drift on
      mismatch. Hermetic `mvp-parity-slice` gate (`test_parity_slice.c`, in
      `make ci-mvp-gates`) proves MATCH/DRIFT with a negative control.
      REMAINING ◐→✅: 0 mismatches over the 7-day soak + an exact byte reference.
- [x] **Regtest on-demand mining `generate N`** — FIXED 2026-06-17 (`f83101b81`).
      Root cause was NOT the "utxo_apply WALL" the first investigation guessed
      (refuted: `proof_validate` uses the same raw `active_chain_at` and passes at
      the same height; g_author defaults to UTXO_AUTHOR_STAGE so utxo_apply DOES
      extend the window). REAL cause: a FRESH genesis-only regtest node never seeds
      the genesis anchor (every `tip_finalize_stage_seed_anchor` caller is an
      import/snapshot/reindex path; the runtime re-seed can't reach genesis since
      `cursor < nHeight=0` is never true), so staged cursors stuck at 0 → the first
      block recorded no utxo_apply row (`utx=-1` → `block-not-finalized-by-reducer`).
      FIX = seed the genesis anchor once inside `reducer_ingest_block`,
      `fMineBlocksOnDemand`-gated (mainnet/testnet byte-identical) + only at an
      unseeded genesis tip. Copy-proven `generate 5` → tip 5, rejects=0; durable
      guard `test_reducer_ondemand_genesis_seed`; test_parallel 0/427, lint clean
      (E13 + `test_consensus_parity` green). **`make test-two-node-peer-tip` now
      PASSES** (A mines 10 → B syncs → kill-9 B → A +5 → B recovers to peer-tip 15
      via P2P re-sync). Both opt-in (spawn real nodes) → hermetic-CI promotion remains.
- [x] **Regtest kill-9 RESTART-DURABILITY (the C7 single-node blocker)** — FIXED
      2026-06-17 (`341020c05`, owner-gated, copy-proven). Root cause: on the kill-9
      fallback the block_index is rebuilt with publish_tip=false and coins-restore
      does not install, so active_chain_tip()==NULL; genesis_init then tries to
      promote genesis (h=0) which CSR correctly rejects as rollback_auth/genesis_init,
      stranding the node at `h=-1` while coins/block_index are durable at N. FIX:
      generalize `block_index_loader_seed_tip_from_finalized` with a genesis-root
      branch — walk pprev from the durable finalized tip to the CANONICAL genesis
      (every link HAVE_DATA+VALID_SCRIPTS, terminus = `params->consensus.hashGenesisBlock`),
      bounded by one `effective_floor` (=0) so a NULL-tip mainnet boot REFUSES
      (3.1M>50000), gated by a `coins_applied>=tip` precondition; + a defensive
      genesis_init skip in boot.c. 3-lens consensus review converged after closing
      two blockers (gap-cap-vs-floor, canonical-terminus). Proven: `make lint` clean
      (E13), `make test_parallel` 0/427 + 10 new `bil/seedfin` cases, own kill-9
      copy-prove (generate 5 → kill-9 → restart → getblockcount==5, `(root=genesis)`
      log), and **`make test-crash-bootstrap` PASSES (`height_regress: 0`)**.
      **Live deploy owner-gated** (new binary on the live datadir = wipe+cold-import).
- [x] **C5 store gate → real ivk-decrypt purchase (Slice 1, additive+hermetic)** —
      landed as `store_e2e_shielded` and wired into `ci-mvp-gates` with the
      false-green sentinel. It leaves the old `store_e2e` gate intact, builds a
      params-free Sapling output to a merchant wallet, decrypts it through
      `wallet_try_sapling_decrypt`, persists the recovered note, proves
      `db_store_received_payment_for_memo` credits only the matching
      `ZCL23ORDER:<order_id>` memo, and proves the legacy address+amount finder
      over-counts a same-address wrong-order payment. Current focused proof:
      `ZCL_STRESS_TESTS=1 ZCL_TEST_ONLY=store_e2e_shielded build/bin/test_zcl`
      passes.
- [ ] **Cleanup** — comment STRIP/REWORD pass + doc-pointer fixes; gate with
      `make lint && make test_parallel`.
- [x] **Code-review remediation** (secondary hardening lane; must not displace
      the #1 spine) — the open, verified subset of the 2026-06-27 audit, batched
      into parallel-worktree lanes with a fix + proof-gate per item (tracked in
      the removed `archive/code-review-remediation-2026-06-30.md`, recoverable
      with `git log --follow -- docs/work/archive/code-review-remediation-2026-06-30.md`).
      Autonomous review-remediation items are now closed; continue MVP work from
      the sovereign cure and fresh soak window.

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
- [x] **C5 store gate Slice 2 (memo-bound reconcile + no fake address fallback)
      — LANDED** — the live `store_process_payments` reconcile calls the
      memo-bound finder `db_store_received_payment_for_memo`; the old
      amount/address finder has no app callers. `zslp_payment_generate_address`
      now refuses to create an order unless the merchant wallet has a seeded
      Sapling keystore, so it cannot bind to a synthetic or unrecoverable
      `zs1_pay_<time>` address. App-layer only; no consensus path touched.

### C. OPERATIONAL (network/config, not code; proves C3/C6/C7)
- [ ] **Prove C3 cold-sync end-to-end between zcl23 nodes** — a second zcl23 node
      EXISTS (`zclassic23-soak`, P2P 8043 / RPC 18242); remaining = prove the
      FlyClient + SHA3 snapshot SERVE path to a fresh peer.
- [x] **Peer floor restored** (2026-06-13) — 5 healthy peers / 5 groups; both
      units carry external addnodes (a localhost-only addnode set can NEVER
      converge a cold import). Do NOT lower the ≥3 floor.
- [x] **zclassicd oracle up** (2026-06-12) — RPC 8232 reachable; the C8 parity
      oracle runs against it continuously (read-only; per doctrine never stop
      `zclassicd`).
- [ ] **Run the 7-day soak (C6)** — NOT ACCRUING on canonical while its shielded
      history is incomplete. After cure/copy proof, rebaseline an exact release
      candidate and require gap ≤1, exact same-height hash, complete security
      posture, continuous evidence, RSS plateau, and zero manual restarts — measure against
      [`../USER_BENCHMARKS.md`](../USER_BENCHMARKS.md) /
      [`../BENCHMARKS_LOG.md`](../BENCHMARKS_LOG.md).
- [ ] **Full-binary kill-9 (C7)** — extend `make chaos`
      ([`../CHAOS_HARNESS.md`](../CHAOS_HARNESS.md)) from the SQLite-atomicity
      slice to restart-to-peer-tip (opt-in `make test-two-node-peer-tip` already
      proves it; remaining = hermetic-CI promotion). Operator coverage:
      [`../RUNBOOK.md`](../RUNBOOK.md).

**Gating summary:** C3/C6/C8 are blocked on the current canonical shielded-state
wedge. The next promotion is the complete atomic cure plus copy proof; only then
can a fresh exact-parity 168-hour window begin. CI promotion (A) gates honest
measurement; the boot refactor gates nothing v1.

---

## Off the v1 path (reference — do NOT start until v1 buckets clear)

Architecture axis (~90% done): [`../REFACTOR_STATUS.md`](../REFACTOR_STATUS.md),
[`../FRAMEWORK.md`](../FRAMEWORK.md). The only remaining size debt is the three
`config/` boot files (`boot.c`, `boot_services.c`, `boot_index.c`), frozen
shrink-only by the size gate; seam plan was in `boot-decomposition-seams.md`,
removed from the tree — recover with
`git log --follow -- docs/work/archive/boot-decomposition-seams.md`.
Safe-execution method for any consensus-critical change: [`fast-path.md`](./fast-path.md).
