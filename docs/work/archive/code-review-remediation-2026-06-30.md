# Code-Review Remediation Backlog (2026-06-30)

> **What this is.** The *open, actionable* subset of the 2026-06-27 full audit
> ([archive evidence ledger](INDEX.md), 322 findings,
> 0 crit / 22 high), re-verified against live code on 2026-06-30 and turned into
> execution-ready batches with a fix + proof-gate for every item.
>
> **Where it sits.** This is a **secondary hardening lane**. It does **not**
> displace the #1 spine ([`FORWARD_PLAN.md`](../FORWARD_PLAN.md): CI-enforce MVP +
> the sovereign `-refold-from-anchor` cure). MVP = 4/8; that gate is sovereign-cure
> + soak, not this list. Run this lane in parallel worktrees when the spine isn't
> blocked, or hand individual lanes to workers.

## Status since the review landed

Of the 12 file:line-confirmed HIGH/quick-win bugs, **12 are fixed** (verified in
code through 2026-07-01 — note: the memory claim that "items 1–8 shipped" was
wrong on item 5; the 2026-06-30 pass closed qw9/qw10/qw12, and the 2026-07-01
pass closed qw5/qw11). The 96 post-review commits went heavily into god-function
decomposition (33 `split`/`audit: split`) and live-wedge liveness (~37
sync/boot/cure commits; live status is operational evidence and must be
checked separately). The review's two weakest dimensions —
**DRY** and **TESTED** — were barely advanced. This doc is the remainder.

```
quick-win bug fixes   ████████████████████  12/12  (qw11 pinned inert)
cross-cutting cleanups ██████████████████░░  ~90%   (cc-blob + cc-mostwork + cc-gtod + cc-arwrite + cc-ccoins done)
TESTED gap gates      ████████████████████  4/4    (all four review test gaps closed)
```

Update 2026-06-30: the P1 opening batch is implemented and proof-gated in this
branch: qw9, qw9-callers, qw10, and TESTED gap #1 (`test_sprout_phgr13_kat`).
Verification: focused `test_parallel --only=...`, `make check-test-registration`,
`make lint-fast`, and full `make test`.

Update 2026-06-30: TESTED gap #2 (`test_snapshot_apply_coins_kv`) is implemented
and proof-gated in this branch. It extracts the production snapshot→coins_kv
apply transaction, proves good apply count+SHA3, wrong-count rollback, and
tampered-body refusal. Verification: focused `snapshot_apply_coins_kv` plus the
nearby `load_verify_boot`, `refold_retro_validate`, `refold_auto_arm`,
`refold_from_anchor_fatal`, `boot_refold_window_extend`, and
`loader_owns_seed_gate` groups.

Update 2026-06-30: TESTED gap #3 (`test_gap_fill_frontier_window`) is
implemented and proof-gated in this branch. It extracts the production
gap-fill window seam (`gap_fill_compute_window` +
`gap_fill_window_walk_start`) and pins reducer-frontier anchoring, HAVE_DATA
exclusion, and bottom-window selection for large gaps. Verification:
`build/bin/test_parallel --only=gap_fill_frontier_window`,
`build/bin/test_parallel --only=download`, `make check-test-registration`, and
`make lint-fast`; full `make test` passed 0/480 groups failed, 14 self-skipped
(102.7 s wall).

Update 2026-06-30: qw12 is implemented and proof-gated in this branch.
`node_db_open()` now checks `node_db_migrate()` before snapshot staging cleanup
and fails closed on newer on-disk schema versions (`schema_version >
NODE_DB_MAX_SCHEMA`). The regression test proves the refused open returns
false, leaves `ndb.open=false`/`ndb.db=NULL`, and does **not** delete staged
snapshot rows or `snapshot_staging_%` state. Verification:
`build/bin/test_parallel --only=db_migration_idempotent`,
`--only=schema_migration`, `--only=sqlite`, `make check-test-registration`,
`make lint-fast`; full `make test` passed 0/480 groups failed, 14 self-skipped
(101.2 s wall).

Update 2026-07-01: live explorer projection sparse-hole remediation is
implemented, proof-gated, and deployed. Diagnostics now report
`missing_heights` and `first_missing_height` while preserving real
transactions/outputs/integrity counts. The projection backfill watcher rewinds
`sync_projection_tip_height` to the first missing connected block minus one and
reuses the existing catchup service, with bounded retry logging. Live proof:
initial `explorer_index_state` had `missing_heights=23`,
`first_missing_height=3155856`; after deploy the watcher rewound to `3155855`,
catchup rebuilt from there, and final `chain_evidence` reported
`state=complete`, `missing_heights=0`, `first_missing_height=-1`,
`height=3166809`, `blocks=3166810`. Public `/api/v1/factoids` and
`/api/v1/hodl` answered; HODL was `fresh=true`.

Update 2026-07-01: qw5 is implemented and proof-gated in this branch.
`utxo_import_raw_entry.value_len` is now `uint32_t`, the LevelDB reader no
longer clips CCoins values at 65535 bytes, and values above the explicit
4 MiB cap fail the import instead of accepting a truncated UTXO set. The
regression `test_utxo_import_pipeline` builds a synthetic whole-transaction
CCoins blob above 65 KiB and verifies all outputs decode with the correct
height, then proves the oversize cap refuses. Verification:
`build/bin/test_parallel --only=utxo_import_pipeline`, plus nearby
`--only=chainstate_legacy_reader`, `--only=snapshot_apply_coins_kv`,
`--only=ldb_snapshot`, and `--only=utxo_recovery_service`.

Update 2026-07-01: cc-blob is implemented and proof-gated in this branch.
The fixed-size model blob reads in `file_offer.c`, `swap_contract.c`,
`znam.c`, and `zmsg.c` now use `AR_READ_BLOB`; the `swap_contract.c`
redeem-script read now requires both a sane declared length and enough SQLite
blob bytes before copying. `test_blob_read_bounds` inserts short/NULL blobs
through raw SQLite fixture setup and reads them back through the public model
APIs, proving malformed fixed blobs zero instead of over-reading. A new hard
lint gate, `check-blob-read-bounds`, freezes the class for app models.
Verification so far: `build/bin/test_parallel --only=blob_read_bounds`,
nearby `--only=file_market`, `--only=znam`, `--only=htlc`, `--only=models`,
`make check-blob-read-bounds`, `make check-doc-accuracy`,
`make check-test-registration`, and `git diff --check`.

Update 2026-07-01: cc-mostwork is implemented and proof-gated in this branch.
`select_most_work_eligible()` is now the single shared most-work eligibility
predicate for the reducer window selector and `find_most_work_chain()`. The
activation path keeps its side effects (stale-fork/stuck logging and gap-fill
kick), while the shared selector owns the skip filters, ancestry walk,
below-tip refusal, and equal-work failed-incumbent adoption. The regression
`test_most_work_selector` pins failed candidates, header-only/no-data
candidates, below-tip stale forks, equal-work failed-incumbent sibling adoption,
and normal forward higher-work fork selection against the production selector.
Verification: `build/bin/test_parallel --only=most_work_selector`,
`--only=invalidateblock`, `--only=active_chain_extend`,
`make check-test-registration`, `make lint`, and full `make test` passed
0/483 groups failed, 14 self-skipped (117.9 s wall). No live deployment was
attempted; copy-prove H* climb remains required before promoting this into the
soak node.

Update 2026-07-01: cc-gtod is implemented and proof-gated in this branch. All
non-platform `gettimeofday()` call sites were converted to the named platform
time helpers: true elapsed deltas use `platform_time_monotonic_us()`, while
epoch/surfaced timestamps, wallet backup path timestamps, and the cross-node
P2P latency pair use `platform_time_realtime_us()`. The raw-clock lint now bans
`gettimeofday()` as well as `clock_gettime`, `time(NULL)`, and `getrandom`.
Verification: `rg -n "gettimeofday\\s*\\(" app lib config tools -g '*.c'
-g '*.h' -g '!vendor/**' -g '!build/**'` returned empty; `ZCL_LINT_MODE=FAIL
tools/lint/check_no_raw_clock_outside_platform.sh`; focused groups
`wallet_backup`, `crypto`, `workpool`, `rpc_timeout`, `game`, `msg_handlers`,
`db_txn`, `consensus_reject_index`, `zclassicd_oracle`,
`snapshot_sync_service`, `models`, and `event`; `make lint`; full `make test`
passed 0/483 groups failed, 14 self-skipped (103.4 s wall); and
`make -j$(nproc) build/bin/zclassic23`. The standalone `zcl-browser` target was
not built because `webkit2gtk-4.1` is unavailable in this environment.

Update 2026-07-01: cc-arwrite is implemented and proof-gated in this branch.
The four already-wrapped writes now use intent-correct `AR_STEP_WRITE`, and the
23 formerly unwrapped `config/` + `src/` raw `sqlite3_step()` sites now use
`AR_STEP_ROW_READONLY` for SELECTs and `AR_STEP_WRITE` for the three scalar
writes. `check_raw_sqlite.sh` now scans `config/` and `src/` as well as
`app/`, `tools/`, and `lib/`; the only surfaced extra raw-step count is the
existing progress.kv kernel-store marker in `config/src/boot_refold_staged.c`.
Verification: `tools/scripts/check_raw_sqlite.sh` clean; lint-style manual scan
empty; `test_make_lint_gates`; focused `block_index_loader`,
`utxo_import_pipeline`, `sync_service`/`snapshot_sync_service`, and `boot_`
groups; `make lint`; full `make test` passed 0/483 groups failed,
14 self-skipped (107.2 s wall); and `make -j$(nproc) build/bin/zclassic23`.
Historical live read-only check after those gates: zclassic23 was serving at
raw tip 3166010 with 4 peers, H* / served frontier at 3166009 due a
`tip_finalize_log` `missing-success-row` at 3166010, `/api/hodl` fresh to the
served frontier, `/api/factoids` refusing while explorer index is ahead of H*,
and legacy `zclassicd` oracle still blocked on RPC `-28`.

Update 2026-07-01: cc-arwrite-exec's high-value slice is implemented and
proof-gated in this branch. Direct node.db DML through
`sqlite3_exec(ndb->db|ndb.db, "INSERT/DELETE/UPDATE/REPLACE ...")` is now banned
by `check_raw_sqlite.sh`; callers must use the prepared
`ar_exec_write_sql()` / `AR_STEP_WRITE` path or a reviewed helper. The remaining
transaction-control, PRAGMA, ATTACH/DETACH, schema DDL, projection-store, and
progress.kv exec calls are intentionally outside this narrow DML gate. Converted
production writes: importchainstate wallet/address rebuilds, boot wallet UTXO
rebuild, snapshot import `main.utxos` clear/copy, and block-index cache clear.
Verification: planted `raw_sqlite_exec_node_db_fixture.c` trips the gate,
`tools/scripts/check_raw_sqlite.sh` clean, `make t ONLY=make_lint_gates`, and
`make -j32 build-only`.

Update 2026-07-01: cc-ccoins is implemented and proof-gated in this branch.
The three CCoins decoders remain intentionally separate, but
`test_ccoins_decoder_kat` now seeds hand-built CCoins records into a temporary
LevelDB and compares the real `coins_db`, `chainstate_legacy_reader`, and
`utxo_import_decode_entry` paths. The KAT covers coinbase, vout0-only,
multi-mask-byte rows, special script sizes `0..5`, and a raw `nSize > 6`
script. It caught a real importer divergence: special sizes `4/5` must decode
to uncompressed P2PK scripts, not compressed P2PK. Verification:
`build/bin/test_parallel --only=ccoins_decoder_kat`, adjacent
`utxo_import_pipeline`, `chainstate_legacy_reader`,
`domain_consensus_coins_math`, and `snapshot_apply_coins_kv`, plus
`make check-test-registration`, `make lint`, full `make test` passed
0/484 groups failed, 14 self-skipped (113.0 s wall), and
`make -j$(nproc) build/bin/zclassic23`.

Update 2026-07-01: TESTED gap #4 (`test_boot_snapshot_drop_bodiless`) is
implemented and proof-gated in this branch. The regression exposes the
production bodiless-HAVE_DATA drop seam through a `ZCL_TESTING` wrapper and
pins the intended data-cleanup contract: above- and below-seed blocks with
missing/empty blk files lose `BLOCK_HAVE_DATA` and fall back to
`BLOCK_VALID_TREE`, while the seed-height block is protected and real
non-empty blk files keep their data flags. Verification:
`build/bin/test_parallel --only=boot_snapshot_drop_bodiless`, nearby
`boot_snapshot_failure_memory`, `boot_refold_window_extend`,
`load_verify_boot`, `refold_auto_arm`, and `refold_body_span_contiguous`,
`make check-test-registration`, `tools/scripts/check_raw_sqlite.sh`,
`git diff --check`, `make check-doc-accuracy`, focused data/API groups
`activerecord`, `db_validators`, `models`, `api`, `mcp_controllers`, and
`http_middleware`, `make lint`, and full `make test` passed 0/485 groups failed,
14 self-skipped (102.8 s wall).

Update 2026-07-01: REST API consistency / website freshness hardening is
implemented and deployed. `/api/factoids` now follows the same H*-honest
contract as `/api/hodl`: it serves 200 JSON capped to the served frontier and
reports `chain_height`, `served_height`, `indexed_height`, and `index_capped`
instead of returning a transient 503 when the explorer projection is one block
ahead of H*. The HODL endpoint still refreshes synchronously when the cache lags
the served tip. Verification: focused `api`, adjacent `mcp_controllers`,
`http_middleware`, `activerecord`, and `models`, `git diff --check`,
`make check-doc-accuracy`, `make lint`, full `make test` passed 0/485 groups
failed, 14 self-skipped (108.7 s wall), `make -j$(nproc) build/bin/zclassic23`,
and `make deploy`. Live post-deploy check: public `/api/status` healthy at
height/served/indexed/header/peer-best 3166043 with `gap=0`, `index_gap=0`,
`operator_needed=false`; `/api/hodl` fresh at height 3166043 with
`skipped_rows=0`; `/api/factoids` returns 200 JSON at
`chain_height=served_height=indexed_height=3166043` and `index_capped=false`.

Update 2026-07-01: compact public status false-alarm hardening is implemented
and deployed. `/api/status` now treats the normal one-block H* race as green:
when `served_height` is one block behind indexed/header state and native health
is serving, it returns `status=healthy`, `operator_needed=false`, and
`primary_blocker=none`. Material served-frontier gaps (`gap > 1`) still report
`catching_up` or `download_queue_idle` with a named next endpoint. Verification:
`build/bin/test_parallel --only=api`, `make lint`, `make deploy`, and live
post-deploy `/api/status` at served height 3166103 / indexed+header 3166104
with `gap=1`, `status=healthy`, `operator_needed=false`.

Update 2026-07-01: handoff status is refreshed for the next developer. Running
build `8f82a9a3c` is healthy/serving at local tip with REST/HODL/factoids tied
to the served frontier; the formal 168h soak judge is still **NOT_MET** because
the current evidence window includes two operator interventions. Treat this
review-remediation lane as closed; continue MVP work from the sovereign cure and
fresh soak window, not by reopening these hardening items.

Update 2026-07-01: qw11 is implemented and proof-gated in this branch.
`wallet_sapling_notes` now has a validated `source` field (`local` by default,
`view` for zclassicd mirror placeholders). `wv_sync_wallet_from_zclassicd()`
writes Sapling placeholders as `source='view'`; successful empty RPC result
arrays clear stale mirror rows; and Sapling refreshes replace only view rows, so
real local/catchup notes cannot be clobbered by an external wallet-view sync.
The shielded spend path now returns the explicit error
`view-only balance synced from zclassicd` when an address has only view-synced
placeholder balance. The live-deploy root cause was also fixed: the partial
`idx_snote_view_address` index belongs in the v21 migration, not the baseline
schema create path, because existing v20 databases do not have `source` yet.
Verification: focused `db_migration_idempotent`, `wallet_funds_safety`,
`activerecord`, `db_validators`, `wallet_view`, `wallet_view_port`, `api`,
`mcp_controllers`, and `http_middleware`; `git diff --check`;
`make check-test-registration`; `make check-doc-accuracy`;
`tools/scripts/check_raw_sqlite.sh`; `make lint`; full `make test` passed
0/485 groups failed, 14 self-skipped; `make -j$(nproc) build/bin/zclassic23`;
and `make deploy`. Live DB inspection confirmed the `source` column, migration
`021`, and `idx_snote_view_address`.

## Trap-check results (the most important planning output)

Reading `AGENT_TRAPS.md` + surrounding code **reclassified three "bugs"** — do
**not** let the next push chase these as defects:

| Item | Review said | Verified verdict | Action |
|---|---|---|---|
| **qw11** `wallet_view_sync` fabricated sapling crypto | HIGH fund-loss | **INTENTIONAL scaffolding** — fake `ivk` from `SHA256(txid+outindex)` never matches the real key's `ivk`, so notes are structurally non-spendable by the spend/balance filter. No funds at risk. | **DONE 2026-07-01:** source-marked as `view`, clobber-guarded, explicit view-only spend error, and inertness regression pinned |
| **cc-ccoins** CCoins decoder ×3 | DRY: single-source | **INTENTIONAL** — the bounds/output split is load-bearing (`coins_db.c:116-123` forbids folding; trusted-import vs untrusted-DoS-cap differ) | **DONE 2026-07-01:** cross-decoder KAT added instead of folding |
| **cc-mostwork** selector ×2 | DRY: unify | **INTENTIONAL today (no divergence)** but recommend extracting the shared *eligibility predicate* — divergence already wedged this project once | P2, behavior-preserving, copy-prove H\* CLIMB |

And the trap-check **found a sub-bug the review missed**: `swap_contract.c`
`redeem_script` has a genuine two-column OOB — `rlen` (col 13) bounds the memcpy
but is never checked against col 12's actual byte count (col13=256 + 4-byte col12
over-reads 252 B). Folded into **cc-blob-swap** below.

---

## Priority tiers

- **P0** — none in this code-review lane. Live soak status is tracked under
  the MVP/soak spine, not as a review-remediation P0 (latest check while
  refreshing this handoff, 2026-07-01: running build `8f82a9a3c` is
  healthy/serving via native P2P at `log_head=3166109`; public `/api/status`
  shows the expected one-block H* race as green; `/api/hodl` is fresh at
  3166108; `/api/factoids` is capped to the served frontier with unsafe
  sections suppressed; legacy `zclassicd` oracle is still advisory-blocked on
  RPC `-28`; formal soak is `NOT_MET`
  (`operator_intervention_detected_x2`)).
- **P1** — closed in this branch: **qw10, qw9, tested-gap #1 (PHGR13 KAT)**.
- **P2** — silent corruption / class-cure: **cc-blob DONE 2026-07-01**,
  **cc-mostwork DONE 2026-07-01**, **cc-gtod DONE 2026-07-01**,
  **cc-arwrite DONE 2026-07-01**. No autonomous P2 remains in this lane.
- **P3** — hygiene / documentation: **qw11 DONE 2026-07-01**.

## Restart command

Give the next developer this exact goal:

```text
continue zclassic23 development; review-remediation hardening is closed in docs/work/code-review-remediation-2026-06-30.md. Start with docs/HANDOFF.md, verify live node/API/HODL/factoids, then continue MVP from the sovereign cure plus fresh soak window.
```

## Completed first moves

The original opening batch is done in this branch:

1. **qw9 + qw9-callers** — RPC output buffer guards and caller failure handling.
2. **qw10** — fail-loud on diverged Sapling tree.
3. **tested-gap #1** — Sprout PHGR13 real-proof KAT.
4. **tested-gap #2** — production snapshot→coins_kv apply transaction KAT.
5. **tested-gap #3** — gap-fill reducer-frontier window KAT.
6. **qw12** — fail-closed on newer `node.db` schema before staging cleanup.
7. **qw5** — import CCoins values above 65 KiB without truncation; fail closed
   above the explicit 4 MiB cap.
8. **cc-blob** — fixed-size model blob reads use `AR_READ_BLOB`; swap
   redeem-script reads are byte-count bounded; hard lint gate added.
9. **cc-mostwork** — `select_most_work_eligible()` is shared by both most-work
   selectors; parity fixture covers failed/no-data/below-tip/equal-work cases.
10. **cc-gtod** — raw `gettimeofday()` use is removed from app/lib/config/tools
    outside `lib/platform`; elapsed timings and realtime/epoch timestamps now
    use named helpers, and the raw-clock lint bans the class.
11. **cc-arwrite** — raw `sqlite3_step()` is eliminated from `config/` and
    `src/` via intent-named `AR_STEP_ROW_READONLY` / `AR_STEP_WRITE` wrappers,
    and `check_raw_sqlite.sh` now scans those roots.
12. **cc-ccoins** — three intentional CCoins decoders are pinned by a
    cross-decoder KAT; the import decoder now handles special script sizes
    `4/5` as uncompressed P2PK.
13. **tested-gap #4** — bodiless snapshot cleanup is pinned: missing/empty blk
    files clear borrowed `BLOCK_HAVE_DATA` without touching the protected seed
    block or valid non-empty blk files.
14. **REST API freshness** — `/api/factoids` now caps to served H* like
    `/api/hodl` and reports served/indexed/capped metadata instead of serving a
    transient website 503 during normal projection-vs-H* skew.
15. **qw11 wallet-view inertness** — zclassicd Sapling view placeholders are
    source-marked (`view`), refreshed/cleared without deleting local notes,
    excluded from real-IVK spend selection, and surface an explicit view-only
    spend error.

## Next move

Run the remaining focused gates, then full `make test`, build/deploy, and live
MCP/REST freshness checks before promoting this branch. The review-remediation
lane has no autonomous bug item left beyond the owner decision below.

---

## Execution lanes (parallel worktrees)

Each lane is one branch / one merge. Items inside a lane touch related files;
lanes are mutually file-disjoint except where noted. **Ordering gotchas are the
load-bearing part** — the timing and AR-lint lanes red-bar the build if landed in
the wrong order.

### Lane A — wallet safety  *(P1 + P3, wallet-funds)*
| id | sev | fix | files | gate |
|---|---|---|---|---|
| **qw10** | P1 | **DONE 2026-06-30.** Wire the already-present divergence detection into a hard refusal: extract `bool rescan_result_consensus_valid(our_root, header_root, mismatches)` (false on all-zeros header_root OR root!=our_root OR mismatches>0); gate the save block on it so a divergence does **not** persist `sapling_tree`/witnesses and returns an explicit `status="diverged"` error instead of `witnesses_saved:N`. | `wallet_rescan_controller_witness.c:191-261,293-299` | new `test_rescanwitnesses_diverge_guard.c` truth table → `test_parallel`; copy-prove: force diverged tree on a copy, assert `node.db` `sapling_tree_rescan` unchanged |
| **qw11** | P3 | **DONE 2026-07-01.** Kept the placeholder (intentional), added validated `source='view'`, made zclassicd wallet-view refresh replace/clear only view rows, and made shielded spend return `view-only balance synced from zclassicd` when only view rows exist for the source address. Recorded in `AGENT_TRAPS.md`. | `wallet_view_sync.c`; `sapling_note.c`; `wallet_shielded_send_shielded.c` | `test_wallet_funds_safety`: placeholder `ivk` != real keystore `ivk`; real-IVK selection sees only the local note; empty view refresh clears view rows without deleting the local note; spend error is explicit |

### Lane B — explorer OOB  *(P1 + P3, no risk)*
| id | sev | fix | files | gate |
|---|---|---|---|---|
| **qw9** | P1 | **DONE 2026-06-30.** **Root fix = terminate at entry.** Top of `rpc_call()`: `if (!out||outmax==0) return -1; out[0]='\0';` — every `-1` path then leaves a valid empty C-string, so caller `strstr` parsers read one NUL not 64-256 KB of uninit stack. `outmax==0` guard also closes a latent `outmax-1` underflow. Mirror to sibling `wv_rpc_call`. | `explorer_controller.c:254-258` (+latent OOB `:313-318`); `wallet_view_helpers.c:92-102` | new `test_explorer_rpc_call.c`: dead port → assert `n==-1 && buf[0]=='\0'`; build under existing ASan config so a pre-fix `strstr` tail-run trips |
| **qw9-callers** | P3 | **DONE 2026-06-30.** Add `n<=0` guards at the 6 ignoring sites (mirrors the already-correct `_tx.c:56`/`_address.c:192`). Defense-in-depth on top of qw9. | `explorer_controller_dashboard.c:35,40,54,64`; `explorer_controller_block.c:49,61` | extend the same test: dead port → response == not-found view |

### Lane C — db import / open  *(P2)*
| id | sev | fix | files | gate |
|---|---|---|---|---|
| **qw5** | P2 | **DONE 2026-07-01.** A coins `'c'` value is a whole-tx CCoins blob and can legitimately exceed 65535 B (multi-output payouts; the 413 grandfathered oversize txs up to 1.92 MB). `value_len` is now `uint32_t`; the silent clip is gone; values above the explicit 4 MiB cap set `iter_error=true` + `request_stop` through a `zcl_result` helper. | `utxo_import_pipeline.h`; `node_db_import_service.c`; `utxo_import_pipeline.c` | `test_utxo_import_pipeline`: synthetic CCoins >65535 B decodes all outputs with correct height; oversize → hard-fail |
| **qw12** | P2 | **DONE 2026-06-30.** Capture `node_db_migrate`'s return before snapshot staging cleanup; on newer on-disk schema, `sqlite3_close` + `open=false` + `db=NULL` + `return false`. The test also proves staging rows/state are not deleted by the refused open. | `database.c:485-490`; `test_db_migration_idempotent.c` | `test_parallel --only=db_migration_idempotent`; nearby `schema_migration` + `sqlite`; full `make test` |

### Lane D — timing class (Gate #19)  *(P2/P3, no risk)* — **land as ONE PR**
> **Ordering law:** convert **every** site first, flip the ban regex **last** (same
> commit). The moment `gettimeofday` is banned, `make ci` runs `ZCL_LINT_MODE=FAIL`
> and any unconverted site red-bars. **Not all sites → monotonic** — wall-clock /
> epoch / cross-node-latency fields must stay realtime via a new helper.

| id | sev | fix | files |
|---|---|---|---|
| **cc-gtod-realtime-helper** | P3 | **DONE 2026-07-01.** Added `platform_time_realtime_us()` (`= clock_now_wall_ms()*1000`) under the gate-exempt `lib/platform/` so wall-clock sites need zero `// platform-ok` markers. | `lib/platform/include/platform/time_compat.h` |
| **cc-gtod-elapsed-named** | P3 | **DONE 2026-07-01.** True elapsed deltas now use `platform_time_monotonic_us()`. | `db_txn.c`; `mmb_leaf_store.c`; `wallet_gui.c`; `zcl-browser.c`; timing tests |
| **cc-gtod-wall-named** | P3 | **DONE 2026-07-01.** Epoch/surfaced timestamps use `platform_time_realtime_us()`, not monotonic time. | `consensus_reject_index.c`; `event.c`; `zclassicd_oracle_service.c`; `wallet_backup_service.c` |
| **cc-gtod-p2p-pair** | P3 | **DONE 2026-07-01.** `p2p_game` send-stamp + `msgprocessor` receive-latency stay on the same realtime basis for cross-node latency math. | `p2p_game.c`; `msgprocessor.c` |
| **cc-gtod-surfaced** | P2 | **DONE 2026-07-01.** The surfaced app/lib/config/tools sites are triaged: deltas to monotonic; epoch/filename/cross-node to realtime; comment prose reworded. | see update note above |
| **cc-gtod-gate** | P2 | **DONE 2026-07-01.** Added `\|\bgettimeofday\s*\(` to the raw-clock regex and reworded the comment false-positive. | `tools/lint/check_no_raw_clock_outside_platform.sh`; `snapshot_sync_service.c` |

Final gate: `rg` for `gettimeofday()` outside platform is empty;
`ZCL_LINT_MODE=FAIL tools/lint/check_no_raw_clock_outside_platform.sh`, focused
timing-adjacent tests, `make lint`, full `make test`, and
`make -j$(nproc) build/bin/zclassic23` are green.

### Lane E — fixed-blob bounds (AR_READ_BLOB)  *(P2/P3, no risk)*
> **Ordering law:** 4 conversions + ASan test first; **freeze the lint baseline last**
> (else the ratchet trips on the very offenders being fixed).

| id | sev | fix | files |
|---|---|---|---|
| **cc-blob-fileoffer** | P2 | **DONE 2026-07-01.** `AR_READ_BLOB` ×3 (`root_hash`32/`z_addr`43/`peer_ip`16) — peer-gossip table, highest value. | `file_offer.c:83-97` |
| **cc-blob-swap** | P2 | **DONE 2026-07-01.** `AR_READ_BLOB` for `secret_hash`/`secret`/`funding_txid`; redeem_script now clamps to `rlen<=256 && col12_bytes>=rlen`. | `swap_contract.c:125-154` |
| **cc-blob-znam** | P3 | **DONE 2026-07-01.** `AR_READ_BLOB` ×2 txids. | `znam.c:159-165` |
| **cc-blob-zmsg** | P3 | **DONE 2026-07-01.** `AR_READ_BLOB` ×2 (`msg_id`/`txid`). | `zmsg.c:83-101` |
| **cc-blob-asan-test** | P2 | **DONE 2026-07-01.** `test_blob_read_bounds.c` inserts short/NULL blobs via raw sqlite3, lists/finds via public model APIs, and asserts zeroed fields. | `lib/test/src/test_blob_read_bounds.c` |
| **cc-blob-lint** | P3 | **DONE 2026-07-01.** Hard gate `check_blob_read_bounds.sh` rejects raw fixed-size `column_blob` + `memcpy` without a byte guard. | `tools/lint/check_blob_read_bounds.sh` |

### Lane F — AR write-lifecycle  *(P2/P3, no risk)* — **ordering-coupled**
> **Ordering law:** wrap the sites (a, b) **before** widening the lint roots (c),
> or the gate goes red on the 23 unwrapped sites.

| id | sev | fix | files |
|---|---|---|---|
| **cc-arwrite-a** | P3 | **DONE 2026-07-01.** Renamed `AR_STEP_ROW_READONLY`→`AR_STEP_WRITE` on 4 confirmed write stmts (macro-identical, intent only). | `sync_controller_blocks.c`; `utxo_recovery_backfill.c`; `utxo_import_pipeline.c`; `block_index_loader.c` |
| **cc-arwrite-b** | P2 | **DONE 2026-07-01.** The 23 `config/` + `src/` step sites now use `AR_STEP_WRITE` for writes and `AR_STEP_ROW_READONLY` for SELECTs. | `config/src/boot.c`; `boot_address_backfill.c`; `boot_index.c`; `boot_services.c`; `src/main.c` |
| **cc-arwrite-c** | P2 | **DONE 2026-07-01.** Added `config/ src/` to `check_raw_sqlite.sh` grep roots after conversions. | `tools/scripts/check_raw_sqlite.sh` |
| **cc-arwrite-exec** | P2 | **DONE 2026-07-01 (narrow DML gate).** node.db `INSERT`/`DELETE`/`UPDATE`/`REPLACE` via direct `sqlite3_exec` is banned by `check_raw_sqlite.sh`; converted import/bootstrap/cache rebuild writes use `ar_exec_write_sql()` / `AR_STEP_WRITE`. Transaction/schema/projection/progress-store execs remain intentionally out of scope. | `src/main.c`; `boot_services.c`; `boot_snapshot_import.c`; `block_index_loader.c`; `check_raw_sqlite.sh` |

### Lane G — DRY / parity tests  *(P2/P3, consensus-adjacent → copy-prove)*
| id | sev | fix | files | gate |
|---|---|---|---|---|
| **cc-mostwork** | P2 | **DONE 2026-07-01.** `select_most_work_eligible()` is the shared filters + ancestry walk + below-tip refusal + tie-break called by both sites; `find_most_work_chain()` keeps only gap-fill/logging side effects. No live deployment attempted from this lane. | `process_block_core.c`; `chainstate.c`; `test_most_work_selector.c` | `most_work_selector`, `invalidateblock`, `active_chain_extend`, `make lint`, full `make test`; **copy-prove H\* CLIMB before live** |
| **cc-ccoins** | P3 | **DONE 2026-07-01.** Do **not** fold. `test_ccoins_decoder_kat` proves the 3 decoders agree on hand-built CCoins blobs (coinbase, vout0-only, multi-mask-byte, nSize 0..5, raw>6). The KAT caught and fixed the import decoder's nSize 4/5 compressed-vs-uncompressed P2PK divergence. | `lib/test/src/test_ccoins_decoder_kat.c`; `utxo_import_pipeline.c` | `ccoins_decoder_kat`; adjacent import/legacy/coins-math/snapshot gates |

### Lane H — TESTED gap  *(P1→P3)* — ship #1 standalone first
The review's weakest dimension. Four new `test_<name>.c`, each registered in
**both** `test_parallel.c` (TEST_LIST X-macro) **and** `test.c` (dispatch) or the
parallel runner silently skips. All deterministic (no datadir / network / `~/.zcash-params`).

1. **`test_sprout_phgr13_kat`** (P1) — **DONE 2026-06-30.** Real PHGR13 verifier (`bn254.c:2143 sprout_verify_phgr13`) is exercised by **zero** tests today (`test_snark_kat` explicitly excludes BN254; `test_proof_validate_stage` uses a fake verifier; `test_phgr13_fix` only pins the VK parser and SKIPs if params absent). Embed the 1449-byte VK + one real 296-byte proof + JoinSplit fields as a byte-array fixture. Positive→`true`; flip a proof byte→`false`; flip a nullifier→`false`. Catches both always-reject **and** always-accept.
2. **`test_snapshot_apply_coins_kv`** (P2) — **DONE 2026-06-30.** The real apply (`boot_refold_staged.c:315 mint_load_record_cb` → `coins_kv`, terminal `commitment==checkpoint && count==utxo_count` assert) is now pinned through `boot_snapshot_apply_to_coins_kv`: good→exact-N + matching SHA3; wrong expected count→rollback; tampered→refuses, leaves `coins_kv` empty.
3. **`test_gap_fill_frontier_window`** (P2) — **DONE 2026-06-30.** The small seam (`gap_fill_compute_window()` + `gap_fill_window_walk_start()`) pins that the refill window anchors at the **reducer frontier**, excludes HAVE_DATA, caps at window, and starts from the connectable bottom window. Fails if it anchors at active tip (re-introduces the tip>frontier wedge).
4. **`test_boot_snapshot_drop_bodiless`** (P3) — **DONE 2026-07-01.** Seam on `boot_snapshot_drop_bodiless_have_data_above_seed` (`boot_refold_staged.c`). Above/below-seed bodiless rows lose `BLOCK_HAVE_DATA`, validity is floored to `BLOCK_VALID_TREE`, disk coordinates/tx count are cleared, **seed is protected**, and present non-empty blk files are retained.

---

## Owner decision (0)

- The prior `cc-arwrite-exec` owner decision is resolved for the high-value
  review slice: node.db DML exec is covered by lint and converted. A future
  broader policy for transaction-control/schema/projection `sqlite3_exec` calls
  should be opened only if a concrete bug class appears; do not reopen the
  review-remediation queue for that by default.

## Off this queue (do not start)

- Framework/architecture refactor — off the v1 path (`FORWARD_PLAN.md:208`).
- The #1 spine itself (CI-enforce MVP + sovereign cure) — higher priority than
  this entire lane; this is hardening, not the mission.
