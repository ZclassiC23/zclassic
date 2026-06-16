# Sync-Strength Fix — Implementation Design

**Scope:** (A) guarded repair for `prevout_unresolved` frontier holes (class = any old missing coin); (B) peer-retention + body-download robustness so the node keeps peers and bodies flowing while repairing.
**Repo:** /home/rhett/github/zclassic23.

---

## 1. Mechanism decision

**DECISION: guarded multi-coin backfill ("frontier coin backfill"), NOT rewind/reapply.** The suggested rewind to before creator height 3,073,765 is **structurally impossible on this datadir**, not merely risky:

1. **Delta coverage kills it.** `utxo_apply_delta` covers only [3,132,688..3,135,516] (+200 genesis-era rows). The creator height 3,073,765 is **58,923 heights below the lowest usable delta**. `utxo_apply_emit_inverse_delta` silently no-ops on a missing row (`utxo_apply_delta_reorg.c:120-126`) and the only guard against that — `rewindable_utxo_row` (`stage_repair_reducer_frontier_coin.c:226-268`) — correctly **refuses on the first missing `utxo_apply_log` row**, i.e. at 3,132,687. A rewind cannot reconstruct state it has no inverse images for.
2. **Anchor floor kills it.** H* floors at the durable trusted anchor 3,132,719 (`reducer_frontier.c:373-375`); a rewind below it would drop the public tip below served finality (`tip_finalize_log` ok=1 rows through 3,135,515), violating the never-regress doctrine (`stage_repair.h:157-165`).
3. **The hole isn't a verdict, it's a state gap.** Coin `bf8ae684…f6ff:0` (created h=3,073,765, never spent before 3,135,517) is absent from coins_kv, node.db utxos, AND consensus_snapshot utxos at the 3,132,687 state. It was **lost before the delta window began**. No amount of replaying logged history re-derives it; the raw creating block is the only remaining authority — and it is present and byte-verified (blk00049.dat @64,358,775, tx hash matches exactly).

**Why backfill is consensus-safe (and rewind wouldn't even be safer):** the backfill re-derives the coin from the raw creating block, **hash-verified against the active chain**, then **proves no-spend** by scanning every applied active-chain block in `(creator, frontier)` — and the entire scanned segment is **hash-chain-bound to the hole block's own chain**: a persisted running last-scanned hash prev-links every chunk and resume boundary, terminal linkage extends the chain through `[frontier..H]` to require block H's hash equals the hole row's hash, and the insert transaction re-binds the proof to the *insert-time* active chain. By second-preimage resistance of double-SHA256, every scanned block is provably the ancestor, at its height, of the chain that block H extends — no reorg/oscillation interleaving (including this datadir's documented `reorg_detected` 1→30 episodes inside the scan band) can stitch branches into the proof. Anything unprovable → refuse loudly. The repair is structurally incapable of minting a spent coin (the scan finds the spend → refuse-permanent), an off-chain coin (creating tx must hash-match inside the active-chain block at its indexed height), or a cross-branch coin (chain binding above).

**Class design:** the repair keys on the **lowest reducer hole** when its status is `prevout_unresolved`, enumerates **ALL** unresolved prevouts of the failing block in one pass (not just `first_failure_vin` — a 188-input block may be missing several coins), and restricts itself to coins created **below the delta horizon** (lowest contiguous `utxo_apply_log` height). Coins missing *inside* the delta window indicate a live apply bug and are refused to the operator (masking a double-spend bug there would be a guess). **Refusals are whole-set:** any member of U failing any guard refuses the entire set — no partial inserts, ever (keeps the replay-marker/round bookkeeping single-valued). Everything is per-outpoint one-shot, so any future lost old coin heals through the same gate as it surfaces.

After backfill, **no new cursor surgery is invented**: the existing stale-script replay (`maybe_repair_stale_script`) re-runs on the next condition tick, its dry-run now resolves, and it performs the already-proven rewind/replay of `script_validate`/`proof_validate`/`tip_finalize`. The backfill's only consensus-state mutation is the coin insert(s).

---

## 2. Files, functions, guard ladder

### New files (shape: app/jobs — repairs are Jobs; conditions stay thin per `stage_repair.h:3-7`)

**`app/jobs/include/jobs/stage_repair_coin_backfill.h`** (public API)

```c
struct sqlite3; struct main_state; struct node_db; struct json_value;

enum coin_backfill_status {
    COIN_BACKFILL_NOT_APPLICABLE = 0, /* no unresolved old-coin prevouts → fall through */
    COIN_BACKFILL_SCANNING,           /* no-spend scan progressed; resume next tick */
    COIN_BACKFILL_REPAIRED,           /* coin(s) inserted; stale-script replay proceeds */
    COIN_BACKFILL_OWNER_REFUSED,      /* env ack missing — PAGES DIRECTLY (see below) */
    COIN_BACKFILL_REFUSED_SPENT,      /* prevout provably spent <= frontier-1: genuine reject */
    COIN_BACKFILL_REFUSED_UNPROVABLE, /* guard failed / scan gap / creator unresolvable */
    COIN_BACKFILL_MARKER_SEEN,        /* outpoint already backfilled once and lost AGAIN */
};

struct coin_backfill_outpoint {
    uint8_t  txid[32];          /* internal byte order */
    uint32_t vout;
    int32_t  creator_height;
    int64_t  value;
    uint8_t  script[MAX_SCRIPT_SIZE];
    size_t   script_len;
    bool     is_coinbase;
};
/* NOTE: 64 * sizeof(struct coin_backfill_outpoint) ≈ 644 KB. The U set is
 * heap-allocated ONCE via zcl_malloc per call and freed before return —
 * NEVER stack-allocated (the condition tick runs on the self_heal
 * supervisor thread). */

/* Test seam: production wires stage_repair_read_active_block_checked +
 * app_runtime_node_db(). */
struct coin_backfill_io {
    bool (*read_block)(void *user, int height, struct block *blk,
                       struct uint256 *hash);   /* active-chain, hash-verified */
    void *user;
    struct node_db *ndb;                        /* txid -> creating height (txindex) */
};

struct coin_backfill_result {
    enum coin_backfill_status status;
    int  hole_height;
    int  unresolved_count;     /* |U| */
    int  inserted_count;
    int  scan_next_height;     /* resumable cursor */
    int  scan_top_height;      /* frontier-1 */
    int  creator_floor;        /* min creator height across U */
    int  delta_horizon;        /* lowest contiguous utxo_apply_log height */
    char refuse_reason[64];
};

/* Entry point called from the replay-repair dispatcher. apply=false = detect
 * (enumeration + guards only, no scan, no writes). Returns false only on
 * infrastructure error (caller surfaces COND_REMEDY_FAILED).
 *
 * CONTRACT (normative): guards G1-G8 re-run on EVERY call, including every
 * apply tick of a multi-tick scan; the insert-tx step-1 active-chain
 * re-reads are load-bearing against persistent reorgs. Refusals are
 * whole-set. Every refusal status (OWNER_REFUSED, REFUSED_SPENT,
 * REFUSED_UNPROVABLE, MARKER_SEEN) emits a typed blocker AND
 * EV_OPERATOR_NEEDED directly from this Job (once-latched per
 * (H,holehash,status)); paging never depends on condition-engine attempt
 * exhaustion. */
bool stage_repair_coin_backfill_try(struct sqlite3 *db, struct main_state *ms,
                                    const struct coin_backfill_io *io,
                                    bool apply, struct coin_backfill_result *out);

/* zcl_state dumper (CLAUDE.md "Adding state introspection"). Reentrant-safe. */
bool coin_backfill_dump_state_json(struct json_value *out, const char *key);
```

**`app/jobs/src/stage_repair_coin_backfill.c`** (≤800 lines — E1) — orchestration:
- `find_lowest_prevout_unresolved_hole_unlocked(db, script_cursor, &h, hash_out)` — same shape as `stale_script_hole_unlocked` but reads the row's `block_hash` too; **applies only when the lowest hole among the 3 repairable statuses is `prevout_unresolved`** (single-frontier discipline; lower `internal_error` holes go to the existing replay first).
- `enumerate_unresolved_prevouts(...)` — parse failing block (via `io->read_block`), build intra-block txid set; for each non-coinbase input, resolve in the validator's exact order/semantics (`created_index_prevout`, `script_validate_stage.c:118-189`): intra-block earlier tx → `created_outputs_index_get_bounded` over [frontier..H] → `coins_kv_get_coins` usable iff `c.height < frontier && c.height <= H`. Misses → set U (heap, single `zcl_malloc`). Coin **present but unusable** (height ≥ frontier) → refuse `coin_present_unusable` (metadata tear ≠ missing coin; do not mint).
- `resolve_creator(...)` — `db_tx_find(ndb, txid, &row)` (`app/models/include/models/tx_index.h:28`); then `io->read_block(row.block_height)` and locate the tx by **recomputed double-SHA256 == txid** (never trust `row.tx_index` or `row.block_height` alone — corrupt heights exist; garbage fails the read/hash and refuses).
- `utxo_apply_log_contiguous_floor(db, utxo_cursor)` — lowest L with a `utxo_apply_log` row AND `utxo_apply_delta` row at every height in [L..cursor-1] (= delta horizon; on the COPY: 3,132,688).
- `coin_backfill_insert_tx(...)` — the single write transaction (below).
- `coin_backfill_page_refusal(...)` — typed-blocker + `EV_OPERATOR_NEEDED` emission for every refusal status (below); once-latched per (H, holehash, status) via static atomics, additionally deduped by the blocker primitive's token bucket (`BLOCKER_DEFAULT_RATE_LIMIT_MS`, `util/blocker.h`). Direct-emit precedent: `app/services/src/chain_tip_watchdog.c:193`, `app/services/src/reducer_ingest_service.c:94`.
- marker/round helpers + `coin_backfill_dump_state_json` (atomics snapshot of last result).

**`app/jobs/src/stage_repair_coin_backfill_scan.c`** (≤800 lines) — chunked resumable **chain-bound** no-spend scan:

```c
enum coin_backfill_scan_verdict {
    COIN_SCAN_IN_PROGRESS, COIN_SCAN_CLEAN, COIN_SCAN_SPENT_FOUND, COIN_SCAN_GAP,
    COIN_SCAN_CHAIN_REBOUND,   /* prev-link mismatch on resume/start → restarted from floor */
};
enum coin_backfill_scan_verdict coin_backfill_scan_step(
    struct sqlite3 *db, struct main_state *ms, const struct coin_backfill_io *io,
    int hole_height, const struct uint256 *hole_hash,   /* key binding */
    const struct coin_backfill_outpoint *set, size_t n,
    int floor_height, int top_height, int frontier_at_start,
    int max_blocks, int64_t max_wall_ms,
    int *out_next_height, int *out_spent_height, uint8_t out_spender_txid[32]);
```

**Persisted scan record** — key explicitly bound to the hole: `coin_backfill.scan.<H>.<holehash>` →
`[next_height i32 LE][frontier_at_start i32 LE][last_scanned_hash 32B][set_digest 32B = SHA3-256 of sorted (txid,vout) set]`, written via `progress_meta_set` (restartable derived metadata; internally locked own-tx, `progress_store.c:260-304`, so a kill-9 mid-chunk just rescans one idempotent read-only chunk). On completion the record additionally carries `[top_hash 32B = hash(frontier_at_start-1 … i.e. scan top)]` and a CLEAN flag.

**Chain binding:**
- **Seed:** at scan start, `last_scanned_hash` := the hash-verified active-chain hash at `floor-1` (via `io->read_block`).
- **Every chunk — start, resume, and mid-chunk alike:** for `h` in `[next..min(next+max_blocks, top)]` within `max_wall_ms`: read block via `io->read_block` (hash-verified vs active index) and **require `blk.hashPrevBlock == last_scanned_hash`** before processing it; then advance `last_scanned_hash` := hash(h). This single invariant covers the within-chunk linkage AND the resume boundary (kill-9, multi-tick) AND a reorg landing mid-chunk with fork point ahead of the read position: the first divergent block fails the prev-link against the persisted lineage. Mismatch → `COIN_SCAN_CHAIN_REBOUND`: discard the record, restart from floor with a fresh seed (bounded restart, not a refusal — oscillation is expected on this datadir).
- **Spend check:** for `h ≤ frontier_at_start-1`, check every tx input's prevout against the sorted outpoint set (the creator block itself is included — catches later-tx-in-same-block spends; the creating tx can't false-positive since its inputs are other outpoints). Spend found → `COIN_SCAN_SPENT_FOUND`.
- **Terminal linkage:** after `top = frontier-1` is reached, continue the prev-linked hash-verified walk through `[frontier..H]` — **linkage-only, no spend refusal there** (a spend in the unapplied `[frontier..H-1]` window is *correct* coins state at the frontier snapshot: utxo_apply will consume the coin forward at the spend height and then genuinely reject block H's re-spend — verified forward semantics, `utxo_apply_stage.c:318` next-cursor / per-outpoint delete-then-fail). **Require the walk's terminal block H hash == the hole row's `block_hash`** — this makes the entire scanned segment hash-unique to the hole block's own chain. Only then persist CLEAN + `top_hash` and return `COIN_SCAN_CLEAN`.
- Digest/frontier mismatch on resume → restart from floor. Unreadable/missing block → `COIN_SCAN_GAP`.

### Modified files

**`app/jobs/src/stage_repair_reducer_frontier_coin.c`**
- Expose `read_active_block_checked` as `stage_repair_read_active_block_checked` via `stage_repair_reducer_frontier_internal.h` (production `io->read_block`).
- In `stage_reducer_frontier_try_replay_repairs` (`:713-751`), **after** `maybe_repair_value_overflow`, **before** `maybe_repair_stale_script`:

```c
struct coin_backfill_result cb;
struct coin_backfill_io io = { .read_block = repair_read_block_thunk, .user = ms,
                               .ndb = app_runtime_node_db() };
if (!stage_repair_coin_backfill_try(db, ms, &io, apply, &cb))
    return false;
/* copy cb fields into out->coin_backfill_* */
if (cb.status == COIN_BACKFILL_SCANNING || cb.status == COIN_BACKFILL_REPAIRED) {
    out->repaired = true;  *handled = true;  return true;   /* skip replay this tick */
}
/* NOT_APPLICABLE / refusals fall through to maybe_repair_stale_script */
```

**`app/jobs/include/jobs/stage_repair.h`** — extend `struct stage_reducer_frontier_reconcile_result` (after `:140`): `bool coin_backfill_attempted; int coin_backfill_status; int coin_backfill_hole_height; int coin_backfill_unresolved; int coin_backfill_inserted; int coin_backfill_scan_next; bool coin_backfill_owner_refused; bool coin_backfill_genuinely_invalid;`.

**`app/conditions/src/reducer_frontier_reconcile_light.c`** — (a) snapshot the backfill scan cursor at detect (alongside `snapshot_reducer_cursors`, `:68-86`) and accept its advance in `witness_reducer_frontier_reconcile_light` (`:225-242`) so multi-tick scans are witnessed durable progress and the budget keeps resetting. **The scan-cursor witness explicitly counts absent→present (scan record created) and present→absent (scan completed/consumed) as progress** — the file's local `cursor_changed` convention returns false when `before < 0`, which would burn the first attempt as unwitnessed; the scan-cursor variant must not inherit that. (b) surface `coin_backfill_owner_refused` exactly like `value_overflow_repair_owner_refused` (`:191-197`) → `COND_REMEDY_FAILED`. **This surfacing is belt-and-suspenders observability only — the operator page does NOT depend on it:** the Job itself emits the typed blocker + `EV_OPERATOR_NEEDED` on every refusal status, because `witness_reducer_frontier_reconcile_light` ORs over all seven reducer cursors (`:101-117`) and P1–P4 deliberately keep validate_headers/body_fetch/body_persist moving during the wedge, so engine exhaustion (`condition.c:214-249`) is probabilistic and can be deferred indefinitely — relying on it would be a silent refuse-forever loop, violating the silent-halt doctrine.

### The write transaction (only consensus mutation)

Under `progress_store_tx_lock()` + `BEGIN IMMEDIATE` (template: `stale_script_replay_tx` `:457-509`), all-or-nothing:
1. **Re-bind the proof to the insert-time active chain:** (a) re-verify the hole row is still present at H with the **same** `block_hash`; (b) **re-read the ACTIVE block at H via `stage_repair_read_active_block_checked` inside the transaction and require its hash == the hole row's `block_hash`** (not merely "row present" — the hole row is progress.kv state and survives reorgs); (c) re-read the active-chain hash at `scan_top = frontier-1` and **require it == the persisted `top_hash`** from the CLEAN scan record (by hash uniqueness + the persisted prev-linked lineage, every scanned block is then provably the ancestor of the insert-time active chain at its height); (d) re-read `coins_applied_height` and require it equals `frontier_at_start` recorded in the scan record (kills any scan/insert race);
2. For each u ∈ U: `coins_kv_exists` must be false (else ROLLBACK, refuse);
3. `coins_kv_add(db, u.txid, u.vout, u.value, u.creator_height, u.is_coinbase, u.script, u.script_len)` (`coins_kv.h:35-38`);
4. Per-outpoint one-shot marker — **keyed by outpoint only**: `progress_meta_set_in_tx("utxo_apply.coin_backfill.outpoint.<txid_hex>:<vout>", value = "<H>:<holehash>:<round>")` (H/holehash recorded in the *value* for forensics; the outpoint-only key makes MARKER_SEEN detect a re-lost coin at ANY future hole height H′);
5. Round counter `coin_backfill.rounds.<H>.<holehash>` += 1 (cap 8);
6. `progress_meta_delete_in_tx("reducer_frontier.script_replay_repair.<H>.<holehash>")` — lets the existing replay run **once more** with the new coins (bounded: deletion happens only when a round inserts ≥1 brand-new outpoint, and rounds are capped);
7. `progress_meta_delete_in_tx("coin_backfill.scan.<H>.<holehash>")`;
8. `COMMIT`.

**Touches NOTHING else**: no cursor, no `*_log` row, no `coins_applied_height`, never `tip_finalize_log` (doctrine). Atomicity is real: coins_kv lives IN progress.kv on the same handle (`coins_kv.h:3-19`), so kill-9 anywhere in steps 1–8 is all-or-nothing.

### Guard ladder (every precondition; refuse = log `[coin_backfill]` WARN + result status + **direct page** (typed blocker + `EV_OPERATOR_NEEDED`, once-latched); FATAL only for programming errors. **Refusals are whole-set. G1–G8 re-run every call, including every apply tick.**)

| # | Guard | On failure |
|---|---|---|
| G0 | NULL inputs / snprintf overflow | `LOG_FAIL` (programming error — fail fast, per existing pattern) |
| G1 | `utxo_projection_get_author() == UTXO_AUTHOR_STAGE` | refuse |
| G2 | Lowest repairable hole exists below script cursor AND its status is `prevout_unresolved` | NOT_APPLICABLE |
| G3 | Failing block read **hash-verified** on active chain AND equals the hole row's `block_hash` | refuse `hole_hash_mismatch` (stale row — reorg machinery owns it) |
| G4 | `coins_applied_height` found AND `frontier <= H` (frontier above H = coin tear → L1/L2 domain) | refuse |
| G5 | Enumeration: U nonempty; `|U| <= 64`; no `coin_present_unusable`; outpoint-only one-shot marker already present for any u → `MARKER_SEEN` (coin lost AGAIN, at any hole height — PERMANENT blocker, never loop) | NOT_APPLICABLE / refuse |
| G6 | **Owner gate** `ZCL_REDUCER_COIN_BACKFILL_ACK=1` (mirrors `utxo_apply_delta_repair.c:25-33`; this repair mints state — strictest gate) | `OWNER_REFUSED` → **direct page** (DEPENDENCY blocker, escape_action: "export ZCL_REDUCER_COIN_BACKFILL_ACK=1 in the unit env, restart") + condition `COND_REMEDY_FAILED` |
| G7 | Every u: txindex row found; `0 < creator_height < frontier`; **`creator_height < delta_horizon`** (inside delta window = live apply-bug domain → operator); creator_height ≤ H, and if coinbase `H - creator_height >= COINBASE_MATURITY` (100, `main_constants.h:32`) else the spend is genuinely invalid → leave hole, refuse. **Any u failing ⇒ refuse the whole set** | refuse |
| G8 | Creating block read hash-verified at creator_height on active chain; tx located by **recomputed txid == bytes**; `vout < n_outputs`; `0 <= value <= MAX_MONEY` (`core/amount.h:18`); `script_len <= MAX_SCRIPT_SIZE` | refuse `creator_not_on_active_chain` / bounds |
| G9 | **Chain-bound no-spend proof**: chunked scan of every active-chain block in `[min_creator .. frontier-1]`, hash-verified + **prev-linked against the persisted `last_scanned_hash` across every chunk/resume boundary** (seeded at floor−1; mismatch → restart from floor), zero inputs spending any u; **terminal linkage through `[frontier..H]` requiring hash(H) == hole row hash**. Spend found → `REFUSED_SPENT` + refusal marker `coin_backfill.refused.<H>.<holehash>` (never rescan) + PERMANENT blocker + `EV_OPERATOR_NEEDED` — a proven spend means the failing block double-spends or local history is torn; we refuse, never guess. Gap → `REFUSED_UNPROVABLE` + DEPENDENCY blocker whose escape_action names the **actual** remedy: "fetch deep body h=<N> via rebuild_recent / -cold-import" (a 60k-deep body has no organic fetch path) | refuse |
| G10 | Insert-tx re-checks (active block at H hash == hole hash; active hash at frontier−1 == persisted `top_hash`; frontier unchanged; coins still absent) | ROLLBACK, retry next tick |

**No-loop argument:** per (H, hash): each successful round inserts ≥1 *new* outpoint (outpoint-keyed one-shots); the replay-marker delete only happens in such a round; rounds capped at 8; scan cursor is monotonic per lineage, digest- and chain-bound (a rebind restarts the cursor but only on an actual reorg event, and the insert can only land when a full unbroken lineage reaches a stable H — oscillation delays, never loops unboundedly: each restart requires a fresh prev-link break, and refusal markers stop rescans); refusal markers stop rescans. Engine-level: attempts=5/backoff 30s/witness — witnessed scan progress resets the budget; refusals **page directly from the Job** regardless of what the engine budget does. Transient failures (read errors, lock contention) roll back and retry — resurrectable in-process while the budget lasts; if a zero-peer copy burns 5 attempts on transient infra failures, the page fires and a process restart recovers.

---

## 3. Trigger wiring

**No new Condition.** Detection rides the existing chain (heavy logic in Jobs, per convention): `self_heal.engine` supervisor child (5s tick, `app/supervisors/src/self_heal.c:24-34`) → `condition_engine_tick` → `reducer_frontier_reconcile_light` (COND_CRITICAL, poll 5s, backoff 30s, max 5, gated on `tip_advance_age >= 60s` + peer-lag) → `stage_reducer_frontier_reconcile_light_impl` → `stage_reducer_frontier_try_replay_repairs` → **`stage_repair_coin_backfill_try`** (new hook). `apply=false` detect path runs enumeration+guards only (one block parse — cheap) and sets `repaired=true` when applicable, so detect fires.

- **Attempt budget:** engine-level (5 attempts / 30s backoff); each SCANNING remedy is witnessed by the durable scan-cursor advance (including absent→present) → cleared → re-detect → fresh budget. ~62k-block scan at ~8k blocks or 1.5s per chunk ≈ a handful of remedies, minutes total. A `CHAIN_REBOUND` restart re-walks from floor — same budget mechanics.
- **Done markers:** per-outpoint `utxo_apply.coin_backfill.outpoint.<txid>:<vout>`; refusal marker `coin_backfill.refused.<H>.<hash>`; round cap counter; scan record `coin_backfill.scan.<H>.<hash>`. All in `progress_meta`; insert-side ones written in-tx.
- **Operator paging:** every refusal status emits, **directly from the Job**, a typed blocker (`blocker_set`, `util/blocker.h`) AND `event_emitf(EV_OPERATOR_NEEDED, 0, "coin_backfill h=%d status=%s reason=%s")`, once-latched per (H, holehash, status) and deduped by the blocker token bucket. Mapping: `OWNER_REFUSED` → DEPENDENCY, escape "export ZCL_REDUCER_COIN_BACKFILL_ACK=1, restart unit"; `REFUSED_SPENT` / `MARKER_SEEN` → PERMANENT, id `coin_backfill.<H>`, escape "operator: investigate lost-coin class / consensus divergence"; `REFUSED_UNPROVABLE` → DEPENDENCY with the missing height + "rebuild_recent / -cold-import" in the message. `EV_OPERATOR_NEEDED` already has a forced alerts sink (Gate E9). The condition-side `COND_REMEDY_FAILED` surfacing remains for engine accounting but is no longer the paging path.
- **Hand-off:** after REPAIRED, next tick the backfill returns NOT_APPLICABLE (U empty) → falls through to `maybe_repair_stale_script` → dry-run now resolves via coins_kv (`creator 3,073,765 < frontier 3,135,517`) → existing replay tx rewinds script/proof/tip cursors to 3,135,517 (`rewind_coins=false` since `replay_first == utxo_cursor`) → forward stages re-validate → `utxo_apply` unpins (coin present) → H* climbs → tip_finalize advances.
- **Observability:** new `coin_backfill` subsystem in `zcl_state` — dumper includes last scan lineage hash + rebind count; registration: one line in `app/controllers/src/diagnostics_registry.c:g_dumpers`, enum_csv in `tools/mcp/controllers/diagnostics_controller.c` + `lib/test/src/test_mcp_controllers.c`. Result fields also flow through the L1 result struct → existing condition WARN logs.

---

## 4. Peer-retention / body-download fixes (minimal surgical subset)

| # | Fix | Where | Safety vs malicious peers |
|---|---|---|---|
| P1 | **Wire the usefulness counter on NEW headers only** — call `syncsvc_note_headers_received(node, newly_added)` after the counter updates in `process_headers` (`lib/net/src/msg_headers.c:406-411`; `newly_added` is already computed and in scope — `:359` increments it only for headers not previously in the index, and `:411` derives `already_known = accepted - newly_added`). The function exists with zero production callers (`header_sync_service.c:432-443`). **Do NOT pass `accepted`** — `accepted` includes already-known headers (`was_known` headers still increment it, `:356-359`), so crediting it would let a withholding peer replay known headers to refresh `last_useful_headers_time` forever (defeating rule A) and inflate `total_headers_delivered` (deflecting rule B's worst-peer eviction onto honest peers, `msgprocessor.c:1448-1466`). With `newly_added`: only headers that extend our index count; a replaying/withholding/garbage peer accrues zero and still churns out. | 1 line | Strictly strengthens accounting: only **new-to-index** headers count. Restores rules A/B to their intended semantics without opening a slot-squatting primitive. At frontier parity `newly_added==0` for everyone — which is exactly why P2 exists. |
| P2 | **Frontier-parity gate on the stale-header rules** — in `syncsvc_should_disconnect_stale_header_peer` (`header_sync_service.c:677-693`, add a `best_header_height` param; caller has it in scope at `msgprocessor.c:~1331`): return false when `best_header_height >= node->starting_height - 144`. Same guard before the worst-peer block at `msgprocessor.c:1411-1467`. **Code comment required:** `node->starting_height` is handshake-static, so the parity gate progressively relaxes stale-header discipline for long-lived peers as the chain grows past their connect-time claim — accepted consciously given IBD gating, the loopback lifeline, rule B's per-stall-cycle rotation, and the peer-floor conditions. | header_sync_service.c + msgprocessor.c | When our header frontier is at the peer's claimed tip, getheaders **cannot** be "useful" by construction (new headers only arrive per ~150s block); disconnecting is pure churn. A genuinely withholding peer (claims height ≫ our frontier) is unaffected and still cut. Needed even with P1: at frontier parity `newly_added==0` forever, so P1 alone re-kills at 120s. |
| P3 | **Trusted-peer exemption** on rules A and B (`msgprocessor.c:1396-1409`, `:1411-1467`): skip when `net_addr_is_local(&node->addr.svc.addr) || node->whitelisted` — the same predicate as the misbehavior ban (`net.c:960-969`); precedent in `syncsvc_getheaders_interval` (`header_sync_service.c:360-372`). | msgprocessor.c | Localhost is unspoofable; whitelist is explicit operator intent. Remote default peers keep full stall discipline. Protects the loopback zclassicd lifeline unconditionally. |
| P4 | **tip_watchdog accounting + reachable threshold + hysteresis** (`lib/net/src/tip_watchdog.c:49-59,115-140`, `tip_watchdog.h:37-50`): (a) `download_queue_bytes_estimate = in_flight * BACKPRESSURE_AVG_BLOCK_BYTES + queued * DL_QUEUED_ENTRY_BYTES` — queued entries are hash+bookkeeping only, counted at a new honest `DL_QUEUED_ENTRY_BYTES = 64` (kills the observed false trigger: 3,066 queued ≈ 196KB real, was "estimated" as 6GB; queued alone can never trip — would need ~4M entries); (b) `BACKPRESSURE_AVG_BLOCK_BYTES` 2MiB → **512KiB**, chosen so the trigger is **structurally reachable with margin in BOTH regimes**: non-IBD in-flight cap `DL_MAX_IN_FLIGHT_TOTAL = 1024` (`download.h:28`) gives a max estimate of 1024 × 512KiB = **512MiB > 256MiB HIGH_WATER** (trips at >512 in-flight slots, 2× headroom — the strict `>` comparison at `tip_watchdog.c:121-122` is fine once margin is structural); IBD cap 4096 trips at the same >512. Real per-slot caps for reference: 128/peer, 512 loopback, 1024 total at tip, 4096 IBD (`download.h:26-29`). Received bodies are persisted to disk by the staged pipeline, not held resident — the constant estimates backlog growth; >512 outstanding worst-case bodies is a genuine 100MB–1GB exposure band, the regime the 2026-04-18 backstop exists for (`tip_watchdog.h:5-11`); (c) re-arm latch: after `cooldown_elapsed` exit (`:134-137`), `enter_active` requires the estimate to have dropped below HIGH_WATER/2 since the last exit (static `_Atomic bool g_armed`, init true; set when below low-water; consumed on enter). Keep the 256MiB cap, the 120s cooldown, and `dl_drain_for_backpressure` on entry. **Unit test required** asserting trigger reachability at both caps and non-reachability from queued-only (§5). | tip_watchdog.c/.h | The OOM-guard intent survives and is now *live* at tip. in_flight only grows via **our own getdata**, so no peer can inflate the trigger; unsolicited spam is handled by existing per-message limits and misbehavior scoring. Hysteresis only prevents the observed active→cooldown→active flapping on an unchanged backlog. **Expected behavior note for proofs/deploy:** during repair-pinned catch-up the tip is legitimately stalled while bodies flow, so the (now-working) watchdog will intermittently activate and gate bodies in 120s windows — an intended OOM-guard sawtooth, not the wedge. |
| P5 (small, optional-P2) | Restrict the two addnode-backoff resets to addnodes that have **ever completed a handshake**: `peer_floor_violated.c:112-118` and `reset_local_addnode_backoff` (`app/services/src/sync_monitor.c:210-220`). Deploy note: remove the dead `127.0.0.1:8034` addnode from the live config. | conditions + sync_monitor | Never-handshaken endpoints are dead config, not recoverable peers (they still retry on the connman 1800s backoff cap, `connman.c:734-743`); handshake-proven addnodes keep the fast-recovery reset. Reduces the 2,374 tcp-fail churn noise. |

**Explicitly NOT taken:** parsing block payloads pre-dispatch to exempt solicited bodies — P4 makes it unnecessary and it would add a parse surface in the hot path; any IBD-predicate rewrite — out of scope, riskier.

Combined effect while wedged/repairing: peers survive (P1/P2/P3), headers track the network tip, bodies flow to disk under a sane backpressure bound (P4) — so the moment the repair clears, catch-up is immediate, and the node keeps serving `getdata`/headers to others throughout.

---

## 5. Test plan

Build/lint/test gate: `make -j"$(nproc)" build-only && make lint && make test_parallel` (read the "N passed, M failed" line). Inner loop for the repair tests: `make t ONLY=stage_repair_coin_backfill`; never `test_zcl`.

**`lib/test/src/test_stage_repair_coin_backfill.c`** (auto-globbed; group `stage_repair_coin_backfill`; uses the `coin_backfill_io` seam — fixture block-reader + fixture `node_db`, modeled on `test_stage_reducer_unwedge.c`):
1. **Happy path:** synthetic progress.kv (hole row, cursors, frontier) + fixture chain; coin missing, creator below horizon, unspent → scan completes (multi-chunk, prev-linked end to end), terminal linkage hits H == hole hash; insert tx writes coin + outpoint marker + deletes replay marker; cursors/logs untouched; re-run → NOT_APPLICABLE.
2. **ADVERSARIAL — coin actually spent mid-range:** fixture block at creator+k spends the outpoint → `REFUSED_SPENT`, **zero writes to coins**, refusal marker set, typed blocker + EV_OPERATOR_NEEDED emitted ONCE (latch verified on second call), second call refuses without rescanning (no loop).
3. **ADVERSARIAL — creating block off-chain:** fixture reader returns a block whose recomputed tx hashes don't contain the txid (or hash-check fails) → refuse, no write.
4. **ADVERSARIAL — delta-window creator:** creator_height ≥ delta_horizon → `refused_in_delta_window`, **whole set refused** even when other members pass, no write (bounded fallback = none, by design).
5. **Scan gap:** missing body mid-range → `REFUSED_UNPROVABLE`, DEPENDENCY blocker set with the missing height + rebuild_recent/cold-import escape text, resumable after the body appears.
6. **Coin present-but-unusable** (height ≥ frontier) → refuse `coin_present_unusable`, no write.
7. **Coinbase immaturity** (H − creator < 100) → genuinely-invalid refuse.
8. **Multi-coin block:** two missing coins → both enumerated, ONE scan pass, both inserted atomically; replay marker cleared once.
9. **Resume integrity:** scan cursor persists; tampered set digest or changed frontier → scan restarts from floor; frontier moved between scan and insert → insert ROLLBACK.
10. **Owner gate:** env unset → `OWNER_REFUSED`, no write, **direct page emitted on the first refusing call** (not after exhaustion); cursor movement between ticks does NOT suppress or reset the page latch. **Value bounds:** value > MAX_MONEY → refuse. **Round cap:** 9th round refuses. **Marker-seen reappearing outpoint — at a DIFFERENT hole height H′** → MARKER_SEEN + PERMANENT blocker (outpoint-only key proven).
11. **Dispatcher ordering:** lowest hole is `internal_error` → backfill NOT_APPLICABLE (existing replay owns it).
12. **ADVERSARIAL — chain rebind on resume (kill-9 + reorg):** persist scan state mid-range; fixture reorg replaces blocks (a) above next_height — resume's first block fails prev-link against persisted `last_scanned_hash` → `CHAIN_REBOUND`, restart from floor, **no insert against a mixed-branch proof**; (b) reorg of the hole block H itself after a CLEAN scan → insert-tx step-1 active-H re-read mismatches hole hash → ROLLBACK/refuse; (c) reorg of `frontier-1` after CLEAN → active hash ≠ persisted `top_hash` → ROLLBACK. All three must REFUSE/restart, never insert.
13. **Terminal linkage:** scan reaches top but the `[frontier..H]` walk's block H hash ≠ hole row hash (fixture mid-chunk reorg with fork point ahead of read position) → not CLEAN, restart/refuse, no write.

**Net tests** — extend existing owners: `lib/test/src/test_header_sync_stall.c` (P1: `newly_added>0` refreshes `last_useful_headers_time`; **`accepted>0` with `newly_added==0` (known-header replay) does NOT refresh and does NOT inflate `total_headers_delivered`**; P2: rule skipped at frontier parity, fires when frontier ≪ starting_height; P3: loopback/whitelisted exempt, remote not) and `lib/test/src/test_net.c` watchdog section (P4: **reachability asserted at both caps** — 1024 in_flight × 512KiB > HIGH_WATER activates after stall, 4096 IBD likewise; queued-only backlog (100k entries × 64B) never activates; no re-entry after cooldown without dropping below low-water; `tip_watchdog_test_reset` extended for the latch).

**Registration (3 shared places):** declare `int test_stage_repair_coin_backfill(void);` in `lib/test/include/test/test_helpers.h`; `failures += …;` in `lib/test/src/test.c`; `X(stage_repair_coin_backfill)` in the `TEST_LIST` X-macro `lib/test/src/test_parallel.c:58+`.
