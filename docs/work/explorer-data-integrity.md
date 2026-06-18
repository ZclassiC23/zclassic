# Explorer data-integrity — audit findings + fix plan

Source: `explorer-data-integrity-audit` workflow (2026-06-18, 30 agents, every
finding adversarially re-verified against the live node). The node's **consensus
is fine** — `coins_kv` (what `gettxoutsetinfo` reads) tracks the tip exactly and
parity vs zclassicd is intact. The damage is confined to the explorer **read/
display surface**. Three root defects + two latent corruptions.

## Root defects

### A. `utxos` SQLite mirror is FROZEN (P0 — drives ~11 surfaces)
The mirror sits at the cold-import restore checkpoint **3150900** while the chain
advances (lag grows ~1 block/75s). Its only forward writer,
`process_block_flush_coins` (`lib/validation/src/process_block.c:140`), is **dead
code (zero callers)**. The live reducer stage `apply_coins_kv`
(`app/jobs/src/utxo_apply_stage.c:228-240`) writes **only** `coins_kv`; nothing
mirrors deltas into node.db `utxos`. After cold-import seeds `utxos` once, it is
never re-written. Corrupts: HodlWave, /explorer/stats height+supply, address
balances, richlist, circulating supply, listunspent.

**Fix (node.db-only, never touches the consensus `coins_kv` commit):** add a
forward feeder that brings `utxos` to the `coins_kv` applied frontier
(`coins_kv_get_applied_height`, `lib/storage/include/storage/coins_kv.h:145`).
Two-store reality: `coins_kv` lives in `progress.kv`, `utxos` in `node.db` — they
**cannot** share a txn, so the feeder is an eventually-consistent projection
keyed off the applied-height cursor, replaying `(utxos_max .. applied_height]`
per-block (or reseeding from `coins_kv`, which stores value/height/coinbase/script
per coin — enough to rebuild a `db_utxo` incl. address_hash). Then re-run
`db_utxo_rebuild_wallet_and_address_caches` (`app/models/src/utxo.c:330`).
Mirror-write failures must log loud but **never** roll back the coins txn (mirror
is rebuildable; the chain is not). COPY-PROVE on a cloned datadir / isolated
ports: prove `MAX(height) FROM utxos` advances in lockstep with `gettxoutsetinfo
height`, count converges to `txouts`, a post-3150900 coinbase appears in the
miner's balance, and the mirror does NOT rewind across a restart.

### B. `blocks` projection hole (P1 — gates the factoids/stats historian)
13,383 missing heights (3137533–3150899, the pre-restore window). Validator trips
at `explorer_internal.h:408` (`block_rows != max_height+1`). Catchup
(`app/services/src/node_db_catchup_service.c:303-404`) is forward-only and never
revisits below its cursor. **Caveat:** a cold-import node lacks bodies for that
band — backfill needs a P2P re-fetch of the range, not just a re-index.

### C. Phase-B index never implemented (P1/P2)
`tx_outputs`, `tx_inputs`, `view_integrity`, `op_returns`, `znam_names`,
`sapling_*` have **zero production writers** (all 0 rows). Drives the tx-input
"?" placeholder and ZSLP/ZNAM blindness; also a permanent second gate on the
historian (`explorer_internal.h:434-437`).

## Latent corruptions (masked by B today; bite the moment recovery works)
- **Genesis-hash constant** `explorer_internal.h:21-22` was 11/32 bytes wrong.
  FIXED → `0206260143838b5f…` = byte-reverse(DISPLAY). (this branch)
- **Validator gates on never-written tables** (`tx_outputs>0`,
  `view_integrity==max+1`) — demote to per-section soft guards so the other ~15
  historian sections render.

## Smaller (info surface)
- **`getmininginfo.difficulty`** Bitcoin-mantissa bug (599177 vs 71.6). FIXED →
  `difficulty_from_index` (pow.h). (this branch)
- **/explorer/stats "Current Height"** read frozen `MAX(height) FROM utxos`.
  FIXED → `MAX(height) FROM blocks` (chain tip). (this branch)
- **tx-input "?"** — `db_tx_output_value` reads empty `tx_outputs`; resolve
  prevout value at render time from the prevout body / `coins_kv` instead.

## DO NOT "fix" (correctly empty / expected — verified)
- **/explorer/market** — gossip/RPC-only by design; no peers → empty is correct.
- **/explorer/factoids degraded panel** — the *honest* reference page; it already
  separates "chain height" (blocks) from "highest UTXO height" (utxos) and states
  the suppression reason. /explorer/stats is the one that was wrong (fixed above).
- **`addresses` table** — a faithful in-sync `GROUP BY` of the mirror; self-heals
  once A lands. (6110 ZCL "gap" = the 245 NULL-address UTXOs, reconciles exactly.)
- **ZSLP token tables empty — this IS a real bug (corrected 2026-06-18).** A raw
  scan of the on-disk block files for the `OP_RETURN 6a 04 "SLP\0"` lokad finds
  **4,302 real ZSLP operations on-chain** — the blank page is missing real data
  because the OP_RETURN/`slp_parse` scanner never ran (op_returns = 0 rows). The
  full per-block indexer + reindex surfaces all 4,302. (Earlier "correctly empty"
  was wrong — it read the empty table, not the chain.) **ZNAM is genuinely empty**
  (raw scan for `6a 04 "ZNAM"` = 0 — no names ever registered), so znam_names
  staying empty after reindex is correct.

## Order
A (mirror feeder) → C-demote-gates + genesis (re-green historian) →
B-backfill (needs P2P re-fetch) → tx-"?" → C Phase-B indexers (track).
Every reducer/indexer-touching fix copy-proves on a clone before live and
respects the coins_kv adds-before-spends order + atomic commit boundary.
