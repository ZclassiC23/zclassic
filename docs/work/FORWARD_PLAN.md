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

## ⛔ #1 PRIORITY — the live forward-progress blocker (CONFIRMED 2026-06-04)

> **Read [`working-mvp-strategy.md`](./working-mvp-strategy.md) — the decision
> doc.** Two prior diagnoses here were WRONG: neither the tip_finalize
> window-extender oscillation NOR "drive the reducer from genesis / coins-lag."
> The `C applied coins tip : 5` that drove the genesis framing is
> `diagnose_gap.sh:50` reading a **dead legacy marker**
> (`node_state['cec.coins_best_block_height']`), NOT the reducer cursor.

**Code+live-confirmed:** the **reducer IS the live tip authority and is at the
tip** (cursors `utxo_apply=tip_finalize=3134304`; 637 finalized rows). The wedge
is a **single-frontier-window solution-source desync** (h=3134304..~3134311): the
forward validator reads the Equihash solution from `node.db blocks.solution`
(no row at the frontier) and the recheck path hits `active_chain_at(h)→NULL` one
above the tip, while the valid solution sits unused in `progress.kv
header_solution_repair`. A **dishonest self-healer**
(`stale_validate_headers_repair.c:96-115`) then loops forever — witness returns
`ok` when the poison rows vanish, never when the tip moves, so it rewinds 7
cursors + deletes ~1129 rows every ~5s (`cleared_count=9421`+ live) and never
pages. The exact Law-7 violation: a remedy lying about success, hiding the halt.

**Strategic call (3 adversarial reviewers concur): SHIP ON THE REDUCER.** Don't
revert to legacy (offline-reindex-only). Don't replay from genesis (phantom).
The fix is **collapse the two header-solution stores into one frontier-covering
resolver + make the self-healer's witness honest** — hours, not days, on a
datadir COPY. Next workflows W1..W5 in [`working-mvp-strategy.md`](./working-mvp-strategy.md).

**Window-extender swap — done, correct, but NOT the live fix.** The contiguous
`reducer_extend_window_contiguous` swap (`active_chain_extend_window_have_data`
bounded by the utxo_apply cursor — note the bound is the cursor, **not**
`cursor-1`, which would starve utxo_apply) is implemented, unit-proven (asserts
the exact `tip_finalize:312` pprev-contiguity predicate), code-reviewed, and
**copy-proof-confirmed to do no harm** (pre-change baseline regressed
identically → it's a light-copy clamp, not the change). It is preserved on branch
**`wip/wedge-contiguous-extender`**, held off main because its efficacy can only
be validated once the reducer is driven forward (the live state — reducer at
C=5 — cannot reproduce the oscillation it fixes). Land it as part of the
reducer-drive work, copy-proved with `--full`.

**Method (never skip):** `make diagnose-gap` FIRST (the live triple A/H/C/D);
design + adversarial critique; reset-safe unit test; `repro-on-copy` (use
`--full` for anything that needs `blocks/` — the light copy clamps the tip to
~3,132,299 regardless, so its PASS only means "no NEW regression"). NEVER delete
`tip_finalize_log` rows; NEVER lower the public tip below `coins_best`; NEVER ship
a consensus-adjacent fix without a copy proof.

**The real reducer-drive blocker (owner-gated — consensus authority):** why is the
reducer at C=5, and what drives it forward to the tip? This is the single-engine
cutover (theme #1) + the coins-commitment-persist keystone
([`coins-commitment-persist-plan.md`](./coins-commitment-persist-plan.md)) +
the reducer shielded-consensus enforcement
([`reducer-shielded-consensus-plan.md`](./reducer-shielded-consensus-plan.md)).
Recovery FSM: [`service-state-machine.md`](./service-state-machine.md).

---

## Honest MRS scoreboard (supersedes any stale ✅ in MVP.md)

**Full criteria met: ~2 / 8 (manual). CI-verified full criteria: 0 / 8.**
What landed (S1): `make ci` now runs three **hermetic slice-gates** (◐) that
block the build — #3 sync-FSM, #5 store-proxy, #7 kill-9 SQLite-atomicity —
each FOCUSED via `ZCL_TEST_ONLY` and false-green-guarded. They protect a *slice*
of each criterion but do not prove the full operator claim, so they are ◐, not
✅; the MRS is unchanged. CI? column below: `slice ◐` = hermetic gate green for
a slice; `no` = not gated.

| # | Criterion | Honest status | CI? | Note |
|---|-----------|--------------|-----|------|
| 1 | Single-binary install (clean Ubuntu) | met-manual | no | no clean-container install + `systemctl` CI job |
| 2 | Tor onion bootstrap <60s | met-manual | no | onion live, but <60s timing not measured; non-hermetic → `make ci-stress` only |
| 3 | Cold-start sync to tip <10 min | partial | slice ◐ | sync-FSM gate in `make ci`; real 3M-block sync unproven; node wedged |
| 4 | Receive shielded payment e2e | partial | no | gate exists but opt-in + needs `~/.zcash-params` → `make ci-stress` only |
| 5 | List + sell file via store | partial | slice ◐ | in-process store-proxy gate in `make ci`; real shielded-buy+file-transfer unproven |
| 6 | 7-day soak, zero intervention | **regressing** | no | no soak harness; node wedged — the opposite of soak |
| 7 | Recover from kill -9 <2 min | met-manual | slice ◐ | SQLite-atomicity gate in `make ci`; full-binary restart-to-peer-tip unproven |
| 8 | Consensus parity w/ zclassicd | partial | slice ◐ | diff service **now EXISTS** (`app/services/src/utxo_parity_service.c`, wired `config/src/boot_utxo_parity.c`, ships dormant); hermetic `mvp-parity-slice` gate (`test_parity_slice.c`) in `make ci-mvp-gates` proves the MATCH/DRIFT machinery; full claim still needs the live oracle over the soak window |

---

## Critical path — AUTONOMOUS / OWNER-GATED / OPERATIONAL

Ordering principle: **make the node hold tip → make v1 measurable in CI → prove
features → soak.** Refactor debt does not block a working sovereign node and
must not jump the queue.

### A. AUTONOMOUS (do now — no live mutation, no owner gate)
- [ ] **Fix the wedge** — wire the have-data window extender (see #1 above);
      copy-prove that `reorg_detected_total` stops climbing and the finalized
      tip advances monotonically past the held height. ~55 LOC, HIGH risk.
- [x] **Make criterion tests real CI gates (hermetic slices)** — DONE (S1):
      `make ci` chains `ci-mvp-gates` (#3 sync-FSM, #5 store-proxy, #7 kill-9,
      + `chain_advance_atomicity`), each FOCUSED via `ZCL_TEST_ONLY` and
      false-green-guarded; non-hermetic #2/#4 routed to `make ci-stress` +
      opt-in `mvp-stress` job. These are ◐ slice-gates, not full ✅.
- [ ] **Promote slice-gates to full ✅ gates** — replace #3/#5/#7's slice tests
      with full-scope tests (real sync / real shielded buy / full-binary
      restart-to-peer-tip) and add net-new CI jobs for #1 (clean-container
      install) and #8 (parity). Only then does the CI-verified MRS move.
- [x] **Scope + build the consensus-parity-diff service (C8)** — DONE: the
      standing service exists at `app/services/src/utxo_parity_service.c`
      (wired at boot via `config/src/boot_utxo_parity.c`), ships dormant, and
      diffs the reducer UTXO set against a reference source
      (`utxo_reference_source_{fixture,zclassicd}.c`), emitting drift on
      mismatch. The hermetic `mvp-parity-slice` gate (`test_parity_slice.c`,
      in `make ci-mvp-gates`) proves the MATCH/DRIFT detection machinery with a
      negative control. REMAINING (bucket C): run it against the live oracle
      (zclassicd RPC 8232) over the soak window to move ◐ → ✅.
- [x] **Regtest on-demand mining `generate N` (gates #6 soak + #7 kill-9 teeth)**
      — SOLVED 2026-06-06 (`bcd44e68e`, datadir mismatch). `generate N` works
      end-to-end; reducer-mined blocks survive both a clean restart (`801832692`)
      and a real `kill -9` (`4e7fc176f`), and a follower catches up to peer-tip
      after kill-9 (`f135abb5f`). `make test-crash-bootstrap` /
      `make test-two-node-peer-tip` pass; both are opt-in (spawn real nodes), so
      CI promotion is the only remaining step (tracked under #6/#7 in MVP.md).
- [ ] **Cleanup** — comment STRIP/REWORD pass + doc-pointer fixes; gate with
      `make lint && make test_parallel`.

### B. OWNER-GATED (consensus-critical; explicit owner go + repro-on-copy)
- [ ] **Coins-commitment-persist keystone** — write the 76-byte anchored
      `utxo_sha3` record inside `coins_view_sqlite_batch_write_ex`'s txn
      (`lib/storage/src/coins_view_sqlite.c`), table-derived height/count,
      + `_save_anchored`/`_load_anchor` in `lib/coins/src/utxo_commitment.{c,h}`,
      + re-validating heal in `coins_reconcile_stale_anchor`. Design-of-record
      [`coins-commitment-persist-plan.md`](./coins-commitment-persist-plan.md)
      (adversary-vetted; original verdict DO_NOT_APPLY → corrected design at
      top). **Do NOT apply live without owner go.**
- [ ] Persist `utxo_sha3` at finalized-tip so the self-heal has a fresh input.
- [ ] Deploy the wedge fix (A) live only after a clean copy proof.
- [ ] After the wedge clears, apply the deferred consensus hazards in
      [`concurrency-hazards-consensus-gated.md`](./concurrency-hazards-consensus-gated.md)
      (item 1 = a real bg_validation lock-free `chain_active` UAF, same class
      as the fixed phashBlock bug).
- [ ] MVP feature e2e proofs once the tip holds: C4 (receive shielded) + C5
      (store sell) on a funded test wallet.

### C. OPERATIONAL (network/config, not code; proves C3/C6/C7)
- [ ] **Stand up a second `zcl23` serving node** — C3 cold-sync (FlyClient +
      SHA3 snapshot) is unprovable end-to-end with 0 `zcl23` peers serving the
      snapshot protocol. No code fixes this; a second node must exist.
- [ ] **Restore peer health above the floor of 3** (deliberate policy — do not
      lower it) — supply working `-addnode=` peers with group diversity.
- [ ] **Fix the crash-looping `zclassicd-rhett` oracle** (RPC 8232 unreachable
      → C8 parity has no reference). Investigate its logs; per doctrine do NOT
      stop `zclassicd`.
- [ ] **Run the 7-day soak (C6)** once the tip finalizes: live node + synthetic
      load, RSS plateau, zero manual restarts — measure against
      [`../USER_BENCHMARKS.md`](../USER_BENCHMARKS.md) /
      [`../BENCHMARKS_LOG.md`](../BENCHMARKS_LOG.md).
- [ ] **Full-binary kill-9 (C7)** — extend `make chaos`
      ([`../CHAOS_HARNESS.md`](../CHAOS_HARNESS.md)) from the SQLite-atomicity
      slice to restart-to-peer-tip. Operator coverage: [`../RUNBOOK.md`](../RUNBOOK.md).

**Gating summary:** A.wedge gates C6/C8 and the feature proofs. A.CI-wiring
gates honest measurement of everything. C8 needs the parity service built (A) +
the oracle up (C). C3 needs a second node (C). Boot refactor gates nothing v1.

---

## Off the v1 path (reference — do NOT start until v1 buckets clear)

Architecture axis (~90% done): [`../REFACTOR_STATUS.md`](../REFACTOR_STATUS.md),
[`../FRAMEWORK.md`](../FRAMEWORK.md). The only remaining size debt is the three
`config/` boot files (`boot.c` 3618, `boot_services.c` 3517, `boot_index.c`
1539), frozen shrink-only by the size gate; seam plan in
[`boot-decomposition-seams.md`](./boot-decomposition-seams.md). Engineering
quality detail: [`FINISH_CHECKLIST.md`](./FINISH_CHECKLIST.md). Safe-execution
method for any consensus-critical change: [`fast-path.md`](./fast-path.md).
