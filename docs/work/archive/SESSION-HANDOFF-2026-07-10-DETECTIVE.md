# Session handoff — Detective-node reliability wave (2026-07-10)

**Local `main` advanced from `3b0de63b0` → `82c128c62` (26 commits, NOT pushed
until the final gate confirms green — see "Push status").** This wave executed
the first slice of the "Super-Reliability / Detective Node" program: strengthen
the node's ability to determine the longest *valid* chain from independently
verified evidence, resist adversarial peers, scale the adversarial simulator,
and give the AI operator sharper telemetry + self-debug tooling. It **feeds the
v1 critical path** (sovereign cure → CI-enforce MVP → soak); it did not
queue-jump the sovereign cure.

The full program plan (three tracks A/B/C, waves, all items) is in the approved
plan the session worked from. This doc is the code-level handoff.

## What landed on main (all with hermetic test groups, `make lint` clean)

**Detective / evidence (Track A):**
- **A1 — rung-3 runtime refold-from-anchor** (`ecb22fccb`): the sticky-escalator
  ladder gained its missing terminal rung `STICKY_RUNG_REFOLD_FROM_ANCHOR`
  (`app/services/src/sticky_escalator.c`). Design = **arm + supervised
  self-respawn** (not a racy in-process live refold — that would fork a second
  write path): new `lib/storage/{include/storage,src}/boot_auto_refold.{h,c}`
  writes a bounded, fsync-durable one-shot request; `config/src/boot.c` consumes
  it at the `do_from_anchor` site and runs the SAME `boot_refold_from_anchor_reset`
  the boot path already uses. Gated on `coins_kv_contains_refold_marker()` +
  anchor artifact present; names a typed `BLOCKER_DEPENDENCY`
  (`sticky_escalator.refold_no_anchor_artifact`) when no artifact exists —
  never arms a refold the reset would FATAL-refuse. Attempts increment at
  consume(boot)-time → a FATAL-looping anchor is capped then boots normally
  (escalator pages), no crash-loop. Tests: `test_stall_totality_matrix` R1/R2.
- **A3 — row-ABSENT rowless-hole detector** (`5bbaf043d`): the S8 re-derivation
  invariant had three detectors that all required an *existing* row; a rowless
  hole strictly below the coins frontier was refused by refill and owned by
  nobody (the live 3166989 class). New `absent_script_hole_unlocked` detector in
  `app/jobs/src/stage_repair_reducer_frontier_coin.c` (sound `height < cursor`
  bound) routes to the SAME `stale_script_replay_tx` (no second repair path).
  Tests: `test_stall_totality_matrix` K7/K8/K9, gate on **H\* CLIMB** not "no FATAL".
- **A2 — P2P header-repair fallback** (`b93c94f16` + fix `943c9789a`):
  `stale_validate_headers_repair` no longer depends solely on the local zclassicd
  RPC oracle for backfilling a corrupted/solutionless header. It now tries the
  oracle first, then re-requests the canonical block from peers via the EXISTING
  getdata path (`sync_monitor_queue_active_frontier_body`); the arriving block
  re-validates through `check_block` before use (never swaps a page for an
  unverified one). Source attribution + per-source counters in the
  `header_probe` `zcl_state` dumper. `process_headers` now scores
  `PEER_OFFENCE_INVALID_HEADER` on a `dos>0` header (a forged header page was
  previously free). Test: `header_probe_p2p_fallback`.
  **NOTE (regression caught + fixed):** the new production scoring double-counted
  with a redundant `peer_scoring_record` in the wire byzantine *observer*
  (`lib/sim/src/simnet_wire_byzantine.c`), pushing a single forged header to the
  100-pt ban threshold and leaking ban state across test groups. Fix removed the
  harness's redundant score (production is the single source now). Lesson pinned
  in the "traps" below.
- **A4-small** (`24d1f4858`): documented the single-onion-seed SPOF honestly
  (`lib/chain/src/chainparams.c` — did NOT invent a fake second .onion; runtime
  operator seed file `~/.config/zclassic23/onion-seeds` documented in RUNBOOK)
  and VERIFIED (with file:line evidence) that the peer-floor remedy fires
  oracle-dead and that `blocker_supervisor_sweep`/`worker_on_stall` are already
  wired (sticky-plan item 10 done).

**Simulator + golden tests (Track B):**
- **B0 — simnet_cluster O(N²)→O(N log N)** (`66c07f9ab`): min-heap delivery
  scheduler replacing the per-pop linear rescan; **200× faster at N=100**
  (56.2s→0.28s) with **bit-identical delivery fingerprint** (proven two ways —
  the old order is preserved exactly, so all recorded seeds/capsules stay valid).
  Node caps raised: `CHAOS_SIMNET_MAX_NODES` 16→128, `FZ_MAX_NODES` 4→32
  (default per-seed draw unchanged so `make ci` load is flat). Opt-in N=100 perf
  smoke via `ZCL_SIMNET_PERF=1`. **This unblocks Track-B Wave 2 (byzantine node
  roles / `honest_fraction` — they edit the same `simnet_cluster.c`).**
- **B4 — golden-height UTXO root ladder** (`57855352d`): `tools/gen_utxo_root_ladder.c`
  (standalone oracle generator, mirrors `gen_sha3_windows.c`), generated
  `lib/chain/{include/chain,src}/utxo_root_ladder.{h,c}`, and the live tripwire
  `app/models/src/utxo_root_ladder_verify.c` — compares this node's own
  `coins_kv_boundary_root` against the locked golden table; any mismatch = named
  divergence at a named height (the "state-wrong coin" detector). Test:
  `utxo_root_ladder` (hermetic verifier + `ZCL_UTXO_LADDER_HEAVY` cross-check).
  **⚠ Table is currently near-empty by design — see the boundary-root gap below.**
- **B5 — capsule→scenario bridge** (`4d76f1d08`): `tools/postmortem_to_scenario.c`
  + `make postmortem-to-scenario CAP=<dir>` emits a parseable `.scenario`
  skeleton from a live crash capsule (automates steps 1-2 of the manual
  CHAOS_HARNESS workflow; steps 3-5 stay manual — full single-node→multi-node
  reconstruction is provably lossy). Test: `postmortem_to_scenario`.

**Telemetry / self-debug (Track C):**
- **C2 — metrics baseline/delta** (`764c64465`): `tools/mcp/baseline.c` 16-slot
  ring; `zcl_metrics_baseline_set/_list/_diff` answer "what changed since X" by
  diffing the `render_prometheus` text verbatim (DRY — no metric enumeration
  duplicated); the previously-no-op `zcl_admin` `since` param now resolves to the
  nearest baseline. Test: `mcp_baseline`.
- **C3 — copy-prove agent contract** (`933a18d54`): `agentcopyprove` (native +
  MCP, destructive-tier) makes copy-prove one agent call — shells out to the
  existing `tools/repro_on_copy.sh` (now with `--json`/`--status-file`), async
  with a status handle polled via `zcl_state subsystem=agent_copy_prove`.
  Copy-only safety invariants enforced in code (can never target a live datadir).

## Push status

Local `main` @ `82c128c62`. The final `make -j32 && make lint && test_parallel`
was running when this doc was written; **push only after it confirms
`ALL TESTS PASSED, 0 failed` + lint clean.** Baseline before the wave was
0/527; expected after is 0/536 (5 new groups). Publish policy (unchanged):
origin = `git@github.com:ZclassiC23/zclassic.git`, FF-only, origin holds ONLY
`main`, never flatten. **`make deploy` to the live node stays owner-gated** — no
lane deployed anything; the live node still runs `3b0de63b0`.

## Follow-up work queued (start here next session)

1. **Boundary-root backfill gap (blocks B4 population) — `#1 concrete follow-up`.**
   `coins_kv_boundary_root_set` has ONE call site (`app/jobs/src/tip_finalize_post_step.c`)
   gated off under IBD-defer; the promised catch-up backfill **does not exist**.
   So no datadir has boundary roots recorded → the golden ladder's stride rungs
   are empty (only the h=3,056,758 checkpoint rung + a dense mmb_root@3,000,000
   anchor are populated, both real & cross-checked). Needs a **populate-only**
   backfill walker (consensus-adjacent → copy-prove, likely owner-gated). Ties
   into the sovereign cure + keystone step 2. After it lands, re-run
   `gen_utxo_root_ladder` against a node.db copy to fill the table.
2. **Groth16 SPEND circuit — stages b-d** (branch `groth16/spend-circuit`
   @ `cf27ad2f3`, worktree `scratchpad/wt-groth16-spend`, NOT merged). Leg 6
   finished the COUNTS stage **exactly** (98777/98638/8, all 28 trace sections
   match, witness satisfies all constraints, in-circuit BLAKE2s == reference).
   Next: (b) re-fetch pinned librustzcash under `reference/`, extend the trace
   harness for `ref_spend_*` oracles + `test_groth16_spend_selfverify`; (c) R1CS
   byte-diff (watch operand order of the 3 bare edwards adds rk/cm/rho +
   Not-form Boolean row placement); (d) rs0 byte-identity → random-blind
   self_verify vs `sapling-spend.params` → bundle cutover retiring the
   librustzcash bridge. Resume brief is in
   `git show groth16/spend-circuit:docs/work/GROTH16-C23-PROVER-CURE.md` §B.
   The OUTPUT circuit round-trip is already cured on `groth16/fable-cure`.
3. **Live copy-proves (owner-gated):** A1 arm→respawn→refold→H\* climb on a
   stalled datadir copy; A2 header-repair un-wedge with the oracle process dead;
   A3 below-coins rowless-hole heal. Unit tests pin the mechanisms; the live H\*
   CLIMB is the acceptance gate. `agentcopyprove` (C3) now automates the harness.
4. **Track-B Wave 2** (now unblocked by B0): **B1** byzantine node ROLES +
   `honest_fraction` in `simnet_cluster` (the "100 nodes, 80% honest" primitive —
   bridges `simnet_byzantine` builders into cluster delivery), **B2** nightly
   sweep targets (`simnet-fuzz-sweep`/`simnet-nightly` + timer), **B3** scenario
   library (`tools/sim/scenarios/*.scenario` + `make scenario NAME=`).
5. **Track-C Wave 2:** **C1** unhealthy-only rollup across the (now 65+) zcl_state
   dumpers via a reserved `_health` key read by a registry walk.
6. **Deferred (A4-small write-ups):** second first-party onion seed needs an
   owner-provided persistent `.onion`; default-on snapshot serving (`ZCL_PUBLISH_FASTSYNC_ON_BOOT`
   + `boot_profile_has_file_service` gate) is a policy call; a dead
   `escape_deadline_secs=60` on an action-less worker-stall blocker.

## Traps this wave hit (so you don't)

- **`make all` / plain `make` does NOT build `build/bin/test_parallel`** — it's a
  separate target (`make build/bin/test_parallel`). A plain `make` after a source
  change leaves a STALE test binary; `--only=<group>` will "match no groups" or
  test old code. Bit two lanes + the orchestrator.
- **`--only=<substr>` is a `test_parallel` flag; `ZCL_TEST_ONLY=<group>` is the
  `test_zcl` env selector.** They are different binaries/mechanisms.
- **A group can pass standalone (`test_zcl`, full sequential) but fail under
  `test_parallel --only` / in the parallel suite** when it depends on global
  state a prior group set (peer-scoring/ban list, is_ibd, deferral globals). If
  you add code that touches a global counter/registry, reset it in the affected
  test's setup. The A2 double-scoring regression was exactly this.
- **A subagent's "this already fails on main too" is a claim to VERIFY, not
  trust.** Build a clean-main worktree and reproduce before accepting it (spot-
  check-audit-findings doctrine). A2's claim was false; verifying caught a real
  regression.
- **Fresh worktrees can't LINK** without `cp -a vendor/lib <wt>/vendor/` (the
  gitignored static libs); `make_lint_gates`'s `tools/z` selftest also needs
  `build/bin/zclassic-cli` built, else it false-reds environmentally.
- **Plan mode propagates to subagents/workflow legs** — implementation legs
  silently no-op into plan-only. (Cost the earlier Groth16 workflow 4 legs.)
