# Architecture North Star — one ledger per domain, views only

> **Read this before touching sync, boot, import, install, or any `*frontier`
> / `*cursor` / `pindex_*` code.** It is the standing decision that governs
> how this node acquires and tracks state. It exists because the node spent
> weeks "built but never syncing", and the root cause was always the same
> shape of bug (below). This doc is the cure and the guardrail.

## Verdict: RESCUE, not rewrite (2026-07-21)

The core is correct and expensive: frozen consensus verifiers (Equihash,
Sapling), the append-only log + reducer frontier, P2P/Tor, the swarm
download engine, the bundle format. A rewrite re-earns every parity lesson
(h=478544, BLS infinity, the golden values) to arrive at this same design
with fresh bugs. **The disease is confined to ~5 seams** (import, install,
legacy paths). Fix the seams; keep the core.

## The one bug, every time

Not "too many acquisition paths" (a sovereign node MUST keep genesis-fold as
its trust root). Not "one log would fix it" (that erases the two distinct
trust mechanisms below). The actual disease:

> **A single fact ("header proven to H", "state applied to H") has two or
> three independently-writable copies. One writer updates copy A; a reader
> checks copy B; they disagree.**

D8 in one line: `--importblockindex` PoW-verified the headers and wrote
`pindex_best_header = 3.19M`, but the install gate reads the *other* copy
(the `validate_headers` stage cursor = 0), so it defers forever and the node
folds from genesis. Same fact, two copies, drift.

## Two provenance domains — keep them SEPARATE and VISIBLE

State is trusted by two different mechanisms. Never flatten them into one
number — the difference IS the sovereignty audit trail.

- **Header spine (PoW).** Cheap, top-down. Hash-linked headers whose PoW is
  verified. This is what anchors a checkpoint: a borrowed state snapshot at
  height C is only meaningful if the header at C is real PoW and chains to
  genesis.
- **Derived state.** Expensive, bottom-up. UTXO/anchor/nullifier sets earned
  by replaying block bodies. A bundle *asserts* this at C (hash matches the
  baked root); a genesis fold *derives* it.

Every derived-state row carries a `self_derived | checkpoint` tag (the
existing `rewind_bases.self_derived` bit). "Is my tip earned or borrowed?"
must always be answerable.

```
  ╔═══════════════════════════╗       ╔═══════════════════════════╗
  ║  HEADER LEDGER (PoW spine)║       ║  STATE LEDGER             ║
  ║  append-only, hash-linked ║       ║  append-only; each row    ║
  ║  ALL writers append here: ║       ║  tagged self_derived|ckpt ║
  ║   --importblockindex,     ║       ║  bundle install = append  ║
  ║   header-seed artifact,   ║       ║  ONE ckpt row at C        ║
  ║   live P2P                ║       ║                           ║
  ╚═════════════╤═════════════╝       ╚═════════════╤═════════════╝
                │ fold                               │ fold
                ▼                                    ▼
         header_frontier H_h                  state_frontier H_s
                │       RECONCILE AT CHECKPOINT      ▲
                └── H_h ≥ C ⇒ graft ckpt ⇒ H_s:=C ──┘
                    ⇒ fold bodies C→tip ⇒ H_s → tip

  pindex_best_header · coins applied-height · install-gate · status
        = PURE VIEWS of (H_h, H_s). Not writable. Cannot drift.
```

## Acquisition is a fall-through STACK, not competitors

No ad-hoc "which path wins" precedence. Try in order; each layer that
succeeds appends the SAME facts to the SAME ledgers:

1. verified bundle (fast) → binds if the header spine reaches C
2. swarm bodies from a peer at the checkpoint (medium)
3. genesis fold (slow, sovereign floor — NEVER deleted)

The checkpoint install is not a fragile "wait for a cursor" gate. It is a
labeled splice: **spine reaches C ⇒ graft asserted state at C ⇒ fold the
gap C→tip.**

## The invariants a future LLM MUST obey

1. **Single writer per frontier.** Each frontier (`header_frontier`,
   `state_frontier`, and any shielded sub-frontier) has exactly ONE code
   path that advances it: appending a verified fact to its ledger. If you
   are about to write a height/cursor anywhere else, STOP — you are creating
   a cloned ledger. Make it a view instead.
2. **Readers read the frontier fold, never a side cursor.** Install gate,
   `status`, self-heal, and boot decisions all read the reducer frontier.
   `pindex_best_header` and `coins applied-height` are VIEWS; never gate on
   them independently.
3. **Reconcile at explicit points only.** The header/state domains meet at
   the checkpoint splice and nowhere else. No ad-hoc "if A disagrees with B"
   patches — that smell means two copies exist that shouldn't.
4. **A stall is always a named blocker with a height.** A frontier cannot
   fail to advance without a typed blocker naming why. No silent spins
   (see D7: the Sapling rebuild livelock that logged nothing).
5. **No hidden O(chain) work at boot.** If the bundle ships complete state
   (incl. shielded tree), nothing rebuilds. Boot is O(1) + fold-the-gap.
6. **Fix by DELETING a redundant copy, not adding a guard.** If your change
   reconciles two representations of one fact, you are treating the symptom.
   Demote one of them to a view.

## The rescue sequence (symptom → cure, one seam at a time)

- **D8 [in progress]** install binds on the PoW-verified header frontier the
  importer/artifact already produced — demote the drift, don't add a check.
- Then: audit each writer of `pindex_best_header`, coins applied-height, and
  each stage cursor; demote every non-canonical writer to a view. Each
  demotion is copy-proven (state actually installs / climbs) before merge.
- Enforcement: a lint gate that flags any *write* to a frontier/cursor
  outside its single canonical owner (grep-class gate, same family as
  `check-consensus-parity`). This is what stops re-accretion — a future LLM
  physically cannot add a second writer without tripling the gate.

## The score — `make arch-score` (the LLM's compass)

Every sub-goal is a mechanical KPI with a fixed weight; the total is a 0-100
"how done is the architecture" number. **Run `make arch-score` as you work
and chase the ✗/◐ rows, highest weight first.** A KPI scores its full weight
ONLY on a real proof (a grep with zero violations, or an outcome gate that
emits a PASS) — never partial credit for "looks close". Green theater is
banned: e.g. "readers-read-the-fold" scores only when the end-to-end
stopwatch PASSes, because that is the only proof the install reads the right
authority.

| KPI | wt | proof |
|---|---|---|
| instant-on-e2e | 20 | a `pass` line in the c3-stopwatch ledger (wiped node → install → tip) |
| single-writer-per-frontier | 20 | 0 writers outside each frontier's canonical owner (`arch_frontier_owners.tsv`) |
| readers-read-the-fold | 15 | tied to instant-on-e2e PASS (can't prove it any other way) |
| stay-synced | 15 | c3 report PASS **and** disruption-recovery PASS |
| observability | 10 | all 3 stage dumpers use `progress_store_tx_trylock` |
| no-silent-stall | 10 | the D7 Sapling-persist livelock raises a named blocker |
| no-ochain-boot | 10 | bundle-installed shielded tree skips the boot rebuild |

Ceiling gate (weight 0 but load-bearing): **enforcement** — the
single-writer invariant is lint-enforced (`check_frontier_single_writer.sh`
wired into `make lint`) so a future LLM physically cannot re-clone a ledger.
Until PRESENT, the invariant is a convention, not a law.

**Baseline 2026-07-21: 20/100.** Only observability (A2) and half the
single-writer invariant are done; everything that makes a node actually sync
(instant-on, stay-synced, the install binding, no-silent-stall, no-O(chain)
boot) is 0. That low number is correct and honest — drive it up.

Add a KPI row to `arch_score.sh` whenever a new invariant becomes
mechanically checkable; never inflate an existing one.

## What "done" looks like

A wiped datadir → boot → the bundle installs (state_frontier jumps to C,
tagged `checkpoint`) → folds C→tip → at tip, on a stopwatch, in minutes.
Proven by `make mvp-coldstart-to-tip-stopwatch`, not asserted.
