# ADR-0001: Adopt the AI-Integrated Personal Sovereignty Stack vision

- **Status:** Accepted 2026-05-22.
- **Deciders:** Project maintainer.
- **Supersedes:** the audit-first master plan (archived as `~/.claude/plans/archive/zclassic23-plan-audit-first-2026-05-22.md`) and the 3-epoch revamp framing (`~/.claude/plans/archive/revamp-zclassic23.md`).

---

## Context

For two days (2026-05-20 → 2026-05-22) the project oscillated between three incompatible framings — audit-first, ruthless purge, demolition-only (each rejected below in Alternatives) — while the live node sat wedged at h=3,115,059 (legacy at 3,118,574+). Each framing produced a plan that contradicted the previous one's recent commits. The user named the failure mode in `feedback_dream_first_dont_flip_flop.md` and `feedback_less_is_more_holistic.md`. The maintainer then reframed it with: *"imagine the best thing, the best architecture zclassic23 could possibly be that would get you excited"* — turning "what should we delete?" into "what are we building?", answered in `~/.claude/plans/zclassic23-ideal-architecture.md`.

The wedge at h=3,115,059 is **not** treated as an emergency. It is the canonical example of the structural problem the new architecture solves by construction: chain progress becomes a stage cursor on disk, and "in-memory cache stale, watchdog firing but coordinator unable to decide" exits the state space entirely. Patching it now would be the whack-a-mole pattern the user explicitly rejected.

## Decision

Adopt the **AI-Integrated Personal Sovereignty Stack** vision and the layer-cake architecture in `~/.claude/plans/zclassic23-ideal-architecture.md` as the canonical destination. Operating mode: **build forward, transform in place.**

Concretely:

1. **Target architecture** = the L1–L7 layer cake. North star: *the database is the only truth; chain progress is a stage cursor on disk.*
2. **Positioning** = one ~16 MB dynamically-linked C23 binary on a $200 SBC providing bank + identity + inbox + market + swap + web host + games + AI co-pilot, all behind Tor.
3. **Mode** = build forward. Recent work is bedrock for the destination, not cruft to be torn out. Pure deletions are bounded at ~2,500 LOC (Wave F-1, five dead helpers + dormant mirror-consensus machinery + three deprecated import-orchestrator files).
4. **Roadmap** = six waves (F, S, M, Z, T, R) in `~/.claude/plans/zclassic23-plan.md`. The wedge dissolves naturally at the Wave S cutover; no targeted patch ships before then.

## Consequences

**Positive:**

- Recent shipped work — hexagonal skeleton (`ports/`, `adapters/`, `application/`, `domain/`), shadow feeder, supervisor primitive (Round 5), typed blocker (Round 6), 99 MCP tools — stays as bedrock. Months of work retain their value.
- The wedge stops being a recurring crisis; it becomes the first regression scenario for the Wave T simulator.

**Negative / Risk:**

- The live wedge persists until Wave S lands (estimated 2–4 weeks). Operator must accept the legacy `zclassicd` carrying tip-following duty in the interim.
- Build-forward churns more LOC than delete-then-rebuild, so the working tree gets temporarily larger before it gets smaller.
- Discipline required: any future session proposing "we should just delete X" against bedrock-listed modules must justify it against this ADR.

## Alternatives considered

**(a) Audit-first / permanent legacy authority.** Rejected: positions the hexagonal scaffolding as a verifier-of-truth forever and never lets it mature into the destination. Locks in a two-binary deployment shape that the Personal Sovereignty Stack positioning explicitly rejects (one binary, one onion, one stack).

**(b) Ruthless purge of recently-shipped work.** Rejected: deletes the hexagonal skeleton, shadow feeder, and supervisor work that just shipped. Demoralizing, wasteful, and the exact "schizo flip-flop" pattern the user named.

**(c) Demolition-only plan.** Rejected: framing as "what to delete" never produced an aspirational architecture, only a smaller version of the current shape with the same wedge class.

## References

- `~/.claude/plans/zclassic23-plan.md` — canonical master plan (replaces audit-first version).
- `~/.claude/plans/zclassic23-ideal-architecture.md` — vision / destination doc.
- `~/.claude/plans/archive/zclassic23-plan-audit-first-2026-05-22.md` — superseded plan, kept for archaeology.
- `~/.claude/projects/<this-repo>/memory/feedback_dream_first_dont_flip_flop.md` — user feedback that triggered the pivot.
- `~/.claude/projects/<this-repo>/memory/feedback_less_is_more_holistic.md` — companion feedback on the whack-a-mole pattern.
- `docs/FRAMEWORK.md` — canonical architecture doc. (The L1–L7 layer-cake framing this ADR introduced was superseded 2026-05-23 by the eight-shape framework; see FRAMEWORK.md and the REFACTOR_STATUS decision log.)
