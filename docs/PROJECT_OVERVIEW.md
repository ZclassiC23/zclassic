# Project Overview - File Purpose And Lifecycle

Updated: 2026-06-01

This is the quick map for a developer taking over `zclassic23`. The canonical
architecture is `docs/FRAMEWORK.md`; the live debt board is
`docs/REFACTOR_STATUS.md`; legacy/zclassicd lifecycle details are in
`LEGACY_LIFECYCLE.md`.

## Current Runtime Contract

- Local zclassic23 service listener: P2P `8023`, RPC `18232`.
- Historical ZClassic/zclassicd ports are kept free for sibling processes:
  P2P `8033`, RPC `8232`.
- The deploy unit template (`deploy/zclassic23.service`) must not move the
  local zclassic23 listener back to `8033`.
- zclassic23 may still dial external peers that advertise `8033`; that is
  normal network behavior and does not mean the local listener is conflicting.

## Tracked File Inventory

Current tracked tree after this document is added: 1644 files.

Major counts:

```text
 906 .c
 534 .h
  49 .chtml
  34 .sh
  33 .md
  24 .cfg
  11 .txt
```

Major areas:

```text
 504 app/          framework-shaped application code
 856 lib/          reusable core, protocol, storage, validation, tests
 107 tools/        scripts, MCP support, deploy/bench/soak helpers
  43 domain/       pure functional consensus/wallet/encoding core
  26 adapters/     outbound persistence adapters
  16 ports/        port interfaces
  13 config/       boot/runtime wiring
   9 deploy/       systemd/env deployment files
  18 docs/         architecture, status, ADRs, specs, work protocol
```

## Application Shapes

Every `.c` file under `app/` should be one clear framework shape:

```text
app/controllers/   140 files  RPC/HTTP/MCP request parsing and rendering.
app/services/      123 files  orchestration and side-effect ownership; should
                              prefer struct zcl_result returns.
app/models/         64 files  SQLite/ActiveRecord-style persistence, model
                              validation, hooks, query helpers.
app/jobs/           29 files  staged reducer jobs; each advances a cursor or
                              names a typed blocker.
app/supervisors/    13 files  liveness contracts and domain registration.
app/conditions/     36 files  detect/remedy/witness auto-healing conditions.
app/views/          98 files  HTML/UI rendering fragments and templates.
app/events/          1 file   reserved app event shape; durable event log
                              primitives currently live under lib/storage.
```

Current status: Model, Job, Condition, and many supervisor pieces are real and
enforced. Controller and Service still have mixed-purpose/legacy edges that
should be split when touched.

## Core Library Areas

```text
lib/net/          P2P networking, Tor/onion serving, message processors,
                 compact blocks, fast sync, file service, peer lifecycle.
lib/validation/   consensus validation, process-block reducer support,
                 chainstate, mempool, self-heal recovery paths.
lib/storage/      LevelDB/SQLite storage, event log, projections, UTXO backing.
lib/util/         shared primitives: stage, mailbox, projection, supervisor,
                 logging, safe allocation, tracing, threading.
lib/chain/        chain parameters, PoW, MMR/MMB, checkpoints, subsidy.
lib/coins/        UTXO coin view/cache abstractions and commitments.
lib/rpc/          HTTP/JSON-RPC server/client, legacy RPC clients/timeouts.
lib/crypto/       hash, BLAKE2, Ed25519, Equihash, HMAC, KDF, random secrets.
lib/sapling/      Sapling/Sprout cryptography, notes, keys, proofs, trees.
lib/wallet/       wallet domain implementation, keychain, sqlite wallet store.
lib/script/       script interpreter, HTLCs, standard script helpers.
lib/framework/    condition engine only; primitive re-export headers were
                 deleted and should not be restored.
lib/test/         unit/spec/parallel test harness; largest tracked subtree.
```

## Pure Domain And Ports

```text
domain/consensus/  pure consensus checks with no clock/RNG/I/O.
domain/wallet/     pure wallet key derivation and mnemonic logic.
domain/encoding/   pure base58/bech32 encoding logic.
application/       application-level consensus use-case boundary.
ports/include/     stable port interfaces.
adapters/outbound/ SQLite/file-backed implementations of outbound ports.
```

Rule: `domain/` stays pure. If a domain file needs I/O, time, randomness, a DB,
or app state, the code belongs outside `domain/`.

## Boot, Runtime, Tools, Docs

```text
config/       process boot, app_context defaults, runtime callback injection,
             service startup/shutdown.
deploy/       user systemd units and env templates. Keep zclassic23 local P2P
             on 8023 here.
tools/        lint scripts, MCP reference generation, soak/bench utilities,
             deploy verification, chaos and maintenance helpers.
docs/         framework, status board, ADRs, specs, validation notes, worktree
             protocol. Treat docs/REFACTOR_STATUS.md as the current board.
db/           reference-only SQL dumps (never executed); live migrations are
             embedded C strings in app/models/src/database_migrate.c.
src/main.c    binary entry point, CLI parsing, mode dispatch.
src/cli.c     standalone native RPC client.
Makefile      build/test/lint/deploy orchestration.
```

## Active Legacy Code

Do not delete code just because it is named `legacy`. The active reason for
`legacy_*` code is interoperability with a local C++ `zclassicd`.

Active paths:

```text
-cold-import[=DIR]     active recommended cold start from zclassicd data.
-fastimport[=DIR]      active full-ingest import path from zclassicd data.
-legacy-attach[=DIR]   active attach/snapshot path from running zclassicd.
-nolegacyimport        active opt-out for sibling zclassicd auto-detection.

legacy_mirror_sync_service      runtime drift detector / catch-up coordinator.
legacy_rpc_client               JSON-RPC client for zclassicd:8232.
legacy_chain_oracle             external chain oracle over zclassicd.
blocks_index_legacy_reader      read-only block index LevelDB importer.
chainstate_legacy_reader        read-only chainstate LevelDB importer.
legacy_import controller        RPC/controller surface for import operations.
```

## Deprecated, Retired, Or Removed

Keep these distinctions clear:

```text
Runtime-active but slated narrower:
  legacy_body_pull.c/.h
    Used by legacy_mirror_sync_service for incremental mirror catch-up.
    Old boot-time body-pull import is removed; shrink the API rather than
    expanding it.

Compiler-deprecated wrappers:
  wallet_sqlite_open       -> use wallet_sqlite_open_r
  wallet_sqlite_write_key  -> use wallet_sqlite_write_key_r
  wallet_sqlite_read_keys  -> use wallet_sqlite_read_keys_r
  wallet_sqlite_flush      -> use wallet_sqlite_flush_r

Retired operation:
  runtime reindexchainstate RPC
    Returns a compatibility error; restart with -reindex-chainstate instead.

Removed CLI/surfaces:
  -assumevalid
    Removed; use -deferproofvalidationbelow=<blockhash|0>.
  -importfromlegacy, -legacy-auto-import, -bodypull-from-legacy
    Not present in the current CLI.

Deleted/refactor surfaces that must not be restored:
  public cutover/projection-diff/shadow comparison RPC/MCP apparatus
  app/services/src/cutover_modes.c and its header
  header_admit_stage_diff.c and public diff report API
  legacy block-connect engine file set / deleted engine names
  lib/framework mailbox/projection re-export headers
  app-layer sync planner compatibility wrappers
  snapshot-sync app-layer compatibility wrapper
  condition PR-number scaffold headers
  explorer factoids/stats controller shim headers
  active_chain_set_tip compatibility alias
  production-visible coins_view_cache_flush API
```

## High-Risk Cleanup Rules

- Keep lint baselines empty. Do not add grandfathered entries.
- Do not add upward includes from `lib/` to `app/`.
- Do not add a second chain-state writer; use the tagged canonical writers.
- Do not move local zclassic23 back to `8033`.
- Do not treat green tests as live-node proof; live progress requires a running
  node and observed forward movement/health.
- Prefer deleting zero-purpose wrappers to preserving compatibility shells.
- When touching legacy compatibility code, update `LEGACY_LIFECYCLE.md` if the
  status changes.

## Current Remaining Cleanup Themes

1. Keep production terminology clean: no stale shadow/cutover/projection-diff
   language in active paths.
2. Continue splitting mixed-purpose controller/service files when adjacent work
   makes it safe.
3. Continue auditing process-block split files for stale helper boundaries.
4. Migrate remaining legacy bool compatibility call sites toward
   `struct zcl_result`.
5. Prove live-node progress/soak after code changes, not just build/test health.
