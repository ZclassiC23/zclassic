# Archive evidence index

This is a ledger of historical narratives removed from the working tree on
2026-07-14. It is evidence, not a plan and not current runtime state. Current
truth remains in `docs/HANDOFF.md`; the only ordered queue is
`docs/work/FORWARD_PLAN.md`.

Every old path below had zero basename or path references outside
`docs/work/archive/` immediately before removal. Unique commits, artifact
locations, and terminal verdicts were retained here; the full prose remains in
Git history:

```bash
git log --follow -- docs/work/archive/<old-name>.md
git show <commit-before-removal>:docs/work/archive/<old-name>.md
```

Artifact paths are historical observations, not promises that the files still
exist. Never operate on a live datadir from this ledger.

## Removed narratives

| Date | Old path | Topic | Retained evidence and terminal verdict |
|---|---|---|---|
| 2026-06-20 | `docs/work/archive/sync-keystone.md` | Fast-sync/UTXO trust proposal | The proposed auxiliary MMR/MMB `utxo_root` cannot bind peer-provided state to ZClassic PoW because headers commit neither root. Treat the design as superseded; assisted state must remain restricted until locally re-derived. Earlier transparent-loader cure `ab512d577` did not prove shielded history. |
| 2026-06-21 | `docs/work/archive/recovery-selfheal-redteam-2026-06-21.md` | Recovery red-team | Workflow receipt `wf_82a0127d-dda`. Earlier missing-snapshot blocker was superseded by the borrowed snapshot/window cure `ab512d577`; boot reindex exhaustion was bounded by `186eb74e4`. Historical artifact `/tmp/utxo-anchor-3056758.snapshot` was not the complete-state sovereign cure. Surviving principle: kill-9, swallowed-replay, and full-binary recovery claims require fixture proof. |
| 2026-06-21 | `docs/work/archive/sync-wedge-dissection-2026-06-21.md` | Borrowed-seed forward-sync wedge | Root cause: bulk-copying `coins_kv` and stamping a cursor skipped per-height reducer evidence, creating two authorities. `ab512d577` plus a complete borrowed snapshot above the old wedge restored that instance, but did not prove snapshot contents. Terminal cure remains one-writer local fold of complete transparent and shielded history. |
| 2026-06-24 | `docs/work/archive/act3-seed-anchor-caller-classification.md` | Seed-anchor deletion classification and XOR audit | Dated classification kept live-fold, regtest, and assisted-snapshot callers; marked legacy block-index/reindex seeders for deletion only after the cure. Its claim that `rpc_blockchain_maybe_commit()` had zero runtime callers is stale: `config/src/boot_services.c` later gained one. Re-audit before deletion; the 2026-07-14 consolidation review records the contradiction. Related historical commits: `b2aba3bd7`, `b2482a6ff`. |
| 2026-06-25 | `docs/work/archive/detective-forward-plan.md` | Re-derive-over-frozen-state plan | Superseded first by `sticky-node-plan.md` and then the current sovereign-cure plan. Useful receipts: header-solution repair diagnosis, shipped refold fixes `f311482ae`, `c22baa278`, `9fef4f683`, `079c3a42d`, and state snapshot at `2d8c6e70b`. Historical rollback binary was `~/.local/bin/zclassic23-live.rollback-16de79a5d`; do not assume it remains present. |
| 2026-06-27 | `docs/work/archive/code-review-2026-06-27.md` | 35-lane full-code review | Reported 322 findings: 0 critical, 22 high, 154 medium, 146 low. Its actionable subset was revalidated into retained `code-review-remediation-2026-06-30.md`; current structural priorities are in `CODEBASE-CONSOLIDATION-REVIEW-2026-07-14.md`. Introduction commit `55a254b1c`. |
| 2026-06-29 | `docs/work/archive/commitment-audit-wedge-cure-2026-06-29.md` | Commitment-audit false HOLD | Live build `6b9fa108c` was pinned at H*=3,164,075 because a rebuildable `utxos` projection/XOR audit latched a `chain_linkage` HOLD. Cure `4ef91c5c0` decoupled the advisory audit from consensus finalize, added bounded ownership, and preserved real fold/SHA3 gates. Copy proof was required to show H* climb, not merely a clean boot. |
| 2026-06-29 | `docs/work/archive/commit-audit-cure-review-2026-06-29.md` | Independent deploy review of the commitment cure | Verdict GO, with two same-build hardenings: serialize growth-path checkpoint writes under `progress_store_tx_lock` without a nested `BEGIN`, and correct contradictory co-commit comments. Structural linkage/coinbase/window/mirror holds were explicitly outside this audit's decoupling. Implementation commit `4ef91c5c0`. |
| 2026-06-29 | `docs/work/archive/coin-hole-3164483-next-2026-06-29.md` | Missing-coin repair at block 3,164,483 | Copy proof at `/home/rhett/.zclassic-c23-COPY-20260629-224110-terminal-backfill-proof` re-proved the stale marker, inserted the creator-height 3,164,371 coin, and climbed H* from 3,164,482 to 3,164,627. Historical repro copy: `~/.zclassic-c23-wcommit`. Terminal verdict: patched, copy-proven, deployed, and observed past the blocker; the long txid beginning `dabd990f` is transaction evidence, not a Git commit. |
| 2026-06-30 | `docs/work/archive/code-quality-audit-2026-06-30.md` | Rolling code-quality implementation diary | Merged fixes and deploy receipts are represented by Git, notably `29329bffe`, `e2db5ca87`, `57ce22e78`, `5b4888096`, `b615dee88`, `6e5e60f6a`, and `f0cd0be9a`. Remaining structural themes were freshly re-audited into `CODEBASE-CONSOLIDATION-REVIEW-2026-07-14.md`; stale live heights and file:line claims are not retained as authority. |
| 2026-07-09 | `docs/work/archive/SESSION-HANDOFF-2026-07-09.md` | Simulator foundation and wallet-persistence incident | Session tip `0b8cb4ec8`. The wallet P0 described here was unresolved at handoff and was superseded by the later delta/write-lane work. The durable result is the `lib/sim/simnet` foundation; transient `handoff/*` branch instructions are not current. |
| 2026-07-09 | `docs/work/archive/SESSION-HANDOFF-2026-07-09-EVENING.md` | Wallet P0, simulator phase 2, parser fuzzing | Session tip `b119b339e`; first integration checkpoint `41b9f159d`. Durable commits include `8a1b91884` (UTXO mirror delta), `1b2d4e9dd` (catch-up write lane), and `ee1cf50e2` (owner-gated nullifier backfill). Canonical deployment remained owner-gated. |
| 2026-07-09 | `docs/work/archive/SESSION-HANDOFF-2026-07-09-LATE.md` | `simnet_wire` A/B and orchestration notes | Session tip `7fa940ac2`. Durable result: deterministic in-memory wire transport and adversarial peers. The documented background lanes and scratch logs were ephemeral; later workflow instructions supersede the model/plugin-specific orchestration recipe. |
| 2026-07-09 | `docs/work/archive/SESSION-HANDOFF-2026-07-09-CODEX.md` | Shielded-anchor, proving, wallet, and vendor stabilization | Based on `d8d9b32e9`; deployment verdict HOLD because historical anchors were incomplete and wallet publication/rollback ownership races remained. Pinned external librustzcash source revision: `06da3b9ac8f278e5d4ae13088cf0a4c03d2c13f5`. No live service/datadir mutation was authorized by the handoff. |
| 2026-07-09 | `docs/work/archive/S2-utxo-mirror-delta.md` | Incremental UTXO mirror and node.db lock holder | `8a1b91884` replaced an O(1.3M)-row rebuild-per-block with bounded idempotent deltas. The handoff diagnosed a separate long write transaction in boot catch-up; routing that work through the write lane landed as `1b2d4e9dd`. The narrative predated copy proof/deployment and is not current runtime evidence. |
| 2026-07-10 | `docs/work/archive/GROTH16-C23-PROVER-CURE.md` | Pure-C23 Sapling prover investigation | Branch `fix/groth16-c23-prover`: output circuit count and density-filtered MSM defects were fixed, but branch tip `ddc832a4e` still had a false positive round-trip and the spend circuit was not started. Ground-truth artifacts were stored on branch at `reference/groth16-traces/{output_circuit,spend_circuit}.trace` and `reference/groth16-trace-harness/`. External oracle pin `06da3b9ac8f278e5d4ae13088cf0a4c03d2c13f5`; main's interim verified bridge landed at `f70b368dd`. |
| 2026-07-10 | `docs/work/archive/SESSION-HANDOFF-2026-07-10-P2P.md` | P2P services and proving wave | Main tip `720547d73`; durable bridge `f70b368dd`, sim fix `5861dd8d8`, and shielded-send sim fix `0f938a86a`. Historical unmerged evidence: Groth16 `13b557742`/`ddc832a4e`, storefront `500dd86fe`, MCP-over-onion `9b4ff1465` (explicitly unverified; do not merge from this ledger). |
| 2026-07-10 | `docs/work/archive/SESSION-HANDOFF-2026-07-10-DETECTIVE.md` | Detective-node reliability wave | Git range `3b0de63b0..82c128c62`, now in main. Key commits: terminal refold rung `ecb22fccb`, rowless-hole detector `5bbaf043d`, oracle-dead P2P repair `b93c94f16`, copy-prove contract `933a18d54`, and golden ladder `57855352d`. Unmerged Groth16 spend branch `cf27ad2f3` was historical WIP, not an accepted artifact. |
| 2026-07-10 | `docs/work/archive/SESSION-HANDOFF-2026-07-10-OPERATOR-SNAPSHOT.md` | Target-owned operator truth | Landed as `2b617f58d`. Native `operatorsnapshot` became the one-builder truth contract; old-target compatibility was fail-closed. Focused gates `mcp_controllers` and `syncdiag_rpc` were recorded, but the handoff explicitly required remaining mapped groups/lint before deploy and authorized no canonical restart. |
| 2026-07-10 | `docs/work/archive/SESSION-HANDOFF-2026-07-10-WAVE3.md` | Sapling-frontier stall, watchtower, and peer hardening | Main commits include `c76e40ec3`, `caeaf81bb`, `d290bff62`, `2d3a7152b`, `b67e2fcdf`, `57d661f9f`, `abdebf7c5`, and `a32f44fb1`. Copy proof failed the climb gate: first=max H*=3,176,325 for 1,200s; refusal/page behavior was honest but did not cure the state. Historical fixture: `~/.zclassic-c23-COPY-20260710-135017-sapling-anchor-cure2`, gated on climb past 3,176,326. |
| 2026-07-11 | `docs/work/archive/SESSION-HANDOFF-2026-07-11-ZERO-MCP.md` | Zero-MCP W0 and W1-A | W0 handler composition and W1-A command-handler snapshot were merged and gated; introduction/handoff commit `faed30cea`. W2/W3 execution truth remains `MCP-REMOVAL-WORKLIST.md`, not this session narrative. |
| 2026-07-11 | `docs/work/archive/SESSION-HANDOFF-2026-07-11-W1BC.md` | Zero-MCP W1-B/C native hot-swap retarget | The document was written before commit from `~/github/zclassic23-w1bc`; the completed change landed on main as `88b4e1030`. It introduced the `native.leaves` provider and command-registry batch replacement. Remaining W2/W3 work belongs to the current worklist; transient worktree instructions are obsolete. |

## Purge receipt

- Selection rule: tracked Markdown under `docs/work/archive/`, with no
  case-insensitive basename/path match in any tracked path outside that
  directory.
- Removed: 22 narratives, 346,315 bytes, 5,350 lines before this ledger and
  the inbound-reference consolidation.
- Preserved: commits, branch tips, unique fixture/artifact paths, terminal
  verdicts, and explicit warnings above. Git retains every original narrative.
- Link policy: preserved archive pages point here instead of linking to removed
  files; unrelated pre-existing broken links were not broadened into this
  cleanup.
