# OS-S2 — Boot O(delta): kill the four O(chain) boot passes with persisted cursors

Plan of record: `~/.claude/plans/think-more-about-our-keen-crown.md` §2 SPEED S2 (line 234) + §3
step 3 (WF-substrate). Law 4: *boot is a verification ladder, never a scan*. This doc is the
executable recipe for the four named O(chain) boot passes. All anchors below were re-read on
`main @ 6405cf48d` this session; re-verify line numbers before editing (prose rots, code moves).

## The shipped template (copy this shape)

`pprev` repair is ALREADY cursored — it is the pattern to replicate. Read it first:

- `config/src/boot.c:2474-2489` — the call site: read cursor → pass as `min_height` → repair
  returns `out_max` → persist cursor only if it advanced.
  ```c
  int64_t pprev_done = -1;
  if (g_node_db.open)
      node_db_state_get_int(&g_node_db, "pprev_repaired_height", &pprev_done);
  int pprev_max = -1;
  int pprev_fixed = block_index_repair_pprev(&g_state, ctx->datadir,
                                             (int)pprev_done, &pprev_max);
  ...
  if (g_node_db.open && pprev_max > (int)pprev_done)
      node_db_state_set_int(&g_node_db, "pprev_repaired_height", pprev_max);
  ```
- `app/services/src/block_index_integrity.c:238` signature `(ms, datadir, min_height, out_max_height)`;
  the filter that skips already-verified work is at `block_index_integrity.c:~447`
  (`... && pi->nHeight > min_height`), with genesis/detached roots always included.

Persistence substrate for ALL four cursors (verified):
- Table: `node_state(key TEXT PRIMARY KEY, value BLOB)` — `database_schema.c:299`.
- API: `node_db_state_get_int(ndb, key, &int64)` / `node_db_state_set_int(ndb, key, int64)` —
  decl `app/models/include/models/database.h:151-152`, impl `database_migrate.c:160`.

Reorg invalidation substrate (verified): the finalized-frontier rewind that detects a reorg and
rewinds to the fork is `rewind_cursor_if_active_chain_reorged` — `tip_finalize_stage.c:226`
(called `:730`) and `header_admit_stage.c:385` (called `:644`); `fork_plus1` computed at
`utxo_apply_delta_reorg.c:406`. Add ONE helper `boot_cursor_clamp_on_reorg(ndb, fork_height)`
(new, in `config/src/boot_cursor_state.c` below) and call it from the tip_finalize rewind path
after the rewind commits, so every boot cursor is clamped to `min(existing, fork_height - 1)`.

## New shared module — `config/src/boot_cursor_state.{c,h}`

One file owns the four keys, the clamp, and a shared A/B digest so no cursor logic is scattered.

```c
/* boot_cursor_state.h */
#define BOOT_CUR_HEIGHTS      "heights_repaired_upto"
#define BOOT_CUR_HEIGHTS_CNT  "heights_repaired_count"
#define BOOT_CUR_NCHAINTX     "nchaintx_complete_upto"
#define BOOT_CUR_WALLET       "wallet_last_scanned_height"   /* per-wallet: append ":"<walletid> */
/* single-pass scan facts ride the existing fast_restart shutdown binding, not node_state */

struct node_db;
/* Clamp every boot-derived cursor to fork_height-1 on a confirmed reorg. Idempotent, min-only. */
void boot_cursor_clamp_on_reorg(struct node_db *ndb, int fork_height);

/* Cheap O(n) block-index fingerprint over (phashBlock ^ nStatus ^ nChainWork.lo ^ nHeight ^ nChainTx).
 * One tight XOR-accumulate loop, no arith_uint256_compare, no branches on payload. Used by the
 * scan-facts validation (#3) and by every A/B equality test. */
uint64_t boot_block_index_fingerprint(const struct main_state *ms);
```

`boot_cursor_clamp_on_reorg` body: for each of the three `node_state` int keys, `get_int`; if
present and `> fork_height-1`, `set_int(key, fork_height-1)`. (The scan-facts binding #3 needs no
clamp — see #3 invalidation.) Wire the call into `tip_finalize_stage.c` right after
`rewind_ok = rewind_cursor_if_active_chain_reorged(db)` at `:730` succeeds, passing the fork height
the rewind already computed.

Consensus-parity note (applies to all four): these cursors gate **boot-derived navigation indices
and a wallet projection** — `block_index.nHeight/pprev/nChainTx/nChainWork`, best-header selection,
and `wallet_utxos/wallet_transactions`. NONE touch H\* (the `reducer_frontier` fold over
`progress.kv`), `coins_kv`, or `sapling_anchors/nullifiers`. A stale/wrong cursor can only cause a
*re-scan* (slower boot), never a wrong consensus value — the fold and coins authority are unread by
these passes. The A/B equality test per item is what proves the fast path is byte-identical to the
full path; that is the merge bar.

---

## #1 — `block_index_repair_heights` full map walk

**Current (verified O(chain), cursor-less):** `boot.c:2466`
`index_repaired += block_index_repair_heights(&g_state);` and re-called at `boot.c:2484` only when
`pprev_fixed > 0`. Impl `block_index_integrity.c:225`: Pass-1 (`:246-272`) walks the ENTIRE map
counting `wrong` heights (in-RAM pointer compares over ~3.1M entries); returns 0 at `:274` if
`wrong==0`. No cursor — the full count runs every warm boot.

**Recipe:**
1. Add range overload in `block_index_integrity.c`:
   `int block_index_repair_heights_range(struct main_state *ms, int min_height, int *out_max_height);`
   Restrict Pass-1 count and Pass-2 propagation to entries with `nHeight > min_height` (genesis +
   detached roots ALWAYS included, exactly as pprev at `:447`); track the max height into
   `*out_max_height`. Keep the existing `block_index_repair_heights(ms)` as
   `return block_index_repair_heights_range(ms, -1, out_max ? &... : NULL);` so the second caller at
   `boot.c:2484` and `block_index_heights_repaired()` consumers are unchanged.
2. At the `boot.c:2464-2466` call site, wrap with the pprev shape:
   ```c
   int64_t heights_done = -1, heights_cnt = -1;
   if (g_node_db.open) {
       node_db_state_get_int(&g_node_db, BOOT_CUR_HEIGHTS,     &heights_done);
       node_db_state_get_int(&g_node_db, BOOT_CUR_HEIGHTS_CNT, &heights_cnt);
   }
   /* Fast-skip only when the persisted count matches (no entries added/removed since). */
   int hmax = -1;
   if (g_state.map_block_index.size > 100 &&
       !(heights_cnt == (int64_t)g_state.map_block_index.size && heights_done >= 0))
       index_repaired += block_index_repair_heights_range(&g_state, (int)heights_done, &hmax);
   if (g_node_db.open && hmax > (int)heights_done) {
       node_db_state_set_int(&g_node_db, BOOT_CUR_HEIGHTS, hmax);
       node_db_state_set_int(&g_node_db, BOOT_CUR_HEIGHTS_CNT,
                             (int64_t)g_state.map_block_index.size);
   }
   ```
- **Persisted:** `node_state[heights_repaired_upto]` (high-water height) + `heights_repaired_count`
  (map size guard).
- **High-water update point:** immediately after the repair returns, `boot.c:~2466` (above).
- **Invalidation on reorg:** `boot_cursor_clamp_on_reorg` sets it to `fork-1`; newly-connected
  blocks above the fork get correct `nHeight` at `header_admit` insert, so next boot only re-verifies
  the delta.
- **A/B proof:** boot the datadir copy twice — once with the key deleted (full repair), once warm
  (cursor). Assert `boot_block_index_fingerprint(&g_state)` restricted to the `nHeight` component is
  bit-identical, and `index_repaired` deltas reconcile. Test group: `block_index_integrity` (extend
  `lib/test/src/test_block_index_integrity.c`).

---

## #2 — nChainTx pre-scan

**Current (verified O(chain), cursor-less):** `boot.c:2389-2409` — `nchaintx_already_computed`
walks the WHOLE map (`block_map_next` over ~3.1M entries) checking `nTx>0 && nChainTx==0` and
pprev-consistency. Even on the happy path (prints `"nChainTx already computed, skipping
propagation"` at `:2411`) it pays a full O(n) read. Propagation itself is `:2412-2450`.

**Recipe:**
1. Read cursor before the pre-scan:
   ```c
   int64_t nct_upto = -1;
   if (g_node_db.open)
       node_db_state_get_int(&g_node_db, BOOT_CUR_NCHAINTX, &nct_upto);
   ```
2. Restrict the `nchaintx_already_computed` loop (`:2394-2408`) to entries with
   `bi->nHeight > (int)nct_upto` — a parent at/below the cursor already carries a verified
   contiguous `nChainTx`, so a child above it is checked against the trusted parent total. If no
   violation is found above the cursor, skip propagation (unchanged behavior, smaller scan).
3. When propagation DOES run (`:2412-2450`), it already forward-fills from genesis; no change needed
   beyond persisting afterward.
4. After the block (`boot.c:~2450`), stamp the cursor to the tip height only when the index is
   proven contiguous-complete (no txn-bearing block with `nChainTx==0` at/below tip):
   ```c
   int tip_h = active_chain_height(&g_state.chain_active);
   if (g_node_db.open && tip_h > (int)nct_upto)
       node_db_state_set_int(&g_node_db, BOOT_CUR_NCHAINTX, tip_h);
   ```
- **Persisted:** `node_state[nchaintx_complete_upto]`.
- **High-water update point:** end of the nChainTx block, `boot.c:~2450`.
- **Invalidation on reorg:** `boot_cursor_clamp_on_reorg` → `fork-1`. `nChainTx = pprev->nChainTx +
  nTx` changes when the branch changes below the fork, so re-derivation above `fork-1` is required
  and the clamp forces it.
- **A/B proof:** full vs warm boot; assert the `nChainTx` component of
  `boot_block_index_fingerprint` is identical, and `find_most_work_chain()` returns the same tip
  height (the original bug this pass defends against — `tip=X most_work=Y, Y<<X`). Test group:
  `block_index_integrity`.

---

## #3 — single-pass block-index scan

**Current (verified O(chain), cursor-less):** `boot.c:2634-2683`. One pass derives
`scan_best_header` (most `nChainWork`), `scan_fallback` (most work with `HAVE_DATA + nChainTx>0`),
`scan_reindex_best` (highest with `pprev + nChainTx`), `scan_max_have_data_h`,
`scan_missing_header_data`, and CLEARS `BLOCK_FAILED_MASK` (`:2652-2655`, a write). Consumers:
fast_restart (`:2694-2711`), reindex (`:2714`), chain-tip restore (`:2764`). No cursor.

This one is not a height high-water; it is "persist derived facts at shutdown, cheap count+digest
validate at boot, full scan only on mismatch" (S2 line 234). **Reuse the existing shutdown-facts
mechanism** — do NOT invent a new one:
- Shutdown persist: `boot_fast_restart_capture_shutdown_facts` — `boot_fast_restart.c:154`, called
  at teardown `boot_services.c:1622`. Already binds `tip`, `coins_best`, `block_index_count` and
  refuses to bind unless `container_tip == durable_tip` (`:179`).
- Boot validate/consume: `boot_fast_restart_try` — `boot_fast_restart.c:97`, called `boot.c:2701`.

**Recipe:**
1. Extend `struct fast_restart_shutdown_facts` (in `config/include/config/boot_shutdown_marker.h`)
   with: `uint8_t scan_best_hash[32]; int32_t scan_best_h; uint8_t scan_fallback_hash[32];
   int32_t scan_fallback_h; uint8_t scan_reindex_hash[32]; int32_t scan_reindex_h;
   int32_t scan_max_have_data_h; uint64_t scan_fingerprint;` (fingerprint =
   `boot_block_index_fingerprint`).
2. In `boot_fast_restart_capture_shutdown_facts` (`:182-207`, inside the healthy `else`), fill the
   scan fields from the live `scan_*` pointers (thread the last-computed values through
   `main_state`, or recompute from the map at teardown — cheap, one pass).
3. BEFORE the single-pass scan at `boot.c:2634`, add a cheap-validate gate:
   ```c
   struct shutdown_clean_binding fr_b;
   bool scan_from_binding = false;
   if (!ctx->reindex_chainstate && !rebuilt_from_log && g_node_db.open &&
       boot_shutdown_marker_peek_fast_restart_binding(&fr_b) &&
       fr_b.scan_fingerprint == boot_block_index_fingerprint(&g_state) &&
       fr_b.block_index_count == (int64_t)g_state.map_block_index.size) {
       scan_best_header  = block_map_find(&g_state.map_block_index, /*scan_best_hash*/);
       scan_fallback     = block_map_find(&g_state.map_block_index, /*scan_fallback_hash*/);
       scan_reindex_best = block_map_find(&g_state.map_block_index, /*scan_reindex_hash*/);
       scan_max_have_data_h = fr_b.scan_max_have_data_h;
       scan_from_binding = (scan_best_header != NULL);
   }
   if (!scan_from_binding) { /* existing full single-pass scan block :2646-2683 */ }
   ```
   The fingerprint includes `nStatus`, so any residual `BLOCK_FAILED_MASK` bit changes it → binding
   rejected → full scan (which clears the bit). On a clean warm boot there are no FAILED bits (last
   run cleared them and the binding recorded the clean fingerprint), so skipping the write is safe.
- **Persisted:** the extended `fast_restart` shutdown binding (already a signed/clean-shutdown-gated
  marker; NOT `node_state` — it must co-invalidate with the clean-shutdown quick-check).
- **High-water update point:** shutdown, `boot_fast_restart_capture_shutdown_facts`.
- **Invalidation on reorg / unclean shutdown:** automatic — the binding is only written on a clean
  shutdown at-tip (`container_tip == durable_tip`, `:179`); a reorg or crash leaves them divergent
  or the marker absent, so the next boot recomputes. No clamp needed.
- **A/B proof:** full vs warm boot; assert `scan_best_header/fallback/reindex_best` map to identical
  `{hash,height}`, `scan_max_have_data_h` equal, and `scan_cleared_failed==0` on the warm path. Test
  group: `shutdown_marker` (extend `lib/test/src/test_shutdown_marker.c`) + `block_index_integrity`.

---

## #4 — `wallet_scan_blocks(0, tip)`

**Current (verified O(chain) DISK reads — the dominant cost):** `boot.c:3921-3924` calls
`wallet_scan_blocks(&g_node_db, &g_state.chain_active, &g_wallet, ctx->datadir, 0, tip_h)`. Impl
`wallet_scan.c:149`; the expensive part is Pass-1 (`:203-246`) — a parallel raw byte scan of EVERY
`blocks/blk%05d.dat` file (`:194` loops files 0..199). `start_height` is always 0, so all history
is re-read every boot. Returns `found` (`:298`); no birthday cursor.

**Recipe:**
1. Persist per-wallet cursor keyed by a wallet fingerprint (single wallet today; key
   `wallet_last_scanned_height:<walletid>` supports future multi-wallet):
   ```c
   int64_t last = -1;
   node_db_state_get_int(ndb, wallet_cursor_key(w), &last);
   int birthday = wallet_earliest_key_height(w);      /* 0 if unknown; new keys carry a height */
   int start = (int)((last >= birthday ? last + 1 : birthday));
   if (start < 0) start = 0;
   ```
   At `boot.c:3922`, change `0` → the computed `start` (compute inside `wallet_scan_blocks` so the
   signature is unchanged and the callers stay stable; `wallet_scan.h:20`).
2. Skip Pass-1 FILES below the birthday: map `start_height` → its block's `nFile` via
   `active_chain` (`chain->block_index` for `start`), and begin the file loop at that `min_file`
   instead of 0 (`wallet_scan.c:194` → start `f = min_file`). Pass-2 (`wallet_scan_pass2_execute`)
   already filters decoded blocks to `[start_height, end_height]`, so no double-count.
3. Stamp the cursor after Pass-2 commits, right before `return found;` at `wallet_scan.c:~297`:
   ```c
   if (found >= 0 && ndb && ndb->open)
       node_db_state_set_int(ndb, wallet_cursor_key(w), end_height);
   ```
   (Only after the pass-2 SQLite transaction committed — `found >= 0` is the success signal;
   `-1` paths at `:213/:238` return before this.)
- **Persisted:** `node_state[wallet_last_scanned_height:<walletid>]`.
- **High-water update point:** `wallet_scan.c:~297`, post-commit.
- **Invalidation on reorg:** `boot_cursor_clamp_on_reorg` → `fork-1`, so the reorged range is
  re-scanned into `wallet_utxos/wallet_transactions`. ALSO: a key import below the cursor must reset
  the cursor to `new_key_height - 1` — wire that into the wallet key-import path (grep
  `wallet keystore` add-key site) so a restored old key still gets its history.
- **A/B proof:** full scan (`0..tip`, key deleted) vs cursored warm boot; assert
  `SELECT count(*), <digest>` over `wallet_utxos` and `wallet_transactions` are identical, and
  `found` reconciles (warm `found` counts only the delta). Test group: **NEW** — `wallet_scan.c`
  has no test and no impact-rule mapping today (see Gates below).

---

## New gate + tests (the proof)

1. **Lint / impact-rule (Trap A — REQUIRED to push):** every changed `.c` must map to a focused
   group in `app/controllers/include/controllers/agent_impact_rules.def`.
   - `config/src/boot.c` already maps (`def:155`).
   - `app/services/src/block_index_integrity.c` maps (`def:396`).
   - `config/src/boot_fast_restart.c` + `boot_shutdown_marker.h` map (`def:393`).
   - **`app/controllers/src/wallet_scan.c` has NO mapping and NO test — add both.** New rule:
     `AGENT_IMPACT_RULE("app/controllers/src/wallet_scan.c|app/controllers/include/controllers/wallet_scan.h|lib/test/src/test_wallet_scan_cursor.c", "wallet_scan_cursor make_lint_gates")`.
   - New file `config/src/boot_cursor_state.c` needs a mapping too (fold into the `boot.c` rule
     regex at `def:155` or add its own).
2. **New A/B equality test harness** `lib/test/src/test_boot_odelta_cursors.c` (test group
   `boot_odelta_cursors`): drives a small synthetic block-index fixture (NOT a live datadir) through
   full-then-warm boot for #1/#2/#3, asserting `boot_block_index_fingerprint` and the derived
   `scan_*`/`nHeight`/`nChainTx` facts are byte-identical, plus reorg-clamp forces re-derivation
   above `fork-1`. Register in the test manifest + `make lint` group counts (Merge collision point
   per plan §3b: test-group counts, `tools/scripts/check_doc_counts.sh`).
3. **New `test_wallet_scan_cursor.c`** (group `wallet_scan_cursor`): fixture wallet + fixture
   `blk*.dat`; assert warm-boot `wallet_utxos/wallet_transactions` row-count+digest equals full-scan,
   and that a key-import-below-cursor resets the cursor.

## Acceptance bar

- **Measured boot-to-serving on a full-history datadir COPY** (never a live datadir — copy-prove
  law): capture `[boot] wallet_scan_blocks`, `blkidx.scan` submark, and the nChainTx/height-repair
  timings before/after via the existing `boot_submark`/`boot_phase` timing already in-tree. Target
  **<30s** boot-to-serving (dominated by #4's disk-read elimination; #1/#2/#3 are in-RAM and shave
  the residual O(n) walks).
- **A/B loader-output equality** per item above (fingerprint + wallet-table digest byte-identical
  between full and warm boot). This is the merge bar, run in the lane worktree by the Verify-stage
  sonnet verifier.
- **Consensus-parity confirmation:** `zclassic23 dumpstate reducer_frontier` H\* identical
  full-vs-warm; `coins_kv` count/best-block identical; `sapling_anchors` untouched. These passes
  never read the fold or coins authority — the equality tests plus the H\* check close it.
- Full `make test-parallel` green on integrated main; no `core/`, no datadir, no
  producer touched.
