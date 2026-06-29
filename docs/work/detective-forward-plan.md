# The Detective's Forward Plan

> **SUPERSEDED by [`docs/work/sticky-node-plan.md`](sticky-node-plan.md)** — kept for
> planning lineage. Its Case 2 also lives in [`docs/work/self-verified-tip-plan.md`](self-verified-tip-plan.md).
> Read the sticky-node plan for the live plan of record.

*Synthesized 2026-06-25 from a 5-lane code-grounded recon (workflow `wtr79ytvz`)
that overturned the prior "self-heal selection cure" theory. Every claim below
carries a file:line or a live-runtime fact. Specifics rot — verify fresh.*

## The metric and the disease

**T = the time for the node to reach a tip it has proven *itself*** — valid PoW
plus its own fold of the bodies, trusting zero borrowed pages. "Fast", "secure",
and "never-stuck" are three readings of the same T:

- **Fast** = small T.
- **Secure** = T is reached by the node's own proof, not by trusting an artifact someone else minted.
- **Never-stuck** = T is always finite. A stall is a *named blocker that triggers a re-derivation*, never a silent freeze.

**One disease underlies every halt and every slowdown:** the detective trusts a
**frozen page in its own case file** instead of cheaply re-deriving that page
from PoW-valid evidence. The frozen pages we have hit, in order of discovery:

1. a **borrowed `coins_kv` seed** (zclassicd's conclusion, copied in at boot — `utxo_recovery_restore.c:369`)
2. a **stale `*_log.ok` verdict** (a past stage conclusion the reducer refuses to re-examine)
3. — and now the smoking gun — a **forged header page** at height 3157647.

## Case 1 (ACUTE) — unwedge the live node at 3157647

### Corrected root cause (recon A + B, validated against live `zcl_state` + `node.log`)

> **UPDATE 2026-06-25 (raw-byte forensics — supersedes the "zeroed sapling root"
> attribute in the table below):** the all-zeros sapling root was a **false
> lead** — node.db `blocks.sapling_root` is all-zeros for *every one of
> 3,157,704 rows* (a uniform projection-column artifact), and the canonical
> header's **real** root is non-zero (`2726b99f…`). The true defect is a
> **wrong/truncated Equihash solution in the stored header source**:
> `progress.kv → header_solution_repair[3157647]` has `solution_len=400` where a
> valid Equihash 200,9 solution is **1344 bytes**, so no source reassembles a
> 1487-byte header that hashes to canonical `00000e6b9` → `header-source-hash-mismatch`.
> The wedge *mechanism* and *cure shape* below are unchanged and confirmed; only
> the named defect (truncated solution, not zeroed root) and the repair *source*
> (the full-solution header arrives free in the blocks from the banned peers — no
> separate probe needed) are corrected. Self-heal Part 1 (all-zeros reject) is
> SAFE but **irrelevant** to this wedge; Part 2 (equal-work adopt) is **moot**
> (selection already correct). See memory `project_live_wedge_3157647_*`.

At height 3157647 there are two equal-`nChainWork` siblings:

| | hash | sapling root | body | on active chain | nStatus |
|---|---|---|---|---|---|
| corrupt | `000001a0…` | **all-zeros** | yes (blk file_num=50) | no | `45` = HAVE_DATA \| FAILED_VALID |
| canonical | `00000e6b9…` | (to confirm: expected non-zero) | **none locally** | **yes** | `2` = VALID_TREE, nChainTx=0, nFile=0 |

**Selection is already correct.** The corrupt block is already `FAILED_VALID`
and off-chain; the canonical block is already the active-chain entry. So the
self-heal Part 2 (adopt an equal-work sibling when the incumbent is FAILED) is
**moot for this wedge** — there is nothing to re-select.

The real blocker is a **stored-header vs active-hash inconsistency**:

- node.db's stored header bytes for the 3157647 slot hash to the **corrupt
  `000001a0`**, but the active-chain index entry's hash is the **canonical
  `00000e6b9`**.
- `validate_headers_validator.c:530-560` can't resolve a header source that
  hashes to `00000e6b9` → logs **one** `header-source-hash-mismatch` failure at
  `first_failed_height=3157647` (cursor is already 3159930).
- `reducer_read_back_verdict` (`reducer_ingest_service.c:217-285`) sees
  `failed_count>0` at that height and **rejects the canonical body every time a
  peer delivers it** — `getdata first=00000e6b9…` → `invalid block (dos=100):
  header-source-hash-mismatch` → peer banned. The body never persists,
  `BLOCK_HAVE_DATA` never sets, `tip_finalize` blocks on `have_data_missing`,
  H\* stays frozen at 3157645.

This is the disease exactly: the detective trusts a **forged page** (the wrong
stored header) over the **real evidence** (the PoW-valid canonical header + body
that peers keep handing it) — and bans the witnesses for "lying."

### Two traps the recon surfaced

- **The self-heal Part 1 (all-zeros sapling-root reject) is the wrong layer here
  and is actively dangerous.** If the canonical `00000e6b9` header itself carries
  a zeroed root, Part 1 would mark the just-fetched canonical body FAILED too →
  **both** siblings FAILED → the height becomes permanently unfoldable (a strict
  regression). MUST prove the canonical header is non-zeroed first.
- **The watchdog fights any FAILED-based heal.** `watchdog_check_stuck()`
  (`boot_background_workers.c:502-543`) unconditionally clears `BLOCK_FAILED_MASK`
  on the block at h+1 every 300 s — it would un-fail the corrupt block and
  re-admit the forgery. Any heal that relies on a FAILED flag is undone in ≤5 min.

### The cure (re-derive the forged page — the "third fix")

**Step 0 (gated prerequisite, running now):** on a faithful `cp -a` copy of the
live datadir (NOT reseeded below the wedge — a below-wedge snapshot collapses the
two-sibling state and destroys the test, per recon B), read the **raw block-file
header bytes** for `000001a0` and `00000e6b9` and the node.db row at 3157647, to
confirm: (a) node.db stores the corrupt header in the 3157647 slot, and (b) the
canonical header is recoverable **and non-zeroed**. Build nothing until this is
empirically confirmed.

**The fix** — a self-heal condition that re-derives the header page instead of
trusting the frozen wrong one:

1. **Detect** the contested mismatch: active-chain block at H has no body AND
   validate_headers has a `header-source-hash-mismatch` at H AND the on-disk
   node.db header bytes at H do NOT hash to the active-chain entry's hash.
2. **Re-derive** the correct header from a hash-bound source
   (`header_probe_pull_range` / peer headers); require it to hash to the
   canonical `00000e6b9` and carry valid PoW.
3. **Overwrite** the node.db `blocks` (+ `header_solution_repair`) row at H *by
   height* with the canonical header bytes.
4. **Clear/recheck** the validate_headers ok=0 row at H so
   `reducer_read_back_verdict` stops rejecting the inbound canonical body.
5. The canonical body then persists (`BLOCK_HAVE_DATA` sets), validate_headers
   flips ok=1, the have-data window extender + (already-correct) selection
   advance, **H\* climbs past 3157647 to network tip.**

Plus the two guardrails: make `watchdog_check_stuck()` not clear a *legitimately*
failed block (only transient/spurious failures); keep self-heal Part 1 (it is
parity-restoring — see below) but gate it so it can never mark the active-chain
canonical block FAILED.

**Where the existing self-heal branch fits:** `feat/corrupt-block-selfheal`
(Part 1 + Part 2) is **necessary hardening but not the cure for this wedge.** Its
Part 1 is parity-restoring and worth keeping (guarded); its Part 2 is correct for
the *general* equal-work-FAILED-incumbent case but is moot for 3157647 because
selection already chose right. The missing piece is the header-page re-derivation
above. Also: both most-work selectors filter `nChainTx==0 && !BLOCK_HAVE_DATA`
(`chainstate.c:649`, `process_block_core.c:60`) *before* Part 2's rule, so a
header-only sibling can't reach it until its body lands anyway.

### Gates

- **Copy-prove on a faithful (un-reseeded) fixture** with the real two-sibling
  wedge present. PASS = **H\* climbs past 3157647 to network tip with
  `coins_applied_height == H*+1`** — NOT "booted without FATAL."
- **Parity:** both self-heal parts are **PARITY-RESTORING** (recon E, vs
  zclassicd `main.cpp:2753-2758` ConnectBlock sapling-root reject and
  `main.cpp:~3954` UnwindBlock + FindMostWorkChain equal-work adoption). The
  header-repair is node-local (it makes the node store the canonical PoW-valid
  header the network already agrees on). Run E13 `check-consensus-parity` +
  `test_consensus_parity` before merge. Add the `nSequenceId`/received-order
  tie-break + a 2-valid-sibling test (recon E's gap) so Part 2 matches zclassicd
  exactly when more than one non-failed sibling exists.

**T reading:** smallest possible — surgical repair of one row, no resync,
seconds. The detective fixes the forged page rather than re-investigating the
whole case. (All three resync band-aids already failed — do not re-chase.)

## Case 2 (CHRONIC) — make re-derivation always cheaper than trusting a frozen page

The acute wedge is the third symptom of the one disease. The permanent cure is
the **sovereign self-verified tip**: the node re-derives the whole ledger from a
**self-minted, PoW-checkpoint-bound anchor** faster than any borrowed seed could
be loaded — so trusting a frozen page is never the fast path, and the forged-page
class cannot recur because there is no borrowed page to forge.

### Corrected state (recon C, vs the materially stale plan-of-record doc)

| Act | What | Status (HEAD `2d8c6e70b`) |
|---|---|---|
| 0 | bootable copy / sapling rebuild caps to coins-best | **SHIPPED** (`9fef4f683`; doc wrongly says NOT-STARTED) |
| 1 | re-derive verdicts incl. proof symmetry | **SHIPPED** (`079c3a42d`; doc wrongly says proof-TODO) |
| 2 | cheap re-read / fold speed | **NOT-STARTED** — the T lever |
| 3 | self-mint anchor + delete the borrow | **PARTIAL ~50-60%** (doc overstates ~80%; 7 not 9 seed-anchor callers) |
| 4 | weld the lid | **PARTIAL** (MMB leaf reads real root, but commitment MMR still fed forgeable XOR — `blockchain_controller.c:285`) |

- **G-SOV gate is NOT wired.** The anti-false-green discriminator (H\* climb +
  `coins_applied_height==H*+1` + **not-borrowed**) has no single assertion
  anywhere; `coins_kv_contains_refold_marker()` has zero production
  implementation. **Sovereign cannot currently be machine-distinguished from
  borrowed-and-stamped.**
- **`test_self_folded_anchor.c` is a false-green** — it exists and "passes," but
  its genesis→checkpoint fold-and-compare cases are SKIP/TODO skeletons. The
  "bodies are the authority" property is asserted only as a seam reset, never via
  an actual re-derivation.

### Critical path (corrected order)

1. **Build the truth instrument first.** Implement `coins_kv_contains_refold_marker()`
   and wire the 3-part **G-SOV gate** as one machine-checkable assertion. Until
   this exists, every "sovereign" claim is unfalsifiable.
2. **De-skeleton `test_self_folded_anchor.c`** — make the genesis→checkpoint
   fold-and-compare a real assertion (or a dedicated CI lane), so the self-fold
   is *proven*.
3. **Act 2 — break the T ceiling** so the default refold is painless (recon D
   ordering; #1 pprev-walk and #3 mirror-rebuild are **already shipped** —
   `f311482ae`, `c22baa278` — do not re-propose):
   - **[biggest]** refold-gated: drop the tail-stage `period_secs` 2 → sub-second
     and/or raise the tail batch 100 → ~1000-2000 (`staged_sync_supervisor.c:279`,
     `tip_finalize_stage.h:20`). Each batch is already one COMMIT, so a bigger
     batch is one fsync — this breaks the 50 blk/s ceiling.
   - make the in-RAM fold overlay (`coins_ram.c`, `ZCL_FOLD_INRAM`) the default
     **during refold only** — but first close the 3 seqlock/RCU gate-flip
     blockers (held branch `wave1/act2-coinsram-uaf`).
   - index window-extend candidates by height so
     `active_chain_extend_window_have_data` (`chainstate.c:542`) drops its
     O(3.1M) full-map scan.
4. **Act 3 — self-mint + delete the borrow** (only after 1-3 make the default
   flip safe and provable):
   - replace the hardcoded `REDUCER_FRONTIER_TRUSTED_ANCHOR = 3056758`
     (`reducer_frontier.h:41` + ~15 sites) with the self-minted value
     (`boot_mint_anchor.c:140-191` already folds + hard-asserts against the
     in-binary PoW checkpoint).
   - flip `-refold-from-anchor` to the default boot path (`boot.c:3395-3398`).
   - delete the borrow seed `coins_kv_seed_from_node_db()`
     (`utxo_recovery_restore.c:369`) + add a fail-loud lint gate so it can't
     return.
5. **Act 4 — weld the lid:** replace the forgeable XOR feed into the commitment
   MMR (`blockchain_controller.c:285`) with the real `coins_kv_boundary_root`;
   finish the fail-silent lint-gate fixes + the cursor-continuity CI invariant.

**T reading:** once Act 2+3 land, the detective re-derives the whole ledger from
its own anchor faster than a borrowed seed loads. The acute wedge class cannot
recur, because there is no frozen borrowed page to forge.

## Sequencing & parallelism

- **Case 1 (acute)** and **Case 2 steps 1-2** (G-SOV gate + de-skeleton the
  self-fold test) are independent — run concurrently in worktrees.
- Case 1 is the immediate win (live to tip). Case 2 is the permanent cure. Both
  are the same principle: *never trust a frozen page when you can cheaply
  re-derive it from PoW-valid evidence.*
- Everything copy-proved on a faithful fixture, gated on **H\* CLIMB**, before any
  live deploy (owner-gated). Live deploy of the decoupled binary = `cp` to
  `~/.local/bin/zclassic23-live` + `systemctl --user restart zclassic23`
  (rollback binary saved at `~/.local/bin/zclassic23-live.rollback-16de79a5d`).

## Stale-doc corrections this plan supersedes

- `docs/work/self-verified-tip-plan.md` — Act 0/1 are SHIPPED (not NOT-STARTED /
  proof-TODO); Act 3 is ~50-60% with 7 seed-anchor callers (not ~80% / 9).
- `docs/work/refold-fold-rate-bottlenecks.md` — bottleneck #1 (pprev-walk) and #3
  (mirror-rebuild) are SHIPPED (`f311482ae`, `c22baa278`); the live ceiling is now
  #2 (50 blk/s scheduler).
- Live-wedge memory — root cause is the **forged header row + body-reject**, not a
  selection failure; resync cannot cure it.
