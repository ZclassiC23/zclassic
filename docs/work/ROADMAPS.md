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
  G-SOV sovereignty-gate design record; its own header marks the wedge PROVEN
  cured (`../HANDOFF.md` §0-LATEST) via a different concrete mechanism than
  this plan's Act 3 (checkpoint-content bundle install, not a self-derived
  anchor + borrowed-seed deletion — Act 3's literal steps remain undone).
  Kept live for the G-SOV predicate (still the active sovereignty gate in
  `sovereignty_controller.c`) and the still-open Act 2/4 hardening.
- **[`never-stuck-plan.md`](./never-stuck-plan.md)** — the wedge-class cure
  design (silent-halt-unreachable-by-construction machinery); its own header
  marks the 2026-07-12 wedge diagnosis historical and defers gate authority
  to `SOVEREIGN-NETWORK-ROADMAP.md`, but the hardening map itself is still
  the live design record.
- **[`SOVEREIGN-NETWORK-ROADMAP.md`](./SOVEREIGN-NETWORK-ROADMAP.md)** — the
  durable Phase 0–6 hierarchy and promotion gates (cited at the top of
  `../HANDOFF.md`); ordering authority when other plans differ.
- **[`fast-sync-to-tip-plan-2026-07-16.md`](./fast-sync-to-tip-plan-2026-07-16.md)**
  — the operational (`release_assisted`) import-path cure design
  (`-import-complete-shielded`, `shielded_history_import_service.c`,
  `sovereignty_controller.c`'s self_folded gating). Proven to clear the wedge
  on a datadir copy, but **not** the path that actually cured the live node —
  the sovereign bundle install (below) passed the wedge live on 2026-07-19
  (`../HANDOFF.md` §0-LATEST). Kept live because it documents currently-shipped
  code (the importer + the operational-vs-sovereign trust-mode split), not
  because it is still the active cure track.
- **[`sovereign-cutover-runbook.md`](./sovereign-cutover-runbook.md)** —
  owner-gated live cutover + revert procedure for the sovereign-bundle cure;
  this is the path that actually passed the wedge (`../HANDOFF.md`
  §0-LATEST, 2026-07-19).

## SUPERSEDED — removed from the tree, history only

| Removed doc | Superseded by |
|---|---|
| `architecture-deletion-plan.md` | `docs/FRAMEWORK.md` §9 (deletion targets mostly landed) |
| `next-wave-plan.md` | `FORWARD_PLAN.md` |
| `never-stuck-roadmap-2026-06-28.md` | `never-stuck-plan.md` |
| `sovereign-service-roadmap.md` | `FORWARD_PLAN.md` (sovereign-contracts follow-on tracks there now) |
| `cure-runbook-2026-07-16.md` (2026-07-19 fold) | `sovereign-cutover-runbook.md`'s "Independent replay-receipt authority (`-verify-consensus-bundle=PATH`)" section (the receipt binary-binding gotcha) |
| `canonicalization-backlog.md` (2026-07-19 purge) | none — all 6 backlog rename items shipped (`_ex` symbols no longer exist in-tree) |
| `canonical-cutover-runbook-2026-07-16.md` (2026-07-19 doc-rot fold, N4) | `sovereign-cutover-runbook.md` — its own owner-gated live-cutover procedure was never executed; the sovereign bundle install passed the wedge instead (`../HANDOFF.md` §0-LATEST) |
| `contextual-check-wiring-spec-2026-06-08.md` (2026-07-19 doc-rot fold, N4) | `docs/CONSENSUS_PARITY_DOCTRINE.md` "Landed parity fixes" section (the rule→zclassicd map); the gating rationale itself was already duplicated in `docs/AGENT_TRAPS.md` |
| `security-audit-response-2026-06-09.md` (2026-07-19 doc-rot fold, N4) | `docs/SECURITY_AND_INTEGRITY.md` "Concrete safeguards" section |
| `stability-improvements-2026-06-16.md` (2026-07-19 doc-rot fold, N4) | `tenacity-roadmap.md`'s "Stability hardening backlog" section |
| `hold-class-audit-2026-07-10.md` (2026-07-19 doc-rot fold, N4) | `tenacity-roadmap.md`'s "Hold-class doctrine" section |

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
(`docs/AGENT_TRAPS.md` §2 items 3/5 — JoinSplit Ed25519 sig + `fCoinbaseMustBeProtected`
gating; `docs/AGENT_TRAPS.md` is not editable in the normal doc-rot sweep, so this
file stays even though its own condensed summary now also lives in
`docs/CONSENSUS_PARITY_DOCTRINE.md`'s "Landed parity fixes" section), `tip-durability-collapse.md` (the coins_kv durability rationale, flagged
"deferred deliberately" in the 2026-07-14 purge), and
`canonical-frontier-derived-state-plan.md` (cited by
`app/services/src/utxo_recovery_stale_cursor.c`, `app/jobs/src/utxo_apply_delta.c`,
`app/jobs/include/jobs/reducer_frontier.h`, and `app/models/src/database_schema.c`
for the two-invariant frontier-bounded/coins-bounded design it records — its own
header marks the wedge it was written against as CURED, current gates are in
`SOVEREIGN-NETWORK-ROADMAP.md`). `contextual-check-wiring-spec-2026-06-08.md`,
`security-audit-response-2026-06-09.md`, `stability-improvements-2026-06-16.md`,
and `hold-class-audit-2026-07-10.md` were in this category until the 2026-07-19
doc-rot sweep (N4) re-pointed their few code citations at permanent homes
(`docs/CONSENSUS_PARITY_DOCTRINE.md`, `docs/SECURITY_AND_INTEGRITY.md`,
`tenacity-roadmap.md`) and removed them — see the SUPERSEDED table above.
