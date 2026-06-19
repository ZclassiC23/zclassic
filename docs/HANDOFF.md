# HANDOFF — read this first

**Restart command:** type **`continue zclassic23 development`**.
Verify HEAD with `git status --short --branch`; detect your worktree with `pwd`
(`main` = orchestrator; `wt2`/`wt3` = workers — `docs/work/README.md`).

---

## 0. Meta-discipline (read before you trust ANY claim, including this file)

Trust ONLY what you re-derive from the code THIS minute + the live node. Every
"FIXED/cured/cleared" in older docs is UNPROVEN until you re-read the cited
file:line. The node is a sculpture and the work is a CHISEL — **delete the lie,
don't add another cover.** Be terse with the owner (3-6 lines). less-is-more.

---

## 1. The verified SHAPE (re-confirmed 2026-06-18, main @ 09248a0b4)

A small HONEST core refuses to build on a borrowed foundation; a large accretion
exists only to cover for that foundation. The live wedge is the engine refusing,
not a bug. **Re-read each file:line — these were correct on 2026-06-18 only.**

**HEART — KEEP (genuinely pure):**
- `app/jobs/src/reducer_frontier.c` (787 LOC) — SELECT-only fold (no
  INSERT/UPDATE/DELETE/REPLACE/BEGIN/COMMIT; every `sqlite3_step` carries
  `// raw-sql-ok:progress-kv-kernel-store`). Contiguity ends on a height jump OR
  `ok=0` (`:193`); H* = MIN over each log's ok=1 prefix (`compute_hstar :472-502`);
  guard never rewinds below the anchor (`:554-556`). It refuses + names the
  missing height; it never stamps.
- `lib/util/src/stage.c` `stage_run_once` (`:280`) — single-writer txn:
  `BEGIN IMMEDIATE` (`:316`) → step → `cursor_write_locked` → `COMMIT` (`:347`);
  a JOB_ADVANCED that fails to move the cursor is rolled back + FATAL.
- `app/jobs/src/utxo_apply_stage.c:513-516` co-commits coins + cursor +
  applied_height in ONE txn (`coins_kv_set_applied_height_in_tx`).

**THE LIE — install side, `app/services/src/utxo_recovery_restore.c`:** cold
import copies zclassicd's UTXO set and crypto-checks it at EXACTLY ONE height
(count+root vs the single struct at `lib/chain/src/checkpoints.c:87`,
height 3056758 / count 1354771). Every other height is accepted on a row-count
heuristic (`:323` `imported_count > 100000`), which seeds coins_kv and writes a
durable seed anchor (`:381-384`; comment `:375-379` admits it). The vacuous SHA3
stamp is now gated to the real checkpoint (`:391-393`) — the lie rides the seed
anchor, not a fake stamp.

**TWO wedge causes (one structural pin: H* refusing a non-contiguous log):**
1. Borrowed install above — the heuristic seed anchor leaves a hole.
2. **Forward false-reject** — `app/jobs/src/script_validate_stage.c:449-455` and
   `proof_validate_stage.c` write an `ok=0` row on a GENUINE verify failure yet
   STILL advance the cursor (`cursor_out = cursor_in + 1; JOB_ADVANCED`) → H*
   pins. No in-stage auto-recheck. The repair purge only deletes an `ok=0` row
   whose stored hash MISMATCHES the canonical block
   (`stage_repair_reducer_frontier_purge.c`) — a genuine reject against the real
   block is left in place. No recovery exists today.

**snapshot_verify is NOT sound** (the would-be replacement foundation):
`lib/chain/include/chain/mmb.h:41-48` — the MMB leaf has NO utxo_root.
`snapshot_verify.c:131-134` SHA3-checks the local set only against the peer's
OWN offered root/count. A forged self-consistent set PASSES. Build this before
carving the install crutch.

**Logs are NOT reorg-preserving:** all 8 stage logs are `height INTEGER PRIMARY
KEY` + `INSERT OR REPLACE` (e.g. `script_validate_log_store.c`,
`utxo_apply_log_store.c`) — last-write-wins; a reorg overwrites in place.

**oracle_policy halt = a never-stuck VIOLATION:**
`app/services/src/oracle_policy.c:166-170` latches `OP_HALTED` after
`halt_distinct_heights` zclassicd disagreements; gates
`rolling_anchor_service.c:403,433`. A zclassicd that is merely wrong/behind can
stop chain extension. Demote to evidence-only.

**Accretion to carve** (`stage_repair_*`, `coin_backfill`, `legacy_mirror`,
`zclassicd_oracle`, `utxo_parity`/`parity_poll`, cold-import LDB cluster): carve
LAST, each gated on its replacement. **LOC totals and the "zero feature loss"
claim are UNVERIFIED — treat as hypothesis.** Plan:
`docs/work/never-stuck-plan.md` (untracked).

---

## 2. The 5 missing chisels — BUILD before carving anything

1. **Bind a `utxo_root` into the MMB leaf / proven chain** so `snapshot_verify`
   stops trusting the peer's self-reported root. Keystone, consensus-sensitive →
   own ADR + adversarial negative test (a forged self-root set must REJECT).
2. **Wire the sound verifier into the import door** (`utxo_recovery_restore.c:323`
   accepts on count>100k today).
3. **Fold-from-checkpoint forward** instead of writing the heuristic seed anchor.
   This is ALSO the only way to measure fold cost — no offline from-checkpoint
   fold verb exists (§5).
4. **Make the 8 INSERT-OR-REPLACE logs reorg-correct** (re-key to
   `(height, block_hash)`, readers pick the active-hash row). Replay real reorg
   history first.
5. **Honest forward-false-reject recovery** — a sound way to re-derive and clear
   a genuine `ok=0` (`script_validate_stage.c:449-455`).

Consensus parity is inviolable (`docs/CONSENSUS_PARITY_DOCTRINE.md`): replay any
predicate tightening against REAL history first (the h=478544 lesson).

---

## 3. Features: real vs stub (verified this pass)

**REAL & chain-derived (keep):** wallet, explorer (+projections), ZNAM,
embedded Tor, MCP/RPC, P2P ping/latency.

**STUBS (don't present as working):**
- **Atomic swaps** — builds HTLC script + P2SH + DB row; no funding broadcast,
  no chain monitoring, no settlement.
- **File market** — offer cache + P2P serialize exist; `root_hash` is a SHA3 of
  `"path:size"` PLACEHOLDER; `zmarket_buy` parks forever; no transfer/payment.
- **On-chain ZMSG** — persists a record labeled ONCHAIN; no Sapling memo
  build/broadcast/scan. (Off-chain P2P ZMSG is real, plaintext on wire.)
- **TicTacToe** — correct logic + tests, but no P2P wiring; not playable.

---

## 4. Diagnostics: which lie

**Work as named:** `zcl_status`, `zcl_state`, `zcl_sql`, `zcl_node_log`,
`build/bin/sqlq`.

**`rebuild_recent` — DO NOT trust for a cold-import gap.** The MCP/repair lever
caps at `REBUILD_RECENT_MAX_RANGE 10000` (`repair_controller_rebuild.c:43`) AND
fetches every block from a live zclassicd RPC — useless for a ~95k gap, dead
without zclassicd. The standalone `tools/rebuild_recent.c` also needs a zclassicd
datadir. Neither is chain-derived recovery for the cold gap.

---

## 5. Live ops state & lanes

| Lane | Datadir | P2P/RPC | Deploy |
|---|---|---|---|
| **live** | `~/.zclassic-c23` | 8023 / 18232 | `make deploy` (owner-gated) |
| **dev** | `~/.zclassic-c23-dev` | 8053 / 18252 | `make deploy-dev` |
| **soak** | `~/.zclassic-c23-soak` | 8043 / 18242 | deliberate re-baseline |

`zclassicd` (C++ reference) runs at 8033/8232 — **never stop it.** `zcl-rpc`
default 18232 = the live zcl23 node.

**Re-pull live state yourself** (`zcl_status`, `zcl_state`) before acting; the
snapshot in `never-stuck-plan.md` (tip pinned 3,151,411, `utxo_apply log hole,
first hole h=3056759`) may be stale. Band-aid recovery (a crutch, not the cure):
on a COPY first — `cp -a` the datadir, `--importblockindex $HOME/.zclassic` then
a normal boot, verify tip==network + hash-match vs zclassicd at ≥2 heights, then
deploy. This recovery IS the borrowed-foundation crutch the plan replaces.

No offline from-checkpoint fold verb exists, so fold cost (~95k real blocks, full
script+proof) is UNMEASURED until chisel #3 lands.

**Standing method:** `make deploy` rm's the binary first (the binary rule isn't
depfile-tracked; a stale binary was a real multi-day outage; `deploy_verify.sh`
confirms `build_commit`). Copy-prove before live, never live surgery. Never
weaken a safety/operator gate. Gate every change: `make` + `make lint` +
`make test_parallel` (read the `N passed, M failed` line, not the pipe exit).

---

## 6. Verify before you trust this file

A map, not the territory. Before building on any claim, re-read the cited
file:line — code moves and old narratives recur. Spot-check the heart
(`reducer_frontier.c`), the lie (`utxo_recovery_restore.c:323`, `checkpoints.c:87`),
and the unsound foundation (`mmb.h:41-48`, `snapshot_verify.c:131-134`). If a
claim doesn't match the code you read THIS minute, trust the code. v1 contract:
`docs/MVP.md`; plan: `docs/work/never-stuck-plan.md` + `docs/work/FORWARD_PLAN.md`;
architecture: `docs/FRAMEWORK.md` + `docs/REFACTOR_STATUS.md`.
