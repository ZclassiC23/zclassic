# Code-Review Remediation Backlog (2026-06-30)

> **What this is.** The *open, actionable* subset of the 2026-06-27 full audit
> ([`code-review-2026-06-27.md`](./code-review-2026-06-27.md), 322 findings,
> 0 crit / 22 high), re-verified against live code on 2026-06-30 and turned into
> execution-ready batches with a fix + proof-gate for every item.
>
> **Where it sits.** This is a **secondary hardening lane**. It does **not**
> displace the #1 spine ([`FORWARD_PLAN.md`](./FORWARD_PLAN.md): CI-enforce MVP +
> the sovereign `-refold-from-anchor` cure). MVP = 4/8; that gate is sovereign-cure
> + soak, not this list. Run this lane in parallel worktrees when the spine isn't
> blocked, or hand individual lanes to workers.

## Status since the review landed

Of the 12 file:line-confirmed HIGH/quick-win bugs, **9 are fixed** (verified in
code 2026-06-30 — note: the memory claim that "items 1–8 shipped" was wrong on
item 5; this pass closed qw9/qw10). The 96 post-review commits went heavily into god-function decomposition
(33 `split`/`audit: split`) and live-wedge liveness (~37 sync/boot/cure commits —
the node now holds tip with 0 blockers). The review's two weakest dimensions —
**DRY** and **TESTED** — were barely advanced. This doc is the remainder.

```
quick-win bug fixes   ███████████████░░░░░  9/12   (qw5,12 open; qw11 reclassified)
cross-cutting cleanups ██████░░░░░░░░░░░░░░  ~30%   (0 done, 4 partial, 3 not started)
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

## Trap-check results (the most important planning output)

Reading `AGENT_TRAPS.md` + surrounding code **reclassified three "bugs"** — do
**not** let the next push chase these as defects:

| Item | Review said | Verified verdict | Action |
|---|---|---|---|
| **qw11** `wallet_view_sync` fabricated sapling crypto | HIGH fund-loss | **INTENTIONAL scaffolding** — fake `ivk` from `SHA256(txid+outindex)` never matches the real key's `ivk`, so notes are structurally non-spendable by the spend/balance filter. No funds at risk. | Document + pin inertness (P3), don't "fix into a spend path" |
| **cc-ccoins** CCoins decoder ×3 | DRY: single-source | **INTENTIONAL** — the bounds/output split is load-bearing (`coins_db.c:116-123` forbids folding; trusted-import vs untrusted-DoS-cap differ) | Add a cross-decoder KAT instead of folding (P3) |
| **cc-mostwork** selector ×2 | DRY: unify | **INTENTIONAL today (no divergence)** but recommend extracting the shared *eligibility predicate* — divergence already wedged this project once | P2, behavior-preserving, copy-prove H\* CLIMB |

And the trap-check **found a sub-bug the review missed**: `swap_contract.c`
`redeem_script` has a genuine two-column OOB — `rlen` (col 13) bounds the memcpy
but is never checked against col 12's actual byte count (col13=256 + 4-byte col12
over-reads 252 B). Folded into **cc-blob-swap** below.

---

## Priority tiers

- **P0** — none. Node is at tip, no live blocker.
- **P1** — closed in this branch: **qw10, qw9, tested-gap #1 (PHGR13 KAT)**.
- **P2** — silent corruption / class-cure: **qw5, qw12, cc-gtod (timing class), cc-blob, cc-arwrite, cc-mostwork, tested-gap #3**.
- **P3** — hygiene / documentation: **qw11, cc-ccoins KAT, cc-arwrite renames, tested-gap #4**.

## Restart command

Give the next developer this exact goal:

```text
continue zclassic23 hardening from docs/work/code-review-remediation-2026-06-30.md; start at TESTED gap #3 by extracting and testing gap_fill_compute_window(), then continue with qw12, qw5, cc-blob, cc-mostwork, timing cleanup, and remaining P3 tests/docs. Keep changes scoped, update the remediation doc, run focused tests plus make test.
```

## Completed first moves

The original opening batch is done in this branch:

1. **qw9 + qw9-callers** — RPC output buffer guards and caller failure handling.
2. **qw10** — fail-loud on diverged Sapling tree.
3. **tested-gap #1** — Sprout PHGR13 real-proof KAT.
4. **tested-gap #2** — production snapshot→coins_kv apply transaction KAT.

## Next move

Start with **`test_gap_fill_frontier_window`**. Production
`gap_fill_service.c` already anchors the refill window at `body_fetch_stage_cursor() - 1`
when that frontier is behind the active tip, but the logic is embedded inside
`gap_fill_pass()`. Extract the pure window calculation into
`gap_fill_compute_window()` and test:

1. active tip ahead of reducer/body-fetch frontier anchors at the frontier, not
   active tip;
2. entries with `BLOCK_HAVE_DATA` are excluded by the queue-selection path;
3. large gaps cap at `GAPFILL_WINDOW` and choose the connectable bottom window.

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
| **qw11** | P3 | Keep the placeholder (intentional). Add a `source='view'` marker so a spend attempt returns "view-only balance synced from zclassicd" not a confusing empty-notes error; guard `db_sapling_note_replace_all` from clobbering real catchup notes; record in `AGENT_TRAPS.md`. | `wallet_view_sync.c:69-102,250-270` | regression test: placeholder `ivk` ≠ real keystore `ivk`; note not returned by `..._for_ivk(real_ivk)` |

### Lane B — explorer OOB  *(P1 + P3, no risk)*
| id | sev | fix | files | gate |
|---|---|---|---|---|
| **qw9** | P1 | **DONE 2026-06-30.** **Root fix = terminate at entry.** Top of `rpc_call()`: `if (!out||outmax==0) return -1; out[0]='\0';` — every `-1` path then leaves a valid empty C-string, so caller `strstr` parsers read one NUL not 64-256 KB of uninit stack. `outmax==0` guard also closes a latent `outmax-1` underflow. Mirror to sibling `wv_rpc_call`. | `explorer_controller.c:254-258` (+latent OOB `:313-318`); `wallet_view_helpers.c:92-102` | new `test_explorer_rpc_call.c`: dead port → assert `n==-1 && buf[0]=='\0'`; build under existing ASan config so a pre-fix `strstr` tail-run trips |
| **qw9-callers** | P3 | **DONE 2026-06-30.** Add `n<=0` guards at the 6 ignoring sites (mirrors the already-correct `_tx.c:56`/`_address.c:192`). Defense-in-depth on top of qw9. | `explorer_controller_dashboard.c:35,40,54,64`; `explorer_controller_block.c:49,61` | extend the same test: dead port → response == not-found view |

### Lane C — db import / open  *(P2)*
| id | sev | fix | files | gate |
|---|---|---|---|---|
| **qw5** | P2 | A coins `'c'` value is a whole-tx CCoins blob and can legitimately exceed 65535 B (multi-output payouts; the 413 grandfathered oversize txs up to 1.92 MB). **Widen** `value_len` `uint16_t`→`uint32_t`; replace the silent clip with a generous cap (~4 MB) that on exceed does `iter_error=true` + `request_stop` (hard ZCL_ERR, matches truncation doctrine), not a clip. | `utxo_import_pipeline.h:18`; `node_db_import_service.c:352,359` | unit test: synthetic CCoins >65535 B (~3000 outputs + nonzero height varint) decodes all outputs with correct heights; oversize → hard-fail |
| **qw12** | P2 | Capture `node_db_migrate`'s return at `database.c:486`; on `-2` (on-disk schema > `NODE_DB_MAX_SCHEMA`) `sqlite3_close` + `open=false` + `return false` (run before the staging DELETE so no transient writes hit a newer DB). Callers already handle a false open. | `database.c:485-490` | test: stamp `schema_version = MAX+1`, assert `node_db_open()==false` |

### Lane D — timing class (Gate #19)  *(P2/P3, no risk)* — **land as ONE PR**
> **Ordering law:** convert **every** site first, flip the ban regex **last** (same
> commit). The moment `gettimeofday` is banned, `make ci` runs `ZCL_LINT_MODE=FAIL`
> and any unconverted site red-bars. **Not all sites → monotonic** — wall-clock /
> epoch / cross-node-latency fields must stay realtime via a new helper.

| id | sev | fix | files |
|---|---|---|---|
| **cc-gtod-realtime-helper** | P3 | Add `platform_time_realtime_us()` (`= clock_now_wall_ms()*1000`) under the gate-exempt `lib/platform/` so wall-clock sites need **zero** `// platform-ok` markers. | `lib/platform/include/platform/time_compat.h:44` |
| **cc-gtod-elapsed-named** | P3 | 3 true elapsed deltas → `platform_time_monotonic_us()`. | `db_txn.c:40`; `mmb_leaf_store.c:174,216` |
| **cc-gtod-wall-named** | P3 | `cri_now_us` is **epoch** (`ts_us` since epoch, exposed to MCP) → realtime helper, **not** monotonic. | `consensus_reject_index.c:58` |
| **cc-gtod-p2p-pair** | P3 | `p2p_game` send-stamp + `msgprocessor` receive-latency are a **cross-node** pair → both realtime (monotonic zero-points differ → garbage latency). | `p2p_game.c:16,118`; `msgprocessor.c:586` |
| **cc-gtod-surfaced** | P2 | The ban surfaces ~13 more sites. Triage rule: deltas→monotonic; `*unix*`/`*epoch*`/filename/cross-node→realtime; comment prose→reword. Named: `event.c:288,958`, `wallet_backup_service.c:312`, `zclassicd_oracle_service.c:93` → realtime; `test_workpool/test_crypto/test_rpc_timeout`, `wallet_gui.c`, `zcl-browser.c` → monotonic. | (enumerate via grep below) |
| **cc-gtod-gate** | P2 | Add `\|\bgettimeofday\s*\(` to the regex; reword the one comment false-positive (`snapshot_sync_service.c:103`). | `tools/lint/check_no_raw_clock_outside_platform.sh:19` |

Final gate: `grep -rn 'gettimeofday' app lib config tools | grep -v '^lib/platform/'`
empty; `ZCL_LINT_MODE=FAIL tools/lint/check_no_raw_clock_outside_platform.sh`
exits 0; `make build-only && build/bin/test_parallel` green.

### Lane E — fixed-blob bounds (AR_READ_BLOB)  *(P2/P3, no risk)*
> **Ordering law:** 4 conversions + ASan test first; **freeze the lint baseline last**
> (else the ratchet trips on the very offenders being fixed).

| id | sev | fix | files |
|---|---|---|---|
| **cc-blob-fileoffer** | P2 | `AR_READ_BLOB` ×3 (`root_hash`32/`z_addr`43/`peer_ip`16) — **peer-gossip table, highest value.** | `file_offer.c:83-97` |
| **cc-blob-swap** | P2 | `AR_READ_BLOB` for `secret_hash`/`secret`/`funding_txid`; **+ fix the novel redeem_script two-column OOB** (`rlen` from col13 unchecked vs col12 bytes — clamp `rlen<=256 && col12_bytes>=rlen`). | `swap_contract.c:125-154` |
| **cc-blob-znam** | P3 | `AR_READ_BLOB` ×2 txids. | `znam.c:159-165` |
| **cc-blob-zmsg** | P3 | `AR_READ_BLOB` ×2 (`msg_id`/`txid`). | `zmsg.c:83-101` |
| **cc-blob-asan-test** | P2 | New `test_blob_read_bounds.c`: insert short/NULL blobs via raw sqlite3, list via public API, assert zeroed + no ASan OOB. Self-proving (RED on unconverted, GREEN after). | `lib/test/src/test_blob_read_bounds.c` (new) |
| **cc-blob-lint** | P3 | Ratchet `check_blob_read_bounds.sh` (raw `column_blob`+fixed `memcpy` w/o a `column_bytes` guard) — **freeze baseline after conversions.** | `tools/lint/` (new) |

### Lane F — AR write-lifecycle  *(P2/P3, no risk)* — **ordering-coupled**
> **Ordering law:** wrap the sites (a, b) **before** widening the lint roots (c),
> or the gate goes red on the 23 unwrapped sites.

| id | sev | fix | files |
|---|---|---|---|
| **cc-arwrite-a** | P3 | Rename `AR_STEP_ROW_READONLY`→`AR_STEP_WRITE` on 4 confirmed write stmts (macro-identical, intent only). | `sync_controller_blocks.c:329`; `utxo_recovery_backfill.c:113`; `utxo_import_pipeline.c:62`; `block_index_loader.c:529` |
| **cc-arwrite-b** | P2 | **Scope is 23 sites** (3 writes, 20 reads — many named "writes" are actually SELECT reads). Add `#include "util/ar_step_readonly.h"` to 5 files; `AR_STEP_WRITE` on 3 writes (`boot_address_backfill.c:118`, `main.c:1306,1356`), `AR_STEP_ROW_READONLY` on 20 reads. | `config/src/boot*.c`, `src/main.c` (enumerated in fix-design output) |
| **cc-arwrite-c** | P2 | Add `config/ src/` to `check_raw_sqlite.sh:62` grep roots — **last**. | `tools/scripts/check_raw_sqlite.sh:62` |
| **cc-arwrite-exec** | — | **OWNER DECISION** (see below): the `sqlite3_exec` import-path writes are *not* caught by the step-only lint; widening to cover them is a separate, larger piece. | `src/main.c:1141,1428-1453`; `boot_services.c:535` |

### Lane G — DRY / parity tests  *(P2/P3, consensus-adjacent → copy-prove)*
| id | sev | fix | files | gate |
|---|---|---|---|---|
| **cc-mostwork** | P2 | Extract `select_most_work_eligible()` (the shared filters + ancestry walk + below-tip refusal + tie-break) called by **both** sites; keep each site's documented side-effect difference (gap-fill kick / logging). Behavior-preserving. | `process_block_core.c:23-165`; `chainstate.c:731-782` | fixture block_map (failed / header-only / below-tip fork / equal-work-FAILED-incumbent) → both selectors return same index; **copy-prove H\* CLIMB before live** |
| **cc-ccoins** | P3 | Do **not** fold. Add a cross-decoder KAT proving the 3 decoders agree on hand-built CCoins blobs (coinbase, vout0-only, multi-mask-byte, nSize 0..5, raw>6). | `lib/test/` (new KAT) | KAT green; catches future wire-format divergence |

### Lane H — TESTED gap  *(P1→P3)* — ship #1 standalone first
The review's weakest dimension. Four new `test_<name>.c`, each registered in
**both** `test_parallel.c` (TEST_LIST X-macro) **and** `test.c` (dispatch) or the
parallel runner silently skips. All deterministic (no datadir / network / `~/.zcash-params`).

1. **`test_sprout_phgr13_kat`** (P1) — **DONE 2026-06-30.** Real PHGR13 verifier (`bn254.c:2143 sprout_verify_phgr13`) is exercised by **zero** tests today (`test_snark_kat` explicitly excludes BN254; `test_proof_validate_stage` uses a fake verifier; `test_phgr13_fix` only pins the VK parser and SKIPs if params absent). Embed the 1449-byte VK + one real 296-byte proof + JoinSplit fields as a byte-array fixture. Positive→`true`; flip a proof byte→`false`; flip a nullifier→`false`. Catches both always-reject **and** always-accept.
2. **`test_snapshot_apply_coins_kv`** (P2) — **DONE 2026-06-30.** The real apply (`boot_refold_staged.c:315 mint_load_record_cb` → `coins_kv`, terminal `commitment==checkpoint && count==utxo_count` assert) is now pinned through `boot_snapshot_apply_to_coins_kv`: good→exact-N + matching SHA3; wrong expected count→rollback; tampered→refuses, leaves `coins_kv` empty.
3. **`test_gap_fill_frontier_window`** (P2) — needs a small seam (`gap_fill_compute_window()`). Assert the refill window anchors at the **reducer frontier**, excludes HAVE_DATA, caps at window. Fails if it anchors at active tip (re-introduces the tip>frontier wedge).
4. **`test_boot_snapshot_drop_bodiless`** (P3) — seam on `boot_snapshot_drop_bodiless_have_data_above_seed` (`boot_refold_staged.c:203`). Above/below-seed bodiless → HAVE_DATA cleared; **seed protected**.

---

## Owner decision (1)

- **cc-arwrite-exec** — the AR-bypass the review flagged on `sqlite3_exec` import
  writes (`src/main.c`, `boot_services.c`, the exec-heavy `boot_refold_staged.c` /
  `boot_snapshot_import.c`) is **not** caught by the step-only lint, and
  `AR_ADHOC_SAVE` doesn't apply (no model record — `AR_EXEC` is the only fit).
  Decide: leave exec uncovered (current intentional design) **or** widen the lint
  to scan `sqlite3_exec` writes (separate, larger task). Everything else in this
  doc is autonomous.

## Off this queue (do not start)

- Framework/architecture refactor — off the v1 path (`FORWARD_PLAN.md:208`).
- The #1 spine itself (CI-enforce MVP + sovereign cure) — higher priority than
  this entire lane; this is hardening, not the mission.
