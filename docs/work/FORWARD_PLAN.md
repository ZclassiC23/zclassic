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

> The recurring cold-import wedge was recovered 2026-06-16; the node is healthy
> at the chain tip and hardening + cleanup are merged to main. Cold-import
> bootstrap robustness is the remaining open item — program of record
> [`rock-solid-program-2026-06-16.md`](./rock-solid-program-2026-06-16.md) +
> [`stability-improvements-2026-06-16.md`](./stability-improvements-2026-06-16.md).
> Coin-tear fixture `~/.zclassic-c23-cointear-fixture-20260612` (KEEP).

**The #1 work now:** promote ◐ slice-gates to full ✅ CI gates and accumulate
soak/canary/seal evidence (first seal ratification expected at grid 3,146,000,
`zcl_state subsystem=seal`) — only ✅ moves the MRS; the **Critical path**
checklist below is the detail. Cold-import bootstrap hardening = a write-time
import correctness gate (REFUTED: float-H* (consensus-unsafe) + build-L2
(forbidden rung); delete the ladder LAST); root-cause on the preserved
fixture, datadir COPY never live ([`fast-path.md`](./fast-path.md)).

**Standing method (never skip):** copy-prove on a fixture before live; NEVER
delete `tip_finalize_log` rows; NEVER lower the public tip below `coins_best`;
NEVER ship a consensus-adjacent change without a copy proof.

**MRS scoreboard:** see **[`docs/MVP.md`](../MVP.md)** (scoreboard of record).
Summary: ~2/8 met by hand, 0/8 CI-verified full.

---

## Critical path — AUTONOMOUS / OWNER-GATED / OPERATIONAL

Ordering principle: **the node holds tip → make v1 measurable in CI → prove
features → soak.** Refactor debt does not block a working sovereign node and
must not jump the queue.

### A. AUTONOMOUS (do now — no live mutation, no owner gate)
- [x] **Criterion tests are real CI gates (hermetic slices)** — `make ci` chains
      `ci-mvp-gates` (#3 sync-FSM, #5 store-proxy, #7 kill-9 + `chain_advance_atomicity`),
      each FOCUSED via `ZCL_TEST_ONLY` and false-green-guarded; non-hermetic
      #2/#4 routed to `make ci-stress` + opt-in `mvp-stress`. These are ◐, not ✅.
- [ ] **Promote slice-gates to full ✅ gates** — replace #3/#5/#7's slice tests
      with full-scope tests (real sync / real shielded buy / full-binary
      restart-to-peer-tip) and add net-new CI jobs for #1 (clean-container
      install) and #8 (parity). Only then does the CI-verified MRS move.
- [x] **Consensus-parity-diff service (C8)** — exists at
      `app/services/src/utxo_parity_service.c` (wired at boot via
      `config/src/boot_utxo_parity.c`), default-ON when a zclassicd oracle
      resolves (since 2026-06-12), diffs the reducer UTXO set against a reference
      (`utxo_reference_source_{fixture,zclassicd}.c`), emitting drift on
      mismatch. Hermetic `mvp-parity-slice` gate (`test_parity_slice.c`, in
      `make ci-mvp-gates`) proves MATCH/DRIFT with a negative control.
      REMAINING ◐→✅: 0 mismatches over the 7-day soak + an exact byte reference.
- [~] **Regtest on-demand mining `generate N`** — PARTIAL. The in-process
      reducer mining engine is green (`make mvp-it-works`: real regtest block,
      tip 0→1, finalized by `tip_finalize`). But the **full-binary `generate`-RPC
      path does not advance a spawned node's tip** (verified 2026-06-17 via the
      new `make mvp-verify`): a fresh regtest node boots correctly to genesis,
      yet `generate N` leaves tip=0, so **`make test-two-node-peer-tip` FAILS**
      and `make test-crash-bootstrap` soft-passes as "KNOWN BLOCKED (owner-gated
      reducer boot-init)". The earlier "works end-to-end / both pass" claim
      (`801832692`/`4e7fc176f`/`f135abb5f`) is STALE for the full-binary path.
      **Exact signature** (isolated regtest node, `generate 5`, 2026-06-17):
      `[ondemand-reject] h=1: val=1 bod=1 bod=1 scr=1 pro=1 utx=-1 tip=-1` →
      `REJECTED by reducer: block-not-finalized-by-reducer`; `generate` returns
      `[]`, tip stays 0 for 30s. The mined block clears validate/body/script/proof
      but **`utxo_apply` fails (`utx=-1`)** so `tip_finalize` never fires. This is
      the documented "WALL" (`utxo_apply` resolves a successor-less self-mined
      block via the raw finalized-window accessor → NULL above the finalized tip;
      only `validate_headers` has the `vh_resolve_bi` above-window fallback) — and
      it is a **REGRESSION**: [[project_mvp_regtest_mining_rootcause_2026-06-05]]
      records it SOLVED 2026-06-06 (`bcd44e68e`, "generate 3 → getblockcount 0→3,
      rejects=0"). **Next concrete C7/C3/C6 target:** give `utxo_apply` (and the
      body/script/proof stages) the above-window resolver `validate_headers` uses,
      OR hold the active-chain window across the on-demand drain — `fMineBlocksOnDemand`-
      gated so mainnet/testnet stay byte-identical (network-safe by construction);
      copy-prove before any deploy. Run `make mvp-verify` for current per-member status.
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
- [ ] **Run the 7-day soak (C6)** — IN FLIGHT on `zclassic23-soak`: live node +
      load, RSS plateau, zero manual restarts — measure against
      [`../USER_BENCHMARKS.md`](../USER_BENCHMARKS.md) /
      [`../BENCHMARKS_LOG.md`](../BENCHMARKS_LOG.md).
- [ ] **Full-binary kill-9 (C7)** — extend `make chaos`
      ([`../CHAOS_HARNESS.md`](../CHAOS_HARNESS.md)) from the SQLite-atomicity
      slice to restart-to-peer-tip (opt-in `make test-two-node-peer-tip` already
      proves it; remaining = hermetic-CI promotion). Operator coverage:
      [`../RUNBOOK.md`](../RUNBOOK.md).

**Gating summary:** the node holds tip (recovered 2026-06-16) → C3/C6/C8 are
unblocked for live forward progress; CI promotion (A) gates honest measurement;
the boot refactor gates nothing v1.

---

## Off the v1 path (reference — do NOT start until v1 buckets clear)

Architecture axis (~90% done): [`../REFACTOR_STATUS.md`](../REFACTOR_STATUS.md),
[`../FRAMEWORK.md`](../FRAMEWORK.md). The only remaining size debt is the three
`config/` boot files (`boot.c`, `boot_services.c`, `boot_index.c`), frozen
shrink-only by the size gate; seam plan in
[`boot-decomposition-seams.md`](./boot-decomposition-seams.md). Engineering
quality detail: [`FINISH_CHECKLIST.md`](./FINISH_CHECKLIST.md). Safe-execution
method for any consensus-critical change: [`fast-path.md`](./fast-path.md).