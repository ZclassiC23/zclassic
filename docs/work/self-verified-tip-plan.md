# The Self-Verified Tip Plan — the detective's forward plan

> **PROVEN LIVE (2026-07-19) — the G-SOV property this plan targets is now
> real on the serve node** (`docs/HANDOFF.md` §0-LATEST: AT NETWORK TIP on
> self-verified state, past the historical shielded-anchor wedge). Read the
> mechanism carefully before citing this file as "the cure landed exactly as
> designed": the live cure passed the wedge via the
> **checkpoint-content consensus-bundle install** path
> (`docs/work/sovereign-cutover-runbook.md`,
> `config/src/consensus_state_snapshot_install_checkpoint_authority.c`), which
> proves the same G-SOV predicate (coins reproduce the compiled SHA3
> checkpoint + Sapling frontier roots to the validated header) a different way
> than Act 3 below specifies. **Verified against HEAD (2026-07-19):** Act 3's
> literal steps are NOT done — `REDUCER_FRONTIER_TRUSTED_ANCHOR` is still a
> hardcoded `3056758` (`app/jobs/include/jobs/reducer_frontier.h:66`, not
> self-derived), `-refold-from-anchor` is still an explicit opt-in flag, not
> the cold-start default (`src/main.c:4151`, `config/src/boot.c:3904`), and
> the borrowed-seed loader is not deleted. Act 2's vehicle
> (`-fold-inram`/`storage/coins_ram.h`) exists and is wired into the
> from-genesis mint path (`src/main.c:4186-4190`) — treat as PARTIAL, not
> gate-proven here; verify against `refold-fold-rate-bottlenecks.md` before
> citing it as closed. This file is retained as the **design reference** for
> the G-SOV predicate (still the live sovereignty gate,
> `app/controllers/src/sovereignty_controller.c`) and for the still-open
> hardening in Acts 2–4; it is no longer an in-progress execution log — read
> `docs/HANDOFF.md` for current status, not the Act markers below.

> **One metric: T = time-to-self-verified-tip.** Wall-clock from a cold or
> stalled start until `(tip, utxo)` is backed by PoW + the node's *own* fold,
> with **zero borrowed trust**. *Fast*, *secure*, and *never-stuck* are three
> readings of the same T.

This is the canonical statement of the **sovereign cure** that
[`FRAMEWORK.md`](../FRAMEWORK.md) §0 names as "the destination in flight, not
the proven present." It is the same plan referenced by
[`FORWARD_PLAN.md`](./FORWARD_PLAN.md) #1 (sovereign cure) and
[`never-stuck-plan.md`](./never-stuck-plan.md) — restated in the
language of the Prime Directive and the Ten Laws. (The predecessor
`archive/sync-fix-plan-2026-06-21.md` was removed from the tree; recover
with `git log --follow -- docs/work/archive/sync-fix-plan-2026-06-21.md`.)

Drafted, then adversarially stress-tested against the repo (ordering
feasibility, false-green gates, already-shipped work, deletion/parity safety) —
corrections folded in. **Verify fresh before acting; specifics rot.**

---

## The frame — the detective *is* the Prime Directive

The Prime Directive (`FRAMEWORK.md` §0): *keep `(tip, utxo)` equal to the
network's best valid chain — or name, precisely, the one input it is missing.*
In plain language: **a detective who has all the evidence names the suspect, or
names exactly which clue he is missing — he never just stops.**

The node already holds every clue (block bodies on disk) and a locally pinned
case-file checkpoint (the baked SHA3 checkpoint @3,056,758, recorded at
`lib/chain/src/checkpoints.c:86-104`). The checkpoint is not a ZClassic-header
state commitment; sovereignty comes from reproducing it from validated bodies.
The one defect: it reads conclusions
off **index cards** — persisted `*_log.ok` cursors and a *borrowed* `coins_kv`
copy minted by `zclassicd` (`app/services/src/utxo_recovery_restore.c:369`) —
instead of re-deriving from the evidence, because re-deriving is currently too
slow to do as a reflex.

**Every halt and every slowdown is the same disease:** trusting a *frozen
artifact* (a stale verdict row, a borrowed copy, a cached pointer) over a cheap
re-derivation from bodies + the baked checkpoint. That is exactly the failure
`FRAMEWORK.md` §0 says the architecture exists to remove ("a second write path,"
"authoritative chain state in RAM," a "self-healer that could lie"). The cure is
the one property: **every served fact is a re-derivation, never a frozen row or
a borrowed copy.**

---

## Aesthetic alignment — each act to the shapes and the Ten Laws

| Act | Shape(s) touched | Ten Laws it serves | Prime-Directive property advanced |
|-----|------------------|--------------------|-----------------------------------|
| **0** Bootable copy | Service (boot), Model (sapling tree) | **6** (checkpoint, don't tear ordering to fail-fast), **10** (forward progress is the bar) | The sapling rebuild derives its endpoint from the durable cursor (`coins_applied_height`), not a projection that ran ahead. |
| **1** Re-derive verdicts | **Condition** (`{detect, remedy, witness}`), Job (stage) | **7** (a remedy/verdict that lies is forbidden — a persisted `ok=0` that means "couldn't tell" is the inverse lie), **6** (a transient verdict is re-derivable) | A "can't tell" never becomes a permanent "invalid"; the healer is symmetric across script + proof. |
| **2** Cheap re-read | Domain / validation core (pure), Storage (coins overlay) | **4** (pure core replays fast), **8** (DRY the knowledge, *not* the cycles — no indirection in the hot loop), **9** (mutable inside under clear ownership — fix the overlay UAF) | Re-derivation from bodies becomes fast enough to be the default reflex. |
| **3** Self-mint anchor + delete borrow | **Model / Storage authority** (`coins_kv`, one writer), the subtraction | **2** (one way in — delete the second seed path), **8** (one source of truth per fact), **10** | Removes the borrowed trust root: `(tip, utxo)` becomes fully self-derived. **This is FRAMEWORK §0 "second honesty" made true.** |
| **4** Weld the lid | Enforcement (§5 gates), Condition | **10** (never weaken a gate; a hollow gate *is* a weakened gate), **2** (one way out) | The Prime-Directive property (`G-SOV`) becomes a gate the build enforces; a refactor cannot quietly re-borrow. |

---

## Crosscutting rules (every act)

- **Copy-prove, never live-first.** Live is deploy-blocked / un-rebootable
  (sapling rebuild FATALs pre-fold). Every act is proven on a datadir *copy*.
- **Gate on H\* CLIMB, not "booted w/o FATAL."** Real signal:
  `reducer_frontier_provable_tip_cached()` climbing past the wedge, with
  `coins_applied_height == H* + 1` (no rowless/stamped span).
- **Sovereignty is the 3-part `G-SOV` gate, not tip-reachability** — a node can
  climb H\* to the network tip on a *stamped borrowed* `coins_kv` and pass a
  naive gate while still resting on borrowed trust.
- **Parity replay only where a predicate changes.** These acts change
  state/derivation/perf, not consensus predicates → no fork expected. The one
  predicate in flight (coinbase-maturity default-off, `utxo_apply_delta.c:60`)
  stays walled off; flipping it needs a full-history replay (h=478544 lesson).

---

## ACT 0 — Discharge the detective (make a bootable copy) · **✅ LANDED (`9fef4f683`, verified 2026-07-11)**

> The sapling-tree rebuild resolves its endpoint from coins-applied state
> (`app/controllers/src/sync_controller_sapling_tree.c:46-71`: caps `chain_tip`
> to `coins_best = coins_applied_height − 1`), so a copy boots without the
> pre-fold sapling FATAL. Minting (`-mint-anchor`, `config/src/boot_mint_anchor.c:62`)
> and the #1/#3 fold-rate wins are also LANDED — see
> [`refold-fold-rate-bottlenecks.md`](./refold-fold-rate-bottlenecks.md). The
> remaining sovereign-cure gate is producing a verified `utxo-anchor.snapshot`
> (mint in flight) then the refold cutover; the #2 scheduler ceiling (~50 blk/s)
> is what makes both slow.

- **Detective beat:** before re-interviewing anyone, he has to get back in the
  room — the door is locked.
- **The literal move:** on a copy, make the sapling tree rebuild resolve its
  endpoint from coins state, not the dangling header tip.
  `sapling_tree_rebuild` reads `active_chain_height()`
  (`app/controllers/src/sync_controller_sapling_tree.c:42`), set at boot
  (`config/src/boot.c:3226`) *before* the fold → points at header tip ~3157703
  while active tip is 3157646 → tip NULL → `tip_missing_sapling_root` FATAL
  (`7ffb3be68`, fail-closed). **Not a one-liner:** the rebuild replays only
  `BLOCK_HAVE_DATA` blocks (`:158`), so plumb `coins_applied_height` (or
  `min(coin_best, header_tip)` capped to HAVE_DATA) in as the endpoint — OR
  clear headers above `coins_applied_height` first. Also carry the fail-closed
  guard into the cold-refold path (`config/src/boot_refold_staged.c:886-908`
  currently only *logs* a sapling mismatch).
- **Gate (`G-SOV`):** on the copy, H\* > 3156171, `coins_applied_height == H*+1`,
  tip hash matches `zclassicd`.
- **False-green & stronger gate:** "boots to RPC" ≠ fold succeeded (a pre-fold
  and a post-RPC FATAL both "booted"). Gate on H\* climbing past the wedge
  (`test_reducer_forward_progress_gate` PART-1 pattern), not RPC reachability.

## ACT 1 — Stop trusting bad index cards · **SHIPPED**

- **Detective beat:** a witness once said "I can't tell" under bad lighting; he
  wrote "unreliable" and never re-interviewed him. Fix: re-interview.
- **Landed (script path):** a transient `internal_error` is retriable, not
  terminal; the heal deletes both `script_validate_log` and
  `proof_validate_log` in the same transaction, rewinds downstream cursors, and
  leaves genuine script-invalid verdicts terminal. Earlier "heal never clears
  upstream logs" framing is **stale — false at HEAD**.
- **Landed (proof path):** `proof_validate_log.status='internal_error'` is now
  symmetric with script validation. The replay ladder detects a proof-only
  transient where `script_validate` already passed, deletes the stale
  `proof_validate_log` rows, rewinds proof/tip state, records the one-shot
  marker, and leaves `proof_invalid` / script-owned failures terminal. Focused
  proof: `make t ONLY=reducer_frontier_reconcile_light` covers the readable
  stale-proof production route, and `test_stage_repair` covers the direct
  proof-internal-error rewind hook.
- **Gate:** H\* climbs past 3157647 on the Act-0 copy.
- **False-green & stronger gate:** a re-read stale verdict looks like a
  re-derived one. Current tests assert the stale row is removed/rewound; future
  Act-0 copy proof should still gate on H\* climb past the wedge with fresh
  proof rows re-derived by the forward stage, not merely on the repair function
  returning success.

## ACT 2 — Make re-reading cheap (the measured speed root) · **PARTIAL — see top-of-file note**

- **Detective beat:** re-reading the file is the right reflex, but it takes all
  night — so he cheats with cards. Make re-reading fast.
- **The literal move:** the dominant refold cost is the per-block ~3.1M-node
  `pprev`-walk in `active_chain_fill_window`
  (`lib/validation/src/chainstate.c:350-391`, via
  `reducer_extend_window_to_candidate`), ~76% of refold CPU (measured —
  `docs/work/refold-fold-rate-bottlenecks.md`). Build a **refold-gated** fast
  path: under refold, fill the window from coins state / a cached pointer; under
  normal boot, keep the conservative iteration so the boot path is
  byte-identical (Law 8: don't add hot-loop indirection; Law 9: clear
  ownership). The `coins_ram` overlay (`-fold-inram`, `37f431aaa`) is the
  vehicle, **but its cross-thread reader UAF must be fixed first.**
- **Gate:** (1) fold-rate up; (2) H\* CLIMB; (3) normal boot **byte-identical**
  (`coins.db` checksum vs HEAD).
- **False-green & stronger gate:** rate can tick up from smaller batches while
  the walk still burns ~76%. Quantitative gate: `active_chain_fill_window` CPU
  fraction **drops below 40%** on a flamegraph. Profile before claiming done.

## ACT 3 — Notarize his own case file (the prize) · **PARTIAL ~80%**

- **Detective beat:** stop carrying a copy of someone else's case file.
  Re-derive it from the evidence once, notarize it yourself, burn the borrowed
  copy.
- **The literal move (3 unlock changes, then the deletion):**
  1. **Self-derive the anchor** — fold genesis→3,056,758 vs the baked checkpoint
     (`config/src/boot_mint_anchor.c:152-186`); replace the hardcoded
     `REDUCER_FRONTIER_TRUSTED_ANCHOR=3056758`
     (`app/jobs/include/jobs/reducer_frontier.h:66`, line drifted from `:39`
     as of 2026-07-19 — re-grep before editing) with the self-derived value
     at its call sites (`app/jobs/src/reducer_frontier.c:451-457,554`).
  2. **Flip the default** — make `-refold-from-anchor` the default cold start
     (today needs a CLI flag, `src/main.c:4151` as of 2026-07-19, drifted from
     `:1793`; default boot imports the borrowed seed via
     `utxo_recovery_import_ldb`, `config/src/boot.c:2845`, drifted from
     `:2487` — re-grep before editing).
  3. **Delete the borrow** — remove the `coins_kv` seed copy
     (`utxo_recovery_restore.c:369`) and the ~9k-LOC carve in dependency order
     per the removed `archive/architecture-deletion-plan.md` (recover with
     `git log --follow -- docs/work/archive/architecture-deletion-plan.md`).
- **9-caller caution (corrected from 5):** `tip_finalize_stage_seed_anchor` has
  **9 production callers**. KEEP the consensus-critical ones
  (`app/services/src/reducer_ingest_service.c` live fold; the from-anchor
  `boot_refold_staged.c`; `snapshot_apply.c`); DELETE cold-import ones
  (`block_index_loader_rebuild.c`, `reindex_epilogue.c`) after the cure ships.
  Classify each (Law 1: know the shape) before cutting — deleting a KEEP caller
  trips a live predicate.
- **Gate:** a **FRESH** datadir (no `~/.zclassic`) folds genesis→tip, reproduces
  the SHA3 root + 1,354,769 UTXOs; copy-prove `G-SOV`; *then* delete. Write
  `test_self_folded_anchor` (does not exist yet).
- **False-green & stronger gate (two traps):**
  - *Borrowed-seed no-op:* `-load-verify-boot` no-ops on a stamped `coins_kv`
    (`coins_kv_is_proven_authority()==true`). Defeat: `G-SOV` part 3 — assert a
    from-anchor reset ran (`coins_kv_contains_refold_marker()`).
  - *Baked-checkpoint byte-match:* the fresh test can pass by reading the
    *compiled* checkpoint. Defeat: a `_no_binary_checkpoint` variant — null the
    compiled checkpoint, fold from bodies, recompute SHA3, assert it equals the
    original (bodies are the authority, not the binary).
  - Don't delete behind a default-false flag (it lingers). Delete outright + add
    a fail-loud lint gate: `grep 'utxo_recovery_import_ldb\|coins_kv_seed_from_node_db\|COINS_KV_MIGRATION_COMPLETE_KEY' --include=*.c → exit 2 on any match`.

## ACT 4 — Keep the detective honest forever (lock-in) · **PARTIAL**

- **(a) Kill the forgeable XOR feed — HALF-DONE.** The MMB leaf already reads
  the real persisted `boundary_root` (`app/controllers/src/blockchain_controller.c:189`),
  but `rpc_blockchain_maybe_commit` still writes the O(1) `xor_accumulator` into
  the commitment MMR (`:285`) — a forgeable, never-set-verified value. **First
  confirm whether that commitment MMR is consensus-critical or RPC-export-only**
  — that decides enforce-vs-delete.
- **(b) Fix the 7 fail-silent lint gates + a meta-gate** (feed each gate empty
  input → assert exit 2). This extends the existing self-test pattern
  (`test_make_lint_gates.c`, `FRAMEWORK.md` §5). Re-audit `check_one_write_path`,
  `check_stage_log_reorg_unsafe_ratchet`, `check_projections_pure` **before** the
  Act-3 deletions, so a hollow gate can't let a forbidden pattern back in via a
  renamed symbol. Doc: [`lint-gate-hollowness-audit.md`](./lint-gate-hollowness-audit.md).
- **(c) Delete confirmed-dead scaffolding** — `lib/validation/src/verify_queue.c:130`
  is `additive_unwired` (no non-test callers) + the other confirmed-dead paths.
- **(d) CI invariant:** no path advances `coins_applied_height` without the
  co-committed log row (`tip_finalize_log` vs `utxo_apply_log` continuity — the
  one-write-path gate of `FRAMEWORK.md` §5, made specific to the cursor).
- **Gate:** `make ci` green; deletions do not regress `G-SOV` H\* CLIMB.

---

## Order & why

**0 → 1(proof symmetry) → 2 → 3 → 4.** Act 0 is the literal prerequisite —
nothing copy-tests until the copy boots, and today it does not. Act 1's script
path already restored forward progress (`37f431aaa`); only proof symmetry
remains, riding Act 0's copy. Act 2 makes Act 3's from-anchor refold fast enough
to be the *default* reflex. Act 3 is the prize — flips T from ∞ (borrowed) to
finite (sovereign), deletes ~9k LOC. Act 4 welds the lid so a refactor can't
quietly re-borrow.

## `G-SOV` — the composite sovereignty gate (every act uses it)

A boot is *self-verified-sovereign* iff **all three** hold:

1. `H* ≥ anchor` **and** `H*` climbs past the target wedge height (forward
   progress, not just a floor);
2. `coins_applied_height == H* + 1` — continuous log coverage, no NULL/stamped
   span (`tip_finalize_log` vs `utxo_apply_log` contiguity);
3. `coins_kv_is_proven_authority() == false`, **or** (`== true` **and**
   `coins_kv_contains_refold_marker()` — a single kernel-store key
   (`COINS_KV_SELF_FOLDED_KEY`, `progress.kv` pre-flip / `consensus.db` after
   the A3+A4 kernel-store flip, `docs/HANDOFF.md` §0-LATEST) proving a
   from-anchor reset ran). This separates self-derived from borrowed-and-stamped.

`G-SOV` is the quantitative Prime Directive: `health = network_tip − log_head`,
plus the predicate that `log_head` rests on the node's own fold, not a borrow.

---

## End state

A detective who, dropped cold into the room with only his **own notarized
case-file**, reconstructs the whole case from the evidence on the table in
**bounded time** (Act 2 cheap re-read; Act 3 self-derived file); **never files a
missing-evidence report on evidence that's on the table** (Act 1, symmetric
across script + proof); and **cannot be fooled by a forged index card** (Act 4:
XOR feed gone, borrowed copy deleted, lint gates that actually fire). T is
finite, and every reading of it — fast, secure, never-stuck — is the same
number.

This is `FRAMEWORK.md` §0 made true: the UTXO trust root stops being borrowed,
the fold becomes pure from genesis, and the silent halt stays unrepresentable.
