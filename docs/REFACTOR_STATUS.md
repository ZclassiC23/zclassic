# Refactor Status — Purpose-Per-File Finish Board

> Updated 2026-06-03. This file is the current debt board for finishing the
> framework refactor. `docs/FRAMEWORK.md` remains the architecture.
>
> ⚠️ **This is the ARCHITECTURE-AXIS board (~90% done). It is NOT the v1 path.**
> The v1 bar is [`docs/MVP.md`](./MVP.md); THE plan is
> [`docs/work/FORWARD_PLAN.md`](./work/FORWARD_PLAN.md). Do not start
> architecture work until the v1 buckets are clear.

## 2026-06-03 — Architecture conformance board (the "everything has a place" axis)

Full-tree audit (read from the actual tree + Makefile, not prose). The `app/`
layer is already conformant; the remaining purpose-per-file debt is concentrated
and named below. This axis is **independent of the §3 live-tip runtime cluster**
(which is owner-gated, validate-on-copy).

**Verified-clean facts:**
- 4 layers, dependency direction proven clean: `domain/` #includes `app/`+`lib/` **0×**
  (pure core); `app/` depends inward on `domain|ports|application` **18×**.
- All 11 lint baselines = **0 entries** (shape/size/result-type/sqlite/supervisor/
  blocker/layering gates pass with zero grandfathered exceptions) — but scoped to
  `app/` only. `app/` largest file = exactly 800 (at the ceiling, tightly held).

**Remaining debt, ranked (tracked as session Waves A–E):**

| Rank | Item | Real numbers | Target ("the zclassic23 way") | Wave |
|------|------|--------------|-------------------------------|------|
| 1 | `config/` boot monolith | boot_services.c **3517** (was 4100; `boot_tip_hooks.c`+`boot_projections.c` extracted), boot.c **3607**, boot_index.c **1524** — UNGATED (E1 scopes app/ only) | pure verbatim extraction **EXHAUSTED** (prove-first, 5-agent audit: 0 of 6 remaining units cleanly movable — all blocked by the shared `S` static or a cross-TU call-seam). Next = `S`-into-handle seam redesign, then unit moves. Verified ordered plan: `docs/work/boot-decomposition-seams.md` | D |
| 2 | Storage-adapter seam — **RESOLVED-CLOSED (outbound-only by design)** | `check_raw_sqlite.sh` reports CLEAN, empty allowlist; outbound persistence adapters are real and wired (`adapters/outbound/persistence/`: 15 ports + 12 sqlite impls, writes out through swappable ports — Law 2); the inbound "repository" adapter layer deliberately does NOT exist (Models ARE storage / own reads — Law 5), same reserved-empty-by-design posture as `app/events/` (Rank 5); the 49 raw-sqlite app/ sites are all legit: Models ARE storage (AR internals), Jobs use progress-kv kernel store, Views are read-only introspection | NOT a migration — closed by design; optional future read-only chain_state port stays optional. FRAMEWORK §3 row 8 documents the outbound-only rule. | E (resolved/closed) |
| 3 | `domain/` fronted by thin `lib/` wrappers | divergent duplicate-name pairs both compile: base58 (38 vs 151), bech32 (24 vs 164), upgrades (122 vs 233) | migrate callers to `domain/`, delete the `lib/` wrapper, seal with `test_domain_*` | A |
| 4 | Supervisor shape partial | only net/chain/staged_sync declared (6 .c); rest hand-wired in boot_services.c | folds into Rank 1 | D |
| 5 | `app/events/` empty (0 files) | "reserved" shape; event primitives live in `lib/storage/event_log.c` | **RESOLVED — keep reserved-and-empty by design** (audit: no misplaced Event code; concept owned by `lib/event/` + `lib/storage/event_log` + projections; lone app subscriber is a Service). README + FRAMEWORK §3 row 7 document the keep-empty rule; `events` now in Makefile `APP_DIRS` for build/lint symmetry. | — |
| 6 | Controller/Service legacy compat | baselines 0 (no NEW violations); import/sync controllers still orchestrate; services keep bare-bool compat APIs | subtraction, not new structure | B/C |

Mapping to FINISH_CHECKLIST: Wave B = §5.3 (line 186, split the 8 files at the
800 ceiling), Wave C = §5.6 (line 189, rename `*_controller`/`*_repository` →
`*_service`). Waves A/D/E are deeper REFACTOR_STATUS items not yet enumerated as
FINISH_CHECKLIST checkboxes.

**Wave A — DONE (`41174e498`).** base58 + bech32 lib wrappers collapsed into the
domain core: all callers migrated to `domain_encoding_*`, the 4 wrapper files
deleted, obsolete wrapper-parity sub-tests dropped from the seal tests. Build +
test_parallel 0/290 + lint 33 gates green. The 3rd "duplicate" — `upgrades.c` —
was investigated and **correctly KEPT** (verdict: not a duplicate; `lib/consensus/
upgrades.c` is the sole definition of the consensus data tables
NetworkUpgradeInfo/SPROUT_BRANCH_ID/EquihashUpgradeInfo, `domain/consensus/
upgrades.c` holds the pure activation-height arithmetic that reads them — a correct
layering, no symbol collision). `lib/keys/*` + `lib/consensus/params.c` remain a
future scoping item (not duplicated today). Rank 3 now: 2 of 3 collapsed, 1 kept
by design.

**Wave D / Rank 1 — STEP 2 done (`02d184c6d`).** First increment of the boot
monolith decomposition. The 5 process_block tip-publication hooks + the gap-fill
kick were pure adapters (all input by parameter, no shared boot statics) → moved
verbatim to a new `config/src/boot_tip_hooks.c`, wired via one seam
`boot_register_process_block_hooks(svc)`; the NULL teardown stays inline.
`boot_services.c` 4100 → **3926**. Build + test_parallel 0/290 (incl. the
`test_make_lint_gates` wiring self-test, updated for the new split) + lint green;
**boot smoke-test on a datadir copy confirmed behavior-neutral** — the node boots
past the hook registration, restores chain state to tip 3,132,741, and starts
runtime services with no crash (the repro INCONCLUSIVE verdict was the pre-existing
header-sync stall + slow mmb_register boot, not the refactor). STEP 1 (service-kernel
adapters, ~720 lines) is **deferred**: a read-only analysis found it blocked by
category-3B shared statics (`boot_profile_has_*`, `svc_clock_ms`) + cross-TU
static-call seams — it needs a real seam design, not a pure move. Remaining boot
units (projection storage, background workers, shutdown phases) are queued as
future increments, each boot-validated on a copy.

**Wave D / Rank 1 — STEP 3 done (`08cb86586`).** Projection storage extracted: the
event_log + 10 per-domain reducer projections (utxo/mempool/peers/block_index/znam/
wallet/contacts/onion/hodl) + their open/close lifecycle (4 functions) moved verbatim
to `config/src/boot_projections.c`. A 4-unit read-only static-sharing workflow proved
projection-storage had **ZERO category-3B shared statics** (all 10 `g_phase4_*` handles
unit-only). One clean seam: `boot_start_projection_storage` took the anchor-seed
`node_db` from the boot-local static `boot_node_db()` (used by 36 stayers) → now passed
by **parameter** (caller passes `boot_node_db()`), matching the step-2 by-param
discipline. `boot_services.c` **3926 → 3517** (4100 → 3517 cumulative). Build +
test_parallel 0/290 + lint green; **boot smoke-test confirmed the extraction is
behavior-neutral** — all 10 projections opened + caught up, the anchor-seed seam worked
(`anchor-seed refused: already seeded`), boot proceeded to runtime header-sync. The
4-unit workflow ranked the remaining units: shutdown-phases (3 shared statics, low),
msg-callbacks (1 shared, 26 fns), background-workers (3 shared, medium, 28 fns).

**SURFACED P0 (pre-existing, NOT this refactor) — SIGSEGV in `push_getheaders_from`.**
The step-3 boot smoke-test crashed (signal 11) in the P2P header-sync path —
`push_getheaders_from` ← `process_headers` ← `msg_process_messages`
(`lib/net/src/msg_headers.c`, untouched by Wave D). Mechanism: the exponential-locator
branch (msg_headers.c:746-767) walks the `pprev` chain dereferencing `walk->phashBlock`
/ `walk->pprev`; on a **corrupted block index** ("Block index heights may be corrupted")
a garbage `pprev` is non-null but invalid → segfault. This is a §3-cluster symptom
(block-index integrity), not a clean null-guard. Captured for the §3 work; it also means
future Wave-D boot smoke-tests will hit this crash on the wedged datadir — distinguish
via the backtrace (a Wave-D regression would crash in `config/boot_*`, not `lib/net`).
NOTE: the live node has since advanced **3,132,687 → 3,133,966** (real forward progress).

## Objective

Finish the ZClassic23 refactor by deleting stale shadow/cutover and scaffold
code, making every remaining file match one clear framework shape and purpose,
shrinking all lint baselines to zero, updating docs to reflect the
authoritative reducer architecture, and proving it with clean tests plus a live
node soak.

## Current Truth

- The reducer/staged pipeline is the authoritative chain-advance architecture.
- The public cutover/projection-diff MCP/RPC apparatus has been removed.
- The old legacy block-connect engine files are gone.
- Production C/H surfaces no longer describe active reducer read-model paths as
  shadow/cutover/projection-diff infrastructure, and they no longer use the
  deleted single-engine block-connection names for current reducer behavior;
  stale Phase/PR/dissolve scaffold labels are also gone from guarded
  production comments. Remaining historical wording is test/doc context only.
- E1, E2, E6, supervisor, E7, typed-blocker, controller raw-SQL adoption,
  lib-layering, and raw allocation debt are at zero grandfathered entries.
  Remaining refactor work is now code-shape cleanup, process-block splitting,
  doc honesty, and live-node proof rather than baseline burn-down.
- **Single-engine completion.** The single-engine plan/design docs
  (`docs/work/single-engine-newcode-plan.md` / `-design.json`) were deleted
  once the log→projection→Job reducer became the only engine; there is no
  runtime "flip" flag. The step/wave labels those docs used — `B3`, `B7`,
  `B8`, `DRIVER-FOLLOWER` — are therefore obsolete. They no longer name any
  live behavior; where they survive it is as stale comment text. Residual
  in-code label hits (production source + the `one_write_path_baseline.txt`
  header) are pending a single central scrub; the canonical author-default is
  `UTXO_AUTHOR_STAGE` (`UTXO_AUTHOR_LEGACY` is the test-only emitter path),
  and the comments in `app/jobs/src/utxo_apply_delta.c` and
  `lib/storage/include/storage/utxo_projection.h` now say so.

## Completed Architecture Moves

- `app/` is organized by framework shape: controllers, services, models, jobs,
  supervisors, conditions, events, views.
- Model lifecycle and validation gates are enforced.
- Staged reducer Jobs live under `app/jobs/` and use the Job advance/block
  contract.
- Conditions are real `{detect, remedy, witness}` files.
- Domain extraction is real: 21 pure domain modules with matching domain tests.
- Outbound persistence ports/adapters exist for several services.
- MCP route counts reflect the post-apparatus surface: 98 total tools.
- `header_admit_stage_diff.c` and the public diff report API were deleted;
  the survivor header-admit test now verifies reorg self-heal directly through
  the durable log.
- `app/services/src/cutover_modes.c` and
  `app/services/include/services/cutover_modes.h` were deleted; the
  header-admit, validate-headers, and tip-finalize stages no longer expose
  SHADOW/AUTHORITATIVE runtime switches.
- The active-chain window move no longer publishes the public reducer
  authority. `active_chain_move_window_tip()` is a local cache/window
  primitive; tip authority is updated only through `tip_finalize_stage` or
  explicit trusted bootstrap/repair APIs.
- Trusted restored tips now stamp a durable `tip_finalize` anchor/cursor when
  the persisted stage cursor lags the restored public tip. Stale low
  `tip_finalize` cursors cannot replay old rows and republish a low public tip.
- `tip_finalize` failure rows such as `upstream_failed` advance the stage
  cursor only; they do not move public tip authority.
- `staged_sync_supervisor` no longer describes the active staged reducer Jobs
  as a shadow pipeline, and its unused `datadir`/conservation-diff API
  parameter was removed.
- The body_persist through utxo_apply Job headers/sources/tests no longer use
  comparison-era wording for the active reducer stages; the health controller
  comment now frames `log_head` as the reducer log head.
- Boot-time projection/event-log fan-out is now named
  `boot_start_projection_storage` / `boot_stop_projection_storage`; the
  `config/src/boot*.c` production boot surface no longer describes this active
  read-model wiring as shadow/cutover infrastructure.
- `utxo_recovery_restore.c` now carries rich `struct zcl_result` status inside
  its import/restore result structs, logs non-OK statuses at the boot caller,
  and is removed from the E2 service-result baseline.
- `utxo_recovery_service.c` now carries rich `struct zcl_result` status inside
  its recovery execution result, logs non-OK execution failures at the boot
  caller, and is removed from the E2 service-result baseline.
- `utxo_recovery_backfill.c` is split out as the shielded-value backfill
  service helper, with its own `zcl_result` argument validation and an explicit
  `-1` failure return. The split keeps `utxo_recovery_service.c` under the
  framework file-size ceiling.
- `chain_state_repository.c` now exposes `csr_commit_tip_result()`, a
  `struct zcl_result` wrapper over the legacy `enum csr_result` commit API, and
  is removed from the E2 service-result baseline while call sites migrate.
- `legacy_mirror_sync_service.c` now exposes
  `legacy_mirror_sync_request_catchup_result()`, routes the
  `legacy_mirror_stuck` condition through the rich result surface, and is
  removed from the E2 service-result baseline. `check_one_result_type` now
  supports the intended empty-baseline state.
- `sync_controller_import.c` is below the E1 file-size ceiling after extracting
  the LevelDB UTXO decode/bind helpers into `utxo_import_pipeline.c`, a Service
  helper with `zcl_result` writer-bind status. The E1 baseline no longer
  grandfathers `sync_controller_import.c`.
- `legacy_import.c` is below the E1 file-size ceiling after extracting raw
  block scanning, BIP34 discovery, Sapling prefilter, and decrypt workers into
  private controller helpers `legacy_import_scan.c` / `legacy_import_scan.h`.
- `sync_controller_catchup.c` is below the E1 file-size ceiling after extracting
  Sapling tree rebuild logic into `sync_controller_sapling_tree.c` and wallet /
  mempool persistence into `sync_controller_persistence.c`.
- `legacy_mirror_sync_service.c` is below the E1 file-size ceiling after moving
  lifecycle, stats, dump-state, and test-surface code into
  `legacy_mirror_sync_state.c` behind `legacy_mirror_sync_internal.h`.
- `tools/scripts/check_file_size_ceiling.sh` now supports the intended empty
  baseline state, and `tools/scripts/file_size_ceiling_baseline.txt` contains
  no file entries.
- Typed-blocker adoption is no longer grandfathered. The legacy mirror and
  mirror-consensus public stats now expose typed blocker classes plus
  reason/id fields, `block_source_policy` no longer carries a raw
  `selection_blocker` C field, the legacy JSON keys are preserved for clients,
  and `tools/scripts/typed_blocker_baseline.txt` is empty.
- Active-chain cache/window moves have been separated from public tip authority:
  production cache updates now call `active_chain_move_window_tip()`. The E6
  one-write-path baseline was initially reduced from 34 to 26 write surfaces
  after moving public tip authority to reducer stages and explicit
  repair/bootstrap APIs.
- `docs/work/` now contains only the parallel-worktree protocol. Obsolete
  cutover/B8 runbooks, stale reducer-ingest design snapshots, and a paused
  worker assignment that referenced deleted files were removed; source/test
  comments that pointed at those deleted docs are now self-contained.
- Production comments/log labels under `app/`, `lib/storage/`,
  `lib/validation/`, and `tools/mcp/` no longer call active projection paths
  shadow/cutover/projection-diff machinery. UTXO projection emit helpers are
  now named `*_projection`, not `*_shadow`.
- Production comments in the header-probe Job, UTXO reimport flag,
  chain-restore seams, block-source policy seams, process-block core, boot
  wiring, and connman now describe current ownership/purpose instead of old
  Phase/PR/dissolve code-motion scaffolding. `test_make_lint_gates` now has a
  guard for those stale labels.
- UTXO reducer/recovery comments now describe current ownership instead of
  B3/B5/C3 code-motion scaffolding. The guarded production files include the
  UTXO apply inverse-delta seam, the UTXO reimport sentinel, and boot
  projection-storage wiring.
- Wallet, transaction, repair, store, shielded-wallet, wallet-rescan,
  wallet-view, and sync controller file headers now describe their current RPC
  or helper purpose instead of D5 split/code-motion history. The guarded
  scaffold-label lint test now covers those controller files and rejects stale
  `Split out of`, `behavior byte-identical`, `behavior unchanged`,
  `pre-split monolith`, and `extracted from` wording.
- Explorer/store/wallet GUI view headers, their thin controller seams, and the
  related diagnostics/block-index/chain-state/bg-validation service comments
  now describe current ownership instead of D2/D5/checklist/code-motion
  history. The guarded scaffold-label lint test now covers those files and
  rejects `checklist item`, `checklist D5`, `moved out of`,
  `move, not a redesign`, `prior controller implementation`, `Extracted from`,
  `pure refactor`, `single-engine replacement`, and boot-decomposition phase
  wording.
- Process-block ownership comments now describe current helper boundaries
  instead of split/code-motion scaffolding. The guarded scaffold-label lint
  test now covers the process-block top-level wiring, crash hooks,
  failed-child propagation, flush policy, internal helper header, and missing
  UTXO self-heal coordinator, and rejects stale split/code-motion labels there.
- Wallet/explorer view-template comments now describe current View ownership
  instead of extraction/parity history. The scaffold-label lint guard now covers
  the remaining wallet/explorer view fragments plus the adjacent explorer block
  controller and rejects generic stale parity wording such as `byte-identical`,
  `prior inline`, and `not a redesign`.
- Job-stage helper comments now describe current reducer-stage ownership
  instead of B2/single-engine/parity/code-motion scaffold. The scaffold-label
  lint guard now covers the stage helpers, block-header emit helper, body
  persist, header admit, validate-headers helper/report/internal seams, and
  tip-finalize post-step helper, and rejects stale `B2:`, `copy-pasted`,
  `Precedent:`, and generic `single-engine` wording.
- Production-wide cold-start boot/projection comments and CLI help now say
  event-log or reducer path instead of `single-engine`; snapshot activation's
  local cold-start seed helper was renamed to the event-log projection boot
  seed, and the source scan is clean for that retired term outside the lint
  gate vocabulary.
- Wallet read aggregate/activity helpers now live in
  `app/models/src/wallet_tx_reads.c` instead of being parked at the bottom of
  `sapling_note.c`. Database and wallet model sibling comments now describe
  current ownership instead of byte-identical/file-size split history, and the
  scaffold-label guard covers the touched model files plus UTXO recovery
  backfill comments.
- Snapshot, background-validation, block-index, chain-evidence, and consensus
  reject service comments now describe current service ownership instead of
  phase-aligned/code-motion/verbatim split history. The scaffold-label guard
  now covers the snapshot sync source/header split, bg-validation private
  header, chain-evidence store, and consensus reject index, and rejects generic
  `verbatim` / `Pure code-motion` wording in guarded production comments.
- Explorer factoids/stats controller compatibility shim headers were deleted.
  API and explorer controllers now include the View shape headers directly,
  while adjacent diagnostics, blockchain, supervisor, boot-index, and
  header-accept comments describe current ownership instead of legacy
  extraction/verbatim/single-engine scaffold. The scaffold-label guard covers
  those files and blocks the deleted shim/include and stale wording patterns.
- Crypto registry, chaos harness, platform clock/RNG, storage projection
  backing, UTXO/wallet projections, and nearby reducer/controller comments now
  describe active ownership instead of skeleton, idle-PR, future-wave, phase,
  or byte-parity scaffold history. The scaffold-label guard now covers those
  files and rejects the stale labels in guarded production comments.
- Event-log, projection payload, block-index projection, projection replay boot
  wiring, and diagnostics registry comments now describe the current durable
  reducer/projection replay surface instead of Phase-4/Phase-7 scaffold
  history. The scaffold-label guard now covers those storage and diagnostics
  files and rejects `Phase 4` / `Phase 7a` labels there.
- Production UTXO projection authorship is fixed on the stage/reducer path; the
  old author switch setter is now a `ZCL_TESTING`-only API, removing it from
  the E6 production write-surface baseline.
- Stale lib-layering baseline entries for removed single-engine validation
  files were deleted, dropping the lib-to-app include baseline from 101 to 82.
- Stale lib-layering baseline entries for removed
  `msg_version.c` / `msgprocessor_snapshot.c` includes were deleted, and file
  manifest protocol declarations moved into
  `lib/net/include/net/file_manifest.h`, dropping the lib-to-app include
  baseline from 82 to 79.
- The next lib-layering slice moved `zcl_node_db_path()` from app views to
  `lib/util`, moved UTXO script classification from the ActiveRecord model to
  `lib/script`, replaced the `msg_internal.h` service include with a forward
  declaration, and removed the storage-layer model include from
  `coins_view_sqlite.c`, dropping the lib-to-app include baseline from 79 to
  75.
- Schema migration now lives in the Model shape instead of `lib/storage`, and
  file-offer SQLite persistence moved from `lib/net/src/file_market.c` into
  `app/models/src/file_offer.c` behind the FileOffer model lifecycle. The
  lib-to-app include baseline is down from 75 to 71.
- ZMSG SQLite persistence moved from `lib/net/src/zmsg.c` into
  `app/models/src/zmsg.c` behind the Zmsg model lifecycle. `lib/net/src/zmsg.c`
  now owns only wire serialization, message IDs, and the in-memory delivery
  cache. The lib-to-app include baseline is down from 71 to 68.
- ZNAM at-rest record structs and SQLite persistence moved from
  `lib/znam/include/znam/znam.h` / `lib/znam/src/znam.c` into
  `app/models/include/models/znam.h` / `app/models/src/znam.c`. The lib ZNAM
  files now own only OP_RETURN protocol parsing/building, and the lib-to-app
  include baseline is down from 68 to 65.
- Swap-contract persisted records and SQLite persistence moved from
  `lib/script/include/script/htlc.h` / `lib/script/src/htlc.c` into
  `app/models/include/models/swap_contract.h` /
  `app/models/src/swap_contract.c`. The lib HTLC files now own only script
  building/parsing, address helpers, secrets, and swap IDs, and the lib-to-app
  include baseline is down from 65 to 62.
- Addrman sidecar integrity moved from `app/services` into `lib/net`, and the
  shared SHA3 sidecar helper moved from `app/services` into `lib/storage`.
  `connman.c` now includes `net/addrman_integrity.h` for peers.dat integrity,
  while block-index sidecar service code uses the storage helper. The
  lib-to-app include baseline is down from 62 to 61.
- Mining found-block submission moved out of `lib/mining` and into its callers.
  `gen_context` now exposes a found-block callback, boot/controller code owns
  reducer ingestion for mined blocks, and `lib/mining/src/miner.c` no longer
  includes the app activation service. The lib-to-app include baseline is down
  from 61 to 60.
- Connman onion peer discovery is now a net-layer callback registered by boot.
  The shared `struct onion_peer` contract lives under `lib/net`, boot injects
  `blog_discover_onion_peers()`, and `lib/net/src/connman.c` no longer
  includes the blog controller. The lib-to-app include baseline is down from
  60 to 59.
- Onion service blog serving and peer discovery are now net-layer app-handler
  callbacks registered by boot. `lib/net/src/onion_service.c` owns Tor/onion
  routing only, delegates blog responses through `onion_blog_serve_fn`, and no
  longer includes the blog controller. The lib-to-app include baseline is down
  from 59 to 58.
- Compact-block reducer submission is now a net-layer callback registered by
  boot. `lib/net/src/msg_compact.c` owns only BIP152 reconstruction and peer
  scoring, while boot maps completed compact blocks to
  `REDUCER_SRC_COMPACT`. The lib-to-app include baseline is down from 58 to
  57.
- Handshake peer persistence is now a net-layer callback registered by boot.
  `lib/net/src/msg_version.c` owns version/verack protocol state only, while
  boot owns the async `db_service_enqueue_write()` /
  `db_peer_save_advisory()` model write. The lib-to-app include baseline is
  down from 57 to 55.
- Metrics service/model gauges and connman known-ZCL23 peer selection are now
  boot-owned callback injections. `lib/metrics/src/metrics.c` owns console and
  Prometheus gauge publishing without including app services/models, and
  `lib/net/src/connman.c` owns outbound peer selection without including the
  Peer model or runtime singleton. The lib-to-app include baseline is down
  from 55 to 50.
- Tx wallet persistence and the snapshot-active query are now net-layer
  callbacks registered by boot. `lib/net/src/msg_tx.c` owns transaction relay,
  mempool classification, peer scoring, Dandelion propagation, and inventory
  request policy without including app controller/model/service headers. The
  lib-to-app include baseline is down from 50 to 46.
- P2P block reducer submission is now a net-layer callback registered by boot,
  and block-message snapshot gating uses the already-injected snapshot-active
  callback. `lib/net/src/msg_blocks.c` owns block/getdata/getblocks protocol
  handling without including app controller/model/activation/snapshot headers.
  The lib-to-app include baseline is down from 46 to 41.
- Block-connected tip observers are now a net-layer callback registered by
  boot. `lib/net/src/msg_blocks.c` no longer includes the sync monitor service;
  boot owns the `sync_monitor_on_block_connected()` side effect. The
  lib-to-app include baseline is down from 41 to 40.
- Block-sync planning for invalid-block retries and valid-block acceptance now
  runs through net-internal message-processor helpers. `lib/net/src/msg_blocks.c`
  owns block/getdata/getblocks wire handling without including the block sync
  service. The lib-to-app include baseline is down from 40 to 39.
- Stale unused app-layer includes were removed from `msg_headers.c`,
  `msgprocessor.c`, and `msgprocessor_snapshot.c`. FlyClient proof building is
  now a boot-owned callback injection, so `msgprocessor_snapshot.c` no longer
  reaches into the blockchain controller or the MMB leaf-store model. The
  lib-to-app include baseline is down from 39 to 32.
- Snapshot block-piece serving now callback-injects block-hash range loading
  and local UTXO SHA3 computation from boot. `msgprocessor_snapshot.c` no
  longer includes the Block model or dereferences `struct node_db`; the
  lib-to-app include baseline is down from 32 to 31.
- ZMSG, file-offer, and file-service P2P persistence is now callback-injected
  from boot. `lib/net/src/msgprocessor.c` owns protocol handling without
  including the node DB model header or the FileService model; the lib-to-app
  include baseline is down from 31 to 29.
- Snapshot-sync service accessors moved from `lib/net/src/msgprocessor.c` into
  `lib/net/src/msgprocessor_snapshot.c`, where the snapshot service dependency
  already belongs. Generic message-processor orchestration no longer includes
  the snapshot sync service, and the lib-to-app include baseline is down from
  29 to 28.
- Header/block sync planner contracts now live in
  `lib/sync/include/sync/sync_planner.h`. The old app-layer re-export headers
  for header, block, and aggregate sync planning were deleted; app/test callers
  include the lib-owned planner contract directly, and the lib-to-app include
  baseline remains empty.
- `lib/event/include/event/event.h` no longer re-exports the sync/snapshot
  FSM API from `lib/sync`. The event header owns event bus declarations plus
  peer state only; app/config/net/test callers that use `sync_get_state()`,
  `enum sync_state`, `snapsync_get_state()`, or snapshot FSM names include
  `sync/sync_state.h` directly.
- The header-anchor repair path no longer requires the net header handler to
  include the app chain-tip service. The current CSR-less fallback routes
  through boot-owned chain-state callbacks, and the lib-to-app include
  baseline is down from 24 to 23.
- Peer header votes for the quorum oracle are now callback-injected from boot.
  `lib/net/src/msg_headers.c` records accepted fast-sync peer header votes
  through the message-processor callback surface, while boot owns
  `quorum_oracle_record_peer_header_vote()`. The lib-to-app include baseline
  is down from 23 to 22.
- Process-block `node_db` open checks now route through the runtime boundary.
  `config/src/runtime.c` owns the one `struct node_db` layout check, while
  `lib/validation/src/process_block.c` keeps the DB handle opaque and no
  longer includes the DB model header. The lib-to-app include baseline is down
  from 22 to 21.
- Process-block flush-policy DB state persistence, sync-batch flush, and WAL
  checkpoint operations now route through the same runtime boundary.
  `lib/validation/src/process_block_flush_policy.c` no longer includes the DB
  model header or dereferences `struct node_db`; the lib-to-app include
  baseline is down from 21 to 20.
- Process-block self-heal durable UTXO max-height checks now route through the
  runtime boundary. `config/src/runtime.c` owns the SQLite query over `utxos`,
  while `lib/validation/src/process_block_self_heal.c` keeps the DB handle
  opaque for that check and no longer includes the DB model header. The
  lib-to-app include baseline is down from 20 to 19.
- Process-block self-heal tx-index recovery now routes through a runtime-owned
  `app_runtime_tx_index_hit` result. `config/src/runtime.c` owns the TxIndex
  model lookup, while `lib/validation/src/process_block_self_heal.c` keeps the
  model record type opaque and no longer includes the TxIndex model header.
  The lib-to-app include baseline is down from 19 to 18.
- Fast-sync chunk apply now uses direct SQLite bind calls plus the lib-side
  `AR_STEP_WRITE` helper instead of direct ActiveRecord bind/step macros.
  `lib/net/src/fast_sync.c` no longer directly includes the ActiveRecord or DB
  model headers, and the lib-to-app include baseline is down from 18 to 16.
- Fast-sync snapshot prebuild now takes a caller-owned serializer callback.
  Boot injects the UTXO model serializer through
  `boot_serialize_utxo_snapshot()`, while `lib/net/src/fast_sync.c` owns only
  protocol pathing and metadata publishing. The lib-to-app include baseline is
  down from 16 to 15.
- Header-sync snapshot active/anchor access is now callback-injected through
  the message processor. Boot wires `snapsync_is_active()` plus anchor get/set
  callbacks, while `lib/net/src/msg_headers.c` uses
  `msg_processor_snapshot_active()` / anchor helpers and no longer includes the
  snapshot sync service. The lib-to-app include baseline is down from 15 to
  14.
- Header activation, block-file scanning, height-repair state, and
  post-activation anchor repair are now callback-injected through the message
  processor. Boot owns `activation_request_connect()`,
  `activation_clear_anchor()`, `scan_block_files_mark_data()`,
  `block_index_heights_repaired()`, and `bii_repair_post_activation_anchor()`,
  while `lib/net/src/msg_headers.c` uses app-free message-processor helpers.
  The lib-to-app include baseline is down from 14 to 12.
- Header best-tip promotion and snapshot-anchor recommit now go through
  boot-owned message-processor callbacks. Boot owns
  `csr_commit_header_tip()` / `csr_commit_tip()` and the `ZCL_TESTING`
  fallback, while `lib/net/src/msg_headers.c` uses
  `msg_processor_commit_header_tip()` /
  `msg_processor_recommit_snapshot_anchor()` and no longer includes the
  chain-state repository. The lib-to-app include baseline is down from 12 to
  11.
- Stale `process_block_core.c` app includes for deleted legacy engine surfaces
  were removed. Gap-fill wakeups are now boot-injected through a
  mutex-protected `process_block_set_gap_fill_kick()` hook, so validation no
  longer includes the gap-fill service or chain-tip service and keeps only the
  real chain-evidence/chain-state-repository app edges. The lib-to-app include
  baseline is down from 11 to 3.
- Process-block tip publication is now boot-owned through
  `process_block_set_tip_publication_hooks()`. Validation passes
  `process_block_tip_evidence` over the hook boundary, while boot translates
  that evidence to the chain-evidence controller / CSR app services and owns
  the test fallback. `lib/validation/src/process_block_core.c` no longer
  includes chain-evidence or chain-state-repository headers, and the
  lib-to-app include baseline is down from 3 to 1.
- Process-block runtime hook dispatch and failed-child propagation were split
  out of `process_block_core.c`. `process_block_runtime_hooks.c` now owns the
  mutex-protected gap-fill and tip-publication callback bridges, while
  `process_block_failed_child.c` owns the bounded `BLOCK_FAILED_CHILD`
  propagation helper and OOM-amplifier guards. `process_block_core.c` is down
  from 1065 to 893 lines.
- Block-index disk placement and hydration moved from
  `process_block_core.c` into `process_block_index.c`. The new file owns
  `find_block_pos()`, `block_index_refresh_header()`,
  `block_index_hydrate_from_disk()`, and the test hydration wrapper. At that
  slice, `process_block_core.c` was down from 893 to 776 lines.
- Block-index persistence snapshot construction is centralized in
  `process_block_index.c` via `block_index_snapshot_for_persist()`.
  `process_block_invalidate.c` and `process_block_revalidate.c` no longer
  duplicate the field-by-field `disk_block_index` copy, and their production
  comments now describe reducer/stage authority instead of deleted
  single-engine files.
- Tip-publication evidence and commit mechanics moved from
  `process_block_core.c` into `process_block_tip_publish.c`. The new file owns
  `process_block_tip_is_best_work()`, `process_block_commit_tip()`,
  `update_tip()`, `process_block_commit_tip_ext()`, and the test wrappers,
  leaving `process_block_core.c` focused on chain selection and active-tip
  child discovery. `process_block_core.c` is down from 776 to 486 lines.
- Active-tip child discovery and disk verification moved from
  `process_block_core.c` into `process_block_tip_child.c`. The new file owns
  `process_block_verify_active_tip_child_on_disk()`,
  `find_best_active_tip_child()`, and
  `find_verified_unlinked_active_tip_child()`, leaving
  `process_block_core.c` focused on chain selection and contextual-header
  skip logic. Stale monolith includes were also pruned, and
  `process_block_core.c` is down from 486 to 247 lines.
- Contextual-header skip policy moved from `process_block_core.c` into
  `process_block_contextual_header.c`. The new file owns
  `process_block_should_skip_contextual_header()` plus its sparse
  retarget/MTP-window helper, leaving `process_block_core.c` focused on
  best-work chain selection. `process_block_core.c` is down from 247 to 177
  lines.
- Legacy zclassicd RPC missing-UTXO recovery moved from
  `process_block_self_heal.c` into `process_block_self_heal_legacy_rpc.c`.
  The new file owns the compatibility RPC body builder, JSON-lite response
  parsing, raw-transaction decode, txid verification, and event emission for
  the legacy-RPC recovery source. `process_block_self_heal.c` is down from
  751 to 578 lines.
- Bounded chain-scan missing-UTXO recovery moved from
  `process_block_self_heal.c` into `process_block_self_heal_chain_scan.c`.
  The new file owns the active-chain disk walk, tx-index backfill, and scan
  hit/exhaustion accounting/events. `process_block_self_heal.c` is down from
  578 to 451 lines.
- SQLite tx-index missing-UTXO recovery moved from
  `process_block_self_heal.c` into
  `process_block_self_heal_sqlite_tx_index.c`. The new file owns the
  runtime-owned TxIndex hint lookup, block-map source resolution,
  consensus-backed disk verification, local txid verification, and UTXO
  injection for that recovery source. `process_block_self_heal.c` is down
  from 451 to 361 lines.
- Self-heal scan counters and operator tunables moved from
  `process_block_self_heal.c` into `process_block_self_heal_scan_state.c`.
  The new file owns the `g_self_heal_*` atomics, public stats snapshot, and
  `ZCL_SELF_HEAL_SCAN_*` environment parsing. `process_block_self_heal.c` is
  down from 361 to 302 lines.
- Self-heal hot-loop / reimport policy moved from
  `process_block_self_heal.c` into `process_block_self_heal_hot_loop.c`.
  The new file owns `needs_reimport` flag writes, recent-reimport detection,
  activation-pause get/clear APIs, test trigger hooks, and shutdown requests.
  `process_block_self_heal.c` is down from 302 to 177 lines.
- Recovered-UTXO cache injection moved from `process_block_self_heal.c` into
  `process_block_self_heal_inject.c`. The new file owns verified recovered-tx
  materialization into the coins cache plus the recovery-source diagnostic,
  leaving `process_block_self_heal.c` focused on missing-UTXO failure
  tracking. `process_block_self_heal.c` is down from 177 to 138 lines.
- The snapshot-sync router contract now lives in
  `lib/net/include/net/snapshot_sync_contract.h`. The old
  app-layer compatibility wrapper was deleted; app/config/test callers include
  the lib-owned contract directly, and the lib-to-app include baseline remains
  empty.
- Read-only chain diagnostics no longer flush consensus state as a side
  effect. `getdataintegrity`, `gethodlwave`, and `gethodlwaveimage` scan the
  persisted read models instead of calling `coins_view_cache_flush()`, dropping
  the E6 one-write-path baseline from 24 to 21 write surfaces.
- The redundant `coins_view_sqlite_batch_write()` compatibility wrapper was
  deleted. Vtable, reducer-stage, and test callers now use
  `coins_view_sqlite_batch_write_ex(..., NULL)` when no path commitment write
  is needed, leaving one SQLite coins flush entry point and dropping the E6
  one-write-path baseline from 21 to 17 write surfaces.
- The `active_chain_set_tip()` compatibility alias was deleted, and tests now
  call `active_chain_move_window_tip()` directly when they need to seed the
  in-memory active-chain cache. Production process-block flushing now refuses
  to fall back to generic `coins_view_cache_flush()` when the reducer SQLite
  writer was not installed; only `ZCL_TESTING` keeps that fallback for harness
  setup. The E6 one-write-path baseline is down from 17 to 16 write surfaces.
- Boot reindex no longer calls `coins_view_cache_flush()` directly.
  `reindex_chainstate()` now routes start, periodic, and final UTXO flushes
  through a local helper that calls `coins_view_sqlite_batch_write_ex()` and
  clears the cache only after a successful durable write. The E6 one-write-path
  baseline is down from 16 to 13 write surfaces.
- Shutdown no longer calls `coins_view_cache_flush()` directly. Emergency,
  network-quiesce, and final shutdown UTXO flushes now route through a local
  helper that calls `coins_view_sqlite_batch_write_ex()` and clears the cache
  only after a successful durable write. The E6 one-write-path baseline is
  down from 13 to 10 write surfaces.
- Runtime `reindexchainstate` no longer owns a second replay writer. The RPC
  is now an explicit retired-operation compatibility error that directs
  operators to restart with `-reindex-chainstate`, whose boot path already uses
  the reducer/boot SQLite writer. `repairutxos` no longer flushes the
  projection-backed coins cache; repaired UTXOs persist through the UTXO model
  and the cache remains a process-local read cache. The E6 one-write-path
  baseline is down from 10 to 5 write surfaces.
- `coins_view_cache_flush()` is no longer a production-visible API. The
  remaining child-cache flush behavior is renamed
  `coins_view_cache_flush_for_testing()` behind `ZCL_TESTING`, and production
  comments now refer to the flush policy rather than the old generic cache
  flush entry point. The E6 one-write-path baseline is down from 5 to 3 write
  surfaces.
- The E6 one-write-path baseline is empty. The canonical
  `coins_view_sqlite_batch_write_ex()` contract/implementation and the
  process-block flush-policy call are explicitly tagged as destination writer
  surfaces, alongside the already-tagged reducer, boot-reindex, shutdown, and
  vtable-adapter surfaces. Untagged new chain-state writers now fail the
  ratchet without grandfathering.
- `wallet_scan.c` and `legacy_import.c` no longer call `sqlite3_exec()`
  directly; their checked exec helpers route through `node_db_exec()`, dropping
  the controller raw-SQL baseline from 14 to 12 controller files.
- `snapshot_controller.c`, `wallet_shielded_controller.c`, and
  `repair_controller.c` were removed from the controller raw-SQL baseline.
  Snapshot exec helpers now take `struct node_db *` and route through
  `node_db_exec()`, `z_listunspent` uses the existing block model height query,
  and UTXO height-repair count/update knowledge lives on `models/utxo`. The
  controller raw-SQL baseline is down from 12 to 9 controller files.
- `repair_controller_utxo.c` and `sync_controller_blocks.c` were removed from
  the controller raw-SQL baseline. `repairutxos` transaction control now uses
  `node_db_begin()` / `node_db_rollback()` / `node_db_commit()`, and
  per-block Sapling tree persistence is owned by
  `db_block_update_sapling_tree_data()`. The controller raw-SQL baseline is
  down from 9 to 7 controller files.
- `sync_controller_import.c` was removed from the controller raw-SQL baseline.
  Its post-import UTXO row/distinct-txid validation now calls
  `db_utxo_count_rows_and_distinct_txids()`, keeping table cardinality SQL on
  the UTXO model. The controller raw-SQL baseline is down from 7 to 6
  controller files.
- `wallet_controller_keys.c` was removed from the controller raw-SQL baseline.
  Key readback now uses `wallet_sqlite_read_single_key()`, rollback uses the
  new `wallet_sqlite_delete_key_r()`, and `test_wallet_persistence_cycle`
  covers the delete-key roundtrip. The controller raw-SQL baseline is down
  from 6 to 5 controller files.
- `blockchain_controller.c` was removed from the controller raw-SQL baseline.
  MMR/MMB/commitment-MMR state persistence now uses `node_db_state_get()` /
  `node_db_state_set()`. The controller raw-SQL baseline is down from 5 to 4
  controller files.
- `file_controller_export.c` was deleted from the controller layer. Consensus
  snapshot export now lives in
  `consensus_snapshot_export_service_run()` with a `struct zcl_result` service
  return, and boot/test callers use the service header directly. The
  controller raw-SQL baseline is down from 4 to 3 controller files.
- `blockchain_controller_admin.c` was removed from the controller raw-SQL
  baseline. `importchainstate` now calls
  `db_utxo_rebuild_wallet_and_address_caches()`,
  `db_utxo_total_value()`, and `db_wallet_utxo_balance()` instead of owning
  cache-rebuild and reporting SQL directly. `test_models` covers the derived
  wallet/address cache rebuild. The controller raw-SQL baseline is down from
  3 to 2 controller files.
- `snapshot_controller_txindex.c` and `dbquery_controller.c` were removed from
  the controller raw-SQL baseline. The tx-index job now routes additive-build
  database tuning through `db_tx_configure_additive_build()` and the block-file
  scan query through `db_block_prepare_file_position_scan()`. The `zcl_sql`
  diagnostic primitive now prepares statements through
  `node_db_prepare_readonly_query()`, which rejects writable statements at the
  database boundary. The controller raw-SQL baseline is empty.
- Production raw malloc/calloc/realloc allowlist debt is at zero active
  entries after migrating the boot-services DB path allocation to
  `zcl_malloc()` and the connman deferred-free resize to `zcl_realloc()`.
- Test-only small-projection comparison helpers were removed from the
  production `lib/storage` API. The contacts, onion-announcement, and HODL
  projection parity check now lives in `test_small_projections`, which compares
  the projection SQLite files directly against the legacy fixture database.
- Production C/H comments and diagnostics in `app/`, `lib/`, `config/`, and
  `tools/` no longer use the deleted single-engine block-connection names for
  active behavior. Architecture docs now show block intake as
  `reducer_ingest_block` plus the eight staged reducer Jobs, and
  `test_make_lint_gates` has a production-source guard that fails if those
  names reappear outside tests/views.
- The condition-layer PR-number scaffold headers were deleted. Each affected
  sync/peer/snapshot condition now has a condition-named public header for its
  registration and `ZCL_TESTING` hooks, and the old PR-number test groups were
  renamed to behavior-named condition suites.
- `lib/framework/README.md` no longer describes Phase-0 scaffold or retired
  macro DSL forms; it now documents the framework primitives that actually
  exist today.
- The `lib/framework` mailbox/projection re-export headers were deleted.
  `header_admit_inbox.h` includes the real `util/mailbox.h` primitive with
  typed inbox helpers, and `chain_projection.c` includes `util/projection.h`
  directly for snapshot reads. `lib/framework` now owns only the real
  condition engine rather than forwarding primitive headers.
- `utxo_recovery_service.h` no longer re-exports the storage-owned
  `utxo_reimport_flag` primitive. Boot and tests that check or clear the
  needs-reimport sentinel include `storage/utxo_reimport_flag.h` directly,
  while the recovery service header owns only recovery-service contracts.
- Staged Job/progress/supervisor comments and diagnostics now describe the
  current reducer stage names instead of old wave/stage-number scaffold
  history. The scaffold-label guard covers those stage/progress files and
  rejects those retired labels there.
- Reducer-ingest, header-admit model, invalidate/revalidate, mining/repair,
  and Job README comments now describe the staged Job pipeline directly
  instead of the old wave labels. The scaffold-label guard now covers those
  files too and rejects the hyphenated retired wave label plus the stale
  chain-selection marker it flushed out.
- Failed-block revalidation, chain-domain escalation, and boot supervisor
  comments now describe current ownership instead of old Wave-M / round
  scaffold labels. The scaffold-label guard now covers the revalidation
  headers/source plus the chain supervisor header and rejects the retired
  Wave-M label across the guarded production comment set.
- `docs/PROJECT_OVERVIEW.md` now records the current tracked file inventory,
  framework-shape purpose map, active legacy lifecycle, and deprecated/retired
  surfaces for the next developer. The deploy service template now keeps the
  local zclassic23 listener on `8023` and drops the stale no-op `-shadow`
  comparison flag/comment so future deploys do not conflict with a sibling
  zclassicd/zcashd listener.

## Active Debt

### Delete Or Move Out Of Production

- No production `shadow`/`cutover`/`projection-diff` matches remain in
  `app/`, `lib/storage/`, `lib/validation/`, or `tools/mcp/` C/H files outside
  tests/views. Keep this at zero; normalize remaining historical test/doc
  wording only when it obscures current behavior.
- No production deleted single-engine block-connection names remain in
  `app/`, `lib/`, `config/`, or `tools/` C/H files outside tests/views. The
  lint-gate self-test now guards that absence.

### E1 Oversized App Files

`tools/scripts/file_size_ceiling_baseline.txt` is empty. There are no
grandfathered oversized app `.c` files; keep this gate at zero.

### E2 Service Result Debt

`tools/scripts/one_result_type_baseline.txt` is empty. The file-level ratchet is
at zero grandfathered service files. Remaining work is call-site cleanup:
legacy compatibility bool APIs should migrate toward `struct zcl_result` as
their owning files are split or touched for adjacent debt.

### E6 One-Write-Path Guardrail

`tools/scripts/one_write_path_baseline.txt` is empty. Canonical reducer, boot,
shutdown, vtable-adapter, process-block flush-policy, and SQLite writer
surfaces carry inline `one-write-path-ok:<tag>` markers. Any untagged new
chain-state writer fails the ratchet.

The final form remains one durable writer and one cursor authority. Current
guardrail: low-level active-chain cache/window moves and stale `tip_finalize`
cursors cannot regress the public reducer tip.

### Supervisor Debt

`tools/scripts/supervisor_baseline.txt` is empty. Every long-running service
that gate tracks now registers a supervisor liveness contract.

### Typed Blocker Debt

`tools/scripts/typed_blocker_baseline.txt` is empty. Raw blocker string fields
and legacy blocker setters are not grandfathered; keep this gate at zero.

### Controller And Layering Debt

- Lib-layering debt is at zero grandfathered entries. Keep
  `tools/scripts/lib_layering_baseline.txt` empty.
- Controller raw-SQL debt is at zero grandfathered files. Keep
  `tools/lint/no_raw_sqlite_in_controllers_baseline.txt` empty.
- `lib/validation/src/process_block_core.c` now owns best-work chain
  selection only.
- `lib/validation/src/process_block_index.c` now owns block-file placement,
  block-index hydration, and shared `disk_block_index` persistence snapshot
  construction for invalidate/revalidate status flips.
- `lib/validation/src/process_block_self_heal.c` now owns missing-UTXO
  failure tracking only; keep auditing the process-block split set for stale
  helper boundaries as adjacent recovery code changes.

## Next Work Order

1. Delete zero-purpose scaffolding and stale build references.
2. Keep production terminology clean; normalize remaining historical test/doc
   fixture names only when touched for adjacent work.
3. Keep import/catchup/legacy-import code below the file-size ceiling while
   moving remaining mixed-purpose code toward the correct framework shape.
4. Audit the now-small process-block split files for stale helper boundaries
   and delete any remaining zero-purpose scaffolding.
5. Keep every lint baseline empty while continuing process-block and
   mixed-purpose file cleanup.
6. Run `make lint`, rebuild `test_parallel`, run the suite, then prove live
   node progress with a soak.

## Latest Verification

- Runtime/status sample at 2026-06-01 18:26 UTC: zclassic23 and zclassicd are
  separated correctly at the socket layer. zclassic23 is active under user
  systemd with PID 3091443 on local P2P `8023` and RPC `18232`; zclassicd is a
  separate process on P2P `8033` and RPC `8232`. zclassic23 RPC returned height
  3,130,701, four peers in the sample, and `gettxoutsetinfo` reported
  1,362,095 UTXOs. Caveat: zclassic23 dumped core once at 18:24:39 UTC
  (`status=11/SEGV`) and systemd restarted it at 18:24:49 UTC. `coredumpctl`
  is not installed and no core file was visible under
  `/home/rhett/.zclassic-c23/cores`; treat live-soak proof as still missing.
  The mirror status was `observing`, with zclassicd height still reported as 0
  from zclassic23's mirror view.
- Runtime/status sample at 2026-06-01 18:14 UTC: zclassic23 was active under
  user systemd with PID 3030320, local P2P `8023`, RPC `18232`, and no local
  `8033` listener owned by zclassic23. RPC returned height 3,130,701 and best
  hash `00001c91107ae83de903678cc3891cd82e59d9b37c4d7bbcf283f7212dd763c8`;
  `gettxoutsetinfo` reported 1,361,783 UTXOs. The legacy mirror was running
  but blocked on `zclassicd:8232` being unreachable (`rpc-unreachable`), not on
  a port conflict.
- Failed-block revalidation label scan after the revalidation/supervisor
  cleanup: clean for `Wave M` / `Wave-M` across the touched revalidation,
  chain-supervisor, activation-controller, and boot-service files.
- `make test_parallel`: pass after rebuilding the parallel runner with the
  widened scaffold-label guard.
- Focused filtered tests passed after the revalidation/supervisor cleanup:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`
  (`0/1` failed in 11s),
  `build/bin/test_parallel --only=revalidate --timeout=120 --verbose`
  (`0/1` failed in 1s),
  `build/bin/test_parallel --only=chain_activation_controller --timeout=120 --verbose`
  (`0/1` failed in 1s), and
  `build/bin/test_parallel --only=supervisor --timeout=120 --verbose`
  (`0/2` failed in 1s).
- `make -j$(nproc)`: pass after the revalidation/supervisor cleanup.
- `make lint`: pass after the revalidation/supervisor cleanup; all
  zero-baseline ratchets remain clean.
- `build/bin/test_parallel --timeout=180`: pass after the revalidation/supervisor
  cleanup, `0/279` groups failed in 57.0s.
- Live sample attempt at 2026-06-01 18:10:29 UTC after the
  revalidation/supervisor cleanup: no continuity proof was available.
  `systemctl --user status zclassic23` could not connect to the user bus,
  `./tools/zcl-rpc getblockcount` and `gettxoutsetinfo` exited with code 7,
  no `zclassic23` process was running, no `8023` or `18232` listener existed,
  and the last 20 minutes of `zclassic23` journal output had no entries. `ss`
  did show the separate `zclassicd` process listening on `8033` and `8232`;
  this slice did not touch port wiring or restart services.
- Active reducer terminology scan after the reducer-ingest/header-admit
  cleanup: clean for retired wave labels and the stale chain-selection marker
  across production C/H files, `app/jobs/README.md`, `docs/FRAMEWORK.md`, and
  the current status-board sections.
- `make test_parallel`: pass after rebuilding the parallel runner with the
  widened scaffold-label guard.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass
  after widening the guard to cover reducer-ingest/header-admit/mining/repair
  comments; `0/1` groups failed in 12.0s.
- `make -j$(nproc)`: pass after the reducer-ingest terminology cleanup.
- `make lint`: pass after the reducer-ingest terminology cleanup; all
  zero-baseline ratchets remain clean.
- `build/bin/test_parallel --timeout=180`: pass after the reducer-ingest terminology
  cleanup, `0/279` groups failed in 56.0s.
- Post-status doc/scans after the reducer-ingest terminology cleanup:
  `tools/scripts/check_doc_accuracy.sh` passed, `git diff --check` passed, the
  zero-baseline / allowlist scan found no non-comment entries in the ratcheted
  baseline files, the production C/H scan under app controllers, services,
  jobs, conditions, supervisors, models, lib, and MCP found no `shadow`,
  `cutover`, `projection-diff`, or `projection_diff` terminology, and the
  active reducer terminology scan remained clean.
- Live sample attempt at 2026-06-01 18:00:39 UTC after the reducer-ingest
  terminology cleanup: no continuity proof was available. `systemctl --user
  status zclassic23` could not connect to the user bus, `./tools/zcl-rpc
  getblockcount` and `gettxoutsetinfo` exited with code 7, no `zclassic23`
  process was running, no `8023` or `18232` listener existed, and the last 20
  minutes of `zclassic23` journal output had no entries. `ss` did show the
  separate `zclassicd` process listening on `8033` and `8232`; this slice did
  not touch port wiring or restart services.
- Staged reducer label scan after the stage/progress/supervisor cleanup:
  clean for retired wave/stage-number labels across the guarded staged-sync
  headers, stage sources, progress store, diagnostics registry, boot, and
  staged-sync supervisor files.
- `git diff --check`: pass after the staged reducer label cleanup.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass
  after widening the production-comment guard to cover the stage/progress
  files; `0/1` groups failed in 11.0s.
- `build/bin/test_parallel --only=staged_sync_supervisor --timeout=120 --verbose`:
  no validation group exists for this exact filter; the harness reported that
  it matched no groups.
- `build/bin/test_parallel --only=supervisor --timeout=120 --verbose`: pass; `0/2`
  supervisor groups failed in 1.0s.
- `make -j$(nproc)`: pass after the staged reducer label cleanup.
- `make lint`: pass after the staged reducer label cleanup; all zero-baseline
  ratchets remain clean.
- `build/bin/test_parallel --timeout=180`: pass after the staged reducer label
  cleanup, `0/279` groups failed in 57.0s.
- Live sample attempt at 2026-06-01 17:49:42 UTC after the staged reducer
  label cleanup: no continuity proof was available. `systemctl --user status
  zclassic23` could not connect to the user bus, `./tools/zcl-rpc
  getblockcount` and `gettxoutsetinfo` exited with code 7, no `zclassic23`
  process was running, no `8023` or `18232` listener existed, and the last 20
  minutes of `zclassic23` journal output had no entries. `ss` did show the
  separate `zclassicd` process listening on `8033` and `8232`; this slice did
  not touch port wiring or restart services.
- Post-status doc/scans after the staged reducer label cleanup:
  `tools/scripts/check_doc_accuracy.sh` passed, `git diff --check` passed, the
  zero-baseline / allowlist scan found no non-comment entries in the ratcheted
  baseline files, the production C/H scan under app controllers, services,
  jobs, conditions, supervisors, models, lib, and MCP found no `shadow`,
  `cutover`, `projection-diff`, or `projection_diff` terminology, and the
  staged reducer label scan remained clean for retired wave/stage-number
  labels.
- `make -j$(nproc)`: pass after normalizing projection-storage/event-log
  comments and rebuilding `zclassic23` / `test_zcl`.
- `make lint`: pass; all framework, lib-layering, controller raw-SQL,
  one-write, service-result, supervisor, typed-blocker, raw allocation, file
  size, and doc gates stayed at zero grandfathered entries.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`
  (`0/1` failed in 12s),
  `build/bin/test_parallel --only=event_log --timeout=120 --verbose`
  (`0/1` failed in 37s), and
  `build/bin/test_parallel --only=projection --timeout=120 --verbose`
  (`0/12` failed in 25s).
- `build/bin/test_parallel --timeout=180`: pass after this projection-storage
  comment/guard cleanup, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 17:36:22 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `8233`, `18232`, or `8232` listener,
  `systemctl --user status zclassic23` could not connect to the user bus, and
  read-only journal checks had no entries. The service was not restarted; this
  slice stayed read-only and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after normalizing active utility/storage comments
  and rebuilding `zclassic23` / `test_zcl`.
- `make lint`: pass; all framework, lib-layering, controller raw-SQL,
  one-write, service-result, supervisor, typed-blocker, raw allocation, file
  size, and doc gates stayed at zero grandfathered entries.
- Focused filtered test passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`
  (`0/1` failed in 11s) after widening the scaffold-label guard and clearing
  the newly surfaced labels.
- `build/bin/test_parallel --timeout=180`: pass after this comment/guard cleanup,
  `0/279` groups failed in 57.0s.
- `tools/scripts/check_doc_accuracy.sh`, `git diff --check`, the empty
  baseline scan, the production shadow/cutover scan, and the focused
  stale-scaffold scan all passed after the status update.
- Quick live sample attempt at 2026-06-01 17:22:56 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `8233`, `18232`, or `8232` listener,
  `systemctl --user status zclassic23` could not connect to the user bus, and
  read-only journal checks had no entries. The service was not restarted; this
  slice stayed read-only and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after deleting the explorer factoids/stats
  controller compatibility headers and routing callers directly to the view
  headers.
- `make test_parallel`: pass after rebuilding the parallel runner with the
  widened scaffold-label guard.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`
  (`0/1` failed in 11s),
  `build/bin/test_parallel --only=explorer --timeout=120 --verbose`
  (`0/1` failed in 1s),
  `build/bin/test_parallel --only=api --timeout=120 --verbose`
  (`0/1` failed in 1s),
  `build/bin/test_parallel --only=domain_consensus_header_accept --timeout=120 --verbose`
  (`0/1` failed in 1s), and
  `build/bin/test_parallel --only=supervisor --timeout=120 --verbose`
  (`0/2` failed in 1s).
- `make lint`: pass; all framework, layering, controller raw-SQL, one-write,
  service-result, supervisor, typed-blocker, raw allocation, and doc gates
  stayed at zero grandfathered entries.
- `build/bin/test_parallel --timeout=180`: pass after this shim deletion / comment
  cleanup, `0/279` groups failed in 57.0s.
- `tools/scripts/check_doc_accuracy.sh`, `git diff --check`, the empty
  baseline scan, the deleted explorer-shim include scan, the focused
  stale-scaffold scan, and the production shadow/cutover scan all passed after
  this slice.
- Quick live sample attempt at 2026-06-01 17:07:07 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `8233`, `18232`, or `8232` listener,
  `systemctl --user status zclassic23` could not connect to the user bus, and
  read-only journal checks had no entries. The service was not restarted; this
  slice stayed read-only and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after normalizing service comments around snapshot
  sync, background validation, block-index loading, chain-evidence storage,
  and consensus reject indexing.
- `make test_parallel`: pass after rebuilding the parallel runner with the
  widened scaffold-label guard.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`
  (`0/1` failed in 11s),
  `build/bin/test_parallel --only=snapshot --timeout=120 --verbose`
  (`0/8` failed in 7s),
  `build/bin/test_parallel --only=bg_validation --timeout=120 --verbose`
  (`0/1` failed in 1s),
  `build/bin/test_parallel --only=block_index_loader --timeout=120 --verbose`
  (`0/1` failed in 1s), and
  `build/bin/test_parallel --only=consensus_reject --timeout=120 --verbose`
  (`0/2` failed in 1s).
- `make lint`: pass; all framework, layering, controller raw-SQL, one-write,
  service-result, supervisor, typed-blocker, raw allocation, and doc gates
  stayed at zero grandfathered entries.
- `build/bin/test_parallel --timeout=180`: pass after this service-comment cleanup,
  `0/279` groups failed in 56.0s.
- `tools/scripts/check_doc_accuracy.sh`, `git diff --check`, the empty
  baseline scan, the production shadow/cutover scan, the service stale-scaffold
  scan, and the focused phase-label scan all passed after the status update.
- Quick live sample attempt at 2026-06-01 16:58:22 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `8233`, `18232`, or `8232` listener,
  `systemctl --user status zclassic23` could not connect to the user bus, and
  read-only journal checks had no entries. The service was not restarted; this
  slice stayed read-only and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after removing the `utxo_reimport_flag` storage
  primitive re-export from `utxo_recovery_service.h`.
- `make test_parallel`: pass after rebuilding the parallel runner with direct
  `storage/utxo_reimport_flag.h` includes at the actual reimport-flag call
  sites.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=utxo_recovery_service --timeout=120 --verbose`,
  `build/bin/test_parallel --only=utxo_reimport_flag --timeout=120 --verbose`,
  `build/bin/test_parallel --only=orphan_utxo_above_tip --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `make lint`: pass; all framework, layering, controller raw-SQL, one-write,
  service-result, supervisor, typed-blocker, raw allocation, and doc gates
  stayed at zero grandfathered entries.
- `build/bin/test_parallel --timeout=180`: pass after the reimport-flag include
  boundary cleanup, `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 15:26:24 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and read-only journal checks
  had no entries. The service was not restarted; this slice stayed read-only
  and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after deleting the framework mailbox/projection
  re-export headers and routing callers to the real util primitives.
- `make test_parallel`: pass after rebuilding the parallel runner with the
  direct util primitive includes.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=mailbox --timeout=120 --verbose`,
  `build/bin/test_parallel --only=mailbox_adoption --timeout=120 --verbose`,
  `build/bin/test_parallel --only=projection_adoption --timeout=120 --verbose`,
  `build/bin/test_parallel --only=header_admit_stage --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `make lint`: pass; all framework, layering, controller raw-SQL, one-write,
  service-result, supervisor, typed-blocker, raw allocation, and doc gates
  stayed at zero grandfathered entries.
- `build/bin/test_parallel --timeout=180`: pass after the framework re-export deletion,
  `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 15:19:45 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and read-only journal checks
  had no entries. The service was not restarted; this slice stayed read-only
  and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after removing the `event/event.h` sync-state
  compatibility re-export and adding direct `sync/sync_state.h` includes at
  the real sync/snapshot FSM call sites.
- `make test_parallel`: pass after rebuilding the parallel runner with the
  direct sync-state include path.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=event --timeout=120 --verbose`,
  `build/bin/test_parallel --only=sync_state_fsm --timeout=120 --verbose`,
  `build/bin/test_parallel --only=state_machine --timeout=120 --verbose`,
  `build/bin/test_parallel --only=wallet_view --timeout=120 --verbose`,
  `build/bin/test_parallel --only=node_health --timeout=120 --verbose`,
  `build/bin/test_parallel --only=block_pruning --timeout=120 --verbose`,
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=net --timeout=120 --verbose`.
- `make lint`: pass; all framework, layering, controller raw-SQL, one-write,
  service-result, supervisor, typed-blocker, raw allocation, and doc gates
  stayed at zero grandfathered entries.
- `build/bin/test_parallel --timeout=180`: pass after the `event.h` re-export removal,
  `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 15:11:12 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and read-only journal checks
  had no entries. The service was not restarted; this slice stayed read-only
  and preserved the `8023` port expectation.
- `git diff --check`: pass after deleting the three sync-planning app-layer
  re-export headers and routing app/test callers directly to
  `sync/sync_planner.h`.
- `make -j$(nproc)`: pass after the sync planner include migration.
- `make test_parallel`: pass after rebuilding the parallel runner with direct
  sync planner includes.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=header_sync --timeout=120 --verbose`,
  `build/bin/test_parallel --only=sync_service --timeout=120 --verbose`,
  `build/bin/test_parallel --only=integrity --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `make lint`: pass after the focused lint-gate test removed its temporary
  fixtures; all framework, layering, controller raw-SQL, one-write,
  service-result, supervisor, typed-blocker, raw allocation, and doc gates
  stayed at zero grandfathered entries.
- Deleted sync wrapper include searches returned no matches across guarded
  app/lib/config/tools/docs C/H and Markdown surfaces.
- `build/bin/test_parallel --timeout=180`: pass after the sync wrapper deletion,
  `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 14:58:08 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and read-only journal checks
  had no entries. The service was not restarted; this slice stayed read-only
  and preserved the `8023` port expectation.
- `git diff --check`: pass after deleting the snapshot-sync app-layer
  compatibility header and routing app/config/test callers directly to the
  lib-owned router contract.
- `make -j$(nproc)`: pass after the contract include migration.
- `make test_parallel`: pass after rebuilding the parallel runner with the
  direct contract include path.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=snapshot_sync_service --timeout=120 --verbose`,
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=peer_snapshot_conditions --timeout=120 --verbose`.
- `make lint`: pass after rerunning it outside the focused lint-gate test's
  temporary fixture window; all framework, layering, controller raw-SQL,
  one-write, service-result, supervisor, typed-blocker, raw allocation, and doc
  gates stayed at zero grandfathered entries.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- All tracked lint baselines/allowlists remain empty:
  `find tools -type f \( -name '*baseline*.txt' -o -name '*allowlist*.txt' \)`
  reported 0 non-comment entries for every tracked file.
- Deleted wrapper include searches returned no matches across guarded
  app/lib/config/tools/docs C/H and Markdown surfaces.
- `build/bin/test_parallel --timeout=180`: pass after the snapshot-sync wrapper
  deletion, `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 14:49:42 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and read-only journal checks
  had no entries. The service was not restarted; this slice stayed read-only
  and preserved the `8023` port expectation.
- `git diff --check`: pass after deleting condition-layer PR scaffold headers
  and renaming the watchdog condition test groups to behavior names.
- `make -j$(nproc)`: pass after the condition header/test rename and README
  doc cleanup.
- `make test_parallel`: pass after rebuilding the parallel runner with
  `sync_watchdog_conditions` and `peer_snapshot_conditions`.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=sync_watchdog_conditions --timeout=120 --verbose`,
  `build/bin/test_parallel --only=peer_snapshot_conditions --timeout=120 --verbose`,
  and `build/bin/test_parallel --only=chain_advance_coordinator --timeout=120 --verbose`.
- `make lint`: pass; all framework, layering, controller raw-SQL, one-write,
  service-result, supervisor, typed-blocker, raw allocation, and doc gates
  stayed at zero grandfathered entries.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- All tracked lint baselines/allowlists remain empty:
  `find tools -type f \( -name '*baseline*.txt' -o -name '*allowlist*.txt' \)`
  reported 0 non-comment entries for every tracked file.
- Stale scaffold searches returned no matches for the deleted PR-number
  condition/test group names or retired framework README scaffold paths.
- Production stale terminology searches still returned no matches for
  `shadow`/`cutover`/`projection-diff`/`projection_diff` in the guarded
  production C/H surface, nor for deleted single-engine block-connection names
  across production C/H files.
- `build/bin/test_parallel --timeout=180`: pass after the condition scaffold cleanup,
  `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 14:39:15 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and recent read-only journal
  checks had no entries. The service was not restarted; this slice stayed
  read-only and preserved the `8023` port expectation.
- `git diff --check`: pass after normalizing deleted single-engine
  block-connection names to reducer/stage language across production C/H files
  and architecture docs.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Production deleted-engine terminology search returned no matches across
  `app/`, `lib/`, `config/`, and `tools/` C/H files outside tests/views.
- Current reducer docs search returned no matches for deleted engine names in
  `docs/` or `BOOT_INVARIANTS.md`.
- Production stale shadow/cutover terminology search remains clean:
  `rg -n "shadow|cutover|projection-diff|projection_diff" app lib/storage lib/validation tools/mcp --glob '*.[ch]' --glob '!lib/test/**' --glob '!app/views/**'`.
- `make -j$(nproc)`: pass after the production comment/diagnostic cleanup and
  new lint-gate assertions.
- `make lint`: pass after the docs update; all architecture/baseline gates
  report zero grandfathered entries.
- `make test_parallel`: pass after rebuilding the parallel runner with the new
  `test_make_lint_gates` assertions.
- Focused filtered test passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`, including
  the new production-source guard for deleted engine names.
- `build/bin/test_parallel --timeout=180`: pass after the deleted-engine terminology
  cleanup, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 14:23:39 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and recent read-only journal
  checks had no entries. The service was not restarted; this slice stayed
  read-only and preserved the `8023` port expectation.
- `git diff --check`: pass after centralizing block-index persistence snapshot
  construction in `process_block_index.c`.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Production stale terminology searches returned no matches:
  `rg -n "shadow|cutover|projection-diff|projection_diff" app lib/storage lib/validation tools/mcp --glob '*.[ch]' --glob '!lib/test/**' --glob '!app/views/**'`
  and the deleted-engine wording search across production C/H files.
- All tracked lint baselines/allowlists remain empty:
  `find tools -type f \( -name '*baseline*.txt' -o -name '*allowlist*.txt' \)`
  reported 0 non-comment entries for every tracked file.
- `make -j$(nproc)`: pass after adding
  `block_index_snapshot_for_persist()` and routing invalidate/revalidate
  status persistence through it.
- `make lint`: pass after the block-index snapshot cleanup; E1, E2, E6,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates all report zero grandfathered entries.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=process_block_revalidate --timeout=120 --verbose`,
  `build/bin/test_parallel --only=invalidateblock --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `make test_parallel`: pass after rebuilding the parallel runner.
- `build/bin/test_parallel --timeout=180`: pass after the block-index snapshot cleanup,
  `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 14:02:48 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and recent read-only journal
  checks had no entries. The service was not restarted; this slice stayed
  read-only and preserved the `8023` port expectation.
- `git diff --check`: pass after splitting recovered-UTXO injection out of
  `process_block_self_heal.c`.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Production stale terminology search:
  `rg -n "shadow|cutover|projection-diff|projection_diff" app lib/storage lib/validation tools/mcp --glob '*.[ch]' --glob '!lib/test/**' --glob '!app/views/**'`
  returned no matches.
- All tracked lint baselines/allowlists remain empty:
  `find tools -type f \( -name '*baseline*.txt' -o -name '*allowlist*.txt' \)`
  reported 0 non-comment entries for every tracked file.
- `make -j$(nproc)`: pass after adding
  `lib/validation/src/process_block_self_heal_inject.c`.
- `make lint`: pass after the recovered-UTXO injection split; E1, E2, E6,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates all report zero grandfathered entries.
- `make test_parallel`: pass after rebuilding the parallel runner with the
  updated process-block split guard.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=self_heal_scan_fallback --timeout=120 --verbose`,
  `build/bin/test_parallel --only=connect_tip_hot_loop_exit --timeout=120 --verbose`,
  `build/bin/test_parallel --only=utxo_activation_paused --timeout=120 --verbose`,
  and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `build/bin/test_parallel --timeout=180`: pass after the recovered-UTXO injection
  split, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 13:50:18 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and recent read-only journal
  checks had no entries. The service was not restarted; this slice stayed
  read-only and preserved the `8023` port expectation.
- `git diff --check`: pass after splitting self-heal scan state and hot-loop
  policy out of `process_block_self_heal.c`.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Production stale terminology search:
  `rg -n "shadow|cutover|projection-diff|projection_diff" app lib/storage lib/validation tools/mcp --glob '*.[ch]' --glob '!lib/test/**' --glob '!app/views/**'`
  returned no matches.
- All tracked lint baselines/allowlists remain empty:
  `find tools -type f \( -name '*baseline*.txt' -o -name '*allowlist*.txt' \)`
  reported 0 non-comment entries for every tracked file.
- `make -j$(nproc)`: pass after adding
  `lib/validation/src/process_block_self_heal_scan_state.c` and
  `lib/validation/src/process_block_self_heal_hot_loop.c`.
- `make lint`: pass after the self-heal scan-state / hot-loop split; E1, E2,
  E6, supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates all report zero grandfathered entries.
- `make test_parallel`: pass after rebuilding the parallel runner with the
  updated process-block split guard.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=self_heal_scan_fallback --timeout=120 --verbose`,
  `build/bin/test_parallel --only=connect_tip_hot_loop_exit --timeout=120 --verbose`,
  `build/bin/test_parallel --only=utxo_activation_paused --timeout=120 --verbose`,
  and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `build/bin/test_parallel --timeout=180`: pass after the self-heal scan-state /
  hot-loop split, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 13:41:39 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and recent read-only journal
  checks had no entries. The service was not restarted; this slice stayed
  read-only and preserved the `8023` port expectation.
- `git diff --check`: pass after splitting SQLite tx-index recovery out of
  `process_block_self_heal.c`.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Production stale terminology search:
  `rg -n "shadow|cutover|projection-diff|projection_diff" app lib/storage lib/validation tools/mcp --glob '*.[ch]' --glob '!lib/test/**' --glob '!app/views/**'`
  returned no matches.
- All tracked lint baselines/allowlists remain empty:
  `find tools -type f \( -name '*baseline*.txt' -o -name '*allowlist*.txt' \)`
  reported 0 non-comment entries for every tracked file.
- `make -j$(nproc)`: pass after adding
  `lib/validation/src/process_block_self_heal_sqlite_tx_index.c`.
- `make test_parallel`: pass after rebuilding the parallel runner with the
  updated process-block split guard.
- `make lint`: pass after the self-heal SQLite tx-index split; E1, E2, E6,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates all report zero grandfathered entries.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=self_heal_scan_fallback --timeout=120 --verbose`
  and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `build/bin/test_parallel --timeout=180`: pass after the self-heal SQLite tx-index
  split, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 13:31:29 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and recent read-only journal
  checks had no entries. The service was not restarted; this slice stayed
  read-only and preserved the `8023` port expectation.
- `git diff --check`: pass after splitting bounded chain-scan recovery out
  of `process_block_self_heal.c`.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Production stale terminology search:
  `rg -n "shadow|cutover|projection-diff|projection_diff" app lib/storage lib/validation tools/mcp --glob '*.[ch]' --glob '!lib/test/**' --glob '!app/views/**'`
  returned no matches.
- All tracked lint baselines/allowlists remain empty:
  `find tools -type f \( -name '*baseline*.txt' -o -name '*allowlist*.txt' \)`
  reported 0 non-comment entries for every tracked file.
- `make -j$(nproc)`: pass after adding
  `lib/validation/src/process_block_self_heal_chain_scan.c`.
- `make lint`: pass after the self-heal chain-scan split; E1, E2, E6,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates all report zero grandfathered entries.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=self_heal_scan_fallback --timeout=120 --verbose`
  and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `build/bin/test_parallel --timeout=180`: pass after the self-heal chain-scan split,
  `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 13:22:26 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and recent read-only journal
  checks had no entries. The service was not restarted; this slice stayed
  read-only and preserved the `8023` port expectation.
- `git diff --check`: pass after splitting legacy zclassicd RPC recovery out
  of `process_block_self_heal.c`.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Production stale terminology search:
  `rg -n "shadow|cutover|projection-diff|projection_diff" app lib/storage lib/validation tools/mcp --glob '*.[ch]' --glob '!lib/test/**' --glob '!app/views/**'`
  returned no matches.
- All tracked lint baselines/allowlists remain empty:
  `find tools -type f \( -name '*baseline*.txt' -o -name '*allowlist*.txt' \)`
  reported 0 non-comment entries for every tracked file.
- `make -j$(nproc)`: pass after adding
  `lib/validation/src/process_block_self_heal_legacy_rpc.c`.
- `make lint`: pass after the self-heal legacy-RPC split; E1, E2, E6,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates all report zero grandfathered entries.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=self_heal_scan_fallback --timeout=120 --verbose`
  and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `build/bin/test_parallel --timeout=180`: pass after the self-heal legacy-RPC split,
  `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 13:14:17 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and recent read-only journal
  checks had no entries. The service was not restarted; this slice stayed
  read-only and preserved the `8023` port expectation.
- `git diff --check`: pass after splitting contextual-header skip policy out
  of `process_block_core.c`.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Production stale terminology search:
  `rg -n "shadow|cutover|projection-diff|projection_diff" app lib/storage lib/validation tools/mcp --glob '*.[ch]' --glob '!lib/test/**' --glob '!app/views/**'`
  returned no matches.
- All tracked lint baselines/allowlists remain empty:
  `find tools -type f \( -name '*baseline*.txt' -o -name '*allowlist*.txt' \)`
  reported 0 non-comment entries for every tracked file.
- `make lint`: pass after the contextual-header split; E1, E2, E6,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates all report zero grandfathered entries.
- `make -j$(nproc)`: pass after adding
  `lib/validation/src/process_block_contextual_header.c`.
- `build/bin/test_parallel --timeout=180`: pass after the contextual-header split,
  `0/279` groups failed in 56.0s. This includes the
  `skip_contextual:*` chain tests and the process-block split guard in
  `test_make_lint_gates`.
- Quick live sample attempt at 2026-06-01 13:06:19 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and recent read-only journal
  checks had no entries. The service was not restarted; this slice stayed
  read-only and preserved the `8023` port expectation.
- `git diff --check`: pass after emptying the E6 baseline.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- `tools/scripts/check_one_write_path.sh`: pass with 0 grandfathered write
  surfaces and no new violations.
- All tracked lint baselines/allowlists are empty:
  `find tools -type f \( -name '*baseline*.txt' -o -name '*allowlist*.txt' \)`
  reported 0 non-comment entries for every tracked file.
- `make -j$(nproc)`: pass after emptying the E6 baseline and tagging the
  canonical writer surfaces.
- `make lint`: pass after emptying the E6 baseline; E1, E2, E6, supervisor,
  E7, typed-blocker, raw-sqlite-step, controller raw-SQL, lib-layering, and
  raw-malloc gates all report zero grandfathered entries.
- `build/bin/test_parallel --timeout=180`: pass after emptying the E6 baseline,
  `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 12:56:06 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and recent read-only journal
  checks had no entries. The service was not restarted; this slice stayed
  read-only and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after making the cache flush helper test-only.
- `git diff --check`: pass.
- `tools/scripts/check_one_write_path.sh`: pass with 3 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=coins --timeout=120 --verbose`,
  `build/bin/test_parallel --only=chain_stall_repro --timeout=120 --verbose`,
  `build/bin/test_parallel --only=consensus_compat --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `make lint`: pass after the test-only cache flush shrink; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates remain at zero active debt, while E6 is
  3 grandfathered write surfaces.
- `build/bin/test_parallel --timeout=180`: pass after the test-only cache flush shrink,
  `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 12:49:34 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and recent read-only journal
  checks had no entries. The service was not restarted; this slice stayed
  read-only and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after retiring runtime `reindexchainstate` replay
  and removing the `repairutxos` coins-cache flushes.
- `git diff --check`: pass.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- `tools/scripts/check_lib_layering.sh`: pass with 0 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 5 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `ZCL_TEST_ONLY=rpc_safety build/bin/test_zcl`,
  `build/bin/test_parallel --only=rpc --timeout=120 --verbose`,
  `build/bin/test_parallel --only=chain --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `make lint`: pass after the runtime reindex/repair flush shrink; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates remain at zero active debt, while E6 is
  5 grandfathered write surfaces.
- `build/bin/test_parallel --timeout=180`: pass after the runtime
  reindex/repair-flush shrink, `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 12:41:35 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and recent read-only journal
  checks had no entries. The service was not restarted; this slice stayed
  read-only and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after routing shutdown UTXO flushes through the
  SQLite coins writer.
- `git diff --check`: pass.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- `tools/scripts/check_lib_layering.sh`: pass with 0 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 10 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=boot --timeout=120 --verbose`,
  `build/bin/test_parallel --only=shutdown --timeout=120 --verbose`,
  `build/bin/test_parallel --only=coins --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `make lint`: pass after the shutdown flush routing; E1, E2, supervisor, E7,
  typed-blocker, raw-sqlite-step, controller raw-SQL, lib-layering, and
  raw-malloc gates remain at zero active debt, while E6 is 10 grandfathered
  write surfaces.
- `build/bin/test_parallel --timeout=180`: pass after the shutdown flush routing,
  `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 12:28:33 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and the recent read-only
  journal checks had no entries. The service was not restarted; this slice
  stayed read-only and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after routing boot reindex UTXO flushes through the
  SQLite coins writer.
- `git diff --check`: pass.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- `tools/scripts/check_lib_layering.sh`: pass with 0 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 13 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=boot --timeout=120 --verbose`,
  `build/bin/test_parallel --only=block_index --timeout=120 --verbose`,
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=validation --timeout=120 --verbose`,
  `build/bin/test_parallel --only=coins --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=chain --timeout=120 --verbose`.
- `make lint`: pass after the boot reindex flush routing; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates remain at zero active debt, while E6 is
  13 grandfathered write surfaces.
- `build/bin/test_parallel --timeout=180`: pass after the boot reindex flush routing,
  `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 12:20:19 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and the recent read-only
  journal checks had no entries. The service was not restarted; this slice
  stayed read-only and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after deleting the `active_chain_set_tip()`
  compatibility alias and making the process-block `coins_view_cache_flush()`
  fallback test-only.
- `git diff --check`: pass.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- `tools/scripts/check_lib_layering.sh`: pass with 0 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 16 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=chain --timeout=120 --verbose`,
  `build/bin/test_parallel --only=validation --timeout=120 --verbose`,
  `build/bin/test_parallel --only=tip_finalize --timeout=120 --verbose`,
  `build/bin/test_parallel --only=header_admit --timeout=120 --verbose`,
  `build/bin/test_parallel --only=utxo_activation --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=reducer_stage --timeout=120 --verbose`.
- `make lint`: pass after the active-chain alias deletion and process-block
  fallback tightening; E1, E2, supervisor, E7, typed-blocker,
  raw-sqlite-step, controller raw-SQL, lib-layering, and raw-malloc gates
  remain at zero active debt, while E6 is 16 grandfathered write surfaces.
- `build/bin/test_parallel --timeout=180`: pass after the active-chain alias deletion
  and process-block fallback tightening, `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 12:11:24 UTC after this slice did
  not prove live-node health: no `zclassic23` process was running, `zcl-rpc`
  exited 7 for both `getblockcount` and `gettxoutsetinfo`, `ss` showed no
  `8023`, `8033`, `18232`, or `8232` listener, `systemctl --user status
  zclassic23` could not connect to the user bus, and the recent read-only
  journal checks had no entries. The service was not restarted; this slice
  stayed read-only and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after deleting the redundant
  `coins_view_sqlite_batch_write()` wrapper and routing callers through
  `coins_view_sqlite_batch_write_ex(..., NULL)`.
- `git diff --check`: pass.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- `tools/scripts/check_lib_layering.sh`: pass with 0 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 17 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=coins --timeout=120 --verbose` and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `make lint`: pass after the coins SQLite wrapper deletion; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates remain at zero active debt, while E6 is
  17 grandfathered write surfaces.
- `build/bin/test_parallel --timeout=180`: pass after the coins SQLite wrapper
  deletion, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 12:02:56 UTC after the coins SQLite
  wrapper deletion did not prove live-node health: no `zclassic23` process was
  running, `zcl-rpc` exited 7 for both `getblockcount` and
  `gettxoutsetinfo`, `ss` showed no `8023`, `8033`, `18232`, or `8232`
  listener, `systemctl --user status zclassic23` could not connect to the
  user bus, and the recent read-only journal checks had no entries. The
  service was not restarted; this slice stayed read-only and preserved the
  `8023` port expectation.
- `make -j$(nproc)`: pass after moving active-tip child discovery and disk
  verification into `process_block_tip_child.c`.
- `make -j$(nproc) test_parallel`: pass after rebuilding the parallel runner
  for the new process-block tip-child boundary assertions.
- `git diff --check`: pass.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- `tools/scripts/check_lib_layering.sh`: pass with 0 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 21 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=chain --timeout=120 --verbose`,
  `build/bin/test_parallel --only=torn_index --timeout=120 --verbose`,
  `build/bin/test_parallel --only=validation --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=block_scan --timeout=120 --verbose`.
- `make lint`: pass after the active-tip child split; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL,
  lib-layering, and raw-malloc gates remain at zero active debt, while E6 is
  21 grandfathered write surfaces.
- `build/bin/test_parallel --timeout=180`: pass after rebuilding `test_parallel`,
  `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 11:50:21 UTC after the
  active-tip child split did not prove live-node health: no `zclassic23`
  process was running, `zcl-rpc` exited 7 for both `getblockcount` and
  `gettxoutsetinfo`, `ss` showed no `8023`, `8033`, or `18232` listener,
  `systemctl --user status zclassic23` could not connect to the user bus,
  and the recent read-only journal checks had no entries. The service was not
  restarted; this slice stayed read-only for live checks and preserved the
  `8023` port expectation.
- `make -j$(nproc)`: pass after callback-injecting header activation,
  block-file scan, height-repair state, and post-activation anchor repair
  through the message processor.
- `tools/scripts/check_lib_layering.sh`: pass with 12 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations; only existing `boot_services.c`
  `coins_view_cache_flush()` baseline line numbers shifted after adding the
  boot-owned callbacks.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=header_sync --timeout=120 --verbose`,
  `build/bin/test_parallel --only=net --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=block_index_integrity --timeout=120 --verbose`.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- `make lint`: pass after the header activation/index callback injection; E1,
  E2, supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 12 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the header activation/index
  callback injection, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 09:49:59 UTC after the header
  activation/index callback injection did not prove live-node health: no
  `zclassic23` process was running, `zcl-rpc` exited 7 for both
  `getblockcount` and `gettxoutsetinfo`, `ss` showed no `8023`, `8033`, or
  `18232` listener, the previous 20 minutes of `zclassic23.service` journal
  had no entries, and the broader read-only journal scan still shows the
  earlier OOM kill at 2026-06-01 07:58:58 UTC. The service was not restarted;
  this slice stayed read-only for live checks and preserved the `8023` port
  expectation.
- `make -j$(nproc)`: pass after callback-injecting header-sync snapshot
  active/anchor access through the message processor.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  header snapshot callback lint-gate assertions.
- `tools/scripts/check_lib_layering.sh`: pass with 14 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations; only existing `boot_services.c`
  `coins_view_cache_flush()` baseline line numbers shifted after adding the
  boot-owned callbacks.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=header_sync --timeout=120 --verbose`,
  `build/bin/test_parallel --only=net --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=snapshot_sync_service --timeout=120 --verbose`.
- `make lint`: pass after the header snapshot callback injection; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 14 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the header snapshot callback
  injection, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 09:38:28 UTC after the header
  snapshot callback injection did not prove live-node health: no `zclassic23`
  process was running, `zcl-rpc` exited 7 for both `getblockcount` and
  `gettxoutsetinfo`, `ss` showed no `8023`, `8033`, or `18232` listener, the
  previous 20 minutes of `zclassic23.service` journal had no entries, and the
  broader read-only journal scan still shows the earlier OOM kill at
  2026-06-01 07:58:58 UTC. The service was not restarted; this slice stayed
  read-only for live checks and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after callback-injecting fast-sync snapshot
  serialization from boot.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  expanded fast-sync layering lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 15 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations; only existing `boot_services.c` baseline
  line numbers shifted after adding the boot-owned serializer callback.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=fast_sync --timeout=120 --verbose`,
  `build/bin/test_parallel --only=snapshot_sync_service --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=net --timeout=120 --verbose`.
- `make lint`: pass after the fast-sync serializer callback injection; E1,
  E2, supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 15 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the fast-sync serializer
  callback injection, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 09:27:00 UTC after the fast-sync
  serializer callback injection did not prove live-node health: no
  `zclassic23` process was running, `zcl-rpc` exited 7 for both
  `getblockcount` and `gettxoutsetinfo`, `ss` showed no `8023`, `8033`, or
  `18232` listener, the previous 20 minutes of `zclassic23.service` journal
  had no entries, and the broader read-only journal scan still shows the
  earlier OOM kill at 2026-06-01 07:58:58 UTC. The service was not restarted;
  this slice stayed read-only for live checks and preserved the `8023` port
  expectation.
- `make -j$(nproc)`: pass after removing fast-sync's direct ActiveRecord and
  DB model includes.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  fast-sync SQLite helper lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 16 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=fast_sync --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=net --timeout=120 --verbose`.
- `make lint`: pass after the fast-sync AR/DB include removal; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 16 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the fast-sync AR/DB include
  removal, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 09:18:00 UTC after the fast-sync
  AR/DB include removal did not prove live-node health: no `zclassic23`
  process was running, `zcl-rpc` exited 7 for both `getblockcount` and
  `gettxoutsetinfo`, `ss` showed no `8023`, `8033`, or `18232` listener, the
  previous 20 minutes of `zclassic23.service` journal had no entries, and the
  broader read-only journal scan still shows the earlier OOM kill at
  2026-06-01 07:58:58 UTC. The service was not restarted; this slice stayed
  read-only for live checks and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after routing process-block self-heal tx-index
  lookup through the runtime boundary.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  expanded self-heal runtime-boundary lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 18 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=self_heal --timeout=120 --verbose`,
  `build/bin/test_parallel --only=validation --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=chain --timeout=120 --verbose`.
- `make lint`: pass after the self-heal tx-index/runtime boundary move; E1,
  E2, supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 18 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the self-heal tx-index/runtime
  boundary move, `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 09:07:58 UTC after the
  self-heal tx-index/runtime boundary move did not prove live-node health: no
  `zclassic23` process was running, `zcl-rpc` returned connection failure for
  both `getblockcount` and `gettxoutsetinfo`, `ss` showed no `8023`, `8033`,
  or `18232` listener, and a read-only journal scan still shows the earlier
  `zclassic23.service` OOM kill at 2026-06-01 07:58:58 UTC. The service was
  not restarted; this slice stayed read-only for live checks and preserved the
  `8023` port expectation.
- `make -j$(nproc)`: pass after routing process-block self-heal durable UTXO
  max-height reads through the runtime boundary.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  expanded process-block runtime-boundary lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 19 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=self_heal --timeout=120 --verbose`,
  `build/bin/test_parallel --only=validation --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=chain --timeout=120 --verbose`.
- `make lint`: pass after the self-heal/runtime boundary move; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 19 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the self-heal/runtime boundary
  move, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 08:58:37 UTC after the
  self-heal/runtime boundary move did not prove live-node health: no
  `zclassic23` process was running, `zcl-rpc` returned connection failure for
  both `getblockcount` and `gettxoutsetinfo`, `ss` showed no `8023`, `8033`,
  or `18232` listener, and a read-only journal scan still shows the earlier
  `zclassic23.service` OOM kill at 2026-06-01 07:58:58 UTC. The service was
  not restarted; this slice stayed read-only for live checks and preserved the
  `8023` port expectation.
- `make -j$(nproc)`: pass after routing process-block flush-policy DB state,
  sync-batch, and WAL checkpoint operations through the runtime boundary.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  expanded process-block runtime-boundary lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 20 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations; only existing `process_block_flush_policy.c`
  baseline line numbers shifted after removing the DB model include.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=validation --timeout=120 --verbose`,
  `build/bin/test_parallel --only=wallet_flush_rollback --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=chain --timeout=120 --verbose`.
- `make lint`: pass after the flush-policy/runtime boundary move; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 20 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the flush-policy/runtime
  boundary move, `0/279` groups failed in 57.0s.
- Quick live sample attempt at 2026-06-01 08:47:50 UTC after the
  flush-policy/runtime boundary move did not prove live-node health: no
  `zclassic23` process was running, `zcl-rpc` returned connection failure for
  both `getblockcount` and `gettxoutsetinfo`, `ss` showed no `zclassic23`,
  `8023`, `8033`, or `18232` listener, and a read-only journal scan still
  shows the earlier `zclassic23.service` OOM kill at 2026-06-01 07:58:58 UTC.
  The service was not restarted; this slice stayed read-only for live checks
  and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after routing process-block `node_db` open checks
  through the runtime boundary.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  process-block/runtime lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 21 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- `tools/scripts/check_doc_accuracy.sh`: pass with docs and Makefile agreeing
  on all 31 lint gates.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=validation --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=chain --timeout=120 --verbose`.
- `make lint`: pass after the process-block/runtime boundary move; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 21 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the process-block/runtime
  boundary move, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 08:39:09 UTC after the
  process-block/runtime boundary move did not prove live-node health: no
  `zclassic23` process was running, `zcl-rpc` returned connection failure for
  both `getblockcount` and `gettxoutsetinfo`, `ss` showed no `zclassic23`,
  `8023`, `8033`, or `18232` listener, and a read-only journal scan still
  shows the earlier `zclassic23.service` OOM kill at 2026-06-01 07:58:58 UTC.
  The service was not restarted; this slice stayed read-only for live checks
  and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after callback-injecting quorum-oracle peer header
  votes from boot.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  header-vote callback lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 22 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations; only existing `boot_services.c` baseline line
  numbers shifted with the new boot-owned callback.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=header_sync --timeout=120 --verbose`,
  `build/bin/test_parallel --only=net --timeout=120 --verbose`,
  `build/bin/test_parallel --only=sync_service --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=msg_handlers --timeout=120 --verbose`.
- `make lint`: pass after the header-vote callback injection; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 22 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the header-vote callback
  injection, `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 08:29:36 UTC after the header-vote
  callback injection did not prove live-node health: no `zclassic23` process
  was running, `zcl-rpc` returned connection failure, `ss` showed no
  `zclassic23`, `8023`, `8033`, or `18232` listener, and a read-only journal
  scan still shows the earlier `zclassic23.service` OOM kill at
  2026-06-01 07:58:58 UTC. The service was not restarted; this slice stayed
  read-only for live checks and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after isolating the `msg_headers.c` chain-tip
  test fallback from the app service header.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 23 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=header_sync --timeout=120 --verbose`,
  `build/bin/test_parallel --only=net --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=sync_service --timeout=120 --verbose`.
- `make lint`: pass after the chain-tip fallback isolation; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 23 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the chain-tip fallback
  isolation, `0/279` groups failed in 61.1s.
- Quick live sample attempt at 2026-06-01 08:20:54 UTC after the chain-tip
  fallback isolation did not prove live-node health: no `zclassic23` process
  was running, `zcl-rpc` returned connection failure, `ss` showed no
  `zclassic23`, `8023`, `8033`, or `18232` listener, and a read-only journal
  sample still reported `zclassic23.service` was OOM-killed at
  2026-06-01 07:58:58 UTC. The service was not restarted; this slice stayed
  read-only for live checks and preserved the `8023` port expectation.
- `make -j$(nproc)`: pass after moving the sync planner API contract into
  `lib/sync/include/sync/sync_planner.h`.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  lint-gate ownership assertions.
- `tools/scripts/check_lib_layering.sh`: pass with 24 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=header_sync --timeout=120 --verbose`,
  `build/bin/test_parallel --only=sync_service --timeout=120 --verbose`,
  `build/bin/test_parallel --only=net --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=integrity --timeout=120 --verbose`.
- `make lint`: pass after the sync planner contract move; E1, E2, supervisor,
  E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 24 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the sync planner contract move,
  `0/279` groups failed in 56.0s.
- Quick live sample attempt at 2026-06-01 08:10:13 UTC after the sync planner
  contract move did not prove live-node health: `systemctl --user is-active
  zclassic23` could not connect to the user bus, `zcl-rpc` returned connection
  failure, `ss` showed no `zclassic23`, `8023`, `8033`, or `18232` listener,
  and a read-only journal sample reported `zclassic23.service` was OOM-killed
  at 2026-06-01 07:58:58 UTC. The service was not restarted because this slice
  was constrained to read-only live checks.
- `make -j$(nproc)`: pass after moving snapshot-sync accessors into
  `lib/net/src/msgprocessor_snapshot.c`.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  lint-gate ownership assertions.
- `tools/scripts/check_lib_layering.sh`: pass with 28 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=net --timeout=120 --verbose`,
  `build/bin/test_parallel --only=snapshot_sync_service --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=msg_handlers --timeout=120 --verbose`.
- `make lint`: pass after the snapshot-sync accessor move; E1, E2, supervisor,
  E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 28 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the snapshot-sync accessor move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 07:54:10 UTC after the snapshot-sync accessor
  move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting ZMSG, file-offer, and
  file-service P2P persistence from boot.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  P2P app-persistence callback lint-gate assertions.
- `tools/scripts/check_lib_layering.sh`: pass with 29 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations after updating the same three
  `boot_services.c` baseline line numbers shifted by the boot-owned callback.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=net --timeout=120 --verbose`,
  `build/bin/test_parallel --only=file_market --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=models --timeout=120 --verbose`.
- `make lint`: pass after the P2P app-persistence callback move; E1, E2, supervisor,
  E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 29 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the P2P app-persistence callback move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 07:44:34 UTC after the P2P app-persistence callback
  move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after moving block-sync planning for invalid-block
  retries and valid-block acceptance behind net-internal message-processor
  helpers, and removing the final app-service include from
  `lib/net/src/msg_blocks.c`.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  expanded lint-gate assertions.
- `tools/scripts/check_lib_layering.sh`: pass with 39 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=msg_handlers --timeout=120 --verbose`,
  `build/bin/test_parallel --only=net --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=sync_service --timeout=120 --verbose` (2 matched
  groups).
- `make lint`: pass after the block-sync planning helper move; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 39 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the block-sync planning helper
  move, `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 07:06:19 UTC after the block-sync planning
  helper move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting the block-connected
  observer from boot and removing the sync monitor service include from
  `lib/net/src/msg_blocks.c`.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  expanded lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 40 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations after updating the same three
  `boot_services.c` baseline line numbers shifted by the boot-owned callback.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=msg_handlers --timeout=120 --verbose`,
  `build/bin/test_parallel --only=net --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=chain_activation_controller --timeout=120 --verbose`.
- `make lint`: pass after the block-connected observer callback move; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 40 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the block-connected observer
  callback move, `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 06:54:50 UTC after the block-connected
  observer callback move: `systemctl --user is-active zclassic23` reported
  `active`, `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting P2P block reducer
  submission from boot and removing app controller/model/activation/snapshot
  includes from `lib/net/src/msg_blocks.c`.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 41 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations after updating the same three
  `boot_services.c` baseline line numbers shifted by the boot-owned callback.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=msg_handlers --timeout=120 --verbose`,
  `build/bin/test_parallel --only=net --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=chain_activation_controller --timeout=120 --verbose`.
- `make lint`: pass after the P2P block callback move; E1, E2, supervisor,
  E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 41 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the P2P block callback move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 06:43:25 UTC after the P2P block callback
  move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting tx wallet persistence and
  the snapshot-active query from boot, removing all app controller/model/service
  includes from `lib/net/src/msg_tx.c`.
- `make test_parallel`: pass after rebuilding the parallel runner for the new
  lint-gate assertion.
- `tools/scripts/check_lib_layering.sh`: pass with 46 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations after updating the same three
  `boot_services.c` baseline line numbers shifted by the boot-owned callbacks.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=msg_handlers --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=net --timeout=120 --verbose`.
- `make lint`: pass after the tx callback move; E1, E2, supervisor, E7,
  typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 46 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the tx callback move,
  `0/279` groups failed in 85.1s.
- Quick live sample at 2026-06-01 06:32:22 UTC after the tx callback move:
  `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting metrics service/model
  gauges and connman known-ZCL23 peer selection from boot.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  metrics/connman callback boundary and lint-gate assertion update.
- `tools/scripts/check_lib_layering.sh`: pass with 50 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations after updating three `boot_services.c`
  baseline line numbers shifted by the boot-owned callbacks.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`,
  `build/bin/test_parallel --only=net --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=mcp_metrics --timeout=120 --verbose`.
- `make lint`: pass after the metrics/connman callback move; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 50 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the metrics/connman callback
  move, `0/279` groups failed in 87.1s.
- Quick live sample at 2026-06-01 06:19:03 UTC after the metrics/connman
  callback move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting handshake peer persistence
  from boot, removing `msg_version.c`'s Peer model/database includes, and
  logging the boot-owned peer-save false-return path.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  peer-save callback move and the lint-gate assertion update.
- `tools/scripts/check_lib_layering.sh`: pass with 55 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations after updating three `boot_services.c`
  baseline line numbers shifted by the boot-owned peer-save helper.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=msg_handlers --timeout=120 --verbose`,
  `build/bin/test_parallel --only=net --timeout=120 --verbose`,
  `build/bin/test_parallel --only=peer_lifecycle --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `make lint`: pass after the peer-save callback move; E1, E2, supervisor,
  E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 55 grandfathered includes.
- `build/bin/test_parallel --timeout=180`: pass after the peer-save callback move,
  `0/279` groups failed in 78.1s.
- Quick live sample at 2026-06-01 06:05:55 UTC after the peer-save callback
  move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting compact-block reducer
  submission from boot and removing `msg_compact.c`'s activation-service
  include.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  compact-block callback move.
- `make lint`: pass after the compact-block callback move; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 57 grandfathered includes.
- `tools/scripts/check_lib_layering.sh`: pass with 57 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations after updating three `boot_services.c`
  baseline line numbers shifted by the callback helper.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=compact_blocks --timeout=120 --verbose`,
  `build/bin/test_parallel --only=msg_handlers --timeout=120 --verbose`,
  `build/bin/test_parallel --only=net --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `build/bin/test_parallel --timeout=180`: pass after the compact-block callback move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 05:49:27 UTC after the compact-block
  callback move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after removing the test-only small-projection
  comparison helpers from the production storage API.
- `make test_parallel`: pass after rebuilding the parallel runner for the
  local projection-table comparison test move.
- `make lint`: pass after the small-projection production-surface cleanup; E1,
  E2, supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 58 grandfathered includes.
- `build/bin/test_parallel --only=small_projections --timeout=120 --verbose`: pass
  with the parity checks comparing projection SQLite files directly against
  the legacy fixture DB.
- `build/bin/test_parallel --timeout=180`: pass after the small-projection production
  API cleanup, `0/279` groups failed in 77.1s.
- Production stale-surface search:
  `rg -n "shadow|cutover|projection-diff|projection_diff" app lib/storage lib/validation tools/mcp --glob '*.[ch]'`
  now returns only generated wallet CSS `box-shadow` matches under
  `app/views`, not reducer/cutover/shadow code.
- Quick live sample at 2026-06-01 05:34:31 UTC after the small-projection
  cleanup: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1357526`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting onion-service app handlers
  from boot, removing `onion_service.c`'s blog-controller include, and adding
  direct `/blog` route coverage for the injected handler. `make test_parallel`
  was then run explicitly to rebuild the parallel runner.
- `make lint`: pass after the onion-service callback move; E1, E2,
  supervisor, E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and
  raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, and lib-layering is 58 grandfathered includes.
- `tools/scripts/check_lib_layering.sh`: pass with 58 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=net --timeout=120 --verbose`,
  `build/bin/test_parallel --only=blog --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `build/bin/test_parallel --timeout=180`: pass after the onion-service callback move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 05:23:49 UTC after the onion-service
  callback move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1354429`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023` with no `8033` listener in the sample, and a
  journal scan over the previous 10 minutes found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, corrupt-state, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after callback-injecting connman onion peer discovery
  from boot and removing `connman.c`'s blog-controller include.
- `make lint`: pass after the connman callback move; E1, E2, supervisor, E7,
  typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 59 grandfathered includes.
- `tools/scripts/check_lib_layering.sh`: pass with 59 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=net --timeout=120 --verbose`,
  `build/bin/test_parallel --only=blog --timeout=120 --verbose`,
  `build/bin/test_parallel --only=connman_addnode_fallback --timeout=120 --verbose`,
  and `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `build/bin/test_parallel --timeout=180`: pass after the connman callback move,
  `0/279` groups failed in 58.0s.
- Quick live sample at 2026-06-01 05:09:42 UTC after the connman callback move:
  `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1354429`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023`, and a journal scan over the previous 10
  minutes found no low-tip regression, integrity failure, OOM, fatal,
  segfault, assert, panic, corrupt-state, or `DB_ERR_TIP_MISMATCH` signal.
  This is a continuity check, not the final soak.
- `make -j$(nproc)`: pass after moving mined-block submission behind a
  caller-owned `gen_context` callback and removing the mining library's app
  activation-service include.
- `make lint`: pass after the mining callback move; E1, E2, supervisor, E7,
  typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 60 grandfathered includes.
- `tools/scripts/check_lib_layering.sh`: pass with 60 grandfathered
  lib-to-app includes and no new violations.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces and no new violations.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=mining --timeout=120 --verbose` and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `build/bin/test_parallel --timeout=180`: pass after the mining callback move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 04:59:05 UTC after the mining callback move:
  `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1354429`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023`, and a journal scan over the previous 10
  minutes found no low-tip regression, integrity failure, OOM, fatal,
  segfault, assert, panic, corrupt-state, or `DB_ERR_TIP_MISMATCH` signal.
  This is a continuity check, not the final soak.
- `make -j$(nproc)`: pass after moving addrman sidecar integrity to `lib/net`
  and generic SHA3 sidecar I/O to `lib/storage`.
- `tools/scripts/check_lib_layering.sh`: pass with 61 grandfathered
  lib-to-app includes and no new violations.
- `make test_parallel`: pass after rebuilding the standalone parallel-test
  runner with the moved sidecar sources.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=addrman_integrity --timeout=120 --verbose`,
  `build/bin/test_parallel --only=block_index_integrity --timeout=120 --verbose`,
  `build/bin/test_parallel --only=block_index_sidecar_port --timeout=120 --verbose`,
  and `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `build/bin/test_parallel --timeout=180`: pass after the sidecar ownership move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 04:47:38 UTC after the sidecar ownership
  move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1354429`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023`, and a journal scan over the previous 10
  minutes found no low-tip regression, integrity failure, OOM, fatal,
  segfault, assert, panic, corrupt-state, or `DB_ERR_TIP_MISMATCH` signal.
  This is a continuity check, not the final soak.
- `make -j$(nproc)`: pass after moving swap-contract persisted records and
  SQLite persistence into the SwapContract model shape.
- `make lint`: pass after the swap persistence move; E1, E2, supervisor, E7,
  typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 62 grandfathered includes.
- `make test_parallel`: pass after rebuilding the standalone parallel-test
  runner with the moved SwapContract model persistence sources.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=htlc --timeout=120 --verbose`,
  `build/bin/test_parallel --only=protocols --timeout=120 --verbose`,
  `build/bin/test_parallel --only=models --timeout=120 --verbose`,
  `build/bin/test_parallel --only=db_validators --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `build/bin/test_parallel --timeout=180`: pass after the swap persistence move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 04:38:22 UTC after the swap persistence
  move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1354429`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023`, and a journal scan over the previous 10
  minutes found no low-tip regression, integrity failure, OOM, fatal,
  segfault, assert, panic, corrupt-state, or `DB_ERR_TIP_MISMATCH` signal.
  This is a continuity check, not the final soak.
- `make -j$(nproc)`: pass after moving ZNAM at-rest records and SQLite
  persistence into the Znam model shape.
- `make lint`: pass after the ZNAM persistence move; E1, E2, supervisor, E7,
  typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 65 grandfathered includes.
- `make test_parallel`: pass after rebuilding the standalone parallel-test
  runner with the moved ZNAM model persistence sources.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=znam --timeout=120 --verbose`,
  `build/bin/test_parallel --only=protocols --timeout=120 --verbose`,
  `build/bin/test_parallel --only=models --timeout=120 --verbose`,
  `build/bin/test_parallel --only=db_validators --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `build/bin/test_parallel --timeout=180`: pass after the ZNAM persistence move,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 04:28:58 UTC after the ZNAM persistence
  move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1354429`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023`, and a journal scan since
  2026-06-01 04:22:00 UTC found no low-tip regression, integrity failure, OOM,
  fatal, segfault, assert, panic, corrupt-state, or `DB_ERR_TIP_MISMATCH`
  signal. This is a continuity check, not the final soak.
- `make -j$(nproc)`: pass after moving ZMSG SQLite persistence into the Zmsg
  model shape.
- `make lint`: pass after the ZMSG persistence move; E1, E2, supervisor, E7,
  typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 68 grandfathered includes.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=protocols --timeout=120 --verbose`,
  `build/bin/test_parallel --only=models --timeout=120 --verbose`,
  `build/bin/test_parallel --only=db_validators --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `build/bin/test_parallel --timeout=180`: pass after the ZMSG persistence move,
  `0/279` groups failed in 58.0s.
- Quick live sample at 2026-06-01 04:16:14 UTC after the ZMSG persistence
  move: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1353559`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8023` / `[::]:8023`, and a journal scan since
  2026-06-01 04:10:00 UTC found no low-tip regression, integrity failure, OOM,
  fatal, segfault, assert, panic, corrupt-state, or `DB_ERR_TIP_MISMATCH`
  signal. This is a continuity check, not the final soak.
- `make -j$(nproc)`: pass after moving schema migration and file-offer
  persistence into the Model shape.
- `make lint`: pass after moving schema migration into `app/models` and
  file-offer SQLite persistence into the FileOffer model; E1, E2, supervisor,
  E7, typed-blocker, raw-sqlite-step, controller raw-SQL, and raw-malloc gates
  remain at zero active debt, E6 is 24 grandfathered write surfaces, and
  lib-layering is 71 grandfathered includes.
- `make test_parallel`: pass after rebuilding the suite binary with the moved
  schema migration and FileOffer model persistence sources.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=schema_migration --timeout=120 --verbose`,
  `build/bin/test_parallel --only=file_market --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=models --timeout=120 --verbose`.
- `build/bin/test_parallel --timeout=180`: pass after the Model-shape persistence
  move, `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 04:04:04 UTC after the Model-shape
  persistence move: `systemctl --user is-active zclassic23` reported
  `active`, `getblockcount=3130701`,
  `gettxoutsetinfo.height=3130701`, `txouts=1353559`, RPC listened on
  `127.0.0.1:18232`, P2P listened on `0.0.0.0:8023` / `[::]:8023`, and a
  journal scan since 2026-06-01 03:58:00 UTC found no low-tip regression,
  integrity failure, OOM, fatal, segfault, assert, panic, or
  `DB_ERR_TIP_MISMATCH` signal. This is a continuity check, not the final
  soak.
- `make -j$(nproc)`: pass after moving `zcl_node_db_path()` into `lib/util`,
  moving UTXO script classification into `lib/script`, removing the
  storage-layer UTXO model include, and replacing the net internal service
  include with a forward declaration.
- `make lint`: pass; E1, E2, supervisor, E7, typed-blocker,
  raw-sqlite-step, controller raw-SQL, and raw-malloc gates remain at zero
  active debt, E6 is 24 grandfathered write surfaces, and lib-layering is 75
  grandfathered includes.
- `make test_parallel`: pass after adding direct coverage for
  `zcl_node_db_path()` and `utxo_classify_script()`.
- Focused filtered tests passed:
  `build/bin/test_parallel --only=path_check --timeout=120 --verbose`,
  `build/bin/test_parallel --only=script --timeout=120 --verbose`,
  `build/bin/test_parallel --only=coins_view --timeout=120 --verbose`,
  `build/bin/test_parallel --only=fast_sync --timeout=120 --verbose`,
  `build/bin/test_parallel --only=tor --timeout=120 --verbose`,
  `build/bin/test_parallel --only=net --timeout=120 --verbose`, and
  `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`.
- `build/bin/test_parallel --timeout=180`: pass after the lib-layering shrink,
  `0/279` groups failed in 58.0s.
- Quick live sample at 2026-06-01 03:47:30 UTC after the lib-layering shrink:
  `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1349221`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8033` / `[::]:8033`, and a journal scan since
  2026-06-01 03:31:00 UTC found no low-tip regression, integrity failure, OOM,
  fatal, segfault, assert, panic, or `DB_ERR_TIP_MISMATCH` signal. This is a
  continuity check, not the final soak.
- `make -j$(nproc)`: pass after emptying the controller raw-SQL baseline.
- `make lint`: pass; E1, E2, supervisor, E7, typed-blocker,
  raw-sqlite-step, controller raw-SQL, and raw-malloc gates remain at zero
  active debt, E6 is 24 grandfathered write surfaces, and lib-layering is 79
  grandfathered includes.
- `make test_parallel`: pass after the controller raw-SQL baseline reached
  zero.
- `ZCL_LINT_MODE=RATCHET tools/lint/check_no_raw_sqlite_in_controllers.sh`:
  pass with an empty baseline. WARN mode reports 0 direct raw controller SQL
  calls.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  the controller raw-SQL baseline reached zero.
- `build/bin/test_parallel --only=mcp_controllers --timeout=120 --verbose`: pass after
  moving the `zcl_sql` prepare path behind
  `node_db_prepare_readonly_query()`.
- `build/bin/test_parallel --only=sqlite --timeout=120 --verbose`: pass, including the
  snapshot tx-index job start/join path.
- `build/bin/test_parallel --only=models --timeout=120 --verbose`: pass, including
  tx-index bulk-load lifecycle coverage.
- `build/bin/test_parallel --timeout=180`: pass after the controller raw-SQL baseline
  reached zero, `0/279` groups failed in 59.0s.
- Quick live sample at 2026-06-01 03:31:20 UTC after the controller raw-SQL
  baseline reached zero: `systemctl --user is-active zclassic23` reported
  `active`, `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1349221`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8033` / `[::]:8033`, and a 10-minute journal scan found no low-tip
  regression, integrity failure, OOM, fatal, segfault, assert, or panic signal.
  This is a continuity check, not the final soak.
- `git diff --check`: pass after the `blockchain_controller_admin.c`
  controller raw-SQL shrink.
- `make -j$(nproc)`: pass after moving consensus snapshot export out of the
  controller layer into `consensus_snapshot_export_service_run()`.
- `make test_parallel`: pass after touching
  `lib/test/src/test_file_controller.c`.
- `make lint`: pass; E1, E2, supervisor, E7, typed-blocker, raw-sqlite-step,
  and raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, lib-layering is 79 grandfathered includes, and controller raw-SQL is
  3 grandfathered controller files.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces, no new ones, after keeping the existing boot-services
  `coins_view_cache_flush` E6 entries line-stable.
- `ZCL_LINT_MODE=RATCHET tools/lint/check_no_raw_sqlite_in_controllers.sh`:
  pass with 3 grandfathered controller files, no new ones. WARN mode now
  reports 12 direct raw controller SQL calls across those 3 files.
- `build/bin/test_parallel --only=file_controller --timeout=120 --verbose`: pass after
  moving consensus snapshot export to the service layer; `0/1` filtered groups
  failed in 2.0s.
- `build/bin/test_parallel --timeout=180`: pass after the controller raw-SQL shrink,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 03:08:22 UTC after the controller raw-SQL
  shrink: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1349221`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8033` / `[::]:8033`, and a journal scan since
  2026-06-01 02:50:00 UTC found no low-tip regression, integrity failure, OOM,
  fatal, segfault, assert, or panic signal. This is a continuity check, not the
  final soak.
- `git diff --check`: pass after the consensus snapshot export service move.
- `make test_parallel`: pass after adding
  `wallet_sqlite_delete_key_r()` and the wallet persistence roundtrip test.
- `make -j$(nproc)`: pass after moving `wallet_controller_keys.c` key
  readback/rollback SQL into `wallet_sqlite`.
- `make lint`: pass; E1, E2, supervisor, E7, typed-blocker, raw-sqlite-step,
  and raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, lib-layering is 79 grandfathered includes, and controller raw-SQL is
  5 grandfathered controller files.
- `ZCL_LINT_MODE=RATCHET tools/lint/check_no_raw_sqlite_in_controllers.sh`:
  pass with 5 grandfathered controller files, no new ones. WARN mode now
  reports 21 direct raw controller SQL calls across those 5 files.
- `build/bin/test_parallel --only=wallet_persistence_cycle --timeout=120 --verbose`:
  pass, including the new `delete_key_r` persisted-key removal case.
- `build/bin/test_parallel --only=wallet --timeout=120 --verbose`: pass after moving
  wallet-key readback/rollback SQL into `wallet_sqlite`; `0/32` filtered groups
  failed in 7.0s.
- `build/bin/test_parallel --timeout=180`: pass after the wallet/controller raw-SQL
  shrink, `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 02:50:33 UTC after the wallet/controller
  raw-SQL shrink: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1349221`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8033` / `[::]:8033`, and a journal scan since
  2026-06-01 02:50:00 UTC found no low-tip regression, integrity failure, OOM,
  fatal, segfault, assert, or panic signal. This is a continuity check, not the
  final soak.
- `make -j$(nproc)`: pass after moving `sync_controller_import.c` UTXO
  cardinality validation into the UTXO model.
- `make lint`: pass; E1, E2, supervisor, E7, typed-blocker, raw-sqlite-step,
  and raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, lib-layering is 79 grandfathered includes, and controller raw-SQL is
  6 grandfathered controller files.
- `ZCL_LINT_MODE=RATCHET tools/lint/check_no_raw_sqlite_in_controllers.sh`:
  pass with 6 grandfathered controller files, no new ones. WARN mode now
  reports 23 direct raw controller SQL calls across those 6 files.
- `build/bin/test_parallel --only=sync_service --timeout=120 --verbose`: pass after
  moving sync-import UTXO cardinality validation into the UTXO model; filtered
  run covered both `test_sync_service` and `test_snapshot_sync_service`.
- `build/bin/test_parallel --only=utxo_recovery_service --timeout=120 --verbose`:
  pass.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  the controller raw-SQL baseline shrink.
- `build/bin/test_parallel --timeout=180`: pass after the controller raw-SQL shrink,
  `0/279` groups failed in 58.0s.
- Quick live sample at 2026-06-01 02:43:32 UTC after the controller raw-SQL
  shrink: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1349221`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8033` / `[::]:8033`, and a journal scan since
  2026-06-01 02:40:00 UTC found no low-tip regression, integrity failure, OOM,
  fatal, segfault, assert, or panic signal. This is a continuity check, not the
  final soak.
- `make -j$(nproc)`: pass after moving `repair_controller_utxo.c` transaction
  control to `node_db_*()` and `sync_controller_blocks.c` Sapling tree writes
  to the Block model.
- `make lint`: pass; E1, E2, supervisor, E7, typed-blocker, raw-sqlite-step,
  and raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, lib-layering is 79 grandfathered includes, and controller raw-SQL is
  7 grandfathered controller files.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces, no new ones, after keeping the `repair_controller_utxo.c`
  `coins_view_cache_flush` E6 lines stable.
- `ZCL_LINT_MODE=RATCHET tools/lint/check_no_raw_sqlite_in_controllers.sh`:
  pass with 7 grandfathered controller files, no new ones. WARN mode now
  reports 24 direct raw controller SQL calls across those 7 files.
- `build/bin/test_parallel --only=sync_service --timeout=120 --verbose`: pass after
  moving Sapling tree block persistence into the Block model; filtered run
  covered both `test_sync_service` and `test_snapshot_sync_service`.
- `build/bin/test_parallel --only=sapling_tree --timeout=120 --verbose`: pass.
- `build/bin/test_parallel --only=utxo_activation_paused --timeout=120 --verbose`: pass
  after moving `repairutxos` transaction control to `node_db_*()`.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  the controller raw-SQL baseline shrink.
- `build/bin/test_parallel --timeout=180`: pass after the controller raw-SQL shrink,
  `0/279` groups failed in 57.0s.
- Quick live sample at 2026-06-01 02:34:30–02:35:07 UTC after the follow-up
  controller raw-SQL shrink: `systemctl --user is-active zclassic23` reported
  `active`, `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1349221`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8033` / `[::]:8033`, and a short log tail found no low-tip
  regression, integrity failure, OOM, fatal, segfault, or assert signal. This
  is a continuity check, not the final soak.
- `make -j$(nproc)`: pass after the snapshot/wallet/repair controller raw-SQL
  shrink and UTXO model helper extraction.
- `make lint`: pass; E1, E2, supervisor, E7, typed-blocker, raw-sqlite-step,
  and raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, lib-layering is 79 grandfathered includes, and controller raw-SQL
  was then 9 grandfathered controller files.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces, no new ones, after keeping the blockchain admin E6 baseline
  line-stable.
- `ZCL_LINT_MODE=RATCHET tools/lint/check_no_raw_sqlite_in_controllers.sh`:
  pass with then-9 grandfathered controller files, no new ones. WARN mode then
  reported 28 direct raw controller SQL calls across those 9 files.
- `build/bin/test_parallel --only=wallet --timeout=120 --verbose`: pass after replacing
  the shielded wallet height fallback with `db_block_max_height_any_status()`.
- `build/bin/test_parallel --only=snapshot_sync_service --timeout=120 --verbose`: pass
  after routing snapshot checked exec helpers through `node_db_exec()`.
- `build/bin/test_parallel --only=utxo_recovery_service --timeout=120 --verbose`: pass
  after moving UTXO missing-height count/repair SQL into `models/utxo`.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  the controller raw-SQL baseline shrink.
- `build/bin/test_parallel --timeout=180`: pass after the controller raw-SQL shrink,
  `0/279` groups failed in 58.0s.
- Quick live sample at 2026-06-01 02:24:01–02:24:57 UTC after the controller
  raw-SQL shrink: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `txouts=1349221`, RPC listened on `127.0.0.1:18232`, P2P listened on
  `0.0.0.0:8033` / `[::]:8033`, and `getpeerinfo` showed active peers with
  peer tip around `3131915`. A short log tail found no low-tip regression or
  integrity failure. This is a continuity check, not the final soak.
- `git diff --check`: pass after the controller raw-SQL shrink.
- `make -j$(nproc)`: pass after the file-manifest header extraction, controller
  raw-SQL cleanup, and raw allocation wrapper migration.
- `make lint`: pass; E1, E2, supervisor, E7, typed-blocker, raw-sqlite-step,
  and raw-malloc gates remain at zero active debt, E6 is 24 grandfathered write
  surfaces, lib-layering is 79 grandfathered includes, and controller raw-SQL
  was then 12 grandfathered controller files.
- `make test_parallel`: pass after making the `body_fetch_stage` crash-replay
  test deterministic with a child-ready pipe.
- `tools/scripts/check_one_write_path.sh`: pass with 24 grandfathered write
  surfaces, no new ones.
- `tools/scripts/check_lib_layering.sh`: pass with 79 grandfathered lib-to-app
  includes, no new ones.
- `ZCL_LINT_MODE=RATCHET tools/lint/check_no_raw_sqlite_in_controllers.sh`:
  pass with then-12 grandfathered controller files, no new ones.
- `tools/scripts/check_raw_malloc.sh`: pass with no active production raw
  malloc/calloc/realloc allowlist entries.
- `build/bin/test_parallel --only=file_controller --timeout=120 --verbose`: pass after
  moving file manifest protocol declarations into `lib/net`.
- `build/bin/test_parallel --only=file_market --timeout=120 --verbose`: pass.
- `build/bin/test_parallel --only=wallet --timeout=120 --verbose`: pass after routing
  wallet scan / legacy import table clearing through `node_db_exec()`.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  the lib-layering/controller-SQL/raw-malloc baseline shrink.
- First full `build/bin/test_parallel --timeout=180` after the edits exposed the
  existing timing-sensitive `test_body_fetch_stage` crash subcase
  (`zero-progress case is consistent`, `rows bounded by chain size`). The crash
  test now waits for a child-ready pipe after deterministic drain; focused
  rerun passed.
- Second full `build/bin/test_parallel --timeout=180`: pass with `0/279` groups failed
  in 121.0s.
- Live-node sample at `2026-06-01 02:09:55 UTC`: not proven. `systemctl --user
  is-active zclassic23` could not connect to the user bus, `./tools/zcl-rpc
  getblockcount` exited 7, and no `zclassic23`/`zclassicd` process or
  `18232`/`8232`/`8033` listener was visible.
- `rg -ni "shadow|cutover|projection-diff|shadow-diff" app lib/storage lib/validation tools/mcp --glob '*.c' --glob '*.h' --glob '!lib/test/**' --glob '!app/views/**'`:
  clean after normalizing uppercase production "Shadow" comments too.
- `rg -n "shadow|cutover|projection-diff|shadow-diff" app lib/storage lib/validation tools/mcp --glob '*.c' --glob '*.h' --glob '!lib/test/**' --glob '!app/views/**'`:
  clean after deleting stale production wording and renaming UTXO projection
  emitters.
- `make -j$(nproc)`: pass after the production wording cleanup and UTXO
  projection emitter rename.
- `make lint`: pass after re-anchoring the existing
  `utxo_projection_set_author` grandfathered E6 line; E1, E2, supervisor, E7,
  and typed-blocker baselines remain at zero, E6 remains at 26, and
  lib-layering remains at 101.
- `build/bin/test_parallel --only=utxo_apply_authorship --timeout=120 --verbose`:
  pass after the UTXO projection emitter rename.
- `build/bin/test_parallel --only=reorg_projection_parity --timeout=120 --verbose`:
  pass after the projection wording cleanup.
- `build/bin/test_parallel --only=reorg_parity --timeout=120 --verbose`: pass.
- `build/bin/test_parallel --only=block_index_backfill --timeout=120 --verbose`: pass.
- `build/bin/test_parallel --only=mcp_controllers --timeout=120 --verbose`: pass.
- `build/bin/test_parallel --timeout=180`: pass after the production wording cleanup,
  `0/279` groups failed in 67.0s.
- `git diff --check`: pass after the production wording cleanup and status
  update.
- `rg` for deleted `docs/work` plan names and source-comment references:
  clean after removing obsolete cutover/B8 runbooks, stale reducer-ingest design
  snapshots, and the paused import-path worker assignment.
- `make lint`: pass after the `docs/work` stale-plan deletion; E1, E2,
  supervisor, E7, and typed-blocker baselines remain at zero, E6 remains at 26,
  and lib-layering remains at 101.
- `git diff --check`: pass after the `docs/work` stale-plan deletion.
- `make -j$(nproc)`: pass after splitting active-chain cache/window moves from
  public tip authority.
- `make lint`: pass; E1, E2, supervisor, E7, and typed-blocker baselines are at
  zero, E6 is down to 26 grandfathered write surfaces, and lib-layering remains
  at 101.
- `build/bin/test_parallel --only=chain_state_repo --timeout=120 --verbose`: pass after
  the active-chain cache/window API split.
- `build/bin/test_parallel --only=chain_tip --timeout=120 --verbose`: pass.
- `build/bin/test_parallel --only=tip_finalize_stage --timeout=120 --verbose`: pass;
  includes the authority guard proving a raw low-level active-chain cache move
  does not lower public reducer height.
- `build/bin/test_parallel --only=invalidateblock --timeout=120 --verbose`: pass after
  migrating invalidate-path cache movement to `active_chain_move_window_tip()`.
- `build/bin/test_parallel --only=process_block_revalidate --timeout=120 --verbose`:
  pass.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass;
  includes the E6 fixture that proves a new writer still trips the ratchet.
- `build/bin/test_parallel --timeout=180`: pass after the active-chain cache/window API
  split, `0/279` groups failed in 69.0s.
- `git diff --check`: pass after the active-chain cache/window API split and
  doc update.
- `make -j$(nproc)`: pass after the projection-storage boot rename.
- `make test_parallel`: pass.
- `build/bin/test_parallel --only=tip_finalize_stage --timeout=120 --verbose`: pass;
  includes `authority_guard` and `stale_cursor`, proving raw low-level
  active-chain cache moves do not lower public reducer height and stale low
  `tip_finalize` cursors anchor above a restored high tip instead of replaying.
- `build/bin/test_parallel --only=reducer_ingest_e2e --timeout=120 --verbose`: pass.
- `build/bin/test_parallel --only=chain_restore_service --timeout=120 --verbose`: pass.
- `build/bin/test_parallel --only=supervisor --timeout=120 --verbose`: pass after the
  staged-sync supervisor API/comment cleanup.
- `build/bin/test_parallel --only=body_persist_stage --timeout=120 --verbose`: pass
  after the S-5 Job/test wording cleanup.
- `build/bin/test_parallel --only=script_validate_stage --timeout=120 --verbose`: pass
  after the S-6 Job/test wording cleanup.
- `build/bin/test_parallel --only=proof_validate_stage --timeout=120 --verbose`: pass
  after the S-7 Job/test wording cleanup.
- `build/bin/test_parallel --only=utxo_apply_stage --timeout=120 --verbose`: pass after
  the S-8 Job/test wording cleanup.
- `build/bin/test_parallel --only=boot_phase --timeout=120 --verbose`: pass after the
  boot projection-storage wording/function rename.
- `build/bin/test_parallel --only=utxo_recovery_service --timeout=120 --verbose`: pass
  after rebuilding `test_parallel`; covers the new import/restore `zcl_result`
  status paths, including invalid-context errors. Re-run after the execution
  status/backfill split: pass.
- `build/bin/test_parallel --only=chain_state_repo --timeout=120 --verbose`: pass after
  adding `csr_commit_tip_result()`; covers the `zcl_result` failure wrapper.
- `build/bin/test_parallel --timeout=180`: pass, `0/279` groups failed in 72.1s after
  the recovery execution status/backfill split. Re-run after the CSR wrapper:
  pass, `0/279` groups failed in 70.0s.
- `git diff --check`: pass after the S-5..S-8, health-comment, and
  projection-storage boot cleanup; re-run after the recovery split: pass.
- `make -j$(nproc)`: pass after the recovery execution status/backfill split
  and again after the CSR wrapper.
- `rg` over `config/src/boot_services.c` and `config/src/boot.c` finds no
  `shadow`/`cutover`/`SHADOW`/`AUTHORITATIVE` boot-surface references.
- `make lint`: pass after the legacy mirror result wrapper; E2 is at zero
  grandfathered service files, E7 has zero grandfathered entries, E1 remains
  at 4, typed-blocker remains at 4, lib-layering remains at 101, and E6 remains
  at 34.
  The E6 baseline change only re-anchored the same three
  `config/src/boot_services.c` `coins_view_cache_flush` entries after comment
  cleanup shifted their line numbers.
- `build/bin/test_parallel --only=zclassicd_oracle --timeout=120 --verbose`: pass after
  adding `legacy_mirror_sync_request_catchup_result()`; covers the non-OK
  `zcl_result` path carrying `hash-disagreement`.
- `build/bin/test_parallel --only=legacy_mirror_stuck_condition --timeout=120 --verbose`:
  pass after routing the remedy through the result-returning catchup API.
- `make -j$(nproc)`: pass after the legacy mirror result wrapper.
- `build/bin/test_parallel --timeout=180`: pass after the legacy mirror result wrapper,
  `0/279` groups failed in 79.1s.
- `make -j$(nproc)`: pass after extracting `utxo_import_pipeline.c` from
  `sync_controller_import.c`.
- `build/bin/test_parallel --only=utxo_recovery_service --timeout=120 --verbose`: pass
  after the import-pipeline helper split.
- `build/bin/test_parallel --only=sync_service --timeout=120 --verbose`: pass after the
  import-pipeline helper split.
- `make lint`: pass after the import-pipeline helper split; E1 is down to 3
  grandfathered oversized app files, E2 remains at zero, E7 remains at zero,
  typed-blocker remains at 4, lib-layering remains at 101, and E6 remains at
  34.
- `build/bin/test_parallel --timeout=180`: pass after the import-pipeline helper split,
  `0/279` groups failed in 78.0s.
- `make -j$(nproc)`: pass after the `legacy_import.c`,
  `sync_controller_catchup.c`, and `legacy_mirror_sync_service.c` E1 splits.
- `make lint`: pass after emptying the E1 baseline; E1 is at zero grandfathered
  oversized app files, E2 remains at zero, typed-blocker remains at 4,
  lib-layering remains at 101, and E6 remains at 34.
- `build/bin/test_parallel --only=zclassicd_oracle --timeout=120 --verbose`: pass after
  the legacy mirror state split; covers catchup failure reporting and dump-state
  fields.
- `build/bin/test_parallel --only=legacy_mirror_stuck_condition --timeout=120 --verbose`:
  pass after the legacy mirror state split; covers condition remedy routing.
- `build/bin/test_parallel --only=lag_slo --timeout=120 --verbose`: pass after the
  legacy mirror state split; covers the monitor contract dump shape.
- `build/bin/test_parallel --only=sync_service --timeout=120 --verbose`: pass after the
  sync catchup/persistence split; filtered run covered both `test_sync_service`
  and `test_snapshot_sync_service`.
- `build/bin/test_parallel --only=sqlite --timeout=120 --verbose`: pass after the sync
  persistence split; covers sync job wrappers, mempool persistence, and DB
  service writes.
- `build/bin/test_parallel --only=sapling_tree --timeout=120 --verbose`: pass after the
  Sapling tree rebuild split.
- `build/bin/test_parallel --timeout=180`: pass after the E1 baseline reached zero,
  `0/279` groups failed in 65.0s.
- `git diff --check`: pass after the E1 baseline reached zero and docs were
  updated.
- `make -j$(nproc)`: pass after replacing the remaining typed-blocker baseline
  surfaces with typed class plus reason/id fields.
- `make test_parallel`: pass after the typed-blocker public-struct rename.
- `make lint`: pass after emptying `tools/scripts/typed_blocker_baseline.txt`;
  E1, E2, E7, supervisor, and typed-blocker baselines are at zero. E6 remains
  at 34 and lib-layering remains at 101.
- `build/bin/test_parallel --only=chain_advance_coordinator --timeout=120 --verbose`:
  pass after the source-policy `selection_reason` struct-field rename while
  preserving the legacy `selection_blocker` JSON key.
- `build/bin/test_parallel --only=zclassicd_oracle --timeout=120 --verbose`: pass after
  the legacy mirror and mirror-consensus typed blocker stats rename.
- `build/bin/test_parallel --only=lag_slo --timeout=120 --verbose`: pass after the
  legacy mirror typed blocker stats rename.
- `build/bin/test_parallel --only=syncdiag_rpc --timeout=120 --verbose`: pass after the
  diagnostics JSON compatibility check.
- `build/bin/test_parallel --only=mcp_controllers --timeout=120 --verbose`: pass after
  the MCP status compatibility check.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  the typed-blocker baseline became empty and `check_typed_blocker.sh` learned
  the empty-baseline count.
- `build/bin/test_parallel --timeout=180`: pass after the typed-blocker baseline reached
  zero, `0/279` groups failed in 70.0s.
- Live node negative proof: the 2026-05-31 22:34 UTC restart initially
  restored RPC/UTXO height to `3130701`, then the old stale cursor path
  regressed the public tip back into the ~44k range after background validation
  had time to run.
- Live node patched proof: after the 2026-05-31 22:54 UTC restart, boot
  restored RPC/UTXO height to `3130701`; `tip_finalize` stamped
  `authority anchor cursor from=45685 to=3130702 reason=init_existing_tip`;
  `progress.kv` contains `tip_finalize_log` row `(3130701, anchor, ok=1)`.
- Live soak 2026-05-31 22:56:05–23:00:40 UTC: every 30s sample reported
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `stage_cursor.tip_finalize=3130702`, and the latest tip-finalize row remained
  `(3130701, anchor, ok=1)`. Post-soak log scan found no low-tip `commit_tip`,
  `chain_integrity_failed`, or orphan-UTXO regression. Background validation is
  still running from height `44770` toward `3130701`, so this is a live
  regression proof, not final refactor completion.
- After the staged-sync supervisor cleanup and rebuild, the current binary was
  restarted again at 2026-05-31 23:07 UTC. Samples through 23:10 UTC stayed at
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`, and
  `stage_cursor.tip_finalize=3130702`; logs showed all eight staged jobs
  initialised and no low-tip commit or integrity regression. Background
  validation resumed from height `55001` toward `3130701`.
- Quick live sample at 2026-05-31 23:24 UTC after this cleanup still reported
  `getblockcount=3130701` and `gettxoutsetinfo.height=3130701`. This is a
  continuity check, not a replacement for the required final soak.
- Quick live sample at 2026-05-31 23:32 UTC after the E2 shrink still reported
  `getblockcount=3130701` and `gettxoutsetinfo.height=3130701`.
- Quick live sample at 2026-06-01 00:00:45 UTC after the E2 baseline reached
  zero: `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `stage_cursor.tip_finalize=3130702`, and latest `tip_finalize_log` row
  remained `(3130701, anchor, ok=1)`. A tail scan found no low-tip regression
  or integrity failure. This is a continuity check, not the final soak.
- Quick live sample at 2026-06-01 00:14:19 UTC after the E1 shrink:
  `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, `gettxoutsetinfo.height=3130701`,
  `stage_cursor.tip_finalize=3130702`, and latest `tip_finalize_log` row
  remained `(3130701, anchor, ok=1)`. A tail scan found no low-tip regression
  or integrity failure. This is a continuity check, not the final soak.
- Quick live sample at 2026-06-01 00:47 UTC after the E1 baseline reached zero:
  `systemctl --user is-active zclassic23` reported `active`,
  `getblockcount=3130701`, and `gettxoutsetinfo.height=3130701` with
  `txouts=1348252`; a node.log tail scan found no low-tip commit, integrity
  failure, or orphan-UTXO regression. This is a continuity check, not the final
  soak.
- Live sample attempt at 2026-06-01 01:00 UTC after the typed-blocker shrink:
  no continuity proof was available. `systemctl --user` could not connect to
  the user bus, RPC port `18232` was closed, no `zclassic23` process was
  running, and the journal showed `zclassic23.service` was OOM-killed at
  2026-06-01 00:51:52 UTC after `zclassicd-rhett` was OOM-killed with a 6.0G
  memory peak. This is a failed live sample, not a refactor completion proof.
- Live sample attempt at 2026-06-01 01:10 UTC after the active-chain E6 shrink:
  no continuity proof was available. `systemctl --user` could not connect to
  the user bus, `./tools/zcl-rpc getblockcount` exited with code 7, no
  `zclassic23` or `zclassicd` process was running, and no listener existed on
  ports `18232`, `8232`, or `8033`. This is a failed live sample, not a refactor
  completion proof.
- Live sample attempt at 2026-06-01 01:32 UTC after the production wording
  cleanup: no continuity proof was available. `systemctl --user` could not
  connect to the user bus, `./tools/zcl-rpc getblockcount` exited with code 7,
  no `zclassic23` or `zclassicd` process was running, and no listener existed
  on ports `18232`, `8232`, or `8033`. This is a failed live sample, not a
  refactor completion proof.
- Live sample attempt at 2026-06-01 01:47 UTC after the E6/lib-layering shrink:
  no continuity proof was available. `systemctl --user` could not connect to
  the user bus, `./tools/zcl-rpc getblockcount` exited with code 7, no
  `zclassic23` or `zclassicd` process was running, and no listener existed on
  ports `18232`, `8232`, or `8033`. This is a failed live sample, not a refactor
  completion proof.
- `make -j$(nproc)`: pass after removing stale Phase/PR/dissolve scaffold
  comments from production C/H files.
- `make test_parallel`: pass; rebuilt the updated `test_parallel` binary after
  adding the scaffold-label lint-gate coverage.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  adding the guard that production comments name current purpose instead of old
  refactor scaffold labels.
- `build/bin/test_parallel --timeout=180`: pass after the production scaffold-label
  cleanup, `0/279` groups failed in 56.0s.
- `make lint`: pass after the production scaffold-label cleanup; E1, E2, E6,
  E7, framework-shape, supervisor, typed-blocker, lib-layering, controller
  raw-SQL, raw-allocation, and doc-accuracy gates remain clean with zero
  grandfathered entries.
- `git diff --check`: pass, and the production C/H scan for
  `Phase 3 dissolve`, `dissolve PR`, `PR-*`, `docs/dissolve`,
  `dissolved chain_advance_coordinator`, `Re-homed verbatim`,
  `Behavior-preserving`, `until Phase 3`, `Phase 3 unblocks`, and
  `Phase 3: release refs` is clean.
- Live sample attempt at 2026-06-01 15:37 UTC after the production
  scaffold-label cleanup: no continuity proof was available. `systemctl --user`
  could not connect to the user bus, `./tools/zcl-rpc getblockcount` and
  `gettxoutsetinfo` exited with code 7, no `zclassic23` process was running,
  no listener existed on ports `8023`, `8033`, `18232`, or `8232`, and the last
  20 minutes of `zclassic23` journal output had no entries. This is a failed
  live sample, not a refactor completion proof.
- `make -j$(nproc)`: pass after removing stale B3/B5/C3 code-motion comments
  from the UTXO apply/recovery path and boot projection-storage wiring.
- `make test_parallel`: pass; rebuilt the updated `test_parallel` binary after
  widening the scaffold-label guard.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  widening the production-comment guard to cover the UTXO apply/recovery files.
- `build/bin/test_parallel --only=utxo_apply --timeout=120 --verbose`: pass; covers
  `test_utxo_apply_authorship` and `test_utxo_apply_stage`.
- `make lint`: pass after the UTXO apply/recovery comment cleanup; all
  zero-baseline ratchets remain clean.
- `build/bin/test_parallel --timeout=180`: pass after the UTXO apply/recovery comment
  cleanup, `0/279` groups failed in 57.0s.
- Live sample attempt at 2026-06-01 15:47 UTC after the UTXO apply/recovery
  scaffold-label cleanup: no continuity proof was available. `systemctl --user`
  could not connect to the user bus, `./tools/zcl-rpc getblockcount` and
  `gettxoutsetinfo` exited with code 7, no `zclassic23` process was running,
  no listener existed on ports `8023`, `8033`, `18232`, or `8232`, and the last
  20 minutes of `zclassic23` journal output had no entries. This is a failed
  live sample, not a refactor completion proof.
- Targeted controller scaffold scan after the controller purpose-comment
  cleanup: clean for `Split out of`, `split out of`, `D5`,
  `behavior byte-identical`, `behavior unchanged`, `byte-identically`,
  `pre-split monolith`, and `extracted from` in the touched controller files.
- `git diff --check`: pass after the controller purpose-comment cleanup.
- `make -j$(nproc)`: pass after the controller purpose-comment cleanup.
- `make test_parallel`: pass; rebuilt the updated `test_parallel` binary after
  widening the scaffold-label guard to the controller files.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  widening the production-comment guard to cover the controller files.
- `build/bin/test_parallel --only=wallet --timeout=120 --verbose`: pass; `0/32`
  wallet and wallet-view groups failed in 7.0s.
- `build/bin/test_parallel --only=transaction --timeout=120 --verbose`: pass; `0/1`
  transaction group failed in 1.0s.
- `build/bin/test_parallel --only=repair --timeout=120 --verbose`: no validation group
  exists for this filter; the harness reported `--only=repair matched no
  groups`.
- `build/bin/test_parallel --only=sync_service --timeout=120 --verbose`: pass; `0/2`
  sync and snapshot-sync groups failed in 6.0s.
- `make lint`: pass after the controller purpose-comment cleanup; all
  zero-baseline ratchets remain clean.
- `build/bin/test_parallel --timeout=180`: pass after the controller purpose-comment
  cleanup, `0/279` groups failed in 56.0s.
- Live sample attempt at 2026-06-01 15:57 UTC after the controller
  purpose-comment cleanup: no continuity proof was available. `systemctl
  --user` could not connect to the user bus, `./tools/zcl-rpc getblockcount`
  and `gettxoutsetinfo` exited with code 7, no `zclassic23` process was
  running, no listener existed on ports `8023`, `8033`, `18232`, or `8232`,
  and the last 20 minutes of `zclassic23` journal output had no entries. This
  is a failed live sample, not a refactor completion proof.
- Post-status doc checks after recording the controller slice:
  `git diff --check` passed, `tools/scripts/check_doc_accuracy.sh` passed,
  the zero-baseline/allowlist scan found no non-comment entries in the
  ratcheted baseline files, and the production C/H scan under app controllers,
  services, jobs, conditions, supervisors, models, lib, and MCP found no
  `shadow`, `cutover`, `projection-diff`, or `projection_diff` terminology.
- App-layer scaffold scan after the view/service purpose-comment cleanup:
  clean for `checklist item D2`, `checklist D5`, `D5 split`, `D5 seam`,
  `byte-identically`, `moved out of`, `No behavior change vs the original`,
  `behavior byte-identical`, `pre-split monolith`, `extracted from`,
  `extracted verbatim`, `Extracted from`, `Split out of`, `split out of`,
  `single-engine replacement`, `pure refactor`, `code motion`, `Phase C`,
  `boot decomposition Phase`, and `for file size` under app views,
  controllers, and services.
- Targeted guarded-file scaffold scan after the same cleanup: clean for the
  widened stale-label vocabulary in every newly guarded file.
- `git diff --check`: pass after the view/service purpose-comment cleanup.
- `make -j$(nproc)`: pass after the view/service purpose-comment cleanup.
- `make test_parallel`: pass; rebuilt the updated `test_parallel` binary after
  widening the scaffold-label guard to view/service files.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  widening the production-comment guard; `0/1` group failed in 11.0s.
- Focused coverage after the view/service purpose-comment cleanup:
  `build/bin/test_parallel --only=explorer --timeout=120 --verbose` passed (`0/1`,
  1.0s), `--only=store` passed (`0/10`, 2.0s), `--only=wallet_view` passed
  (`0/2`, 1.0s), `--only=block_index` passed (`0/6`, 3.0s),
  `--only=chain_state_validator` passed (`0/1`, 1.0s), and
  `--only=bg_validation` passed (`0/1`, 1.0s).
- `make lint`: pass after the view/service purpose-comment cleanup; all
  zero-baseline ratchets remain clean.
- `build/bin/test_parallel --timeout=180`: pass after the view/service
  purpose-comment cleanup, `0/279` groups failed in 56.0s.
- Live sample attempt at 2026-06-01 16:11 UTC after the view/service
  purpose-comment cleanup: no continuity proof was available. `systemctl
  --user` could not connect to the user bus, `./tools/zcl-rpc getblockcount`
  and `gettxoutsetinfo` exited with code 7, no `zclassic23` process was
  running, no listener existed on ports `8023`, `8033`, `18232`, or `8232`,
  and the last 20 minutes of `zclassic23` journal output had no entries. This
  is a failed live sample, not a refactor completion proof.
- Process-block scaffold scan after the helper-boundary comment cleanup:
  clean for stale split/code-motion terms across `process_block*.c`,
  `process_block_internal.h`, and the public `process_block*.h` headers. The
  scaffold-label lint guard now covers the process-block top-level wiring,
  crash hooks, failed-child propagation, flush policy, internal helper header,
  and missing-UTXO self-heal coordinator.
- `make test_parallel`: pass after rebuilding the parallel runner with the
  widened process-block scaffold-label guard.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  widening the process-block guard; `0/1` group failed in 11.0s.
- `make -j$(nproc)`: pass after the process-block helper-boundary comment
  cleanup.
- `make lint`: pass after the process-block helper-boundary comment cleanup;
  all zero-baseline ratchets remain clean.
- `build/bin/test_parallel --timeout=180`: pass after the process-block
  helper-boundary comment cleanup, `0/279` groups failed in 56.0s.
- Post-status doc checks after the process-block helper-boundary comment
  cleanup: `git diff --check` passed, `tools/scripts/check_doc_accuracy.sh`
  passed, the zero-baseline/allowlist scan found no non-comment entries in the
  ratcheted baseline files, and the production C/H scan under app controllers,
  services, jobs, conditions, supervisors, models, lib, and MCP found no
  `shadow`, `cutover`, `projection-diff`, or `projection_diff` terminology.
- Live sample attempt at 2026-06-01 16:20 UTC after the process-block
  helper-boundary comment cleanup: no continuity proof was available.
  `systemctl --user` could not connect to the user bus, `./tools/zcl-rpc
  getblockcount` and `gettxoutsetinfo` exited with code 7, no `zclassic23`
  process was running, no listener existed on ports `8023`, `8033`, `18232`,
  or `8232`, and the last 20 minutes of `zclassic23` journal output had no
  entries. This is a failed live sample, not a refactor completion proof.
- Wallet/explorer view-template scaffold scan after the view ownership comment
  cleanup: clean for `byte-identical`, `byte-identically`,
  `prior controller implementation`, `prior inline`, `not a redesign`,
  `move, not a redesign`, `Moved out of`, and `moved out of` under app views,
  the adjacent explorer block controller, and `utxo_apply_stage.c`.
- `make test_parallel`: pass after rebuilding the parallel runner with the
  widened generic parity-wording guard.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  widening the view-template/parity-wording guard; `0/1` group failed in
  11.0s.
- `make -j$(nproc)`: pass after the view-template purpose-comment cleanup.
- `make lint`: pass after the view-template purpose-comment cleanup; all
  zero-baseline ratchets remain clean.
- `build/bin/test_parallel --timeout=180`: pass after the view-template
  purpose-comment cleanup, `0/279` groups failed in 57.0s.
- Post-status doc checks after the view-template purpose-comment cleanup:
  `git diff --check` passed, `tools/scripts/check_doc_accuracy.sh` passed, the
  zero-baseline/allowlist scan found no non-comment entries in the ratcheted
  baseline files, and the production C/H scan under app controllers, services,
  jobs, conditions, supervisors, models, lib, and MCP found no `shadow`,
  `cutover`, `projection-diff`, or `projection_diff` terminology.
- Live sample attempt at 2026-06-01 16:29 UTC after the view-template
  purpose-comment cleanup: no continuity proof was available. `systemctl
  --user` could not connect to the user bus, `./tools/zcl-rpc getblockcount`
  and `gettxoutsetinfo` exited with code 7, no `zclassic23` process was
  running, no listener existed on ports `8023`, `8033`, `18232`, or `8232`,
  and the last 20 minutes of `zclassic23` journal output had no entries. This
  is a failed live sample, not a refactor completion proof.
- App/jobs scaffold scan after the Job-stage purpose-comment cleanup: clean
  for `B2:`, `byte-identical`, `byte-identically`, `copy-pasted`, `verbatim`,
  `pure refactor`, `pre-split`, `Split out of`, `file-size ceiling`,
  `Precedent`, `single-engine`, `Single-engine`, `BLOCKER 1 PIECE`, and
  `PIECE 1`.
- Production-wide `single-engine` source scan after the boot/projection wording
  cleanup: clean across C/H files outside `test_make_lint_gates.c`, where the
  forbidden term is kept only as lint-gate vocabulary.
- `make test_parallel`: pass after rebuilding the parallel runner with the
  widened Job-stage/scaffold-label guard.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  widening the Job-stage and generic `single-engine` guard; `0/1` group failed
  in 11.0s.
- `make -j$(nproc)`: pass after the Job-stage and boot/projection terminology
  cleanup.
- `make lint`: pass after the Job-stage and boot/projection terminology
  cleanup; all zero-baseline ratchets remain clean.
- `build/bin/test_parallel --timeout=180`: pass after the Job-stage and
  boot/projection terminology cleanup, `0/279` groups failed in 56.0s.
- Post-status doc/scans after the Job-stage cleanup: `git diff --check`
  passed, `tools/scripts/check_doc_accuracy.sh` passed, the zero-baseline /
  allowlist scan found no non-comment entries in the ratcheted baseline files,
  the production C/H scan under app controllers, services, jobs, conditions,
  supervisors, models, lib, and MCP found no `shadow`, `cutover`,
  `projection-diff`, or `projection_diff` terminology, and the app/jobs stale
  scaffold scan remained clean.
- Live sample attempt at 2026-06-01 16:41:04 UTC after the Job-stage cleanup:
  no continuity proof was available. `systemctl --user` could not connect to
  the user bus, `./tools/zcl-rpc getblockcount` and `gettxoutsetinfo` exited
  with code 7, no `zclassic23` process was running, no listener existed on
  ports `8023`, `8033`, `8233`, `18232`, or `8232`, and the last 20 minutes of
  `zclassic23` journal output had no entries. This is a failed live sample, not
  a refactor completion proof; the service was not restarted and the 8023 port
  expectation was preserved.
- Model scaffold scan after the wallet-read ownership cleanup: clean for
  `byte-identical`, `byte-identically`, `copy-pasted`, `verbatim`,
  `pure refactor`, code-motion labels, `pre-split`, `Split out of`,
  `file-size ceiling`, `Precedent`, `single-engine`, and `gate E1` across
  `app/models`, the Job files, and the touched UTXO recovery service files.
- Wallet read helper ownership check after the same cleanup:
  `db_wallet_tx_total_fees` and `db_wallet_utxo_recent_activity` are defined
  only in `app/models/src/wallet_tx_reads.c`.
- `make -j$(nproc)`: pass after moving wallet read aggregate/activity helpers
  out of `sapling_note.c`.
- `make test_parallel`: pass after rebuilding the parallel runner with
  `wallet_tx_reads.c` and the widened scaffold-label guard.
- `build/bin/test_parallel --only=make_lint_gates --timeout=120 --verbose`: pass after
  widening the model/UTXO-recovery scaffold guard; `0/1` group failed in
  12.0s.
- Focused filtered tests after the wallet-read ownership cleanup:
  `build/bin/test_parallel --only=wallet --timeout=120 --verbose` passed (`0/32`,
  7.0s) and `build/bin/test_parallel --only=models --timeout=120 --verbose` passed
  (`0/1`, 2.0s). The attempted combined filter
  `--only='wallet|models'` matched no groups because the harness uses literal
  substring matching.
- `make lint`: pass after the wallet-read ownership cleanup; all zero-baseline
  ratchets remain clean and the framework shape scan now covers 252 app `.c`
  files.
- `build/bin/test_parallel --timeout=180`: pass after the wallet-read ownership
  cleanup, `0/279` groups failed in 57.0s.
- Post-status doc/scans after the wallet-read ownership cleanup:
  `git diff --check` passed, `tools/scripts/check_doc_accuracy.sh` passed, the
  zero-baseline / allowlist scan found no non-comment entries in the ratcheted
  baseline files, and the production C/H scan under app controllers, services,
  jobs, conditions, supervisors, models, lib, and MCP found no `shadow`,
  `cutover`, `projection-diff`, or `projection_diff` terminology.
- Live sample attempt at 2026-06-01 16:49:43 UTC after the wallet-read
  ownership cleanup: no continuity proof was available. `systemctl --user`
  could not connect to the user bus, `./tools/zcl-rpc getblockcount` and
  `gettxoutsetinfo` exited with code 7, no `zclassic23` process was running,
  no listener existed on ports `8023`, `8033`, `8233`, `18232`, or `8232`, and
  the last 20 minutes of `zclassic23` journal output had no entries. This is a
  failed live sample, not a refactor completion proof; the service was not
  restarted and the 8023 port expectation was preserved.

Do not mark this refactor complete while any ratchet baseline contains a real
entry or the live node proof is missing.
