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

## #1 PRIORITY — CI-enforce the MVP criteria + accumulate soak/canary/seal evidence

> **The live forward-progress wedge CLEARED 2026-06-12.** The node holds tip
> AND finalizes forward — blocks publish at the arrival tick, restarts keep
> the connected extent, hash-identical to zclassicd at every probed height.
> The 2026-06-12 live coin tear (UTXO set 13-extra/≥1-missing at 3,145,366)
> was CURED 2026-06-13 by wipe + the two-step cold import on the merged
> wave-4 binary: healthy:true at tip, gap 0, hash probes 6/6 MATCH, set
> parity EXACT vs zclassicd, 5 healthy peers / 5 groups, zero blockers.
> Tear root cause still OPEN — fixture
> `~/.zclassic-c23-cointear-fixture-20260612` (KEEP). Current state of
> record: [`../HANDOFF.md`](../HANDOFF.md). Wedge-era decision history:
> [`working-mvp-strategy.md`](./working-mvp-strategy.md) (historical).

**The #1 work now** ([`../MVP.md`](../MVP.md) is the scoreboard of record —
this file defers to it):
1. **Promote ◐ slice-gates to full ✅ CI gates** — #3 real sync, #5 real
   shielded buy, #7 full-binary restart-to-peer-tip, plus net-new jobs for
   #1 (clean-container install) and #8 (parity). Only ✅ moves the MRS.
2. **Accumulate evidence windows** — MVP-C6 soak accrues on the dedicated
   `zclassic23-soak` node (pinned binary, RPC 18242); the C8 parity oracle
   is ACTIVE vs zclassicd (default-ON when an oracle resolves); replay-canary
   nightly/weekly timers install after the first live green run; first seal
   ratification expected at grid 3,146,000 (`zcl_state subsystem=seal`).
3. **Root-cause the coin tear** on the preserved fixture — datadir COPY,
   never live ([`fast-path.md`](./fast-path.md)).

**Standing method (never skip):** copy-prove on a fixture before live; NEVER
delete `tip_finalize_log` rows; NEVER lower the public tip below `coins_best`;
NEVER ship a consensus-adjacent change without a copy proof.

---

## Honest MRS scoreboard (snapshot — [`docs/MVP.md`](../MVP.md) is the scoreboard of record; defer to it on any conflict)

**Full criteria met: ~2 / 8 (manual). CI-verified full criteria: 0 / 8.**
What landed (S1): `make ci` now runs three **hermetic slice-gates** (◐) that
block the build (since grown to five — see [`../MVP.md`](../MVP.md)) — #3
sync-FSM, #5 store-proxy, #7 kill-9 SQLite-atomicity —
each FOCUSED via `ZCL_TEST_ONLY` and false-green-guarded. They protect a *slice*
of each criterion but do not prove the full operator claim, so they are ◐, not
✅; the MRS is unchanged. CI? column below: `slice ◐` = hermetic gate green for
a slice; `no` = not gated.

| # | Criterion | Honest status | CI? | Note |
|---|-----------|--------------|-----|------|
| 1 | Single-binary install (clean Ubuntu) | met-manual | no | no clean-container install + `systemctl` CI job |
| 2 | Tor onion bootstrap <60s | met-manual | no | onion live, but <60s timing not measured; non-hermetic → `make ci-stress` only |
| 3 | Cold-start sync to tip <10 min | partial | slice ◐ | sync-FSM gate in `make ci`; the real two-step cold import is hand-proven (2026-06-13 live redeploy + soak bootstrap), not CI-gated — and the <10 min bound is not yet met (proven imports take ~25+ min) |
| 4 | Receive shielded payment e2e | partial | no | gate exists but opt-in + needs `~/.zcash-params` → `make ci-stress` only |
| 5 | List + sell file via store | partial | slice ◐ | in-process store-proxy gate in `make ci`; real shielded-buy+file-transfer unproven |
| 6 | 7-day soak, zero intervention | accruing | no | wedge CLEARED 2026-06-12; dedicated `zclassic23-soak` node accruing the 168h window; `make soak-ci` bounded proxy + replay canary exist; full 7-day claim unmet |
| 7 | Recover from kill -9 <2 min | met-manual | slice ◐ | SQLite-atomicity gate in `make ci`; full-binary restart-to-peer-tip proven by opt-in `make test-two-node-peer-tip` (2026-06-06); hermetic-CI promotion pending |
| 8 | Consensus parity w/ zclassicd | partial | slice ◐ | diff service EXISTS and is ACTIVE live (`app/services/src/utxo_parity_service.c`, wired `config/src/boot_utxo_parity.c`; default-ON when a zclassicd oracle resolves, 2026-06-12; opt-out `ZCL_PARITY_ORACLE=0`); hermetic `mvp-parity-slice` gate (`test_parity_slice.c`) in `make ci-mvp-gates` proves the MATCH/DRIFT machinery; full claim needs 0 mismatches over the 7-day soak window + an exact byte reference (live reference is coarse/height-only) |

---

## Critical path — AUTONOMOUS / OWNER-GATED / OPERATIONAL

Ordering principle: **the node holds tip (done 2026-06-12) → make v1 measurable
in CI → prove features → soak.** Refactor debt does not block a working
sovereign node and must not jump the queue.

### A. AUTONOMOUS (do now — no live mutation, no owner gate)
- [x] **Fix the wedge** — DONE: the live wedge CLEARED 2026-06-12 (tip_finalize
      +1-convention unification + the cold-import lattice fixes; see
      [`../HANDOFF.md`](../HANDOFF.md)). The node holds tip and finalizes
      forward; restarts keep the connected extent. The window-extender
      framing in earlier revisions of this doc was superseded.
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
      (wired at boot via `config/src/boot_utxo_parity.c`), shipped dormant
      (default-ON when a zclassicd oracle resolves since 2026-06-12), and
      diffs the reducer UTXO set against a reference source
      (`utxo_reference_source_{fixture,zclassicd}.c`), emitting drift on
      mismatch. The hermetic `mvp-parity-slice` gate (`test_parity_slice.c`,
      in `make ci-mvp-gates`) proves the MATCH/DRIFT detection machinery with a
      negative control. REMAINING: the oracle is ACTIVE live (default-ON
      since 2026-06-12); ◐ → ✅ needs 0 mismatches over the 7-day soak window
      + an exact byte reference.
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
- [ ] **Reducer shielded-consensus enforcement** — the nullifier double-spend
      gate landed (`app/jobs/src/utxo_apply_nullifiers.c`, C-3); REMAINING on
      the forward path = anchor membership + ZIP-209 turnstile (design of
      record
      [`reducer-shielded-consensus-plan.md`](./reducer-shielded-consensus-plan.md)
      — DESIGN-only, all 3 reviewers returned `consensus_safe=false`, a
      refinement round is required before any code). Unblocked by the
      2026-06-12 wedge clear; owner-gated + copy-prove.
- [ ] The wedge HAS cleared (2026-06-12) — the deferred consensus hazards in
      [`concurrency-hazards-consensus-gated.md`](./concurrency-hazards-consensus-gated.md)
      are now unblocked (still owner-gated + repro-on-copy; item 1 = a real
      bg_validation lock-free `chain_active` UAF, same class as the fixed
      phashBlock bug).
- [ ] MVP feature e2e proofs (the tip holds as of 2026-06-12): C4 (receive
      shielded) + C5 (store sell) on a funded test wallet.

### C. OPERATIONAL (network/config, not code; proves C3/C6/C7)
- [ ] **Prove C3 cold-sync end-to-end between zcl23 nodes** — a second zcl23
      node now EXISTS (`zclassic23-soak`, P2P 8043 / RPC 18242, stood up
      2026-06-12); remaining = prove the FlyClient + SHA3 snapshot SERVE path
      to a fresh peer.
- [x] **Peer floor restored** (2026-06-13) — live node at 5 healthy peers /
      5 groups; both units carry external addnodes (a localhost-only addnode
      set can NEVER converge a cold import). Do NOT lower the ≥3 floor.
- [x] **zclassicd oracle up** (2026-06-12) — RPC 8232 reachable; the C8 parity
      oracle runs against it continuously (read-only; per doctrine never stop
      `zclassicd`).
- [ ] **Run the 7-day soak (C6)** — IN FLIGHT since 2026-06-12 on
      `zclassic23-soak`: live node + load, RSS plateau, zero manual restarts —
      measure against [`../USER_BENCHMARKS.md`](../USER_BENCHMARKS.md) /
      [`../BENCHMARKS_LOG.md`](../BENCHMARKS_LOG.md).
- [ ] **Full-binary kill-9 (C7)** — extend `make chaos`
      ([`../CHAOS_HARNESS.md`](../CHAOS_HARNESS.md)) from the SQLite-atomicity
      slice to restart-to-peer-tip (the opt-in `make test-two-node-peer-tip`
      already proves it; remaining = hermetic-CI promotion). Operator
      coverage: [`../RUNBOOK.md`](../RUNBOOK.md).

**Gating summary:** the wedge is CLEARED (2026-06-12) — nothing v1 is gated on
it. CI promotion (A) gates honest measurement of everything. C6 is wall-clock
on the soak node. C8's oracle is up + active; it needs the soak window + an
exact reference. C3 needs the snapshot-serve proof against the second node.
Boot refactor gates nothing v1.

---

## Off the v1 path (reference — do NOT start until v1 buckets clear)

Architecture axis (~90% done): [`../REFACTOR_STATUS.md`](../REFACTOR_STATUS.md),
[`../FRAMEWORK.md`](../FRAMEWORK.md). The only remaining size debt is the three
`config/` boot files (`boot.c` 3618, `boot_services.c` 3517, `boot_index.c`
1539), frozen shrink-only by the size gate; seam plan in
[`boot-decomposition-seams.md`](./boot-decomposition-seams.md). Engineering
quality detail: [`FINISH_CHECKLIST.md`](./FINISH_CHECKLIST.md). Safe-execution
method for any consensus-critical change: [`fast-path.md`](./fast-path.md).
