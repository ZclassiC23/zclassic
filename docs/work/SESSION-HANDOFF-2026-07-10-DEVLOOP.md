# Session handoff — instant dev loop + strong-node wave (2026-07-10, evening)

Entry point for the next session / next developer. Read `docs/HANDOFF.md` first for live-node
truth, then this file for the dev-loop wave state. Verify everything against the live nodes and
`git log` — heights and in-flight states below rot fast.

## Shipped and PUSHED (origin/main @1bed282c4)

- **Tier-1 in-process hot-swap** (`lib/hotswap/`, `zcl_agent_hotswap` MCP tool, atomic
  `g_routes[]` re-point, `make hotswap FILES=...`): edit→live ~1.9s on the persistent `-mcp` dev
  process, node never leaves tip. Dev-lane only; release stays 100% static (gates:
  `check-hotswap-dev-only`, source-level; note `vendor/lib/librustzcash.a` pre-existingly pulls
  glibc dlopen symbols into the release link — Rust std internals, NOT our loader; a raw `nm -D`
  scan is therefore NOT the purity gate).
- **Hot-swap phase 2**: REST + diagnostics trampolines (`api_controller_dispatch.c`,
  `diagnostics_dispatch.c` — resident atomic provider slots), `config/hotswap_eligible.def`
  manifest + two seeded-violation-proven gates (`check-hotswap-eligible-scope`,
  `check-hotswap-static-state`), loader test group. Docs: `docs/work/HOTSWAP.md`.
- **Tier-2 P0+P1**: full `[boot]` sub-marker instrumentation (hotspots now visible:
  finalize_and_build 65.6s, restore_tip 12.1s, repair_relink 13.8s) + `PRAGMA quick_check` skip
  on verified-clean shutdown marker v2 (size + SQLite change-counter + schema bound, single-use)
  + post-READY background quick_check.
- **Fast tests always**: `test_parallel` built from cached per-TU objects — 1-file edit 92s→2s,
  full suite ~100-160s wall; header edits depfile-tracked (was FALSE-GREEN before). `make t
  ONLY=<group>` is the single-group runner; `make test-parallel` is the full suite.
- **Gate robustness**: `check_test_registration.sh` no longer converts a failed (rc>=2) membership
  grep into a phantom orphan (bit us once under post-suite load).
- **Impact-rule mappings** for all new surfaces (pre-push CI needs them; extend
  `app/controllers/include/controllers/agent_impact_rules.def` when adding files).

## Merged locally, push pending post-merge suite (main @7ee9a8738)

- **Blocker stall meta-detector** (`app/conditions/src/blocker_stall_meta_detector.c`): generic
  backstop — ANY active typed blocker with empty `escape_action` + H* frozen >900s (uptime
  clock, movement-reset hysteresis, at-tip guard) → arms sticky escalator + pages, names the
  blocker. Witness = H* climbs past detect baseline. 12 hermetic tests. Verified MERGE-READY
  (adversarial verifier; false-positive analysis clean).
- **Hold-class audit** `docs/work/hold-class-audit-2026-07-10.md`: 12 typed-blocker hold sites;
  only 1 wires a dispatchable escape_action. DEFECTS for follow-up, highest value first:
  - **D1**: `app/jobs/src/utxo_apply_delta_repair.c` :245/:256/:277/:346 hold H* with NO typed
    blocker at all — invisible to `zcl_blockers` AND the meta-detector. Fix = type these holds.
  - **D2**: `proof_validate.internal_error` — transient-described fault held PERMANENT, no
    self-cure escape.
  - **D3**: `utxo_apply.nullifier_backfill_gap` — owner-gated with zero in-tree auto-remedy
    (ties to the populate-only nullifier walker, see memory `nullifier-backfill-gap`).

## In flight at session end (branches on disk; agents die with the session)

- **`dev/shutdown-drain` @1701c2450 (wt3 ~/github/zclassic23-3) — COMMITTED, verifier was
  mid-run.** ROOT CAUSE (a classic): glibc `signal()` under `_POSIX_C_SOURCE` without
  `_DEFAULT_SOURCE` = System V ONE-SHOT semantics — the FIRST SIGTERM ran the handler and reset
  disposition to SIG_DFL; systemd ExecStop's SECOND pulse (2s) killed the node silently
  mid-drain, before WAL checkpoint + marker. Fix: `sigaction` (no SA_RESETHAND) in `src/main.c`;
  durability-first reorder in `boot_services.c` (marker BEFORE the ~465MB block-index flat
  save); `deploy/zcl23-dev.service` drops the 3-pulse ExecStop (live/soak units still carry it —
  owner-gated follow-up). Fixture-proven by the lane (triple SIGTERM → marker + next-boot skip;
  kill -9 → unclean path intact). NEXT: re-run/finish independent verification → merge →
  `make deploy-dev` + INSTALL THE UPDATED UNIT (`deploy/zcl23-dev.service` →
  `~/.config/systemd/user/` + `systemctl --user daemon-reload`) → live double-restart proof:
  first stop writes marker, second boot logs `[boot] quick_check skipped (verified-clean
  shutdown)`.
- **`dev/anchor-tier1b` (wt4 ~/github/zclassic23-4) — WIP, checkpoint-commit requested.** The
  cure for BOTH nodes' current wedge (dev pinned h=3,175,499; live h=3,176,325 since ~07:40):
  borrow sapling anchor tree from zclassicd's chainstate LevelDB via the in-tree legacy reader,
  verify root == PoW-committed `hashFinalSaplingRoot`, seed `anchor_kv`, wired as tier-1b in the
  `sapling_anchor_frontier_unavailable` ladder. Copy-prove fixture (preserved, DO NOT DELETE):
  `~/.zclassic-c23-COPY-20260710-135017-sapling-anchor-cure2`, boot with
  `-paramsdir=$HOME/.zcash-params`, GATE: H* climbs past 3,176,326. See wt4 NOTES/WIP commit for
  exact resume point.
- Both wedges page as `sapling_anchor_frontier_unavailable` tier3 — this is the cure working as
  designed-but-incomplete (named + paged instead of silent), NOT a new regression.

## Remaining roadmap (approved plan `~/.claude/plans/we-need-to-think-tidy-charm.md`)

1. Land shutdown fix live (above), then tier-1b → dev node unwedges → H*-climb proof completes.
2. OWNER CALL: push+deploy cured binary to LIVE (wedged since ~07:40Z; deploying pre-tier-1b
   converts silent wedge to paged wedge only).
3. D1 (highest-value defect), then D2/D3.
4. Tier-2 P2 `fast_restart` wiring (~35-39s; design in the plan file §C) → P5 RPC-before-P2P →
   P3 skip-forward-pass → P4 MMB persist. New hotspot: finalize_and_build 65.6s.
5. Ownership docs: AGENT_TRAPS.md (hot-swaps are ephemeral previews; leaked generations are
   deliberate; marker is single-use), DEV-WATCH-PLAN.md supersession note, MODE=hotswap in
   tools/dev/watch-dev-lane.sh.

## Traps hit this session (don't re-learn)

- Plan mode PROPAGATES into running subagents — exec lanes silently no-op into "writing plans".
  After ExitPlanMode, check every lane has a real diff and re-instruct.
- Subagents parking on background monitors that never fire — tell them to run gates
  synchronously.
- `make t` now REQUIRES `ONLY=`; the full suite is `make test-parallel`.
- A failed grep (rc>=2) inside a gate script must FATAL, not report a violation (fixed in
  check_test_registration.sh; audit other gates on touch).
- Backgrounding with `(...) &` inside a run_in_background Bash detaches the inner job from
  completion notifications — arm an explicit `until grep EXIT=` watch.
