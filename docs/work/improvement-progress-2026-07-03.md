# Improvement progress 2026-07-03

Source backlog: `docs/work/review-2026-07-03-handoff.md`.

Live baseline checked before edits:
- Native `zclassic23 agent`: healthy/serving, served height 3168187, target 3168188, Tor ready, MRS 4/8.
- Native `zclassic23 refold`: not ready, blocker `missing_verified_anchor_snapshot`.
- Worktree: `main`; initial pre-existing untracked fixture file
  `app/services/src/_fsuf_fixture_tmp_controller.c` is now absent after the
  lint-gate test cleanup path ran.

## Checklist

- [x] D: retire wrong-LOG-macro return-type class with lint coverage.
- [x] B: collapse reducer frontier hash agreement to one bounded JOIN.
- [x] F: harden ACME challenge file containment.
- [x] F: fix `api_json_error()` truncated-body length reporting.
- [x] E: fix HODL capped recency ordering and upsert failure propagation.
- [x] G: take the first wallet locking slice for Sapling scans, OVK read, and
  tip-finalize height write.
- [x] H: fix bounded recovery/timer issues already scoped in this batch
  (`clear_failed_above_tip`, sticky escalator timers, `tip_finalize` cursor
  normalization).
- [x] C: replace nullifier same-block duplicate scan with an indexed
  earlier-transaction lookup.
- [x] C: replace intra-block UTXO `lookup_added()` linear scans with an index.
- [x] C: cache `/explorer/hodl` full-UTXO scans by served tip block hash.
- [x] C: collapse explorer deep `tx_outputs` stats into one conditional
  aggregate scan.
- [x] C: replace single-prevout `coins_kv_get_coins()` reconstruction on stage
  hot paths with a point-read that returns value/script/height/coinbase.
- [x] V1: canonicalize the advertised MagicBean/ZClassic23 user-agent while
  keeping old `ZClassic-C23` peer classification compatible.
- [x] V1: accept `-externalip=IPv4:port` so public P2P address advertisement
  matches operator service flags.
- [x] V1: publish the P2P version-advertisement helpers through
  `net/version.h` so identity/reachability callers stop carrying hand-written
  `extern` declarations.
- [x] V1: document `-externalip=IP[:PORT]` in binary help, config comments,
  and the public-node runbook.
- [x] F: bind explorer factoids cache to block hash, not height alone.
- [x] H: make mainnet zk params loader thread-spawn failure a named
  `params_missing` blocker before `CRYPTO_READY`.
- [x] H: gate non-canonical reducer-stage purge on a known safe coins frontier
  so below/unknown-frontier evidence is never turned into an unfillable hole.
- [x] H: remove the `chain_restore_finalize()` `cs_main` -> progress-store
  lock edge by using the raw cached active-chain tip inside the restore
  mutation section.
- [x] C: wire bounded production pruning for the `created_outputs` creation
  index so progress.kv does not grow with every historical output forever.
- [x] E: repair HODL stale-nonzero snapshots by versioning rows and binding
  freshness to source projection coverage.
- [x] A: decide and implement the coordinated `block_index` mutation strategy.
- [x] H: finish the bounded boot/recovery cluster after copy-proving each
  candidate.
- [x] I: delete orphan `zcl-browser` target/tool so the repo stops carrying a
  second unsupported WebKit shell beside the in-binary `-gui` path.
- [x] V1: add canonical `zclassic23` peer identity aliases while preserving
  the legacy `zclassic_c23` operator/API fields.
- [x] V1: finish the first canonical-identity cleanup pass across MCP status,
  deploy verification, and the wallet node page.
- [x] V1: replace `zcl_status` peer identity string scraping with structured
  JSON parsing.
- [x] V1: align `CLIENT_NAME`, postmortem build IDs, and GUI bot checks with
  the canonical `ZClassic23` identity.
- [x] V1: keep the ACME path hardening portable in the clang/fuzzer build and
  keep the nullifier activation blocker reason within its durable cap.
- [x] REST: make route contracts expose explicit CRUD operation/scope metadata
  and pin the generated `/api/v1` + OpenAPI shapes in tests.
- [x] H: clear stale tip-height auto-reindex requests when durable coins
  authority has advanced beyond their anchor, without weakening terminal or
  boot-storage reindex markers.
- [x] V1: make MVP reporters resolve the actual live service datadir/RPC port
  and use the finality-gap at-tip rule, so `make mvp` cannot misreport the
  healthy full-history node as stopped.
- [ ] I: remaining binary/client consolidation remains owner-gated, especially
  `session`/`bot`, `zclassic-cli`/`zcl-rpc`, and any `zclassic23` ->
  `zclassic` naming move.

## Work log

- Started with item D because it is a cheap class retirement: fix the confirmed `_Bool`/`LOG_ERR` site and add a lint gate for wrong LOG macro return types.
- Item D patch:
  - `bg_validation_verify_scripts_parallel()` now logs pthread spawn fallback with `LOG_WARN` and still verifies the range inline.
  - Added `check-log-macro-return-type` to `make lint` and `docs/DEFENSIVE_CODING.md`.
  - Fixed existing mismatches found by the new gate: int-returning list helpers now use `LOG_RETURN(0, ...)`; bool-returning helpers use `LOG_FAIL` or `LOG_WARN` + `return false`; `fast_sync_prebuild_snapshot()` returns explicit `-1`.
  - Added a planted-fixture regression in `test_make_lint_gates`.
- Small correctness patch batch from the review backlog:
  - `api_json_error()` now returns the number of bytes actually written after truncation, avoiding caller over-read risk.
  - HODL history load caps now keep the newest samples and return them in ascending chart order; failed snapshot upserts now propagate failure.
  - `chain_restore_clear_failed_above_tip()` now treats an empty active chain as a no-op instead of walking from height `-1`.
  - `sticky_escalator` now clears rearm/rederive episode timestamps when the episode is cleared.
  - Added focused regressions for API truncation and HODL capped recency.
- Item B patch:
  - `apply_hash_agreement()` now uses one bounded JOIN to find the first
    validate_headers/script_validate hash split instead of preparing and
    stepping two single-height queries for every height in the H* range.
  - The public per-height `reducer_frontier_log_hash_at()` helper remains
    unchanged for diagnostics and external readers.
- Explorer/HTTP security hardening:
  - The HTTP ACME challenge passthrough now validates request paths with
    `path_check_url_arg()`, resolves both ACME root and target with
    `realpath()`, and rejects targets outside the challenge directory.
  - Added temp-dir regression coverage for accepted challenge files, `..`
    rejection, empty-token rejection, and symlink escape rejection.
- Reducer frontier off-by-one fix:
  - `reducer_frontier_log_frontier_above()` now normalizes the
    `tip_finalize` served-height cursor into the next-height scan frame,
    matching `reducer_frontier_log_frontier()`.
  - Added a regression where a served cursor at `A+3` must report frontier
    `A+3`, not `A+2`.
- Wallet locking pass:
  - Sapling keystore presence/address scans now take `sks->cs`, mirroring the
    transparent keystore reader pattern.
  - Transparent-to-shielded send now copies the default OVK while holding
    `sapling_keys.cs`; tip-finalize post-step now writes
    `wallet->best_block_height` under `wallet->cs`.
  - Scope note: pointer-returning Sapling APIs still return internal entries;
    a later API cleanup should add copy-out helpers for long-lived use.
- Item C nullifier hot-path patch:
  - Replaced the per-nullifier linear `nf_seen()` scan with a per-block lookup
    table keyed by `(pool, nullifier)`.
  - The table is populated only after each transaction finishes, preserving the
    existing rule that this layer checks durable rows plus earlier transactions
    in the same block; same-transaction duplicates remain an upstream
    structural-validation responsibility.
  - Added a larger intra-block duplicate regression with 64 earlier nullifiers
    before the duplicate spend.
- Item C transparent UTXO hot-path patch:
  - Replaced `lookup_added()` linear scans in `utxo_apply_compute_block_delta()`
    with a per-block index keyed by `(txid, vout)`.
  - The same index now serves both same-block create-then-spend resolution and
    duplicate-output detection.
  - Added a direct compute-delta regression where tx2 spends tx1's output in
    the same block and asserts the captured restore preimage.
- Item C explorer HODL page cache patch:
  - `/explorer/hodl` now caches the expensive current transparent UTXO scan by
    datadir, served tip height, and served tip block hash.
  - A same-height reorg/hash change invalidates the cached HODL snapshot; a
    repeated request for the same served hash does not rescan all UTXOs.
  - Replaced the page's static chart scratch buffers with request-local checked
    allocations, avoiding concurrent render races between explorer workers.
  - Scope note: API/RPC HODL scan behavior is unchanged in this patch; the API
    already has its own timed response cache.
- Item C explorer deep-stats aggregate patch:
  - `gather_deep_chain_data()` now computes total, P2PKH, P2SH, max-value, and
    total-value fields for `tx_outputs` with one conditional aggregate query
    instead of five separate full-table scans.
  - Added an in-memory explorer regression with mixed script types and values
    to pin the collapsed aggregate semantics.
- Item C `coins_kv` prevout point-read patch:
  - Added `coins_kv_get_prevout()` / `_sqlite()` so callers can fetch one live
    `(txid,vout)` plus height and coinbase metadata without reconstructing a
    sparse `struct coins`.
  - `utxo_apply_stage_lookup_live()`, value-overflow repair dry-runs, and the
    frontier-aware script-validation fallback now use the point-read path.
  - `coins_ram_get_prevout()` now reuses the same durable SQLite point-read on
    cold miss, keeping overlay and steady-state semantics aligned.
  - Full `coins_kv_get_coins()` reconstruction remains for actual whole-tx
    view callers and the coin-backfill classifier that needs txid-level
    metadata.
- V1 MVP reporter target-resolution patch:
  - `tools/scripts/mvp_scoreboard.sh` now resolves the live node's effective
    `-datadir` and `-rpcport` from `zclassic23.service` unless explicitly
    overridden, matching the deploy verifier and the operator `tools/z` path.
  - The scoreboard's "synced" predicate now uses the same `TIP_GAP_OK` budget
    as the live gate instead of strict `blocks == headers`, so a healthy
    one-header race no longer blocks soak-accrual reporting as a wedge.
  - `tools/mvp_gate.sh` uses the same live target resolution, includes the
    target in JSON/human output, and points zclassicd oracle auth at
    `~/.zclassic` by default.
  - The live gate no longer false-fails C4 when `z_gettotalbalance` works but
    no Sapling receive address is listed; creating one would mutate the wallet,
    so that state is now owner/test-gated `BLOCKED`.
  - The scoreboard no longer counts historical soak `NOT_MET` as a regression
    `FAIL`; C6 remains `BLOCKED` until the authoritative 168h judge returns
    `MET`.
  - Added a source-text regression in `test_make_lint_gates` pinning the live
    service lookup and RPC environment threading for both reporters.
- V1 protocol identity patch:
  - The advertised P2P user-agent is now the canonical
    `/MagicBean:2.1.2-beta1/ZClassic23:0.1.0/` string.
  - `connman_init()` now sources its default subver from
    `msg_version_user_agent()` instead of a duplicated literal.
  - Peer classification accepts both the new `ZClassic23` token and the legacy
    `ZClassic-C23` token so diagnostics stay compatible during rollout.
  - This does not rename the binary, service, client, or `CLIENT_NAME`; the
    broader binary/client consolidation remains owner-gated under item I.
- V1 P2P reachability follow-up:
  - `msg_version_set_external_ip()` now accepts both bare IPv4
    (`-externalip=203.0.113.7`) and IPv4 with an explicit advertised port
    (`-externalip=203.0.113.7:8023`).
  - The explicit port overrides the listen-port default used by boot, so
    `getnetworkinfo.localaddresses`, version `addr_from`, and self-address
    suppression all use the same configured public endpoint.
  - Malformed values now log a named diagnostic and leave external address
    advertisement unset instead of failing silently.
  - `getnetworkinfo` regression coverage now proves `localaddresses`,
    `externalip_configured`, and `advertised_subver` reflect the configured
    public endpoint.
  - `connman_init()` regression coverage now asserts the default subver is
    exactly `msg_version_user_agent()`, not merely some string containing
    `ZClassic`.
- V1 P2P version API cleanup:
  - `net/version.h` now declares the advertised user-agent, peer
    classification, and external-address helpers used by boot, connman, RPC,
    node health, and tests.
  - Removed the duplicated local `extern msg_version_*` declarations from
    production and test consumers.
  - `node_health_service` no longer includes the internal message-handler
    header just to classify peers; it uses the same public version contract as
    `getnetworkinfo`.
- V1 P2P operator-discoverability patch:
  - `zclassic23 --help` now lists `-externalip=<ip[:port]>`.
  - `struct app_context.external_ip` and the public-node runbook now describe
    the optional advertised port, including when to use it.
- Explorer factoids production-build fix:
  - The factoids cache reset helper is now compiled only under `ZCL_TESTING`,
    matching its only caller and keeping the production `zclassic23` binary
    clean under `-Werror=unused-function`.
- Item I `zcl-browser` deletion:
  - Removed the standalone `zcl-browser` Makefile target and source file.
  - Cleaned current source comments and the security-audit response note so
    `wallet_gui` is the only documented in-process browser/API bypass
    consumer.
  - Left the broader client consolidation and `zclassic23` -> `zclassic`
    naming decision untouched because that is a coordinated owner-gated move.
- Item F explorer factoids cache patch:
  - `/explorer/factoids` now publishes cache entries with the served tip block
    hash and revalidates that hash before serving cached bytes.
  - The controller no longer loads the legacy unkeyed `factoids.cache` file,
    because it has no hash metadata and cannot prove chain identity.
  - Cache publication now re-checks the start height hash after the historian
    build, discarding the result if the underlying block row changed mid-build.
  - Added a same-height hash replacement regression through
    `explorer_handle_request()` so stale cached factoids HTML cannot leak after
    a reorg/fixture replacement.
- Item H boot/recovery params-thread patch:
  - A mainnet normal boot whose ZK params loader thread cannot start now raises
    the permanent `params_missing` blocker, emits the operator-needed event, and
    parks alive-degraded before `CRYPTO_READY`.
  - Non-mainnet boots keep the existing warning-only behavior, and
    `-mint-anchor-fast` remains exempt because that offline mode intentionally
    skips the crypto stages.
  - Added a `ZCL_TESTING` predicate seam and boot-phase regression coverage for
    the fatal/nonfatal boundary.
- Item H reducer-frontier purge safety patch:
  - Non-canonical stage-log purge now deletes rows only when
    `coins_applied_height` is known and the stale height is at/above that
    frontier; below or unknown frontier, it reports the evidence and leaves all
    rows intact for the replay owners.
  - The guard covers both hash-bearing rows and the hashless downstream rows
    that are transitively purged in the safe unapplied domain.
  - Added direct purge regressions for unknown and below-known coins frontiers,
    plus kept the end-to-end unknown-frontier refusal check.
- Item H chain-restore lock-order patch:
  - `chain_restore_finalize()` no longer calls `active_chain_tip()` while
    holding `cs_main`; it reads `active_chain_cached_tip()` directly, avoiding
    the pre-authority fallback path through `active_chain_height()` and
    `progress_store_tx_lock()`.
  - The restore mutation section remains ordered as `cs_main` followed by the
    small-tip CSR/active-chain write locks, deleting the dormant ABBA edge
    against reducer reconcile's `progress_store_tx_lock` -> `cs_main` order.
- Item C created-output index pruning patch:
  - Added `created_outputs_index_prune_below_limited()`, which deletes all rows
    for at most a bounded number of old heights below a retention floor.
  - `utxo_apply` now ensures the creation-index schema and, after a successful
    `coins_applied_height` co-commit, prunes rows older than the IBD reorg
    allowance plus block-download lookahead plus slack.
  - The prune is enrolled in the same stage transaction and is silent on
    success, loud on failure, so cleanup cannot create an untracked partial
    progress.kv mutation.
- Item C HODL snapshot audit:
  - The review's `hh_compute_snapshot()` O(K²) concern is already gone in the
    current tree: the adapter computes each snapshot with one aggregate query
    over `tx_outputs` plus a filtered `tx_inputs` join.
  - Schema v22 already creates covering indexes for that query
    (`idx_txo_hodl_scan`, `idx_txi_prev_height`), so no code patch was made for
    that specific item.
- Item E HODL stale-nonzero repair patch:
  - `hodl_history` rows now carry `calc_version` and `source_tip_height`, with
    node.db schema v23 defaulting existing rows to stale provenance.
  - `hh_next_fill_height()` no longer does an expensive aggregate-diff scan on
    every idle tick; it asks for the lowest missing/outdated row whose sample
    height is covered by `sync_projection_tip_height`.
  - `hh_upsert_snapshot()` records the current calculation version and the
    source projection cursor used for the snapshot, so rows computed before
    source coverage are refreshed once the projection catches up.
  - Added fixture coverage for stale nonzero repair, source-cursor gating, and
    v23 migration columns.
- Item A block-index/MMR concurrency patch:
  - `struct block_index` cross-thread status and disk-position fields now have
    typed acquire/release helpers; watchdog, background validation, stage, disk
    read, parse-cache, and header-event readers/writers use them instead of
    raw unlocked field access.
  - Header event emission snapshots status and disk positions through the
    helpers, so projection rows do not serialize torn status/file/position
    pairs.
  - MMR, MMB, and commitment-MMR globals now have controller-local mutexes plus
    copy-out snapshot APIs. Production RPC/API/boot/file-manifest readers no
    longer take raw tree pointers.
  - Removed the raw `rpc_blockchain_get_{mmr,mmb,commitment_mmr}` API surface so
    future readers stay on bounded snapshots.
- Item H final boot/recovery cleanup:
  - `reducer_frontier_reconcile_light` now treats coin-backfill insertion as an
    edge-triggered progress signal keyed to the remedy call that produced the
    reconcile result. A stale nonzero `coin_backfill_inserted` result can no
    longer reset the attempt budget forever.
  - The condition detail JSON now exposes the insert-progress baseline and the
    last reconcile remedy call, keeping the refresh path observable.
  - `chain_restore_backfill_nbits_from_disk()` now hydrates missing `nBits`
    first, then recomputes skip/chainwork for repaired entries sorted by height,
    so a hash-map child-before-parent iteration cannot compute child work from
    stale parent work.
  - Allocation failure while tracking repaired `nBits` entries leaves the entry
    unchanged and logs through the existing repair error path instead of
    partially repairing header fields without safe chainwork recomputation.
- V1 peer identity alias patch:
  - `getnetworkinfo`, `getpeerinfo`, peer-lifecycle state dumps, node health
    JSON, and Prometheus now expose canonical `zclassic23*` peer identity
    fields.
  - The older `zclassic_c23*` keys and metric remain as compatibility aliases,
    so existing scripts keep working while new operator surfaces match the
    advertised `/MagicBean:2.1.2-beta1/ZClassic23:0.1.0/` subver.
  - The public-node runbook now points operators at the canonical keys.
- V1 canonical-identity cleanup pass:
  - `zcl_status` now counts peers advertising the canonical `ZClassic23` token
    instead of only recognizing the older `ZClassic-C23` spelling.
  - `deploy_verify.sh` now requires both the canonical `zclassic23*` identity
    fields and the retained `zclassic_c23*` compatibility fields.
  - `/wallet/node` renders `msg_version_user_agent()` through the template
    instead of a hardcoded `/ZClassic-C23:0.1.0/` literal, and the wallet/story
    tests now pin the visible `ZClassic23:0.1.0` identity.
- V1 `zcl_status` parser hardening:
  - The MCP ops status handler now parses `getpeerinfo` as JSON for total,
    inbound/outbound, native ZClassic23/MagicBean classification, and max peer
    height.
  - The rough raw-object count remains only as a malformed-response fallback;
    valid peer arrays no longer depend on substring scans over JSON text.
- V1 client-name identity cleanup:
  - `CLIENT_NAME` now reports `ZClassic23`, so `getnetworkinfo.subversion` and
    postmortem `build_id` metadata no longer expose the old `ZClassic-C23`
    spelling.
  - The wallet GUI bot now checks for `ZClassic23` on the node page.
  - Legacy `ZClassic-C23` peer-subver classification remains accepted for
    rollout compatibility.
- V1 CI portability cleanup:
  - `https_server.c` now exposes `realpath()` under the same `_XOPEN_SOURCE 700`
    feature macro used by the blog controller, so the ACME containment hardening
    builds under the clang/fuzzer CI target.
  - The nullifier activation-gap blocker reason was shortened to fit
    `BLOCKER_REASON_MAX` without compile-time truncation while preserving the
    operator meaning.
- REST/CRUD contract slice:
  - Route contracts now derive `crud_operation`, `resource_scope`, `crud_name`,
    and `id_params` from the registered method/path/action instead of forcing
    humans or clients to infer CRUD shape from prose.
  - The generated OpenAPI operation now mirrors those fields as `x-*`
    extensions so REST clients can distinguish collection, singleton, item, and
    subcollection reads from one contract.
  - The `/api/v1` index buffer now matches the OpenAPI contract buffer size so
    the richer route contract remains available as a complete JSON document.
  - Verification: `make t ONLY=api` PASS.
- Boot stale-auto-reindex guard:
  - `boot_auto_reindex_status()` now exposes the durable marker for read-only
    policy checks.
  - Boot clears a positive tip-height auto-reindex request before consuming it
    when the derived coins-best height has advanced beyond the request anchor.
    Equality remains pending so a freshly requested reindex still runs on the
    next boot.
  - The guard deliberately refuses to clear terminal markers and anchor-0
    boot-storage episodes, preserving the bounded crash-only recovery contract.
  - Added a regression to `test_boot_reindex_terminates`; verification:
    `make t ONLY=boot_reindex_terminates` PASS.

## Verification

- `./tools/lint/check_log_macro_return_type.sh` PASS.
- `make check-log-macro-return-type` PASS.
- `make check-doc-accuracy` PASS.
- `make t ONLY=make_lint_gates` PASS.
- `make t ONLY=api` PASS.
- `make t ONLY=hodl_history_port` PASS.
- `make t ONLY=db_migration_idempotent` PASS.
- `make t ONLY=schema_migration` PASS.
- `make t ONLY=sqlite` PASS.
- `make t ONLY=sticky_escalator` PASS.
- `make t ONLY=chain_restore_service` PASS.
- `make t ONLY=reducer_frontier` PASS.
- `make t ONLY=path_check` PASS.
- `make t ONLY=wallet` PASS.
- `make t ONLY=tip_finalize_post_step` PASS.
- `make t ONLY=utxo_apply_stage` PASS.
- `make t ONLY=utxo_apply_value_balance` PASS.
- `make t ONLY=utxo_apply_authorship` PASS.
- `make t ONLY=explorer` PASS.
- `make t ONLY=explorer` PASS after explorer deep-stats aggregate collapse.
- `make t ONLY=small_projections` PASS.
- `make t ONLY=boot_phase` PASS.
- `make t ONLY=reducer_frontier_reconcile_light` PASS.
- `make t ONLY=reducer_reconcile_witness` PASS.
- `make t ONLY=validate_script_hash_split_repair` PASS.
- `make t ONLY=stall_totality_matrix` PASS.
- `make t ONLY=chain_integrity_failed_condition` PASS.
- `make t ONLY=utxo_recovery_service` PASS.
- `make t ONLY=created_outputs_index` PASS.
- `make t ONLY=body_persist_stage` PASS.
- `make t ONLY=script_validate_stage` PASS.
- `make t ONLY=chain` PASS.
- `make t ONLY=disk_block_io` PASS.
- `make t ONLY=block_parse_cache` PASS.
- `make t ONLY=bg_validation` PASS.
- `make t ONLY=sync_watchdog_conditions` PASS.
- `make t ONLY=block_index_projection` PASS.
- `make t ONLY=have_data_unreadable` PASS.
- `make t ONLY=tip_finalize_post_step` PASS after MMR/MMB snapshot conversion.
- `make t ONLY=file_controller` PASS.
- `make t ONLY=boot_flyclient` PASS.
- `make t ONLY=snapshot_sync_service` PASS.
- `make t ONLY=mmb` PASS.
- `make t ONLY=reducer_reconcile_witness` PASS after edge-triggering
  `coin_backfill_inserted` progress.
- `make t ONLY=condition_engine` PASS after progress-baseline changes.
- `make t ONLY=chain_restore_service` PASS after ordered chainwork
  recomputation.
- `make t ONLY=coins_kv` PASS after `coins_kv_get_prevout()` addition.
- `make t ONLY=coins_ram` PASS after durable point-read reuse.
- `make t ONLY=utxo_apply_stage` PASS after prevout resolver conversion.
- `make t ONLY=script_validate_stage` PASS after prevout resolver conversion.
- `make t ONLY=utxo_apply_value_balance` PASS after repair dry-run conversion.
- `make t ONLY=peer_lifecycle` PASS after canonical user-agent change.
- `make t ONLY=net` PASS after canonical user-agent change.
- `make t ONLY=syncdiag_rpc` PASS after canonical user-agent change.
- `make t ONLY=mcp_controllers` PASS after canonical user-agent change.
- `make t ONLY=net` PASS after external-IP `IP:PORT` parsing.
- `make t ONLY=syncdiag_rpc` PASS after external-IP `getnetworkinfo`
  coverage.
- `make t ONLY=net` PASS after moving `msg_version_*` declarations to
  `net/version.h`.
- `make t ONLY=syncdiag_rpc` PASS after moving `msg_version_*` declarations to
  `net/version.h`.
- `make t ONLY=peer_lifecycle` PASS after moving `msg_version_*` declarations to
  `net/version.h`.
- `make t ONLY=node_health_service` PASS after moving `msg_version_*`
  declarations to `net/version.h`.
- `make zclassic23` PASS after the external-IP help update and factoids
  production-build fix.
- `build/bin/zclassic23 --help | rg 'externalip=<ip\\[:port\\]>'` PASS after
  operator help update.
- `make t ONLY=explorer` PASS after gating the factoids cache reset helper to
  test builds.
- `rg -n "zcl-browser" Makefile tools app lib config src ports adapters deploy
  --glob '!build/**'` returned empty after deleting the orphan browser target
  and source.
- `make zclassic23 zcl-rpc` PASS after deleting `zcl-browser`.
- `make t ONLY=syncdiag_rpc` PASS after adding canonical `zclassic23` peer
  identity aliases.
- `make t ONLY=peer_lifecycle` PASS after adding canonical `zclassic23` peer
  identity aliases.
- `make t ONLY=mcp_controllers` PASS after adding canonical `zclassic23` peer
  identity aliases.
- `make t ONLY=mcp_metrics` PASS after adding canonical `zclassic23`
  Prometheus peer-count aliases.
- `make t ONLY=mcp_controllers` PASS after making `zcl_status` count the
  canonical `ZClassic23` peer token.
- `make t ONLY=mcp_controllers` PASS after replacing `zcl_status` peer string
  scraping with structured JSON parsing and stronger count assertions.
- `make t ONLY=encoding` PASS after changing `CLIENT_NAME` to `ZClassic23`.
- `make t ONLY=postmortem` PASS after changing postmortem build-id
  expectations to `ZClassic23`.
- `make build/bin/bot` PASS after updating the GUI bot node-version
  expectation to `ZClassic23`.
- `make fuzz-ci` PASS after fixing `realpath()` feature exposure and the
  nullifier blocker reason truncation warning.
- `make t ONLY=wallet_view` PASS after making `/wallet/node` render the live
  advertised user-agent.
- `make t ONLY=wallet_node` PASS after the wallet node-page identity cleanup.
- `make t ONLY=wallet_sovereignty` PASS after the wallet node-page identity
  cleanup.
- `make t ONLY=wallet_empowerment` PASS after the wallet node-page identity
  cleanup.
- `make t ONLY=100_stories` PASS after the wallet node-page identity cleanup.
- `make build/bin/session` PASS after updating the session simulator identity
  expectation.
- `make build/bin/bot` PASS after updating the bot simulator identity
  expectation.
- `make session bot` was also attempted; `session` compiled, then the simulator
  failed its pre-existing `z-address shown` runtime check, so that target was
  not counted as a passing verification for this slice.
- `make t ONLY=api` PASS after REST/CRUD contract metadata and `/api/v1`
  contract-buffer sizing.
- `make t ONLY=boot_reindex_terminates` PASS after the stale auto-reindex
  marker guard.
- `git diff --check` PASS after REST/CRUD and stale auto-reindex guard work.
- `make lint` PASS after REST/CRUD and stale auto-reindex guard work;
  file-size warnings remain advisory and are the same known oversized-file
  class called out above.
- `git diff --check` PASS after the `CLIENT_NAME` identity cleanup.
- `make lint` PASS after the `CLIENT_NAME` identity cleanup;
  remaining file-size output is advisory and limited to pre-existing unrelated
  oversized files:
  `app/jobs/src/stage_repair_reducer_frontier.c`,
  `config/src/boot_background_workers.c`, `config/src/boot_refold_staged.c`,
  and `config/src/boot.c`.
- `make ci` PASS after the clang/fuzzer portability cleanup. The seeded
  crash-recovery substep skipped because `/tmp/zcl-crashtest-ci.absent` is not
  present, which is that target's documented skip path.
