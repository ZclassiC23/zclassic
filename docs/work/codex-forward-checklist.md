# Codex forward checklist — long-running work after the 2026-07-03 hardening drop

> **Audience:** Codex (or any AI/human contributor) picking up zclassic23 work.
> **Scope:** the deliberate, higher-risk, longer-running work that was **deferred**
> from the 2026-07-03 parallel hardening drop (commits `57d8b0970` → `dab06cbce`).
> Each item names the risk class, the load-bearing invariant, and a concrete
> acceptance bar — not a vague goal.
> **Read [`docs/AGENT_TRAPS.md`](../AGENT_TRAPS.md) and [`CLAUDE.md`](../../CLAUDE.md)
> FIRST.** Verify every file:line fresh; they rot under refactors.

---

## 0. Inviolable rules (do not relearn these the hard way)

- **Bug-for-bug zclassicd consensus parity.** Never ship a consensus change
  (Equihash params, activation heights, block/tx validity, script flags) to
  zclassic23 first. Where zclassicd has a bug, match the bug. Enforced by
  `check-consensus-parity` (E13) + `test_consensus_parity` golden values. See
  [`docs/CONSENSUS_PARITY_DOCTRINE.md`](../CONSENSUS_PARITY_DOCTRINE.md).
- **Validate against the real CHAIN, not reference text.** The h=478544 lesson:
  a text-copied 102000-byte tx cap false-rejected zclassicd's *own* chain. Any
  tightening of a bounded predicate requires a full-history replay to 0
  false-rejects before it can default ON.
- **Copy-prove before live.** Never operate on the live datadir to test a fix —
  copy it, reproduce on the copy, prove the fix FIRES there, then deploy. Gate
  on **H\* climb**, not "booted without FATAL."
- **Never stop `zclassicd`** (the oracle at `~/.zclassic`, RPC 8232). Services
  run under `linger`; the live zclassic23 node is at `~/.zclassic-c23` (RPC 18232).
- **Local-only CI.** `make ci` is the gate; it runs on this machine, never on a
  paid runner. The pre-push hook (`tools/githooks/pre-push`) runs **full `make ci`
  before every push** — that takes >10 min. Two implications: (1) run `make ci`
  yourself and let it finish before pushing; (2) `git push --no-verify` is only
  acceptable when you have just run `make ci` green in the identical tree (it is
  the documented bypass, not a way to skip verification).
- **Test caching.** `make build-only -j$(nproc)` (depfile-tracked compile check);
  `ZCL_TEST_ONLY=<group> build/bin/test_parallel` for a focused run (the runner
  forks per group). Use `test_parallel`, never the `test_zcl` monolith for iteration.
- **Defensive standards:** every malloc via `zcl_malloc`; every error via
  `LOG_FAIL`/`LOG_ERR`/`LOG_NULL`; every write through the AR lifecycle; every MCP
  handler sets an error body. `make lint` enforces. The lint gate list is
  machine-checked against `docs/DEFENSIVE_CODING.md` (E11) — adding a gate means
  documenting it in BOTH places or the build breaks.

---

## 1. What just landed (2026-07-03) — context, do not redo

| Commit | Change |
|--------|--------|
| `57d8b0970` | Doc-accuracy: fixed 5 conflicting test-counts, FRAMEWORK `15/10/13`→`12+13`, stale `sync-organism-map.md` "Wound 2", CLAUDE.md Vision qualifiers; added `tools/scripts/check_doc_counts.sh` (canonical `<!-- DOC-COUNTS -->` block in `docs/CODEBASE_MAP.md` vs code). |
| `b163395b2` | Wired `check-doc-counts` into `make lint` + documented it in `docs/DEFENSIVE_CODING.md`. |
| `e50e38be5` | **Reproducibility gate:** `tools/scripts/check_reproducible_build.sh` (build twice → byte-compare) + opt-in `make ci-reproducible` + shared `tools/scripts/repro_build_vars.sh`. Produced the project's **first byte-identity proof** (build N == build N+1). NOT in the hot `make ci` path. |
| `dab06cbce` | **MCP two-tier auth:** `ZCL_MCP_DESTRUCTIVE_BEARER_TOKEN` — destructive tools require a separate credential; back-compat (unset == today); least-privilege (destructive token rejected for non-destructive tools); constant-time; auth tier independent of rate-limit bucket. |

---

## 2. Prioritized long-running work

### P0 — Sovereign trust root (`-refold-from-anchor`) — the NORTH STAR
**Why:** "Personal sovereignty stack" is currently borrowed: the live node boots
from a 105 MB snapshot (`utxo-seed-3155842.snapshot`) whose UTXO *content* is
minted by external `zclassicd` and trusted, not re-derived from zclassic23's own
checkpoint. This is the biggest vision-vs-reality gap and gates MVP C3/C6/C8.
**Risk class:** consensus-adjacent (trust root), **owner-gated, copy-prove before live.**
**Where it stands:** cure scaffolding exists — `app/services/src/anchor_selfmint.c`,
`app/jobs/src/refold_progress.c`; the borrowed-seed path
`app/services/src/utxo_recovery_restore.c` is still live-called. `docs/HANDOFF.md`
lists "rung-3 real (runtime refold-from-anchor without restart)" as remaining.
**Bottleneck:** genesis→checkpoint mint is O(height) per block via the pprev-walk
in `lib/validation/src/chainstate.c:350-391` (~76% CPU, ~50 blk/s, 74% commit-fsync).
A faster-fold implementation was built but **not merged** — re-verify its status
and land it (or an equivalent) so the self-mint is tractable.
**Acceptance bar:**
1. Self-mint a UTXO checkpoint at the in-binary PoW anchor height, fold from it.
2. Delete the borrowed-seed loader path (the stopgap), so the node boots sovereign.
3. Copy-prove on a fixture datadir: H* climbs to tip with no `seed_exempt` early-return
   (`app/jobs/src/stage_anchor.c`) carrying a non-folded coin.
4. Hash-identical tip vs zclassicd at multiple heights.

### P1 — Close the three default-off parity predicates
**Why:** three zclassicd-*unconditional* predicates default OFF today, so a stock
install is more permissive than zclassicd on three classes of invalid input.
Each can only over-accept (no mainnet fork vector alone), but strict parity demands
they default ON. **Risk class:** consensus tightening — **full-history replay required first.**
**The three:**
- `g_enforce_sapling_root` (`lib/validation/src/connect_block.c:97`) — zclassicd
  rejects any `hashFinalSaplingRoot` mismatch; c23 default rejects only all-zeros.
- `g_enforce_checkdatasig_sigops` (`connect_block.c:126`) — zclassicd ORs
  `SCRIPT_VERIFY_CHECKDATASIG_SIGOPS` unconditionally; c23 omits it.
- `g_enforce_coinbase_maturity` (`app/jobs/src/utxo_apply_delta.c:60`) — zclassicd
  rejects immature coinbase spend unconditionally.
**How:** each has a COUNT-AND-CONTINUE replay harness (`app/jobs/src/replay_count_only.c`,
env `ZCL_REPLAY_COUNT_ONLY`). Run each over full history → must be **0 fires / 0
false-rejects** → only then flip the default. Never from text (h=478544 lesson).
**Acceptance bar:** 0 false-rejects over genesis→tip for each predicate, then default
ON, with the replay result recorded in the commit message.

### P2 — DRY (Law 8) — the architect-acknowledged weak law
Concrete, bounded duplications to consolidate (these are boilerplate, not hot loops;
Law 8 permits duplicating hot-loop straight-line code, which these are not):
- **Port `8232` #defined 8× under 7+ names** — LANDED 2026-07-08:
  production C now uses one shared `ZCLASSICD_RPC_DEFAULT_PORT` constant in
  `lib/rpc/include/rpc/zclassicd_port.h`; scripts/tests/docs may still mention
  the literal live port intentionally.
  Previous state: across 8 files (`lib/rpc/src/legacy_chain_oracle.c:20`,
  `app/services/src/header_probe.c:48`, `app/services/src/utxo_parity_service.c:73`,
  `app/services/src/utxo_reference_source_zclassicd.c:26`,
  `app/controllers/include/controllers/wallet_view_internal.h:34`,
  `app/controllers/include/controllers/repair_controller_internal.h:61`,
  `app/services/src/legacy_mirror_sync_state.c:159`).
- **The `sqlite3_prepare_v2`/`bind_int(height)`/step/row-or-done/finalize pattern
  repeats ~98× across `app/jobs/src/`** (35 files). Add a
  `progress_kv_step_row(db, sql, height, …)` helper (or widen use of the existing
  `AR_STEP_ROW_READONLY` macro) and migrate call sites incrementally.
- **`AR_CACHED_SAVE` has zero call sites** but is documented as a live lifecycle
  entry point (`docs/DEFENSIVE_CODING.md`). Either implement a real hot-path user
  or remove it from the documented surface.
**Acceptance bar:** `make ci` green; reduced raw-SQL site count (the
`// raw-sql-ok:progress-kv-kernel-store` exception is bounded-by-design, not debt —
do not "fix" it).

### P3 — Live-caught tenacity gaps (observed 2026-07-03 on the live node)
- **`op.build_snapshot_offer` blew its 30-min deadline with `escape_dispatched_total:0`**
  for ~37 min — a transient blocker that exceeded its SLO with no escape action and
  no paging. The promise "operator_needed ⟺ a blocker outlived its SLO" may have a
  gap for background-worker blockers. Investigate `app/supervisors/` + the blocker
  SLO→escape path; add a regression test that a deadline-exceeded blocker either
  dispatches its escape or raises `EV_OPERATOR_NEEDED`.
- **`zclassicd` oracle mirror unreachable** ~71 min (`mirror.rpc-unreachable`,
  1010 fires, `legacy_oracle_usable:false`) — external, but it means C8 parity
  diffing was dark. Confirm the oracle self-recovers or pages.

### P4 — Doc system hardening (the gate exists; extend it)
- **Condition-count drift gate — LANDED 2026-07-08.** `check_doc_counts.sh` now
  checks `condition_registrations` from live `condition_register()` calls, and
  `docs/FRAMEWORK.md` row 6 / `docs/CODEBASE_MAP.md` were corrected to the
  current 30 registered conditions.
- **`docs/work/` has grown back to ~55 files.** The owner purged 55 story files on
  2026-06-18 ("recycling old narratives re-halted forward sync ~103×"). Add a
  doom-archive policy: dated `docs/work/*-2026-0N-*.md` narratives with no live
  citation get archived after N days. Truth lives in CODE + the live node, not narratives.

### P5 — Minor structural cleanups (low risk, good hygiene)
- **Framework-shape gate incoherence:** `tools/lint/framework_shape_check.sh:39,63`
  treats `app/views/` as shape #8, but `docs/FRAMEWORK.md` and
  `docs/HOW_THE_NODE_WORKS.md:108-109` say views is NOT one of the eight (the real
  8th, Storage Adapter, lives in `adapters/`+`ports/` and is NOT scanned). Align the
  gate with the docs (scan `adapters/`/`ports/` for the 8th shape, treat `views/` as
  the non-shape it is).
- **`ci-reproducible` is opt-in and manual.** Consider wiring it into the release
  flow (`tools/release.sh --verify`) so every release is byte-identity-proven, and
  surface the recorded hash in release notes.
- **MCP auth tier follow-up:** now that `ZCL_MCP_DESTRUCTIVE_BEARER_TOKEN` exists,
  consider a third read-only tier for monitoring/introspection credentials, and
  document the two-tier model in `docs/SECURITY_AND_INTEGRITY.md` operator guidance
  (the code-level docs landed in `dab06cbce`; add an operator runbook).

---

## 3. Operational notes for Codex

- **Start every session** with `zcl_status` (live node) + `git log --oneline -5` +
  `cat docs/HANDOFF.md`. A doc can be stale; the node cannot.
- **MCP targets:** `mcp__zcl23-dev__*` → dev node (`~/.zclassic-c23-dev:18252`);
  `mcp__zcl23-live__*` → live node (`~/.zclassic-c23:18232`). Confirm the target
  before acting. (As of 2026-07-03 the dev node was not running — height 0.)
- **Worktree workflow:** the repo uses parallel worktrees (`~/github/zclassic23-2`,
  `-3`, or `.claude/worktrees/`). For parallel agents that mutate files, give each
  its own worktree — they share the main worktree otherwise and collide (this drop
  hit that exact race: a review agent checked out a branch in the shared worktree).
- **Push discipline:** origin holds ONLY `main`, FF-only, never flattened. Prefer
  cherry-pick or rebase+FF over merge commits. After a green `make ci`, push is
  allowed (the hook re-runs `make ci`; if your tool times out at 10 min, run
  `make ci` to completion first, then `git push --no-verify` — that is the sanctioned
  bypass when you have just verified).
