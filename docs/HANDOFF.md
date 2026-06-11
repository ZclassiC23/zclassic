# HANDOFF — read this first

**Restart command:** type **`continue zclassic23 development`**.

State at handoff: main worktree. Verify HEAD with `git status --short --branch`.

---

## 2026-06-11 — 6h live outage root-caused; crash-only boot + FR-3 oversize discovery

**The outage (root-cause chain, verified live):** boot restore INSTALLED
tip 3143175 ABOVE the backable index extent (validated header frontier
3141533; contiguous extent ends 3142801). The anti-rewind finalized floor
(3143171) then REFUSED the only consistent rollback target (3137373) →
post-restore integrity UNRECOVERABLE → FATAL crash-loop → systemd
start-limit gave up → node sat FAILED 6+ hours. A repair safety converted
a recoverable inconsistency into a dead node.

**LANDED on main:**
- `706a7c00a` — crash-only auto-recovery: integrity-unrecoverable boots
  auto-trigger `-reindex-chainstate` instead of FATALing.
- `0b45e93a5` — never-give-up unit: `StartLimitIntervalSec=0` + stepped
  restart backoff (systemd can no longer give up).
- Invariant B (`c8018a388`, prior session): coin-tear derived from
  utxo_apply's OWN log. test_parallel 0/409 green throughout — note that
  it was ALSO green during the entire outage: green is a regression
  floor, not a liveness proof.

**FR-3 oversize discovery (consensus, fix IN FLIGHT):** the auto-reindex
genesis replay FATALed at h=478544 — the canonical chain contains a
125,811-byte tx there (Sapling active at 476969; mine-time rule was 2MB;
zclassicd later tightened to 102000 WITHOUT grandfathering — the
reference cannot resync its own chain). FR-3 (`f8592c386`) copied the
TEXT; the same false-reject also stalls the forward reducer. Fix branch
`fix/consensus-oversize-grandfather`: scan the real chain, grandfather
scanned violations, enforce 102000 above. Rule going forward: validate
against the CHAIN, not the reference text.

**Proven recipe + live redeploy (LANDED, corrects stale docs/memory):**
`--importblockindex $HOME/.zclassic` FIRST (3.14M headers in 60-74s from
the RUNNING zclassicd), THEN the `-cold-import=$HOME/.zclassic` boot →
hash-identical tip in ~25 min, warm-reboot-proven. `-cold-import` alone
leaves a 3.1M-header hole and pins forever. zclassicd ports: P2P=8033
(not 8034 as older notes said), RPC=8232. Live node redeployed via this
recipe.

**IN FLIGHT:** `fix/invariant-a-restore-clamp` (wt2 — tip committable
only ≤ the validated header frontier; evidence-based floor rewind; makes
this outage class unwritable) and `fix/consensus-oversize-grandfather`
(above).

**PROPOSED (code-read confirmed, not yet built):** reindex epilogue is
torn — after `-reindex-chainstate` replay, coins_kv is never reseeded,
the SHA3 commitment is deleted-not-recomputed, and utxo_apply /
coins_applied cursors keep stale pre-reindex values
(`boot_index.c:165-297`, `boot.c:3321-3344`) — the recovery path itself
manufactures the coins_applied>hstar wedge shape.

⚠️ **TEMPORARY:** the live unit injects `-nobgvalidation` via environment
(bg validation would hit the same h=478544 false-reject). REMOVE once the
oversize grandfather fix lands.

---

## 2026-06-10 — mainnet sync wedge cleared END TO END (branch `feature/sync-fixes`)

**VERIFIED LIVE:** trackb (cold import) went from a wiped datadir to the
LIVE TIP in ONE boot — h=3142304 vs zclassicd 3142305, hash-identical at
3142118/3142200/3142300, straight through the old prevout_unresolved
wedge. tracka (trustless genesis sync) unwedged from its h=6756 freeze
and advances steadily with all eight stages ticking. Six root causes,
each diagnosed empirically (supervisor dumpstate ages, /proc per-thread
CPU+wchan, gdb-as-parent repro, raw leveldb probes, SQL over the
imported set):

1. **Download queue O(n²) under dm->cs** — per-item linear dedup +
   sorted-insert memmove against a queue pinned at its 65536 cap;
   bulk enqueues held the lock for minutes, the supervisor thread
   starved, and the staged pipeline stopped ticking. Now an O(1)
   membership set + one sort-merge per bulk call, evict-highest at
   cap. Also: stall recovery collapsed from 12 full block_map scans
   per invocation to one.
2. **Header sync crawled its own known span** — periodic getheaders
   re-anchored at the ACTIVE TIP and the continuation never skipped
   known headers (tracka: +640 headers/30min at 100% CPU). An
   all-known batch now continues from pindex_best_header; tip-class
   anchors prefer best_header (both gated on heights_repaired).
3. **UTXO import silently dropped the txid-keyspace TAIL** — the
   reader's end-of-range check jumped past the publish of the chunk
   being filled: every txid above FF3779C7... (~1,561 records,
   ~4,400 coins) missing on every import with identical
   plausible-looking row counts. THE original prevout_unresolved
   cause. Proven with a raw leveldb probe (501,273 'c' records on
   disk vs 499,712 imported distinct txids).
4. **gap_fill windowed from the active tip, not the reducer
   frontier** — post-hard-kill boots restore the tip above the staged
   cursors and nothing re-downloads below tip+1: the classic
   tip_finalize>utxo_apply inversion wedge. The window bottom now
   clamps to body_fetch's cursor.
5. **coins_kv empty on the FIRST boot after a cold import** — the
   projection-based boot rebuild runs pre-import against an empty
   projection, so the script_validate prevout resolver had no
   pre-anchor coins: first post-anchor spend fails ok=0, the seeded
   anchor is refused, H* falls back to the compiled checkpoint, tip
   pins at the anchor. The import now seeds coins_kv directly from
   node.db utxos (1,344,557 rows, one ATTACH-copy).
6. **Live-chainstate copy hardened** — cp -a of a live LevelDB can
   tear; the copy is now signature-verified point-in-time (retry
   until no source file changed mid-copy, refuse instead of tear).

Plus: validate_headers window report treats a missing log table as
"no data" instead of LOG_ERR spam. (A seventh fix — full block_map
scans per successor hop in msg_headers — was independently fixed on
this main by 416fddb85 and is not re-applied here.)

Ops notes: track units are transient systemd-run (no auto-restart —
the tip watchdog's self-shutdown leaves them dead; use real units with
Restart=on-failure for soaks). zcl-rpc needs ZCL_DATADIR+ZCL_RPCPORT;
state via `zcl-rpc dumpstate '"supervisor"'`, SQL via
`zcl-rpc dbquery '"SELECT ..."'`. ptrace is yama-blocked: run the node
as a gdb CHILD on a copy datadir, or read /proc/PID/task/*/stat+wchan.

---

## ★★★ 2026-06-09 SESSION CHECKPOINT — security-audit remediation landed (read FIRST)

The Round-1 security audit's chain-fatal findings are FIXED on main, gated by
build + `make lint` (all gates) + `test_parallel` **0/380**. Full disposition
(including two findings refuted with citations: the C-4 retarget half is
FALSE — `bad-diffbits` runs live via `accept_block_header`; M-2's proposed
SIGHASH_SINGLE sentinel would CREATE a fork) is in
**`docs/work/security-audit-response-2026-06-09.md`**.

Landed in this checkpoint:
- **C-1** script push stack-overflow fixed at the source (`script_get_op`
  capacity guard) + sigop-walk parity companion + `-fstack-protector-strong`.
- **C-2** coinbase subsidy ceiling live (`bad_cb_amount` status, genesis
  exempt, zclassicd `bad-cb-amount` parity).
- **C-3** consensus nullifier set live (`nullifier_kv`, atomic with the coins
  commit, rewind-safe). Activation-forward on existing datadirs — typed
  blocker `utxo_apply.nullifier_backfill_gap` records the remaining window.
- **#26 / H-1** per-tx contextual rules wired into `script_validate`
  (`script_validate_contextual.c`) per the verified spec + the `tip_h >= 0`
  correction; transient infra failures persist as resurrectable
  `internal_error`, never permanent rejects.
- **C-5/H-3** encrypted wallet backups (`WALLET_BACKUP_PASSWORD`,
  `--decrypt-wallet-backup`) + operator-private `/api` routes 403'd on the
  clearnet listener.
- `consensus/zclassicd-parity-2026-06-08` (7 parity fixes + the #26 spec)
  merged to main.

⚠️ Deployment remains owner-gated: prove on a datadir COPY first (replay
zero-contextual-reject, nullifier false-positive-free advance) per the
response doc. The prevout_unresolved frontier hole (below) is still the live
sync blocker for the wedged datadir.

---

## ★★ 2026-06-08 SESSION CHECKPOINT — consensus-parity axis (supersedes the wedge framing below for forward planning)

Two agents ran in parallel: **consensus parity + MVP-CI** (this branch) and **Codex** (the forward-sync stall — the live blocker, still in flight).

**MERGED to main (2026-06-09):** branch `consensus/zclassicd-parity-2026-06-08` — 7 zclassicd-consensus-parity commits, each proven history-safe — FR-1 tx-expiry strict `>` (`cbee16138`), FR-2 finality=header-time (`e5b44f25b`), FR-3 tx-cap 102000 (`f8592c386`), miner cap 200KB (`8b0a2af1f`), **#1** CHECKDATASIG block-sigops (`e1d125e42`), **#5** reject coinbase→transparent (`08e48a69e`), audit+#26-spec docs (`a56640ef3`).

**VERIFIED-READY (not landed):** `git merge --no-edit mvp/autonomous-honesty-2026-06-08` — proven conflict-free + 378/378 green on `7e18d9c70` (MVP #5 real file delivery + #4/#7 honesty; node.db schema 19→20). Land at a clean Codex checkpoint.

**#26 (root cause of #2/#4) — designed+fork-safety-verified:** wire per-tx contextual rules into the live reducer at the head of `script_validate_stage.c step_validate`, IBD-gated only on the per-tx call + tip-proximity guard. Spec: `docs/work/contextual-check-wiring-spec-2026-06-08.md`.

**Handed to Codex:** #3 JoinSplit Ed25519 sig (`proof_validate_stage.c`), FR-5 `HaveShieldedRequirements` stub (`coins_view.c:480`), FR-4 sapling-root reject — see `docs/work/consensus-parity-supplemental-audit-2026-06-08.md`. Codex's in-flight WIP backed up to origin `wip/codex-forward-sync-checkpoint-2026-06-08`.

**MVP v1: MRS 0/8** (all 8 = green hermetic slice/proxy; none full). 5 doc honesty discrepancies (all UNDER-claim) incl. **D1**: the `mvp-spawn` CI job gates #1/#6/#7 full-binary proxies on EVERY push/PR (MVP.md wrongly says "opt-in"). Full punch-list + next-dev critical path in memory `project_session_handoff_2026-06-08`.

**Next-dev path:** Codex lands forward-sync → merge honesty branch → implement #26 → fix D1-D5 → then live-gated #3/#6/#8.

---

## 2026-06-08 checkpoint — reducer L1/stale-replay work pushed; live wedge still open

Latest checkpoint from main worktree is the reducer self-heal/stale-replay
repair stack. Local validation before commit:

- `make -j$(nproc)` — clean / already up to date.
- `make test_parallel` — clean / already up to date.
- `build/bin/test_parallel` — **0/378 groups failed**.
- `make lint` — **all 35 gates passed**.
- `git diff --check` — clean.

What is in this checkpoint:

- `script_validate` no longer depends on `-txindex` for normal prevout
  lookup. It resolves in-window prevouts through `created_outputs` and older
  live coins through `coins_kv`, bounded by the physical coin frontier.
- Reducer L1 refill now clamps stale `validate_headers`, `body_fetch`, and
  `body_persist` cursors after clearing holes / hash-split rows, and caps
  `tip_finalize` at the physical coin frontier.
- Stale script replay repair is wired through the reducer repair path and can
  rewind replay cursors back to the coin frontier, backfill `created_outputs`,
  and replay under the fixed binary.
- Owner-gated value-overflow one-shot repair is present in
  `utxo_apply_delta_repair.c`; **`utxo_apply_delta.c` was not touched**.
- Copy-proof isolation avoids seed discovery in connect-only mode so copy runs
  stay isolated.
- `reducer_frontier_reconcile_light` witness now accepts durable reducer cursor
  movement as observable progress, so zero-peer copy repairs do not exhaust as
  `unwitnessed`.

Copy-proof status:

- Last copy run:
  `/home/rhett/.zclassic-c23-COPY-20260608-120105-l1-validate-refill-proof5`.
- It successfully fired the value-overflow repair at **3,132,747**, stale
  script replay repair at **3,132,720**, validate hash-split repair, and body
  refetch clamp at **3,134,302**.
- It then wedged at **3,135,517** with `script_validate_log ok=0
  status='prevout_unresolved'`.
- The missing prevout is
  `bf8ae6840fd8c30fdcb968f60afa85b34bc51fc8e518d87d545a553316a9f6ff:0`.
  Raw block scan found it created at **3,073,765** with no prior spend before
  the failing block; it is absent from this copy's `coins_kv` and
  `created_outputs`.

Next developer:

1. Do **not** cut over. The copy proof has not held through tip.
2. Continue on a datadir COPY only. Do not touch the live service, live
   datadirs, or live ports.
3. The next real fix is a guarded repair for the old missing coin frontier
   hole, most likely a targeted `utxo_apply` rewind/reapply from before the
   creator height **3,073,765** using the existing inverse-delta/reorg
   machinery. Do not make `script_validate` silently ignore this; `utxo_apply`
   also needs the coin physically present.
4. `tools/scripts/soak_assert.sh` now resolves `build/bin/zcl-rpc` from the
   repository root, so the soak assertion script works when launched outside
   the repo root.

---

## ★ LIVE WEDGE — STILL OPEN; P2 (self-heal prerequisite) LANDED; next = L0/L1 H* self-heal (2026-06-07) [PRIOR-SESSION REFERENCE; owner reframed onto zclassicd consensus parity 2026-06-08 — see the checkpoint above]

**The live wedge is NOT resolved.** (The 2026-06-06 `validate_headers` recheck
fix below was real and shipped, but it did **not** clear the live node — the
datadir is **multi-epoch torn**, a deeper class than recheck starvation.)

**Live truth (this handoff):** node serves at **3,134,951**, oracle (`zclassicd`,
RPC 8232) at **3,139,290** → **~4,339 behind and not climbing** (`tip_advance_age`
== uptime). The 8-stage reducer's upstream cursors have raced to **3,138,981**
while `tip_finalize` is stuck at **~3,134,954**. Per-height logs, the 8 stage
cursors, and the legacy `block_index` flags drifted to **different heights across
crash/rewind/replay epochs**, and **nothing reconciles them as a window**.
Crucially: **the coins are consistent** (inverse-delta-complete) — this is a
*flag/cursor view* tear, not a coin-money tear. Self-heal is `operator_needed`
(5 attempts exhausted). ⚠️ **Deploy drift:** the *running* binary predates repo
HEAD; a clean redeploy is owner-gated and won't by itself fix a torn datadir.

**The fix (active driver: `docs/work/self-healing-reducer-plan.md`, 2026-06-07):**
compute a provably-consistent frontier H* from durable state, then sweep-heal the
drifted flags/cursors forward over the (H*..served_floor] window — never rewinding
the consistent coins, never deleting a `tip_finalize_log` row.

- **P2 (DONE, on `main` `c81e69ae0..4b60c1149`, fully proven):** `coins_applied_height`
  is co-committed inside the `utxo_apply` txn on **all** write paths (forward,
  reorg-unwind, poison_rewind) → a contiguous applied-coins frontier that can't
  hide an interior hole. Proven by 2 independent workflow impls (agree) + 3
  adversarial verifiers + `test_parallel` **0/375** + `lint` 35-gate + **chaos 9/9**
  + a kill-9 copy-proof (`tools/copyproof_p2_frontier.sh`) holding the invariant on
  the raw crash image. Offline checker: `make p2_invariant_check` -> `build/bin/p2_invariant_check <datadir>`.
- **L0 (task #11) `reducer_frontier_compute_hstar` — DONE** (on `main` `50dfd1753`):
  pure SELECT-only authority in `app/jobs/src/reducer_frontier.c` returning
  {hstar, served_floor}; anchored at the SHA3 checkpoint **3,056,758** (cold-import
  logs are sparse → contiguity-from-genesis is wrong); C1–C6 of the plan, NULL-hash
  = no-evidence, C4 coin-tear = WARN-only. PURE read-only (no mutation — heal is L1).
  Adversarially reviewed (PASS: read-only purity, checkpoint floor, tear-correctness,
  mutation-sensitivity via 2 revert-experiments); `test_reducer_frontier` 5 topologies;
  build green, `test_parallel` **0/376**, `lint` 35-gate. Follow-up: convert the C2/C3
  per-height query loop to set-based SQL before wiring into a hot/boot path.
- **NEXT — L1 (task #12) `reducer_frontier_reconcile_light`:** a Condition that
  sweep-heals `block_index` flags + clears HAVE_DATA holes (so `body_fetch`
  re-requests) + clamps ONLY the `tip_finalize` cursor — never touches coins.
- **L2/L3 (task #13):** forward-immunity for a *future genuine coin tear* +
  subtraction of the now-subsumed legacy repair code. NOT needed for today's wedge.

**Method discipline (mandatory):** diagnose on a datadir **COPY** only (isolated
ports 18299/18933, `setsid`); never touch the live datadir/oracle/ports; H* ≥
checkpoint 3,056,758; never delete a `tip_finalize_log` row; never lower the
public tip below `coins_best`. See `docs/work/fast-path.md` and memory
`project_tipfinalize_precondition_desync_fix_2026-06-07`.

---

## ACTIVE AXIS (2026-06-05): convergence — "everything the zclassic23 way"

Owner directive this run: *"do EVERYTHING the zclassic23 way — dig into every
file, DRY, good API, document everything, use multiple workflows, commit + push
as you go, make the server amazing."* This is the **architecture/quality** axis
(distinct from the v1/live-wedge mission below — both are real; the owner chose
this one). Full state + gotchas: memory
`project_convergence_axis_2026-06-05.md`; ranked work: `docs/work/convergence-backlog.md`.

**Where it stands — the autonomous-safe convergence drive is COMPLETE** (rounds
2–15, all green build0/lint35/test_parallel 0/371, all pushed; latest run
`88349b39d`→`ea7542cda` fixed ~52 real bugs + ~40 docs):
- 8/8 framework shapes real; `framework_shape_allowlist`=0; lint ratchets at 0.
  File-size debt board: `boot.c` (3618, FROZEN — **never touch**) + `boot_services.c`.
- **The whole non-consensus surface is harvested** (lib/net feature transports,
  every app/ controller/view/model, lib/wallet, lib/rpc, lib/storage projections,
  supervisors, metrics). Real bugs killed this session: a wallet keypool concurrent
  OOB + data-race snapshots, 2 key-material `memory_cleanse` leaks, a 1-byte stack
  overflow, a 4-projection event-skip class bug, a post-broadcast send-failure bug,
  a proven send-path tx/entry leak, 11 P2P-transport NULL-derefs/unchecked writes,
  3 shielded signing-path `signature_hash` checks, 5 UTXO script leaks, 2 realloc-OOM
  NULL-derefs, a double-free, a snapshot-anchor UAF race, a malformed-txid uninit read.
- **Consensus-adjacent layer handled the careful way** (rounds 12–15): strict-bar
  audit → read-only prove-or-refute (skeptical analysts + adjudicator) BEFORE any
  edit. Fixed: bg-validation honesty (no false "verified" when script checks skipped
  for missing undo; surfaces `verification_incomplete`), quorum symmetric tally.
  Refuted with evidence: `header_sync:575` (sanity gate; connect_block is the arbiter),
  coins-commitment no-rollback (derivative), `block_index_db` nFile cast (fails closed).
- **CORRECTION recorded**: `zcl_mutex` is RECURSIVE — two earlier "deadlock" calls
  were overclaims (code stands; framing corrected in git + backlog).

**The proven method (repeat it):** read-only audit Workflow → ranked backlog →
parallel **edit-only** adversarially-reviewed Workflows on **DISJOINT** file sets
→ ONE union gate I run myself (`make -j$(nproc) zclassic23` + `make lint` [35
gates] + `build/bin/test_parallel` [expect `0/371`] + boot-smoke if the boot path changed)
→ commit per logical group → push. Redirect `make` output to a file and grep
`error:|warning:` to save context. Mutating workflows on disjoint files may run
concurrently; verify the union together.

**What remains is owner-gated / repro-on-copy ONLY** (the autonomous-safe drive is
done — do NOT re-audit the swept surface, it's harvested; see
`docs/work/convergence-backlog.md` §12, §12-EXT, Round 8–15):
1. **`block_index_loader.c:376` nChainTx DRY** — real but latent (safe error
   direction; `connect_block` re-validates). The clean fix (call
   `block_index_forward_pass` instead of 3 hand-rolled recomputes) touches the
   **never-touch** `config/src/boot.c:2138` — needs an owner call on how to fix
   without editing boot.c. Boot-smoke on a COPY before deploy.
2. **`boot_services.c` shutdown TU** + **flyclient/MMB block** — boot decomposition
   tail; HIGH RISK (coins.db COMMIT-before-`block_index` fsync), needs a real
   SIGTERM stop+restart on a datadir COPY. Do ALONE.
3. **peer-scoring enum extension** (OWNER-GATED — changes DoS-ban policy). Extend
   the enum first; do NOT do a naive `peer_misbehaving`→`peer_scoring_record` swap.
4. **§12 consensus-DECISION items** (coins_view rollback nit, nFile>INT_MAX reject,
   plus the originals) — each repro-on-copy; most already triaged/refuted.
5. More audits ONLY if a NEW subsystem appears — **STOP fanning over swept code.**

**Gotchas that cost time this run (don't relearn):**
- **Boot-smoke light-copy floor is NOT a regression.** `repro_on_copy.sh`
  (default `--light`, no `blocks/`) rewinds tip 3134303→**3132299** given ≥~90s
  (DEGRADED_SERVING, "Not fatal"). PROVE innocence by running HEAD's binary on the
  same window — it floors identically. A crash or a LOWER floor = real regression.
- `test_make_lint_gates.c` BANS ~90 refactor-scaffold substrings in production
  comments ("byte-for-byte", "verbatim", "extracted from", "code motion", …).
  Name the PURPOSE, not the refactor. Renaming a symbol's home also needs the
  gauges/owner-file asserts in that test repointed.
- `check-observability-pairing` is a compiled tool: a raw `fprintf(stderr,…)`
  must be paired (−3/+6 lines) by `event_emit`/terminal `return`/`// obs-ok:`,
  ELSE route it through `LogPrintf` (not flagged). Editing a file can line-shift a
  PRE-EXISTING fprintf out of its window → newly fails.
- Gate #15/#21 (supervisor) now scope `config/src/`; a moved boot worker that
  spawns a thread must keep its `supervisor_register_in_domain`/contract or carry
  `// supervisor-ok:<tag>`.
- `config/src/*.c` is globbed (`CONFIG_SRCS = $(wildcard …)`) — new boot units
  build with NO Makefile edit.
- Adversarial review EARNS its keep (caught the `coins_alloc` 4th caller, a silent
  peer-ban weight change, an over-broad blake2b doc). Always run it; respect FAILs.

---

## The mission is v1 (not the refactor)

The v1 bar is **[`docs/MVP.md`](./MVP.md)** — 8 operator acceptance criteria;
v1 = MRS 8/8. **THE plan is [`docs/work/FORWARD_PLAN.md`](./work/FORWARD_PLAN.md)**
(MVP-anchored, with the live wedge as priority #1 and the autonomous /
owner-gated / operational critical path).

Honest status: **~2/8 met by hand, 0/8 CI-enforced** (every criterion test
gates on `ZCL_STRESS_TESTS=1`, which `make ci` never sets). Do not trust
`make test_parallel` green as a v1 proof — it runs zero MVP criteria.

The framework/architecture refactor is **~90% done and OFF the v1 path.**
`docs/FRAMEWORK.md` (architecture) and `docs/REFACTOR_STATUS.md` (debt board)
are reference. **Do not jump the queue into refactor work** while v1 buckets
are open.

---

## ⛔ #1 priority — the live wedge

**ROOT CAUSE (verified live 2026-06-05, multi-agent dx + direct probing) — it is
NOT a consensus code bug; the code is behaving CORRECTLY.** The node holds at
tip 3,134,303 because block body **3,134,304 is missing** (`legacy_mirror`
`stuck_reason:"missing-have-data"`) and there is **no eligible body source**:
- **Native P2P is correctly gated out by the anti-eclipse floor.** The node's
  ONLY reachable peer is one local MagicBean on `127.0.0.1:8033` that **flaps**
  (`disconnected=748`, `blocks_received=0`, stuck `syncing_headers`); external
  mainnet peers are TCP-unreachable from this box (`addnode_tcp_failures=1765`).
  A single localhost peer can never satisfy `p2p_minimum_viable`
  (`block_source_policy_runtime.c:161` — needs ≥2 healthy on ≥2 distinct IPv4
  groups, or ≥3). This gate is a **safety invariant — do NOT lower it.** A
  diagnosis workflow proposed lowering the floor to 2; that is **WRONG** (the
  effective floor is already 2 + eclipse checks, and the node has only 1 peer)
  and would weaken eclipse defense. Rejected.
- **The co-located zclassicd is healthy and readable but advisory-only.**
  zclassicd (PID 2273227, up 2 days) serves RPC on `127.0.0.1:8232`
  (`getblockcount`→3,136,562 verified) and `legacy_mirror` reads it fine
  (`reachable:true`). But the mirror is `bounded_advisory_fallback` trust and is
  in **"observing" mode** (`last_error:"local sync primary; mirror observing"`)
  — by design it will NOT authoritatively supply consensus blocks. The
  `rpc-unreachable` blocker label is **stale** (mirror actually reads zclassicd).

So `reorg_detected_total` climbing / `finalized_total=0` / the
"no-header-solution-backfill-required" rejections are all **downstream
symptoms** of the body-source starvation, not independent bugs.

**The two real resolutions are OPERATIONAL / OWNER-GATED (no consensus edit):**
1. **Native peer diversity** — get ≥2 honest, reachable, distinct-IP-group
   native peers so P2P clears the eclipse floor and bodies flow. Blocked here by
   the environment (only one flapping localhost peer is reachable). This is the
   trust-preserving fix.
2. **(Owner-gated policy)** Decide whether a fully-validated, co-located
   zclassicd may *authoritatively* supply bodies when native P2P is
   eclipse-starved — the long-standing advisory→authoritative mirror trust
   decision (see the cutover/single-engine history). Do NOT flip unilaterally.

Housekeeping: the `zclassicd-rhett` **systemd unit is crash-looping**
(`NRestarts=3990`, "Cannot obtain a lock on data directory" — it is a duplicate
fighting PID 2273227 for the datadir lock). Harmless to the oracle but pure
wasted CPU; mask/stop it. It is NOT the running oracle — do not confuse them.

Older notes (still valid as background): diagnose on a datadir **COPY**, never
live (`tools/diagnose_gap.sh`, `docs/work/fast-path.md`); the prior have-data
window-extender wiring was reverted (`481c520b9`) for churning `tip_finalize`;
recovery FSM design in `docs/work/service-state-machine.md`.

---

## Do Not

1. Do not weaken a lint gate or grow a baseline.
2. Do not delete `tip_finalize_log` rows or hand-edit stage cursors.
3. Do not ship a consensus-adjacent fix without a datadir-copy proof
   (`tools/repro_on_copy.sh`). The boot self-heal heals only on a `utxo_sha3`
   commitment match; otherwise it preserves FATAL — never weaken that.
4. Do not stop `zclassicd-rhett`; manage long-running services through
   `systemctl --user`.
5. Do not restore deleted cutover/projection-diff/public shadow tooling.
6. Do not move the local `zclassic23` P2P listener back to `8033`; the active
   dev node is on `8023` to avoid a `zcashd` port conflict.

---

## First 5 Minutes

```bash
git status --short --branch
make lint
touch lib/test/src/test_parallel.c && make test_parallel && build/bin/test_parallel
build/bin/zcl-rpc getblockcount        # live tip — is it advancing?
```

If the node is not running, or the tip is not advancing, record that
explicitly before claiming any live proof. Forward progress on the running
node is the real bar.

---

## Where the detail lives

| Need | Doc |
|------|-----|
| The v1 contract (8 criteria) | [`docs/MVP.md`](./MVP.md) |
| **THE plan** (critical path) | [`docs/work/FORWARD_PLAN.md`](./work/FORWARD_PLAN.md) |
| How to execute consensus-critical work safely | [`docs/work/fast-path.md`](./work/fast-path.md) |
| Engineering quality board (41 items) | [`docs/work/FINISH_CHECKLIST.md`](./work/FINISH_CHECKLIST.md) |
| Architecture (canonical) | [`docs/FRAMEWORK.md`](./FRAMEWORK.md) |
| Architecture debt board (off v1 path) | [`docs/REFACTOR_STATUS.md`](./REFACTOR_STATUS.md) |
| Directory / file-purpose map | [`docs/PROJECT_OVERVIEW.md`](./PROJECT_OVERVIEW.md) |

Default to subtraction. Prove on a copy before touching the live chain.
