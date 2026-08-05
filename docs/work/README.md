# Work directory and parallel-worktree workflow

This directory holds the active plans, design records, and the
parallel-worktree protocol. It is not itself a priority queue:

1. [`../HANDOFF.md`](../HANDOFF.md) owns current live facts.
2. [`FORWARD_PLAN.md`](./FORWARD_PLAN.md) is the sole ordered execution plan.
3. [`../MVP.md`](../MVP.md) owns the v1 acceptance contract.

The active #1 track is the sovereign complete-state cure. Architecture
cleanup remains off the v1 path unless the owner explicitly promotes an
item. If this file and `../HANDOFF.md` ever disagree, HANDOFF wins — fix
this file to match it.

Worktrees are dynamic; never infer current workers from a hard-coded path
list. Inspect them with `git worktree list --porcelain`. The checkout at
`~/github/zclassic23` is normally the orchestrator. Every other registered
checkout is a worker or an isolated quality lane and must be inspected
before removal; dirty worktrees are preserved.

## How a worker session starts

Run `pwd` and `git worktree list --porcelain`, then follow
[`agent-protocol.md`](./agent-protocol.md). The assignment owns the branch,
scope, verification, and completion ritual; directory suffixes are labels,
not a permanent server inventory.

## Index — one line per file, annotated

**Authority** column: **PLAN** = ordering authority, read first; **LIVE** =
describes a shipped mechanism/procedure, reference as needed; **DESIGN** =
still-open design record, read before touching the area, not a priority
queue; **RETAINED** = superseded as a current plan but code/tests/scripts
cite specific numbered items from it by name — the file stays for that
citation, `git log --follow -- docs/work/<name>.md` recovers older intent.

| File | Authority | Purpose |
|---|---|---|
| [`FORWARD_PLAN.md`](./FORWARD_PLAN.md) | PLAN | THE ordered execution plan (autonomous / owner-gated / operational) |
| [`self-verified-tip-plan.md`](./self-verified-tip-plan.md) | PLAN | the `G-SOV` sovereignty-gate design + open hardening items; `G-SOV` is the active gate in `sovereignty_controller.c` |
| [`SOVEREIGN-NETWORK-ROADMAP.md`](./SOVEREIGN-NETWORK-ROADMAP.md) | PLAN | durable Phase 0–6 hierarchy and promotion gates; ordering authority when other plans differ |
| [`sovereign-cutover-runbook.md`](./sovereign-cutover-runbook.md) | PLAN/LIVE | owner-gated live cutover + revert procedure for the bundle install path |
| [`ZCODE_DEVELOPMENT_NETWORK.md`](./ZCODE_DEVELOPMENT_NETWORK.md) | PLAN | active agentic C23 development-network contract: canonical task/evidence objects, real ZBuild worker, requester-led P2P work, typed create/use/improve, and durability lanes |
| [`ZCODE_SCIENTIFIC_METAVERSE.md`](./ZCODE_SCIENTIFIC_METAVERSE.md) | PLAN | owner-directed ZCODE scientific object, evidence-network, discovery, proof-of-contribution, committee, and staged-custody implementation plan; includes parallel file ownership and no-live-funds gates |
| [`ZCODE_PLAN.md`](./ZCODE_PLAN.md) | FOUNDATION | original 15-slice ZCODE package-hosting order; slices 1–13 remain live foundations, while payout slices 14–15 are deferred behind the development network; `lib/vcs/include/vcs/package_reward.h` cites its "ZCL fuel economics" section by name |
| [`MARKETPLACE_PLAN.md`](./MARKETPLACE_PLAN.md) | PLAN | owner directive: on-chain P2P ZSLP/ZCL marketplace (same-chain single-tx swap + cross-chain HTLC) over the existing ZSWP/ZSLP primitives; application protocol only, no consensus surface |
| [`shielded-history-importer.md`](./shielded-history-importer.md) | LIVE | reference for the shipped `-import-complete-shielded` operational cure; operational-vs-sovereign trust-mode split |
| [`CONSENSUS-STATE-BUNDLE.md`](./CONSENSUS-STATE-BUNDLE.md) | LIVE | naming/ownership authority for `zcl.consensus_state_bundle.v1` |
| [`fresh-start-seam.md`](./fresh-start-seam.md) | DESIGN | why a genuinely bare boot (empty datadir, isolated `$HOME`, no flags) reaches no state source and folds zero blocks: the two independent seams, every state source and the exact predicate that refuses it, and why the install gate is NOT circular at HEAD |
| [`never-stuck-plan.md`](./never-stuck-plan.md) | DESIGN | the wedge class this doc diagnosed is CURED; retained as the design record for the never-stuck hardening map + the per-height UTXO-ladder gap |
| [`fail-safe-architecture.md`](./fail-safe-architecture.md) | DESIGN | the progress law + universal repair ladder; absorbs `sticky-node-plan.md`'s invariants and `never-stuck-plan.md` §1b |
| [`sticky-node-plan.md`](./sticky-node-plan.md) | DESIGN/RETAINED | the stickiness invariants + gap analysis; §4 (the AAR/MTTUR metric) is cited by `tools/scripts/sticky_matrix.sh`/`sticky_fault_inject.sh` and the Makefile `sticky-matrix`/`sticky-matrix-v1` targets — keep §4 numbered as-is |
| [`canonical-frontier-derived-state-plan.md`](./canonical-frontier-derived-state-plan.md) | DESIGN | frontier-derived-state gates + heal-ladder deletion design; historical input, current gates are in `SOVEREIGN-NETWORK-ROADMAP.md` |
| [`ladder-carve-audit.md`](./ladder-carve-audit.md) | DESIGN | current per-file consumer graph for the borrowed-state ladder deletion; headline verdict: zero LOC deletable today |
| [`reducer-shielded-consensus-plan.md`](./reducer-shielded-consensus-plan.md) | DESIGN | anchor-membership + turnstile enforcement design (not implementation-ready — see its own §8 gaps) |
| [`concurrency-hazards-consensus-gated.md`](./concurrency-hazards-consensus-gated.md) | DESIGN | unfixed cross-thread hazards on the consensus/chain-advance path, boot-validation-blocked |
| [`refold-fold-rate-bottlenecks.md`](./refold-fold-rate-bottlenecks.md) | DESIGN | from-genesis refold fold-rate bottlenecks + fix order |
| [`tip-durability-collapse.md`](./tip-durability-collapse.md) | DESIGN | rationale of record for `coins_kv` as sole live UTXO author |
| [`wt-rom-fetch-engine.md`](./wt-rom-fetch-engine.md) | DESIGN | ROM-bundle fetch engine (client side of ROM delivery): trust model + open items |
| [`wt-zcode-s6-iterative.md`](./wt-zcode-s6-iterative.md) | LIVE | completed S6 iterative DHT assignment and acceptance receipt |
| [`wt-zcode-s6-hardening.md`](./wt-zcode-s6-hardening.md) | LIVE | completed S6 production-hardening assignment and exact gate receipt |
| [`wt-zcode-s7-discovery.md`](./wt-zcode-s7-discovery.md) | LIVE | completed S7 generic provider, local-sovereignty policy, replication, and root-only discovery assignment |
| [`wt-zcode-s7-1-replication.md`](./wt-zcode-s7-1-replication.md) | LIVE | completed S7.1 iterative record discovery, closest-node publication, possession-backed ACK, and provider-directed science proof lane |
| [`wt-zcode-s7-2-space-scout.md`](./wt-zcode-s7-2-space-scout.md) | PLAN | active S7.2 operational-integrity, read-only Sovereign Space and deterministic Scout lane |
| [`os-substrate-plan.md`](./os-substrate-plan.md) | DESIGN | OS-substrate three-rung plan (shell-out removal, `os_proc` shim, sandbox facade) |
| [`os/A1-authority-receipt-idiom.md`](./os/A1-authority-receipt-idiom.md) | DESIGN | the Law-7 privileged-transition authority-receipt idiom, cited by `tools/lint/check_privileged_transition_receipt.sh` |
| [`os/A4-noise-transport-p1.md`](./os/A4-noise-transport-p1.md) | DESIGN | the Noise v2 P2P transport implementation contract |
| [`os/A6-adaptive-client-puzzle.md`](./os/A6-adaptive-client-puzzle.md) | DESIGN | load-adaptive client-puzzle primitive design (not yet built) |
| [`NAT_AND_ONION_TRANSPORT.md`](./NAT_AND_ONION_TRANSPORT.md) | DESIGN | onion-as-universal-rendezvous / clearnet-as-fast-path transport design notes (NAT traversal, onion hosting, package swarm); P2P-layer policy only, no consensus surface |
| [`palace-design.md`](./palace-design.md) | DESIGN | code-legibility layer: file/group purpose, `code room`, the three P1/P2/P3 lint gates (§3 cited by `test_make_lint_gates.c`) |
| [`service-result-convergence.md`](./service-result-convergence.md) | LIVE | `struct zcl_result` convergence ratchet inventory + lane plan for `app/services/`; gate is live, this is the shrinking-floor inventory |
| [`secure-transport-design.md`](./secure-transport-design.md) | DESIGN | Noise_XX v2 transport protocol contract (implemented, default off) |
| [`wire-next-wave-specs.md`](./wire-next-wave-specs.md) | DESIGN | next-wave `simnet_wire` lane specs (eclipse/partition, bandwidth/reorder, app-layer flows) |
| [`session-substrate-probes.md`](./session-substrate-probes.md) | DESIGN | measured rootless-sandboxing capability probes for the multi-user-server program |
| [`LLM-C23-APP-PLATFORM-CHECKLIST.md`](./LLM-C23-APP-PLATFORM-CHECKLIST.md) | DESIGN | future LLM/App platform execution checklist (Phases 3–5); not the current execution queue, cannot displace the sovereign cure |
| [`agent-spend-policy-design.md`](./agent-spend-policy-design.md) | LIVE/RETAINED | scoped agent authority over digital assets — shipped as `agent_sessions` (migration v36) + `app/services/include/services/agent_spend_policy.h`; 16 `.c`/`.h`/test files cite its "Minting + presentation" and "Enforcement" sections by name, so keep those headings as-is |
| [`UX_PLAN.md`](./UX_PLAN.md) | LIVE | the two-lane UX program (shared server-rendered design system + terminal presentation); both lanes have landed, `tools/command/cli_render.h`, `tools/command/native_command.c`, `src/main_cli_modes.c` and `lib/test/src/test_cli_render.c` cite its "terminal lane" by name |
| [`HOTSWAP.md`](./HOTSWAP.md) | LIVE | the dev-only hot-swap mechanisms |
| [`fast-path.md`](./fast-path.md) | LIVE | the information algorithm + fast inner-loop commands for any change |
| [`agent-protocol.md`](./agent-protocol.md) | LIVE | worker startup/completion protocol (this file's companion) |
| [`test-result-cache.md`](./test-result-cache.md) | LIVE | content-addressed per-group test result cache |
| [`stopwatch-gates.md`](./stopwatch-gates.md) | LIVE | the C3 / net-disruption wall-clock stopwatch gates |
| [`coldstart-remote-peer-proof.md`](./coldstart-remote-peer-proof.md) | LIVE | the C3 stopwatch run against a REMOTE peer (`make mvp-coldstart-to-tip-remote`) and what it names |
| [`mvp-ci-map.md`](./mvp-ci-map.md) | LIVE | each MVP criterion → its mechanical CI check |
| [`mvp-live-gate.md`](./mvp-live-gate.md) | LIVE | `tools/mvp_gate.sh`, the live-node MVP probe companion to the CI map |
| [`sim-phase2-plan.md`](./sim-phase2-plan.md) | LIVE | the in-memory simulation network reference |
| [`io-harness-design.md`](./io-harness-design.md) | LIVE | the `simnet_wire` adversarial network-IO harness design |
| [`GROTH16-SPEND-PARITY.md`](./GROTH16-SPEND-PARITY.md) | LIVE | native Sapling spend-circuit differential parity scoreboard |
| [`tenacity-roadmap.md`](./tenacity-roadmap.md) | RETAINED | superseded as a roadmap by `FORWARD_PLAN.md` + `self-verified-tip-plan.md` (`docs/TENACITY.md` remains the standing doctrine); items 3/5, the "Hold-class doctrine" and "Stability hardening backlog" sections, and §4's seal/window design are cited by name from reindex/replay-canary/`seal_kv` code and `tools/scripts/check_blocker_remedy.sh`/`reindex_smoke.sh`/the Makefile |
| [`parity-audit-round2-findings.md`](./parity-audit-round2-findings.md) | RETAINED | superseded audit, retained — findings L1/L2/L3 cited by the consensus-parity lock-in tests |
| [`consensus-parity-supplemental-audit-2026-06-08.md`](./consensus-parity-supplemental-audit-2026-06-08.md) | RETAINED | superseded audit, retained — §2 item 5 cited by `docs/AGENT_TRAPS.md`; landed-fix summary condensed into `docs/CONSENSUS_PARITY_DOCTRINE.md` |
| [`lint-gate-hollowness-audit.md`](./lint-gate-hollowness-audit.md) | RETAINED | the fail-loud-scan-floor lint-gate pattern, cited by `tools/lint/gate_lib.sh` and its self-test |

This table covers **every** tracked file in this directory. Reconcile it after
adding one — `git ls-files docs/work/` minus the paths linked above must be
empty, and an index that does not list everything is an index that lies.

Recover any prior version of a file in this directory with
`git log --follow -- docs/work/<name>.md`.

Before deleting anything here, clear all three: it is classified superseded
above (PLAN / LIVE / DESIGN / RETAINED are not deletion candidates — DESIGN
means still-open, RETAINED means deliberately kept for a by-name citation);
`git grep -n "<name>.md" -- "*.c" "*.h"` is empty; and
`git grep -n "<name>.md" -- "*.md"` is empty. A comment citation does not fail
`check-error-doc-refs` (it only reads string literals) but it is still a
load-bearing pointer for the next reader.

That third check is not theoretical. Seven files already deleted from this
directory are still named from code, tests, and other docs, and nothing fired:

```sh
git grep -ho 'docs/work/[A-Za-z0-9_./-]*\.md' -- '*.c' '*.h' '*.md' '*.sh' Makefile |
  sort -u | while read -r p; do
    git ls-files --error-unmatch "$p" >/dev/null 2>&1 || echo "DANGLING: $p"
  done
```

At the time of writing that prints seven names — two under a since-removed
`archive/` subdirectory (lb1-wiring-design, sovereign-service-roadmap) plus
`coin-backfill-repair.md`, `parallel-state-compiler.md`,
`sync-organism-map.md`, `worktree-cleanup-2026-07-16.md` and
`wt-phase4c-block-index-projection.md` — cited from
`app/jobs/src/stage_repair_coin_backfill*.c`,
`app/jobs/include/jobs/psc_range_fold.h`,
`lib/storage/include/storage/coins_kv.h`, `tools/scripts/worktree_gc.sh`,
`docs/AGENT_TRAPS.md` and six test files. Run the check before deleting, and
repoint or drop the citation in the same commit.

## Active control documents

- **Current work:** [`FORWARD_PLAN.md`](./FORWARD_PLAN.md) (THE plan),
  [`self-verified-tip-plan.md`](./self-verified-tip-plan.md) (the cure
  spine), [`CONSENSUS-STATE-BUNDLE.md`](./CONSENSUS-STATE-BUNDLE.md) (bundle
  naming/ownership authority), and
  [`sovereign-cutover-runbook.md`](./sovereign-cutover-runbook.md) (install
  runbook).
- **Current architecture:** [`../FRAMEWORK.md`](../FRAMEWORK.md) (reference,
  off the v1 path — §9 is the open-item debt board, which self-labels NOT
  the v1 path).
- **Session entrypoint:** [`../HANDOFF.md`](../HANDOFF.md).
- **Worker protocol:** this file plus [`agent-protocol.md`](./agent-protocol.md).

## Worker protocol

Each assignment lives at `docs/work/wt<N>-<slug>.md` and contains:

- **Branch name** — exact name to create
- **Scope** — files this assignment owns; files it must NOT touch
- **Dependencies** — other assignments that must complete first
- **Tasks** — ordered, testable steps
- **Acceptance criteria** — concrete tests that prove done
- **Commit + push instructions** — exact git commands
- **Completion ritual** — what to append at the end

## Conflict avoidance

- **Disjoint file scope**: each assignment lists exact files it owns; no
  other assignment may touch those files until it merges.
- **No concurrent edits to** `../FRAMEWORK.md` §9 (the debt board): only
  orchestrator writes it. Workers append to their own assignment doc.
- **Integrate deliberately**: follow the current operator skill and
  assignment instructions; do not assume every dirty checkout can safely
  rebase or push.

## Failure modes

- **Worker discovers assignment is wrong or impossible** → worker appends a
  `BLOCKED` section to its assignment doc with details, pushes, reports to
  user. Orchestrator session must respond.
- **Worker's tests fail** → worker does NOT merge; pushes a `WIP` branch +
  appends a `FAILED` section with the failing test output.
- **Two workers touch overlapping files (should not happen)** →
  second-to-merge rebases, orchestrator session resolves.
