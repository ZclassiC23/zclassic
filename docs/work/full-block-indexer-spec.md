# Full per-block explorer indexer + reindex — implementation spec

Goal (owner mandate 2026-06-18): **get ALL the data from every block.** The
explorer's per-block projection tables are empty (no production writers); all
block bodies are already on disk (c23 datadir `blocks/` = 51 `blk*.dat`, 6.8G —
no re-download needed). Implement a complete per-block indexer (forward + a full
genesis→tip reindex) populating every projection. **node.db only — never
`coins_kv`/`progress.kv`/consensus/parity** (verified isolated).

Source: `full-block-indexer-spec` workflow (8 agents, file:line-verified). The
canonical, full work order is the `.result.spec` field of
`tasks/wxoenufkg.output` in this session's task dir — the implementer reads that.
This file is the durable summary + guardrails.

## Tables to populate (all 0 rows today)
`tx_outputs`, `tx_inputs`, `op_returns`, `sapling_spends`, `sapling_outputs`,
`joinsplits`, `sprout_nullifiers`, `view_integrity` + on-chain `znam_names`/
`znam_text_records`/`znam_addr_records`. (`utxos` is handled separately by the
coins_kv-fed mirror feeder — do NOT touch utxos here.)

## Shape
- **One hook, one path.** `sync_block_lean` (`app/services/src/node_db_catchup_service.c:117`,
  sole caller `:480`) is the only block-write unit; both forward sync and the
  reindex flow through it. Add a single `index_tx_projections(...)` static helper,
  called once per tx at `:169` after `db_tx_save`.
- **New model TU** `app/models/src/explorer_index.c` (+ header) with 8 `db_*_save`
  fns using the `AR_CACHED_SAVE` idiom (model on `tx_index.c:64-84`); 8 cached
  `INSERT OR REPLACE` stmts in `database.c` PREP block (`:255`), struct fields +
  finalize (`:370`). Idempotent by PK → reindex re-walks safely, fills holes.
- **op_return dispatch:** generic `op_returns` row + `slp_parse` (ZSLP flag) +
  `znam_parse`→apply to `znam_names` (FCFS REGISTER/UPDATE/TRANSFER/SET_*; owner =
  first-input P2PKH address resolved from `tx_outputs`). Only ZSLP + ZNAM (no
  on-chain market protocol exists).
- **view_integrity:** per-height SHA3 chained receipt (formula at
  `explorer_stats_sections.c:440-443`): `SHA3(prev||height||blockhash||sprout_val||
  sapling_val||num_tx||num_js||num_ss||num_so)`, prev carried across the ascending
  walk; also backfill `blocks.sprout_value`/`sapling_value` before `db_block_save`.
- **Reindex driver:** `-reindex-explorer` flag (model on `-reindex-chainstate`,
  `src/main.c:1700`): truncate the projection + znam tables, rewind the shared
  catchup tip to 0, let the existing 0..tip walk re-emit every row; extend the
  turbo scope to DROP/CREATE the new secondary indexes around the bulk load.
  Resumable (100k-block batch commits stamp the tip); fills `blocks`/`transactions`
  holes for free. ~a few hours, SQLite-write-bound.

## Guardrails (lint = `make lint`, `docs/DEFENSIVE_CODING.md`)
AR lifecycle only (no raw `sqlite3_step`); `LOG_FAIL/ERR/WARN` before every error
return (silent-errors gate); `zcl_malloc`; model validate fn or skip-tag (E11);
keep `node_db_catchup_service.c` ≤800 LOC (E1) and `sync_block_lean` ≤500 (E12) by
pushing bulk into `explorer_index.c`; E13 parity untouched.

## Copy-prove (NEVER live surgery)
Clone (`cp -a ~/.zclassic-c23 /tmp/reindex-cp`), run `-reindex-explorer` on
isolated ports (`-port=18099 -rpcport=28232`, or `tools/scripts/isolated_mainnet_env.sh`)
over a SMALL range first (h=0..50000), then the verification query set (PART 7 of
the work order: referential integrity, every-block-has-coinbase, height coverage,
integrity-chain completeness). Only after green: deploy + live `-reindex-explorer`
(explorer serves "degraded/indexing" until the cursor reaches tip).
