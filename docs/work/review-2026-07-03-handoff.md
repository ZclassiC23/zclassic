# Code review 2026-07-03 — handoff & deferred-work checklist

A 15-dimension multi-agent review (87 agents; reviewer → adversarial verify →
synthesis), followed by **first-hand spot-checks of the top findings against
source**. This file is the checklist for the next developer (human or Claude).

**Live node at review time:** at tip, h=3168128, build `61610ecc3`, clean tree.

## ⚠️ Read this before re-reporting anything: the LOG-macro blind spot

`LOG_FAIL` / `LOG_ERR` / `LOG_NULL` (`lib/util/include/util/log_macros.h:29-47`)
are **guard macros that `return`** (`LOG_FAIL`→`return false`, `LOG_ERR`→`return -1`,
`LOG_NULL`→`return NULL`). They are NOT non-returning `fprintf`. The non-returning
ones are `LOG_WARN` / `LOG_INFO` (lines 74-83).

**Three independent agent passes** flagged "missing return after LOG_*" / "falls
through to NULL-deref" at sites that are in fact safe, because the macro returns.
**Always expand the macro before reporting a "missing return" at a `LOG_*` site.**
The real defect class here is *wrong-macro-for-return-type* (see item D), not
"missing return."

### Refuted false positives — DO NOT re-chase
These were flagged as real and verified **safe** on spot-check (the macro returns;
no fall-through, no crash, no parity break):
- `app/controllers/src/transaction_controller.c:356/404/411` (createrawtransaction OOM)
- `app/controllers/src/dbquery_controller.c:100-166` (every gate uses `LOG_FAIL`)
- `app/models/src/database.c:535-555` `node_db_prepare_readonly_query`
- `app/services/src/snapshot_fetch.c:502` (`LOG_ERR` returns before the memcpy)
- `app/controllers/src/repair_controller_utxo.c:84-135` (each branch `free; LOG_FAIL`)
- `lib/validation/src/check_block.c:381-422` `bip34_check_coinbase_height` — the
  "BIP34 OOB read + parity-BREAKING accept" claim. Every size guard uses
  `LOG_FAIL`→returns false; the `memcmp` is only reached when `sig->size >= expect_len`.
  Consensus path is parity-clean.

## Shipped in this review (commit 65a13bb5c, test_parallel 487/487)
- `wallet_scan.c:208`, `wallet_rescan_controller_witness.c:86`, `legacy_import_scan.c:308`
  — NULL-check `zcl_calloc`/`zcl_malloc` before deref.
- `boot_index.c:594` — utxo_max_h promote path now enforces `utxo_recovery_block_trust_rooted`
  (matches the two sibling tip-promotion paths).

---

## Deferred fix checklist (ranked; each needs copy-prove before live)

Priority = severity × likelihood × how cheaply it generalizes its class.

### A. [HIGH, architectural] `block_index` field data races — decide the cs_main strategy
`nStatus` / `nFile` / `nDataPos` are plain non-atomic ints (`lib/.../chain.h:104-109`).
The reducer drive writes them; readers mutate/read them off-thread without `cs_main`:
- `config/src/boot_background_workers.c:566,576` — `watchdog_check_stuck` does
  `bi->nStatus &= ~BLOCK_FAILED_CHILD` from `payment_processor_thread`, no lock.
- `app/services/src/bg_validation_service.c:500` — bg-validation reads `nStatus/nFile/nDataPos` unlocked.
- `app/jobs/src/tip_finalize_post_step.c:190,241` — MMR/MMB append on the drive vs concurrent RPC reads.
- `app/views/src/explorer_pages_hodl.c:155` — static chart buffers raced by HTTPS/onion workers.

**Why this is deferred, not a quick lock:** a one-sided `zcl_mutex_lock(&ms->cs_main)`
in the watchdog does NOT fix the race unless the *writer* (reducer drive
`block_index_set_*`) also takes `cs_main`. Adding cs_main to the drive's hot path
interacts with the LOCK-ORDER LAW (drive holds `coins_kv`) and with the in-flight
Lane-B coins_ram seqlock+RCU work (`project_laneb_coinsram_rcu_held_2026-06-25`).
**The fix is a coordinated block_index mutation strategy** — either (a) cs_main on
both sides, (b) move all nStatus mutation onto the drive thread (watchdog sets a
"please clear this height" flag, drive does it), or (c) the seqlock/RCU treatment
Lane-B started. Decide once, apply to the whole class. TSAN run recommended first.

### B. [MEDIUM, cheap] `apply_hash_agreement` per-height SQL on every finalize
`app/jobs/src/reducer_frontier.c:530` — loops `anchor+1..*hstar` calling
`log_hash_at` (prepare+step+finalize) **twice** per height, on every block finalize.
Bites during catch-up / re-derive (large anchor..hstar span). Convert to one JOIN;
the twin detector `stale_script_hash_split_unlocked` already uses the exact JOIN
(`SELECT MIN(v.height) FROM validate_headers_log v JOIN script_validate_log s …`),
and `log_contiguous_prefix` measured "53× cheaper" for the same conversion.

### C. [MEDIUM] Hot-path O(N²) + per-request full-table scans
- `app/jobs/src/utxo_apply_delta.c:96` `lookup_added` — linear scan of `added[]`
  per input AND per output (intra-block create-then-spend). Use a hash index.
- `app/jobs/src/utxo_apply_nullifiers.c:38` `nf_seen` — O(N²) over the per-block nullifier accumulator.
- `app/views/src/explorer_pages_hodl.c:103` — `hodl_wave_scan_current_utxos` does a
  full `utxos` scan (~1.34M rows) on **every** `/explorer/hodl` hit, `Cache-Control: no-store`.
  Cache the result keyed by tip-hash, or materialize a running aggregate.
- `adapters/outbound/persistence/src/hodl_history_sqlite.c:38` `hh_compute_snapshot` —
  O(K²) over samples on fresh-node backfill / projection rebuild.
- `app/views/src/explorer_stats_gather.c:23` — 5-6 independent full `tx_outputs` scans;
  collapse into one conditional-aggregate query.
- `lib/storage/src/coins_kv.c:360` — two SELECTs + allocates `(max_vout+1)` 10 KB
  `tx_out` slots to read a single vout. Read one row.
- `app/jobs/src/created_outputs_index.c:168` — `created_outputs_index_prune_below`
  has ZERO production callers; the table grows O(chain outputs) in `progress.kv`. Wire the prune.

### D. [MEDIUM, cheap, retires a class] wrong-macro-for-return-type lint gate
- `app/services/src/bg_validation_scripts.c:124` — `LOG_ERR` does `return -1` in a
  `_Bool` function → becomes `true`. On `pthread_create` failure: the inline re-verify
  is skipped, already-created threads aren't joined (leak), and the pass reports scripts valid.
  Fix directly (use `LOG_WARN` + `workers[t].result=false`, or `LOG_FAIL`).
- **Add a lint gate** that bans `LOG_ERR`/`LOG_NULL` in `_Bool`-returning functions
  and `LOG_FAIL` in non-bool functions. This is the *correct* version of the
  "missing-return" gate the synthesis proposed (the macro already returns; the bug is
  using the wrong one). Retires the class.

### E. [MEDIUM] HODL chart correctness cluster (recent work)
- `app/services/src/hodl_history_service.c:92` — `if (!port->upsert_snapshot(...)) { /* empty */ }`
  then `return true` + emit projection → phantom projection on upsert failure. Add `return false`.
- `app/views/src/explorer_pages_hodl.c:159` — fixed 2047-row ascending load drops the
  most recent samples once the table grows past it. Cap by recency or add `ORDER BY height DESC`.
- `adapters/outbound/persistence/src/hodl_history_sqlite.c:150` — repair only re-flags
  `total_zat=0` rows; stale-non-zero rows from a partial projection are never recomputed.

### F. [MEDIUM] Explorer API — three real (self-verified by the explorer pass)
- `app/controllers/src/explorer_controller_pages.c:240-341` — factoids HTML cache key is
  height-only (no block hash); serves stale H during the recompute window and is reorg-blind.
  Bind a block hash into the key (in-memory + on-disk), surface `served_height`/`fresh`
  like the JSON variant (`explorer_factoids_view.c:265-308`), re-check the hash at
  `start_tip` before the publish guard commits.
- `lib/net/src/https_server.c:396-418` — ACME handler path containment is only
  `strstr(filepath, "..")`; route through `path_check_url_arg` (`lib/util/src/path_check.c:26-43`)
  + `realpath()` prefix check (as `blog_controller.c:146-148` does). Not currently
  exploitable (prefix gate + no URL-decode), but brittle.
- `app/controllers/src/api_controller.c:260-289` `api_json_error` — returns
  `hlen+blen` even when the body was truncated to fit `max`; the SSL_write loop then
  over-reads. Latent today (callers pass small static messages). Return
  `hlen + actual_blen` (capture the bounded `json_write` return).

### G. [MEDIUM] Wallet-key locking inconsistency
- `lib/wallet/src/sapling_keys.c:171` — Sapling keystore readers
  (`have_spending_key`/`find_by_ivk`/`find_by_address`) don't take `sks->cs`; the
  transparent keystore (`keystore.c:91-150`) does. Race with concurrent
  `import_xsk`/`new_address`.
- `app/controllers/src/wallet_shielded_send.c:249-252` — t→z OVK read skips the lock.
- `app/jobs/src/tip_finalize_post_step.c:160` — `wallet->best_block_height` written
  from the drive without `wallet->cs`, read unlocked by ZSLP/shielded RPC.
Mirror the transparent-keystore locking convention across all three.

### H. [MEDIUM] Boot / recovery cluster (individually bounded, collectively a pass)
- `app/services/src/chain_restore_repair.c:602` — `chain_restore_finalize` takes
  `cs_main` then resolves tip through `progress_store_tx_lock` → latent ABBA vs the
  reducer reconcile's opposite order (gated only by a comment today).
- `config/src/boot.c:696` — mainnet ZK-params `pthread_create` failure logs WARN but
  never raises the documented `params_missing` PERMANENT blocker (a stall with no named owner).
- `app/jobs/src/stage_repair_reducer_frontier_purge.c:279` — non-canonical purge can
  delete rows below/unknown the coins frontier where no healer refills → strand H*.
  Gate the purge on `coins_applied_found`.
- `app/services/src/sticky_escalator.c:327` — `clear_episode()` doesn't reset
  `g_rearm_until_unix` / `g_rederive_last_repair_unix`; stale per-episode timers can
  suppress the next episode's auto-armed cure.
- `app/conditions/src/reducer_frontier_reconcile_light.c:554` — `progressing()` signal
  (3) has no baseline re-snapshot → can spuriously reset the attempt budget indefinitely.
- `app/jobs/src/reducer_frontier.c:753` — `log_frontier_above` skips the served-tip
  cursor normalization its sibling applies.
- `app/services/src/chain_restore_repair.c:409` — `clear_failed_above_tip` lacks the
  `tip_h >= 0` guard its sibling has (unset tip -1 → clears BLOCK_FAILED on every entry,
  drops genesis HAVE_DATA). `:383` `nChainWork` from hash-unsorted parents.

### I. [LOW, subtract-don't-add] Binary consolidation (owner directive 2026-07-03)
The architecture already bundles every service into one binary (`zclassic23`,
Makefile:505). The split is clients + dev/test/ops tools (legitimate). Cleanups:
- **Delete `zcl-browser`** (target + `tools/zcl-browser.c`) — orphan, duplicates `-gui`,
  fails on headless boxes.
- Fold `session` / `bot` dev scenarios into `test_parallel`.
- Consolidate `zclassic-cli` + `zcl-rpc` into the one installed client.
- **OPEN QUESTION (owner decision):** the binary is built as `zclassic23` everywhere
  (Makefile, systemd unit `zclassic23.service`, `~/.local/bin/zclassic23-live`, MCP
  server names, CLAUDE.md). Owner calls it `zclassic`. A rename is a coordinated
  change (binary path + systemd unit + live-deploy path + MCP datadir refs + docs).

---

## Method / what was covered
- 13 of 15 dimensions completed in the main workflow; `consensus-parity` and
  `explorer-api` initially failed on a rate-limit and were re-run standalone
  (both reports folded in above: consensus parity-clean, explorer solid).
- Raw workflow output: "49 confirmed / 22 refuted." After first-hand spot-check
  (~11 findings read against source, 5 refuted by the LOG-macro blind spot), the
  truly-real set is smaller; the verified-real items are the checklist above.
- **Verification gap to close on the next pass:** any finding whose failure depends
  on a `LOG_*` macro or a "missing return" must be re-verified by expanding the macro.
  The races / O(N²) / unchecked-alloc / lock themes are direct-read and hold up.
