# Session handoff — 2026-07-09 (late): simnet_wire + the orchestration pattern

Resume guide for the next Claude. Verify live state with the node's own tools
before trusting any claim here (`build/bin/zclassic23 mcpcall zcl_status`, or the
`mcp__zcl23-dev__*` / `mcp__zcl23-live__*` tools).

## TL;DR

`origin/main` is at **`7fa940ac2`** (verify: `git log --oneline -1 origin/main`),
clean and pushed. This session shipped **simnet_wire steps A + B** (the
deterministic in-memory P2P wire-simulation harness — the mission) plus four
hardening/DRY lanes and a live-defect fix, all through the pre-push CI gate. The
work was driven by a **Fable-orchestrates / codex-in-Haiku-implements /
Sonnet-verifies** subagent pattern documented below — that pattern *is* the
deliverable the owner asked to preserve.

## The orchestration pattern (READ THIS — it's the point)

Owner directive: Fable coordinates; heavy implementation is delegated to weaker
models and to the **codex CLI (GPT-5.5 xhigh) wrapped inside a Haiku subagent**
to save Fable tokens; run **workflows of subagents** in parallel.

### The proven shape (both workflows this session used it)

`Workflow` script with a `pipeline(LANES, implStage, verifyStage)`:

1. **impl stage** — a **Haiku** agent, `isolation: 'worktree'`, whose whole job
   is mechanical orchestration of the **codex CLI**, which does the actual code
   writing. Returns a structured `IMPL_SCHEMA` object (branch, per-gate
   pass/fail, files_changed, summary, blockers). The Haiku runbook:
   - `git checkout -b <branch>`; `cp -a /home/rhett/github/zclassic23/vendor/lib
     ./vendor/` (worktrees can't link without the prebuilt vendor libs);
   - write the task spec to a `/tmp` file, then run codex **in the background**
     (`Bash run_in_background:true`, 10–30 min):
     `codex exec --dangerously-bypass-approvals-and-sandbox -C "$PWD" "$(cat /tmp/task.md)"`
     — the bypass flag is **required**: codex's own bubblewrap sandbox cannot
     nest inside the agent sandbox (`bwrap: setting up uid map` otherwise);
   - run the gates itself (`make lint`, `make -j`, focused `test_zcl <group>`),
     feed failures back to codex (max ~3 rounds), commit when green, **no push**.
2. **verify stage** — an independent **Sonnet** agent in *its own* worktree that
   re-runs every gate (never trusts the implementer's claims), reads the full
   diff against an acceptance bar, and returns `merge_ready | needs_work`. This
   stage earned its keep both times: it caught a stray committed lint-gate
   fixture that broke `make lint`, and a real cross-lane `printf`-vs-`LOG_*`
   merge collision. **Always keep the verify stage.**
3. **Fable (orchestrator)** then inspects the diff itself, merges `--no-ff` in
   dependency order, rebuilds the *merged* tree (the individual lanes were never
   built together), runs lint + focused tests, and pushes.

Reusable templates saved in-repo (edit + re-invoke, don't retype):
- `docs/work/workflow-template-lane-implement-verify.js` — the generic
  N-lane implement→verify pipeline (this session's lanes A/B/C + next-wave 3/4).
- `docs/work/workflow-simnet-wire-bc.js` — sequential dependent stages
  (step B must merge before step C branches off it).

Memory files with the durable rules:
`reference_codex_cli_in_subagents_recipe`, `feedback_model_tiering_token_efficiency`,
`reference_worktree_vendor_libs_gotcha`, `project_session_state_2026-07-09`.

### Tiering
- **Fable** = orchestration, merge, final review, cross-lane collision guards.
- **Haiku** = the codex runner + mechanical/log-triage/status work.
- **codex GPT-5.5 xhigh** (inside Haiku) = the heavy code writing.
- **Sonnet** = independent verify (rerun gates, judge merge-readiness), and
  scoped diagnosis-first lanes.
- **Opus** = hard deep-debug (e.g. the txkit SIGSEGV lane).

### Gotchas the pattern hits (all real this session)
- **Pre-push gate needs a test mapping.** Changed files with no impact-rule
  mapping fail the pre-push `fast-ci` with "no focused test mapping"; pass
  `ZCL_FAST_TESTS=<group[,group]>` (or extend
  `app/controllers/include/controllers/agent_impact_rules.def`). `lib/sim/*` has
  no mapping yet — a good follow-up.
- **Agent worktrees clobber `core.hooksPath` to an absolute path** (shared git
  config), which fails `make lint`'s `check-git-hooks-installed` in the MAIN
  repo. Fix: `git config core.hooksPath tools/githooks`.
- **Build the merged tree before pushing.** Each lane built in isolation; the
  three-way merge is a config nothing compiled. Always `make -j` + focused tests
  after merging, before the push.
- **Never run two `make deploy*` concurrently.**

## What shipped to origin/main this session

Pushed in sequence, each through the pre-push CI gate:
- **simnet_wire step A** (`45d5ff…` lineage) — deterministic in-memory wire
  transport per `docs/work/io-harness-design.md`: real framed bytes flow through
  the node's actual `p2p_node_receive_bytes` ingress and `send_head` egress with
  **no sockets, no threads**, entropy/clock only via `seed_tape`; same-seed runs
  reproduce a byte-identical FNV delivery fingerprint. Real version/verack
  handshake + ping/pong round-trip. Files: `lib/sim/src/simnet_wire.c`,
  `lib/sim/include/sim/simnet_wire.h`, `lib/test/src/test_simnet_wire.c`.
- **simnet_wire step B** (`7fa940ac2`) — seed-driven adversarial peers
  (MALFORMED_FRAME, BAD_HANDSHAKE, FLOOD, SLOWLORIS) + per-tick monitors:
  recv-queue bounded, misbehaving peers banned/disconnected, no unexpected
  permanent blocker, memory plateau, and a bonus **consensus-unchanged** monitor
  (tip hash + UTXO commitment pinned and asserted unchanged by any adversarial
  input). `lib/sim/src/simnet_wire_peer.c`, `simnet_wire_internal.h`.
- **Gossip blob-read hardening** — fail-closed length checks before copying
  fixed-size fields out of peer-gossip rows (swap_contract/file_offer/znam/zmsg).
- **printf→LOG_\*** in boot/recovery/validation + params_init + explorer_stats —
  keeps `-mcp` stdio JSON clean, diagnostics to node.log.
- **Legacy oracle envelope dedup** + `client.c` Content-Length/snprintf-truncation
  fix (fails closed instead of sending a length that disagrees with the body).
- **Explorer error-page emitter** + shared read-only DB-open consolidation.
- **Sticky-escalator fixes** (verified LIVE defect): `clear_episode()` now
  **withdraws** a pending non-terminal `auto_reindex_request` once `tip > anchor`
  (the residual marker had blocked `make deploy-dev`), and the belt-and-suspenders
  auto-arm is scoped to `condition_engine_get_unresolved_critical_count()` so a
  lingering WARN condition can't cycle the chain-recovery ladder on a healthy node.
  This root-causes the "dev keeps raising reindex markers" recurring issue.

## In flight at handoff (background lanes — will NOT survive a restart)

Relaunch these; branches persist in the worktrees / object store:
- **simnet_wire step C** (byzantine wire bridge) — `sim/wire-byzantine-c`,
  NOT committed. Codex got it building + lint-clean but left **18 test failures**:
  reject_reason not captured, misbehaving peer not banned, and the
  consensus-unchanged monitor tripping (suggests the observation loop mutates
  UTXO state instead of reading it). Relaunch `docs/work/workflow-simnet-wire-bc.js`
  with those three failure modes written explicitly into the step-C spec, or
  hand it to Opus. Steps D (eclipse/partition), E (replay/reorder/bandwidth),
  F (nightly `wire_sweep` + capsule save + grep security-gate) still to queue —
  see `docs/work/io-harness-design.md` "Build order".
- **`test_simnet_txkit` SIGSEGV under `test_parallel`** — branch
  `fix/txkit-parallel-segv` (Opus lane). CONFIRMED real: passes standalone
  (15/15), SIGSEGVs in the full 497-group parallel run at the P2PKH-send check,
  reproduced 2×. Almost certainly cross-group global-state pollution in the
  shared worker process. This is exactly the class the simulator exists to catch
  — worth finishing.
- **`SYNC_AT_TIP` never reached** — branch `fix/sync-at-tip-transition`
  (diagnosis-first). `sync_get_state()` stays `SYNC_BLOCKS_DOWNLOAD` forever on
  an at-tip node though `SYNC_AT_TIP` is defined; enumerate consumers before
  flipping the transition (blast radius).

Also `fix/sticky-reindex-residue` is already merged (its content is on main);
the branch can be deleted.

## Next mission steps (owner priority order)

1. Land wire step C (byzantine injection), then D/E/F — completes the
   adversarial-network harness.
2. Then the standing owner directive: **drive the app-layer flows (ZSLP token
   txs, ZName registration, escrow, Sapling) through the wire harness
   end-to-end, fully simulated, BEFORE any real-ZCL operation.** simnet_wire now
   gives you a real node reachable over a real (in-memory) wire — mint coinbase,
   fund addresses, and push OP_RETURN / Sapling flows through real
   `connect_block` validation with adversarial peers present.
3. Add a `lib/sim/*` mapping to `agent_impact_rules.def` so sim changes get an
   automatic focused-test mapping at push time.

## Current live state (verify, don't trust)

- Live node (`~/.zclassic-c23`, RPC 18232): healthy, at network tip (~3.17M),
  running the prior deployed build. `mcp__zcl23-live__*` datadir was corrected in
  `~/.claude.json` this session (was pointing at `-fullhist`); should read live
  now — confirm with `mcp__zcl23-live__zcl_status`.
- Dev lane (`~/.zclassic-c23-dev`, RPC 18252): a background `make deploy-dev` of
  `7fa940ac2` was launched at handoff (log:
  session-scratchpad `deploy-dev.log`) — verify it finished and is at tip with
  `build/bin/zclassic-cli -datadir=$HOME/.zclassic-c23-dev -rpcport=18252 getblockcount`
  and check the running build_commit.
