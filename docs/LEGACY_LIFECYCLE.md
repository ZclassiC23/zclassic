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

The cold-start path is `build/bin/zclassic23 --importblockindex <datadir>`
(headers from a sibling `zclassicd` datadir) followed by a normal boot,
which auto-reads/links `~/.zclassic` unless `-nolegacyimport` is passed.

| Flag | Module | Status | What it does |
|------|--------|--------|--------------|
| `-nolegacyimport` | (no module — disables) | **Active** | Disable any auto-detection of `~/.zclassic` on boot. Use when you explicitly do not want legacy interaction. Default is to auto-detect. |

There is no `-cold-import`, `-fastimport`, `-legacy-attach`,
`-importfromlegacy`, `-legacy-auto-import`, or `-bodypull-from-legacy`
CLI flag in the current tree. `-nolegacyimport` is still parsed because
boot may auto-detect a sibling `~/.zclassic` through the legacy anchor
path unless disabled.

The old boot-time body-pull path was removed after the body-pull
pathology was diagnosed: it pre-populated
`block_index` with `BLOCK_HAVE_DATA` but never activated those blocks,
leaving `find_most_work_chain` stuck.

---

## Module map

### Bootstrap path (`app/services/src/`, `app/controllers/src/`)

| File | Status | Role |
|------|--------|------|
| `legacy_mirror_sync_service.c` + `.h` | **Active (observe-only)** | Background drift-detector. Periodically calls `getmirrorstatus` and surfaces lag / divergence via `EV_MIRROR_*` events. It no longer applies blocks — it only observes lag against a sibling `zclassicd`. Powers `zcl_mirror_status`. |
| `legacy_import.c` (controller) | **Active** | RPC/controller surface for legacy import operations. Not wired to a `-importfromlegacy` CLI flag in `main.c`. |

### RPC clients (`lib/rpc/src/`)

| File | Status | Role |
|------|--------|------|
| `legacy_rpc_client.c` + `.h` | **Active** | HTTP/JSON-RPC client for talking to `zclassicd:8232`. Used by mirror sync, `zcl_probe_zclassicd`, and `legacy_chain_oracle`. |
| `legacy_chain_oracle.c` + `.h` | **Active** | Treats `zclassicd` as an external chain oracle (hash at height, getblockcount, etc.). Used by quorum / drift checks. |

### Storage readers (`lib/storage/src/`)

| File | Status | Role |
|------|--------|------|
| `blocks_index_legacy_reader.c` | **Active** | Reads `zclassicd`'s block_index LevelDB into our schema. Used by the `--importblockindex` cold-start path. |
| `chainstate_legacy_reader.c` | **Active** | Reads `zclassicd`'s chainstate LevelDB (compressed UTXOs) into our `coins_db`. Used by the cold-start import path. |

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
the `--importblockindex` cold-start path is still the fastest way to
spin up a fresh `zclassic23` against a working `zclassicd`, and drift
detection has caught real bugs.

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
