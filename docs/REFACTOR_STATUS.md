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

