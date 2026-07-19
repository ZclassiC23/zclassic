# Legacy Lifecycle

Modules and CLI flags prefixed `legacy_` interact with a sibling
`zclassicd` (C++ ZClassic) install on the same host. They exist for two
distinct reasons that must not be confused:

1. **Bootstrap** — cold-start a fresh `zclassic23` faster than full IBD
   by reading headers / block-index / chainstate / block bodies from a
   sibling `zclassicd` datadir.
2. **Drift detection** — at runtime, periodically compare our chain
   state against `zclassicd` so divergence is caught early.

This file is the source of truth for which paths exist and what each
one is for. Every `legacy_`-named module in the tree is listed below and
every one is **load-bearing** — none is cruft, and none is scheduled for
removal. A "purge legacy" grep that treats the word "legacy" as "dead
code" is wrong here; confirm against this table and the call sites first.

Cross-references: `CLAUDE.md` (top-level architecture),
`DEFENSIVE_CODING.md` (the rules `legacy_*` modules must follow).

---

## CLI flag map

The cold-start path is `build/bin/zclassic23 --importblockindex <datadir>`
(headers from a sibling `zclassicd` datadir) followed by a normal boot,
which auto-reads/links `~/.zclassic` unless `-nolegacyimport` is passed.

| Flag | Module | Status | What it does |
|------|--------|--------|--------------|
| `-nolegacyimport` | (no module — disables) | **Active** | Disable auto-detection of a sibling `~/.zclassic` on boot. Use when you explicitly do not want legacy interaction. Default is to auto-detect. |

There is no `-cold-import`, `-fastimport`, `-legacy-attach`,
`-importfromlegacy`, `-legacy-auto-import`, or `-bodypull-from-legacy`
CLI flag in the tree. `-nolegacyimport` is parsed because boot may
auto-detect a sibling `~/.zclassic` through the legacy anchor path
unless disabled.

**Invariant — no boot-time body-pull.** No path pre-populates
`block_index` with `BLOCK_HAVE_DATA` from a sibling node without
activating those blocks. Such a path wedges `find_most_work_chain` (it
finds have-data blocks it never advances) and must not be reintroduced.
Bodies enter only through normal activation or the explicit import
readers below.

---

## Module map

Every file below is **Active / load-bearing** and has live callers
outside its own tests. The "Callers" column names the non-legacy code
that reaches each module.

### Bootstrap — header / index / chainstate / body import

| File | Role | Callers |
|------|------|---------|
| `config/src/boot_legacy_import.c` | Pure boot-heuristic predicates (`boot_need_legacy_header_pull`, `boot_need_blocks_table_hydrate`, `boot_dispatch_blocks_table_hydrate`) for the automatic `zclassicd`-LevelDB header pull. Split out of `boot.c` to hold the E1 file-size seam. | `config/src/boot.c`, `config/src/boot_blkidx_ladder.c` |
| `config/src/boot_legacy_blocks.c` + `.h` | Boot-time detection / linking of a sibling `zclassicd`'s on-disk block files. | `config/src/boot.c` |
| `lib/storage/src/blocks_index_legacy_reader.c` + `.h` | Reads `zclassicd`'s `block_index` LevelDB into our schema. Backbone of the `--importblockindex` cold-start path. | `src/main.c` path, `block_log_legacy.c`, `tools/rebuild_recent.c` |
| `lib/storage/src/chainstate_legacy_reader.c` + `.h` | Reads `zclassicd`'s chainstate LevelDB (compressed UTXOs) into our `coins_db`. | `src/main.c`, `app/services/src/shielded_history_import_service.c`, `app/conditions/src/sapling_anchor_frontier_unavailable.c` |
| `adapters/outbound/persistence/src/block_log_legacy.c` + `.h` | Legacy block-body reader adapter (over `blocks_index_legacy_reader` + the mmap reader) that serves raw block bytes from a sibling datadir. | `app/services/src/replay_verify_service.c` |
| `app/controllers/src/legacy_import.c` + `include/controllers/legacy_import.h` | RPC/controller surface for legacy import operations. Not wired to a dedicated CLI flag; reached by boot and the wallet diagnostic controllers. | `config/src/boot.c`, `app/controllers/src/wallet_diagnostic_*.c`, `app/controllers/src/snapshot_controller_import.c` |
| `app/controllers/src/legacy_import_scan.c` + `.h` | Raw blk-file scanner and Sapling decrypt workers for legacy wallet import. | `app/services/src/legacy_import_service.c` |
| `app/services/src/legacy_import_service.c` + `include/services/legacy_import_service.h` | Service-grade orchestration for importing wallet data from a legacy node's raw block files. | `app/controllers/src/legacy_import.c` |

### Drift detection (observe-only)

The mirror observer never applies blocks — it only compares our tip
against a sibling `zclassicd` and surfaces lag / divergence via
`EV_MIRROR_*` events. It powers `zclassic23 ops mirror` and
`zclassic23 dumpstate legacy_mirror`, and its lag/parity SLOs feed live
monitoring; do not remove it.

| File | Role | Callers |
|------|------|---------|
| `app/services/src/legacy_mirror_sync_service.c` + `include/services/legacy_mirror_sync_service.h` | Background drift-detector: catch-up tick, lag/parity comparison, event emission. | boot sync wiring, health/event controllers, `parity_slo_breach`, `block_source_policy_runtime`, `chain_supervisor`, diagnostics registry |
| `app/services/src/legacy_mirror_sync_state.c` | Runtime state snapshots, lifecycle wiring, and test hooks for the observer. | `legacy_mirror_sync_service.c` |
| `app/services/src/legacy_mirror_sync_json.c` | Operator-facing JSON snapshot (the `dumpstate legacy_mirror` predicate). | `legacy_mirror_sync_service.c` |
| `app/services/src/legacy_mirror_sync_parity_trend.c` | Bounded trend-history write for the mirror's consensus-parity comparisons. | `legacy_mirror_sync_service.c` |
| `app/services/src/legacy_mirror_sync_internal.h` | Shared internal header for the four `.c` files above. | mirror-sync `.c` files |
| `app/supervisors/src/legacy_mirror_supervisor.c` + `.h` | Supervisor contract for the mirror observer (registers it on the liveness tree). | `legacy_mirror_sync_service.c`, `legacy_mirror_sync_state.c` |

### RPC clients (`lib/rpc/src/`)

| File | Role | Callers |
|------|------|---------|
| `legacy_rpc_client.c` + `.h` | HTTP/JSON-RPC client for talking to `zclassicd:8232`. | `zclassicd_oracle_service`, `utxo_parity_service`, `utxo_reference_source_zclassicd`, `header_probe` |
| `legacy_chain_oracle.c` + `.h` | Treats `zclassicd` as an external chain oracle (hash-at-height, getblockcount) for quorum / drift checks. | `fast_sync`, `boot_services`, `tip_stall_oracle_rebuild`, `snapshot_verify`, `repair_controller_rebuild` |
| `legacy_header_client.c` + `.h` | Header-fetch client used to pull headers over RPC. | `app/services/src/header_probe.c` |

### Tests

| File | Covers |
|------|--------|
| `lib/test/src/test_block_log_legacy.c` | `block_log_legacy` |
| `lib/test/src/test_boot_legacy_blocks.c` | `boot_legacy_blocks` |
| `lib/test/src/test_chainstate_legacy_reader.c` | `chainstate_legacy_reader` |

Boot-heuristic predicates from `boot_legacy_import.c` are additionally
exercised by `test_boot_phase.c` and
`test_importblockindex_cli_dispatch.c`; the RPC clients and mirror
observer by `test_rpc.c`, `test_zclassicd_oracle.c`, `test_lag_slo.c`,
`test_parity_slo.c`, and `test_block_source_policy.c`.

---

## Self-heal note (not `legacy_`-named, but adjacent)

The missing-UTXO / stuck-tip self-heal coordinator lives in
`lib/validation/src/`. Recovery routes through the reducer (cursor move
+ `reducer_kick`), the same reach the app-layer controllers
`process_block_revalidate.c` / `process_block_invalidate.c` use. There
is no block-disconnect-engine and no legacy-RPC repair path.

Three files are **load-bearing** and must survive any purge:

- `process_block_self_heal.c` — failure-tracking state (`s_utxo_*`),
  `process_block_is_missing_utxo_failure`, `process_block_note_utxo_failure`,
  `ZCL_TESTING` hooks. The surviving coordinator, not residue.
- `process_block_self_heal_hot_loop.c` — hot-loop retry helper.
- `process_block_self_heal_scan_state.c` — scan-state bookkeeping.

Four out-of-band repair islands do **not** exist and must not be
re-added: `process_block_self_heal_chain_scan.c`,
`process_block_self_heal_sqlite_tx_index.c`,
`process_block_self_heal_legacy_rpc.c`,
`process_block_self_heal_inject.c`.

---

## Adding a new `legacy_` module

Don't, unless:

1. The behaviour is **strictly tied to interoperability with an
   external `zclassicd`** (bootstrap, drift detection, oracle). General
   "legacy because written earlier" modules belong in their own
   directory or get renamed.
2. The CLI flag goes through `app_context` and respects
   `-nolegacyimport`.
3. The new module has an entry in the tables above before merge.
