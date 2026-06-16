# Legacy Lifecycle

Modules and CLI flags prefixed `legacy_` interact with an existing
`zclassicd` (C++ ZClassic) install on the same host. They exist for two
distinct reasons that should not be confused:

1. **Bootstrap** — cold-start a fresh `zclassic23` faster than full IBD
   by reading blocks / index / chainstate from a sibling `zclassicd`.
2. **Drift detection** — at runtime, periodically compare our chain
   state against `zclassicd` so divergence is caught early.

This file is the source of truth for which paths are **active**,
**opt-in**, **deprecated**, or **scheduled for removal**. Without it,
the word "legacy" tends to read as "cruft" — most of this code is in
fact load-bearing.

Cross-references: `CLAUDE.md` (top-level architecture),
`DEFENSIVE_CODING.md` (the rules legacy_* modules must follow).

---

## CLI flag map

| Flag | Module | Status | What it does |
|------|--------|--------|--------------|
| `-cold-import[=DIR]` | `legacy_bootstrap_importer.c` (`LEGACY_BOOTSTRAP_IMPORT_COLD`) | **Active** (recommended cold start) | Hardlinks `blk*.dat`, bulk-copies block_index LevelDB, bulk-imports chainstate at the legacy tip. Empty datadir → tip in ~60s. Skips `process_new_block` entirely. |
| `-fastimport[=DIR]` | `legacy_bootstrap_importer.c` (`LEGACY_BOOTSTRAP_IMPORT_DIRECT`) | **Active** | Reads blocks LevelDB + mmaps `blk*.dat`, runs the normal block-ingest path with deferred per-block I/O. Slower than `-cold-import` but exercises the full validation pipeline; auto-triggers a wallet rescan at end. |
| `-legacy-attach[=DIR]` | `legacy_bootstrap_importer.c` (`LEGACY_BOOTSTRAP_IMPORT_ATTACH`) | **Active** | Snapshots a locally running `zclassicd`, imports block index + chainstate, stamps Wave S stage cursors to `legacy_tip+1`, and lets normal live sync resume above the imported tip. |
| `-nolegacyimport` | (no module — disables) | **Active** | Disable any auto-detection of `~/.zclassic` on boot. Use when you explicitly do not want legacy interaction. Default is to auto-detect. |

There is no `-importfromlegacy`, `-legacy-auto-import`, or
`-bodypull-from-legacy` CLI flag in the current tree. `-nolegacyimport`
is still parsed because boot may auto-detect a sibling `~/.zclassic`
through the legacy anchor path unless disabled.

`legacy_body_pull` is not a boot CLI path, but it is still runtime-active:
`legacy_mirror_sync_service` calls `legacy_body_pull_range_incremental()`
to catch up local bodies from a legacy node when mirror lag is detected.
The old boot-time body-pull path was removed after the body-pull
pathology was diagnosed: it pre-populated
`block_index` with `BLOCK_HAVE_DATA` but never activated those blocks,
leaving `find_most_work_chain` stuck.

---

## Module map

### Bootstrap path (`app/services/src/`, `app/controllers/src/`)

| File | Status | Role |
|------|--------|------|
| `legacy_bootstrap_importer.c` + `.h` | **Active** | Canonical mode-driven bootstrap importer. Owns the public `-cold-import`, `-fastimport`, and `-legacy-attach` wrapper contracts plus the shared mode implementation. |
| `legacy_body_pull.c` + `.h` | **Runtime-active mirror catch-up; disabled as boot CLI** | `legacy_mirror_sync_service` calls the incremental range puller when local blocks lag legacy. The old boot-time body-pull import path remains removed (body-pull pathology). SHA3 spotcheck helpers remain callable. **Slated for narrower API.** |
| `legacy_mirror_sync_service.c` + `.h` | **Active** | Background drift-detector. Periodically calls `getmirrorstatus` and surfaces lag / divergence via `EV_MIRROR_*` events. Powers `zcl_mirror_status`. |
| `legacy_import.c` (controller) | **Active** | RPC/controller surface for legacy import operations. Not wired to a `-importfromlegacy` CLI flag in `main.c`. |

### RPC clients (`lib/rpc/src/`)

| File | Status | Role |
|------|--------|------|
| `legacy_rpc_client.c` + `.h` | **Active** | HTTP/JSON-RPC client for talking to `zclassicd:8232`. Used by mirror sync, `zcl_probe_zclassicd`, and `legacy_chain_oracle`. |
| `legacy_chain_oracle.c` + `.h` | **Active** | Treats `zclassicd` as an external chain oracle (hash at height, getblockcount, etc.). Used by quorum / drift checks. |

### Storage readers (`lib/storage/src/`)

| File | Status | Role |
|------|--------|------|
| `blocks_index_legacy_reader.c` | **Active** | Reads `zclassicd`'s block_index LevelDB into our schema. Used by `-cold-import` and `-fastimport`. |
| `chainstate_legacy_reader.c` | **Active** | Reads `zclassicd`'s chainstate LevelDB (compressed UTXOs) into our `coins_db`. Used by `-cold-import`. |

### Self-heal recovery (`lib/validation/src/`)

The missing-UTXO / stuck-tip self-heal coordinator lives here.
Recovery routes through the reducer (cursor move + `reducer_kick`), the
same app-layer controller reach `process_block_revalidate.c` /
`process_block_invalidate.c` take. There is **no longer** any
block-disconnect-engine or legacy-RPC repair path; the per-file detail
is in the table.

| File | Status | Role |
|------|--------|------|
| `process_block_self_heal.c` | **KEPT — load-bearing** | Failure-tracking state (`s_utxo_*`), `process_block_is_missing_utxo_failure`, `process_block_note_utxo_failure`, `ZCL_TESTING` hooks. Recovery retry routes through the reducer. **Do not delete** in any "purge legacy" sweep — it is the surviving coordinator, not island residue. |
| `process_block_self_heal_hot_loop.c` | **KEPT — load-bearing** | Hot-loop retry helper for the coordinator above. |
| `process_block_self_heal_scan_state.c` | **KEPT — load-bearing** | Scan-state bookkeeping for the coordinator above. |
| `process_block_self_heal_chain_scan.c` | **DELETED** (Wave 2, `1cef5fe01`) | Out-of-band chain-scan repair island. |
| `process_block_self_heal_sqlite_tx_index.c` | **DELETED** (Wave 2, `1cef5fe01`) | SQLite tx-index island. |
| `process_block_self_heal_legacy_rpc.c` | **DELETED** (Wave 2, `1cef5fe01`) | Legacy-RPC repair island. |
| `process_block_self_heal_inject.c` | **DELETED** (Wave 2, `1cef5fe01`) | Direct-inject repair island. |

A future "delete legacy" grep that matches `process_block_self_heal` must
treat the three KEPT files as load-bearing and only confirm the four
island files stay absent.

---

## Removal candidates

Nothing in the table above is scheduled for removal in the near term —
the bootstrap path is still the fastest way to spin up a fresh
`zclassic23` against a working `zclassicd`, and drift detection has
caught real bugs.

The narrowest cleanup target is the `legacy_body_pull` API: keep the
runtime incremental catch-up entrypoint used by `legacy_mirror_sync_service`,
and shrink or relocate the SHA3 spotcheck helpers so the removed boot import
shape cannot re-grow.

---

## Adding a new legacy_ module

Don't, unless:

1. The behaviour is **strictly tied to interoperability with an
   external `zclassicd`** (bootstrap, drift detection, oracle). General
   "legacy because written earlier" modules belong in their own
   directory or get renamed.
2. The CLI flag goes through `app_context` and respects
   `-nolegacyimport`.
3. The new module has an entry in the tables above before merge.
