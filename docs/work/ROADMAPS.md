# Roadmap index

This repo has accumulated many dated roadmap docs. This index says which ones
are LIVE (read and act on) and which are SUPERSEDED. It elaborates
[`../HANDOFF.md`](../HANDOFF.md) §6 (the entry point); if the two ever
disagree, HANDOFF wins — fix this file to match it. Superseded roadmaps are
removed from the tree, not archived in-repo; recover any of them from Git
history with `git log --follow -- docs/work/archive/<old-name>.md`.

## LIVE — read these

- **[`FORWARD_PLAN.md`](./FORWARD_PLAN.md)** — THE plan. MVP-anchored,
  autonomous / owner-gated / operational critical path. Start here.
- **[`self-verified-tip-plan.md`](./self-verified-tip-plan.md)** — the
  sovereign-cure spine (the self-verified UTXO/anchor rebuild that deletes
  the borrowed zclassicd-minted seed path).
- **[`never-stuck-plan.md`](./never-stuck-plan.md)** — the wedge-class cure
  design (silent-halt-unreachable-by-construction machinery); its own header
  marks the 2026-07-12 wedge diagnosis historical and defers gate authority
  to `SOVEREIGN-NETWORK-ROADMAP.md`, but the hardening map itself is still
  the live design record.
- **[`SOVEREIGN-NETWORK-ROADMAP.md`](./SOVEREIGN-NETWORK-ROADMAP.md)** — the
  durable Phase 0–6 hierarchy and promotion gates (cited at the top of
  `../HANDOFF.md`); ordering authority when other plans differ.
- **[`fast-sync-to-tip-plan-2026-07-16.md`](./fast-sync-to-tip-plan-2026-07-16.md)**
  — the operational (`release_assisted`) import-path cure design, proven on a
  datadir copy per `../HANDOFF.md` §0-NEWEST; complements, does not replace,
  the sovereign cure above.
- **[`sovereign-cutover-runbook.md`](./sovereign-cutover-runbook.md)** and
  **[`canonical-cutover-runbook-2026-07-16.md`](./canonical-cutover-runbook-2026-07-16.md)**
  — owner-gated live cutover + revert procedures for the sovereign-bundle and
  import-path cures respectively. Nothing in either auto-executes.

## SUPERSEDED — removed from the tree, history only

| Removed doc | Superseded by |
|---|---|
| `architecture-deletion-plan.md` | `docs/FRAMEWORK.md` §9 (deletion targets mostly landed) |
| `next-wave-plan.md` | `FORWARD_PLAN.md` |
| `never-stuck-roadmap-2026-06-28.md` | `never-stuck-plan.md` |
| `sovereign-service-roadmap.md` | `FORWARD_PLAN.md` (sovereign-contracts follow-on tracks there now) |

Recover any removed roadmap with `git log --follow -- docs/work/archive/<old-name>.md`
and `git show <commit-before-removal>^:docs/work/archive/<old-name>.md`. Dated
one-shot audits, postmortems, and superseded session handoffs were removed the
same way and are not individually indexed here — they were point-in-time
records, not roadmaps. Example (2026-07-19 fold): `ed25519-ubsan-analysis-
2026-07-18.md` and `tsan-triage-2026-07-18.md` folded into `docs/BUILD.md`'s
sanitizer sections as known-good-as-of notes; `install-e2e-proof-2026-07-18.md`
folded into `sovereign-cutover-runbook.md`'s "Known-good:
-install-consensus-bundle end-to-end proof" section — recover the originals
the same way if the forensic file:line detail is ever needed again.

## SUPERSEDED but RETAINED — still cited by name from production/test code

These roadmap/audit/design docs are superseded as *current plans* (their
ordering authority moved to `FORWARD_PLAN.md` / `self-verified-tip-plan.md` /
`docs/TENACITY.md`), but code comments across the tree cite specific numbered
items or findings from them by name, so deleting the file would leave those
citations unresolvable. They stay at `docs/work/<name>.md` (not archived):
`tenacity-roadmap.md` (reindex/replay-canary/seal_kv code — items 3/5/7, §4),
`sticky-node-plan.md` (watchdog/escalator code — items #1/#9a/#9b/#10/#13, §4),
`session-substrate-probes.md` (`lib/platform/` sandbox code), `lint-gate-hollowness-audit.md`
(`tools/lint/gate_lib.sh` + its lint-gate self-test), `parity-audit-round2-findings.md`
(consensus-parity lock-in tests, items L1/L2/L3), `consensus-parity-supplemental-audit-2026-06-08.md`
(`docs/AGENT_TRAPS.md` §2 items 3/5 — JoinSplit Ed25519 sig + `fCoinbaseMustBeProtected` gating),
`contextual-check-wiring-spec-2026-06-08.md` and `security-audit-response-2026-06-09.md`
(script-validate wiring + third-party security-audit disposition),
`stability-improvements-2026-06-16.md` (near-term hardening sequencing cited
from `tenacity-roadmap.md`), and `hold-class-audit-2026-07-10.md` /
`tip-durability-collapse.md` (the blocker-meta-detector and coins_kv
durability rationale — the only two the 2026-07-14 purge already flagged as
"deferred deliberately").
