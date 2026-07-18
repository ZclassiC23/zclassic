# Hold-class audit — reducer fail-closed holds (2026-07-10)

**Scope:** every fail-closed *hold* in the reducer stage code under
`app/jobs/src/` — the `utxo_apply`, `script_validate`, `proof_validate`, and
`coin_backfill` stages (and their helpers). A **hold** is any site that refuses
to advance a stage cursor and pins H\* (the reducer-frontier provable tip),
whether by naming a typed blocker (`lib/util/blocker.h`) or by a bare
fail-closed refusal.

**Doctrine under audit (the stickiness invariant):** *a stall must always be a
NAMED blocker WITH an auto-terminating remedy.* A typed blocker that holds H\*
with an **empty `escape_action`** and **no auto-remedy condition** — and no
honest owner-gate rationale — is a defect class. This session's live P0
(`utxo_apply.anchor_backfill_gap`, empty escape, sat silent for hours) was one
instance; Lane 0 cures that instance, this Lane 2 audit enumerates the class and
Lane 2's meta-detector (`app/conditions/src/blocker_stall_meta_detector.c`)
backstops it generically.

Branch `dev/blocker-meta-detector` @ `96a4ff63b`. Every row below was
spot-verified by reading the cited code.

## Blocker-id → escape/remedy table

| # | Hold site (file:line) | Blocker id | Class | `escape_action` set? | Auto-remedy condition | Honest owner-gate? (why) |
|---|---|---|---|---|---|---|
| 1 | `proof_validate_stage.c:155` | `proof_validate.internal_error` | PERMANENT (budget −1) | **empty** | none | Weak: 600 s of bounded re-derive (`PV_UNRESOLVED_BUDGET_SECONDS`) that self-clears if the transient resolves, then pages `EV_OPERATOR_NEEDED` once. Reason text calls it a "transient … fault that did not clear" → a *transient-described* fault held PERMANENT. **See defect D2.** |
| 2 | `proof_validate_stage.c:485` | `params_missing` | PERMANENT | empty | none in-scope (owned by `boot.c` `load_params_thread`) | Yes — proof_validate only re-surfaces a blocker the loader already declared for corrupt/missing Sapling params on disk; operator fixes the params files. |
| 3 | `script_validate_stage.c:207` | `script_validate.prevout_unresolved` | PERMANENT (budget −1) | empty | **`reducer_frontier_reconcile_light`** (indirect: it drives `coin_backfill` to re-derive the missing creating coin; the same prevout then resolves and `sv_unresolved_clear()` fires) | Only if `coin_backfill`'s own ladder terminally refuses (sites 4a–4c). |
| 4a | `stage_repair_coin_backfill_util.c:582` (`COIN_BACKFILL_OWNER_REFUSED`) | `coin_backfill.owner_gate` | DEPENDENCY | **empty** (escape text lives in `reason`, not the dispatchable field) | none — this **is** the terminal leaf of `reducer_frontier_reconcile_light`'s repair attempt | Yes — env ack `ZCL_REDUCER_COIN_BACKFILL_ACK=1` required each restart. |
| 4b | same, `COIN_BACKFILL_REFUSED_UNPROVABLE` | `coin_backfill.unprovable.<h>` | DEPENDENCY | empty | none | Yes for durably-persisted terminal markers ("fetch deep body"); **NO** for the two explicitly-RETRYABLE cases that reuse this same status without persisting a marker (`scan_gap` `:521`, `window_over_budget` `:529`) — those self-heal. |
| 4c | same, default (`REFUSED_SPENT`/`MARKER_SEEN`) | `coin_backfill.<h>` | PERMANENT | empty | none | Yes — proven double-spend / lost-coin / consensus-divergence class; "refuse, never guess". |
| 5 | `utxo_apply_anchors.c:252` | `utxo_apply.anchor_backfill_gap` | PERMANENT | empty | **`sapling_anchor_frontier_unavailable`** (3-tier: seed verified frontier → arm refold → owner-gated) — Lane 0 | Only tier 3 (no verified frontier artifact + no refold artifact, or a genuine below-cursor historical gap). |
| 6 | `utxo_apply_nullifiers.c:290` | `utxo_apply.nullifier_backfill_gap` | PERMANENT | **empty** | **NONE** | Documented owner-gate (backfill or from-genesis resync). **See defect D3 — no auto-remedy path exists in-tree at all.** |
| 7 | `utxo_apply_stage.c:226` | `utxo_apply.apply_failed` | TRANSIENT (budget 5 / 60 s) | empty | none directly (the specific gap classes route to 5/6; genuine consensus rejects are final by design) | Depends on `summary.status`: a genuine reject is final; a gap-class reject is framed by the anchor/nullifier blockers. Bounded retry + self-clear on next clean advance. |
| 8 | `utxo_apply_stage.c:258` | `utxo_apply.fatal_store` | PERMANENT | empty | none | Yes — fires only on genuine store corruption / unrecoverable read failure; durable so `chain_tip_watchdog` classifies it `permanent_blocker_active` instead of burning restart budget. Operator repairs/reindexes. |
| 9 | `utxo_apply_stage_observe.c:166` | `reducer_frontier.upstream_log_hole` | DEPENDENCY | **YES → `reducer_frontier_reconcile_light`** | **`reducer_frontier_reconcile_light`** (exact name match) | No — fully automatic, no deadline; the healthy reference shape. |
| 10 | `utxo_apply_stage_observe.c:233` | `utxo_apply.label_splice` | TRANSIENT (budget 5 / 60 s) | empty | none (self-heals) | No — self-heals as soon as `script_validate` re-binds the verdict to the active-chain block (normal re-org re-derivation). |

## Counts

- **Total distinct typed-blocker hold sites in scope:** 12.
- **With an auto-remedy condition (auto-terminating self-cure path):** 4 —
  sites 3, 5, 9, plus 7/10 self-heal on a clean advance / re-derivation
  (bounded, not a named condition). Only site 9 wires a dispatchable
  `escape_action`; sites 3 and 5 are healed by a condition that keys on other
  state, not on the blocker's escape field.
- **Honestly owner-gated (documented rationale, terminal by design):** 7 —
  sites 2, 4a, 4b (marker case), 4c, 6, 8, and 1 (page-terminal).
- **`escape_action` populated (dispatchable via the supervisor):** **1 of 12**
  (site 9). Every other hold names the fact but wires no escape callback — the
  `chain.blocker_escape` supervisor sweep has nothing to dispatch for 11/12
  holds. This is exactly why the generic meta-detector backstop is needed: an
  empty `escape_action` is the norm, so a hold whose *condition* also goes
  missing (the P0) is silent.
- **DEFECTS (neither an auto-terminating remedy NOR an honest, registry-visible
  named-blocker story):** see below.

## Defects

### D1 — untyped fail-closed holds (value-overflow repair owner-gate) — **primary defect**

`app/jobs/src/utxo_apply_delta_repair.c` refuses the `value_overflow` repair via
**four bare flags, with NO `blocker_set` anywhere in the file**:

- `:245` `local.author_refused` (`author != stage`)
- `:256` `local.owner_refused` — gated on `ZCL_REDUCER_VALUE_OVERFLOW_REPAIR_ACK`
- `:277` `local.cursor_stale_refused`
- `:346` `local.walk_torn_refused`

These propagate up as `struct utxo_apply_value_overflow_repair_result` fields;
`reducer_frontier_reconcile_light.c:388` only `LOG_WARN`s
`rr.value_overflow_repair_owner_refused` and returns `COND_REMEDY_FAILED`.

**Why it is a defect:** this is a genuine fail-closed hold that can pin H\* (the
value-overflow tear persists until the owner acks), yet it is **not a named
typed blocker** — invisible to `zclassic23 core sync blockers`, to `zclassic23 dumpstate
subsystem=blocker`, and therefore to the Lane 2 meta-detector, which keys on the
blocker registry. It violates the doctrine's *"a stall must always be a NAMED
blocker."* The directly analogous `coin_backfill.owner_gate` (site 4a) **does**
name a typed blocker for its equivalent env-ack gate — so this is an
inconsistency, not an intentional design.

**Fix (follow-up):** give the value-overflow owner-gate (and the three sibling
refusals) a typed blocker exactly like `coin_backfill_page_refusal` — a
`DEPENDENCY` blocker `utxo_apply.value_overflow_owner_gate` with the ack env in
the reason. That makes the hold registry-visible and meta-detector-eligible in
one step. Sibling `cursor_stale`/`walk_torn` refusals are transient re-derive
classes → `TRANSIENT` with a bounded budget.

### D2 — `proof_validate.internal_error`: transient fault held PERMANENT with no self-cure escape

Site 1. The fault is described as transient (`sapling_ctx` alloc / verifier
fault), gets 600 s of bounded re-derivation, then is named **PERMANENT** with an
**empty `escape_action`** and a single page — no auto-terminating self-cure
(e.g. a bounded supervised restart to clear a transient allocator/verifier
fault). If the transient never clears on its own, H\* is pinned until an
operator intervenes.

**Why it is (soft) defect territory:** a *transient-described* fault with no
auto-terminating remedy is the stickiness anti-pattern; a genuinely irreducible
internal error deserves the page, but the two are not distinguished. The Lane 2
meta-detector now backstops the H\*-freeze (arms the escalator → its deepest
rung is the real refold-from-anchor, and pages naming the blocker), so this is
no longer *silent*. A cleaner fix is a bounded-restart escape wired to
`proof_validate.internal_error`.

### D3 — `utxo_apply.nullifier_backfill_gap`: owner-gated but ZERO auto-remedy exists

Site 6. Honestly owner-gated in its reason text (requires a shielded-history
backfill or from-genesis resync), so it is **not** a silent/undocumented hold —
it does not meet the strict "neither remedy nor rationale" defect bar. But it is
the strongest **meta-detector target**: PERMANENT, empty escape, and **no
auto-remedy path exists in the tree at all** (cf. memory
`project_nullifier_backfill_gap_2026-07-09`: the real fix is a populate-only
nullifier walker, owner-gated + copy-proven, which must NOT touch the check
path). Until that lands, the Lane 2 meta-detector is this hold's only
auto-terminating response (arm escalator + page). Listed here so the class is
tracked, not lost.

## What the Lane 2 meta-detector covers vs. does not

- **Covers:** every empty-`escape_action` *typed* blocker (sites 1, 2, 4a–4c,
  5, 6, 7, 8, 10) that freezes H\* past the window (default 15 min) — it arms
  the sticky escalator and pages, naming the offending blocker id, with zero
  per-id knowledge. It is a backstop, never a substitute for an instance cure
  (site 5's Lane 0 cure, site 9's healthy condition wiring).
- **Does NOT cover:** D1's untyped holds — they are not in the blocker registry.
  Fixing D1 (typing the value-overflow owner-gate) is the one change that brings
  a currently-invisible hold class under both `zclassic23 core sync blockers` and the
  meta-detector. That is the highest-value follow-up from this audit.
