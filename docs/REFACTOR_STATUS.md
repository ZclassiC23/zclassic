# Refactor Status — Purpose-Per-File Finish Board

> Updated 2026-07-14 (file-size ratchet count re-verified against code; the
> rest of this file is the 2026-06-03 board — treat other
> specific line counts as potentially stale until re-verified). This file is
> the current debt board for finishing the framework refactor.
> `docs/FRAMEWORK.md` remains the architecture.
>
> ⚠️ **This is the ARCHITECTURE-AXIS board (~90% done). It is NOT the v1 path.**
> The v1 bar is [`docs/MVP.md`](./MVP.md); THE plan is
> [`docs/work/FORWARD_PLAN.md`](./work/FORWARD_PLAN.md). Do not start
> architecture work until the v1 buckets are clear.
>
> **The sealed `core/` tree has landed** since this board was last written
> in full and is not reflected in the table below: `domain/consensus/`,
> `lib/consensus/`, the pure-math subset of `lib/core/`, and the pure
> params/verify subset of `lib/chain/` were absorbed into a single sealed
> `core/` tree (`core/consensus/`, `core/params/`, `core/math/`,
> `core/chainparams/`), pinned by `core/MANIFEST.sha3` and the now-HARD
> `check-core-seal` gate. See
> [`docs/adr/0002-sealed-consensus-core.md`](adr/0002-sealed-consensus-core.md)
> for the full rationale and verification notes. This directly touches
> Rank 3 of the table below ("`domain/` fronted by thin `lib/` wrappers")
> and should be re-scoped against the new `core/` layout rather than the
> pre-split `domain/`/`lib/` paths next time this board is revised in
> full.

## 2026-06-03 — Architecture conformance board (the "everything has a place" axis)

Full-tree audit (read from the actual tree + Makefile, not prose). The `app/`
layer is already conformant; the remaining purpose-per-file debt is concentrated
and named below. This axis is **independent of the §3 live-tip runtime cluster**
(which is owner-gated, validate-on-copy).

**Verified-clean facts:**
- 4 layers, dependency direction proven clean: `domain/` #includes `app/`+`lib/` **0×**
  (pure core); `app/` depends inward on `domain|ports|application` **18×**.
- 10 of 11 lint baselines = **0 entries** (shape/result-type/sqlite/supervisor/
  blocker/layering gates pass with zero grandfathered exceptions). The one
  non-empty baseline is file-size: as re-counted 2026-07-14,
  `tools/scripts/file_size_ceiling_baseline.txt` carries **17 grandfathered
  entries** — **14 under `app/`** (largest: `app/views/src/explorer_pages_hodl.c`
  1853, `app/controllers/src/name_controller.c` 1034,
  `app/controllers/src/swap_controller.c` 1013) and **3 under `config/src/`**
  (`boot.c` 3949, `boot_refold_staged.c` 2107, `boot_services.c` 1634).
  The earlier "2 entries / `app/` has no baseline entry" claim is stale —
  do not repeat it without re-counting the baseline file. (A 2026-07-11
  note also listed `config/src/boot_background_workers.c` as baselined;
  that entry no longer exists — the file is 678 lines, under the 800
  ceiling, so it correctly has no baseline line.)

**Remaining debt, ranked (tracked as session Waves A–E):**

| Rank | Item | Real numbers | Target ("the zclassic23 way") | Wave |
|------|------|--------------|-------------------------------|------|
| 1 | `config/` boot monolith | boot.c **3949**, boot_services.c **1634**, boot_refold_staged.c **2107** (re-measured 2026-07-12; earlier splits — `boot_snapshot_failure_memory`, `boot_postmortem`, `boot_datadir_lock`, `boot_shutdown_marker`, `boot_stale_locks`, `boot_blocktree_cleanup`, `app_context`, `boot_legacy_blocks`, `boot_memory_guard`, `boot_flyclient` — landed 2026-06-30) — GATED (E1 covers app/ AND config/; baseline entries remain grandfathered in `file_size_ceiling_baseline.txt` and must not be raised) | continue only behavior-preserving extractions that reduce real coupling; larger moves need explicit seam design. Verified ordered plan: `docs/work/archive/boot-decomposition-seams.md` | D |
| 2 | Storage-adapter seam — **RESOLVED-CLOSED (outbound-only by design)** | `check_raw_sqlite.sh` reports CLEAN, empty allowlist; outbound persistence adapters are real and wired (`adapters/outbound/persistence/`: 13 ports + 13 sqlite impls, writes out through swappable ports — Law 2); the inbound "repository" adapter layer deliberately does NOT exist (Models ARE storage / own reads — Law 5), the same by-design absence as the deleted `app/events/` folder (Rank 5); the 49 raw-sqlite app/ sites are all legit: Models ARE storage (AR internals), Jobs use progress-kv kernel store, Views are read-only introspection | NOT a migration — closed by design; optional future read-only chain_state port stays optional. FRAMEWORK §3 row 8 documents the outbound-only rule. | E (resolved/closed) |
| 3 | `domain/` fronted by thin `lib/` wrappers | divergent duplicate-name pairs both compile: base58 (38 vs 151), bech32 (24 vs 164), upgrades (122 vs 233) | migrate callers to `domain/`, delete the `lib/` wrapper, seal with `test_domain_*` | A |
| 4 | Supervisor shape partial | only net/chain/staged_sync declared (6 .c); rest hand-wired in boot_services.c | folds into Rank 1 | D |
| 5 | `app/events/` deleted 2026-07-17 (was empty, 0 files) | "reserved" shape; event primitives live in `lib/storage/event_log.c` | **RESOLVED-CLOSED — folder deleted, not just kept empty** (audit: no misplaced Event code, ever; concept owned by `lib/event/` + `lib/storage/event_log` + projections; lone app subscriber is a Service). A placeholder folder that will never hold code serves no purpose — deleted rather than kept reserved-and-empty. Removed from Makefile `APP_DIRS` and every lint gate that enumerated it; FRAMEWORK §3 row 7 documents the shape as concept-only (no `app/` folder). | — |
| 6 | Controller/Service legacy compat | baselines 0 (no NEW violations); import/sync controllers still orchestrate; services keep bare-bool compat APIs | subtraction, not new structure | B/C |

Wave B = split the files at the 800 ceiling (the current inventory is the
17-entry baseline above). Wave C (rename `*_controller`/`*_repository` →
`*_service` under `app/services/`) is **DONE** — verified 2026-07-12:
`app/services/src/` contains zero `*_controller.c`/`*_repository.c` files,
and the lint gate `check-framework-filename-suffix`
(`tools/lint/check_framework_filename_suffix.sh`, wired into `make lint`)
is the recurrence guard. Waves A/D/E are deeper REFACTOR_STATUS items
tracked in the table above.

**Landed waves (historical):**

- **Wave A (`41174e498`)** — base58 + bech32 lib wrappers collapsed into the domain
  core (callers → `domain_encoding_*`, 4 wrapper files deleted). The 3rd "duplicate"
  `upgrades.c` is **correctly KEPT**: `lib/consensus/upgrades.c` is the sole definition
  of NetworkUpgradeInfo/SPROUT_BRANCH_ID/EquihashUpgradeInfo, `domain/consensus/
  upgrades.c` holds the pure activation-height arithmetic that reads them (correct
  layering, no collision). Rank 3 = 2 of 3 collapsed, 1 kept by design. `lib/keys/*` +
  `lib/consensus/params.c` are a future scoping item (not duplicated today).
- **Wave D / Rank 1 STEP 2 (`02d184c6d`)** — 5 process_block tip-publication hooks +
  gap-fill kick (pure adapters) moved verbatim to `config/src/boot_tip_hooks.c`, wired
  via `boot_register_process_block_hooks(svc)`. boot_services.c 4100 → 3926. Boot
  smoke-test on a datadir copy = behavior-neutral.
- **Wave D / Rank 1 STEP 3 (`08cb86586`)** — event_log + 10 per-domain reducer
  projections + open/close lifecycle moved verbatim to `config/src/boot_projections.c`
  (proven ZERO shared statics). Anchor-seed `node_db` now passed by parameter.
  boot_services.c 3926 → **3517** (4100 → 3517 cumulative). Boot smoke-test =
  behavior-neutral. Remaining boot units ranked: shutdown-phases (3 shared statics, low),
  msg-callbacks (1 shared, 26 fns), background-workers (3 shared, medium, 28 fns).
  STEP 1 (service-kernel adapters, ~720 lines) **deferred** — blocked by shared statics
  (`boot_profile_has_*`, `svc_clock_ms`) + cross-TU static-call seams; needs a seam
  design, not a pure move.

The STEP-3 boot smoke surfaced a **pre-existing P0 SIGSEGV** in
`push_getheaders_from` (`lib/net/src/msg_headers.c`, untouched by Wave D) — a corrupted
block index yields a garbage non-null `pprev`/`phashBlock` that the locator walk
dereferences. This is a §3-cluster (block-index integrity) symptom; now captured in
MEMORY + the `test_block_map_grow_phashblock` regression test. Wave-D boot smokes on a
wedged datadir will hit it — a Wave-D regression would crash in `config/boot_*`, not
`lib/net`.

## Objective

Finish the ZClassic23 refactor by deleting stale shadow/cutover and scaffold
code, making every remaining file match one clear framework shape and purpose,
shrinking all lint baselines to zero, updating docs to reflect the
authoritative reducer architecture, and proving it with clean tests plus a live
node soak.

## Current Truth

- The reducer/staged pipeline is the authoritative chain-advance architecture.
- The public cutover/projection-diff MCP/RPC apparatus has been removed.
- The legacy block-connect engine (`lib/validation/src/connect_block.c`, 806
  LOC) still ships and is live-called on the reindex/recovery path
  (`config/src/boot_index.c:334`). The staged reducer is the authoritative
  chain-advance engine; deleting the legacy `connect_block` path is tracked
  cleanup, not done (FRAMEWORK.md §2 states the same).
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
  `B8`, `DRIVER-FOLLOWER` — are therefore obsolete for that plan and no
  longer name any live behavior. The central scrub landed 2026-07-13: the
  `B3` hits in `lib/storage/src/coins_view_kv.c`,
  `lib/storage/src/coins_view_projection.c`, and
  `lib/storage/src/utxo_projection.c`, plus the `B8` hit in
  `tools/scripts/one_write_path_baseline.txt` (and its gate script
  `tools/scripts/check_one_write_path.sh` / the Makefile's E6 comment), are
  gone. The canonical author-default is `UTXO_AUTHOR_STAGE`
  (`UTXO_AUTHOR_LEGACY` is the test-only emitter path), and the comments in
  `app/jobs/src/utxo_apply_delta.c` and
  `lib/storage/include/storage/utxo_projection.h` say so. **Not** the same
  label: `app/services/src/oracle_policy.c` and
  `tools/scripts/check_lag_slo_observable.sh` also say `B8`, but that `B8` is
  a distinct, still-live item number from `docs/work/never-stuck-plan.md`
  ("demote the oracle_policy halt path to evidence-only") — leave those.

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

`tools/scripts/file_size_ceiling_baseline.txt` has **17 grandfathered
entries** (re-counted 2026-07-14): 14 under `app/` and 3 under
`config/src/`. The largest review targets are `config/src/boot.c` (3949),
`config/src/boot_refold_staged.c` (2107), `config/src/boot_services.c`
(1634), and `app/views/src/explorer_pages_hodl.c` (1853). The baseline
remains a ratchet and should not be raised; shrink these files through
behavior-preserving splits or explicit seam designs until the gate can go to
zero.

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
