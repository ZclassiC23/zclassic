> **ARCHIVED / SUPERSEDED.** Superseded by docs/work/FORWARD_PLAN.md (THE plan). See `docs/work/ROADMAPS.md` for the live roadmap index. Kept for history — do not act on this as current.

# Next-Wave Improvement Plan — zclassic23 (DRY / security / test)

Read-only Opus pass, 2026-07-09. Launch AGAINST freshly-integrated main (after the
7-lane integration lands). Haiku-wrapped Codex GPT-5.5 xhigh per lane, Sonnet verify.

## Already RESOLVED — do NOT propose
- gettimeofday→monotonic sweep (0 non-test occurrences; platform_time_monotonic_us in use).
- most-work-chain selector dup (find_most_work_chain + active_chain_most_work_candidate both delegate to select_most_work_eligible, chainstate.c:753).
- MMB peak-merge dup (one mmb_merge_after_insert, mmb.c:108).

## Ranked candidate lanes (all file-sets disjoint from the in-flight sim/P0/registration lanes)

### 1. CCoins/coins-record decoder unification (DRY + security, M) — LAUNCH #2
Files: lib/storage/src/coins_db.c, lib/storage/src/chainstate_legacy_reader.c (decode_record), app/services/src/utxo_import_pipeline.c, app/services/src/node_db_import_service.c, app/services/src/block_index_loader_torn_gate.c; NEW lib/storage/src/coins_record_codec.{c,h}. One wire format hand-decoded 4×; consolidating collapses drift + fixes 3 silent-truncation findings (coins_db 4096-vout cap; utxo_import_pipeline short-value→height=0; node_db_import 65535B truncation) into one fail-closed decoder. Behavior-preserving. CONSENSUS-ADJACENT — byte-for-byte preserving, parity inviolable; malformed handling may only move toward fail-closed. NOTE node_db_import_service.c ≠ in-flight node_db_catchup_service.c.

### 2. Nullifier activation-gap backfill (security, L, owner-gated) — LAUNCH #1
Files: app/jobs/src/utxo_apply_nullifiers.c, lib/storage/src/nullifier_kv.c, NEW owner-gated backfill walker under app/services/src/. The live node flags this (review_required_nullifier_backfill_gap, cursor=3155843). Durable Sprout/Sapling nullifier set only populated by blocks applied after table creation → on cold/snapshot datadir every nullifier ≤cursor is ABSENT → pre-activation shielded double-spend ACCEPTED here but REJECTED by zclassicd (opposite-verdict divergence). Real exposure but fail-open + self-only (can be forked off net; cannot make honest net accept invalid). Detector already ships (permanent blocker utxo_apply.nullifier_backfill_gap). Missing = the FIX: a populate-only walker re-extracting nullifiers from already-validated historical blocks below the cursor, clearing the marker. CONSENSUS-ADJACENT: populate-only = parity-restoring, NOT a rule change; MUST NOT modify utxo_apply_check_and_insert_nullifiers; owner-gated + copy-prove before live.

### 3. Peer-gossip blob memcpy hardening (security, S) — LAUNCH #3
Files: app/models/src/swap_contract.c (redeem_script memcpy ~152), file_offer.c, znam.c, zmsg.c (+ lib/znam/src/znam.c, lib/net/src/zmsg.c readers). Copy 16/32/43-byte fixed fields from peer gossip rows without column_bytes checks → OOB read. Mechanical swap to existing checked AR_READ_BLOB macro (activerecord.h). No consensus risk (gossip/market/messaging models).

### 4. printf→LOG_* in boot/recovery/validation + MCP-stdio protection (DRY/correctness, M)
Files: lib/validation/src/process_block_core.c (33/50/82), app/services/src/block_index_loader.c (~24), chain_restore_repair.c (~7), lib/sapling/src/params_init.c, app/views/src/explorer_stats_view.c. Raw stdout from lib/service code corrupts -mcp stdio JSON protocol (params_init worst) + hides diagnostics from node.log. No logic change.

### 5. Explorer HTML error-page + read-only DB-open boilerplate unify (DRY + security, M)
Files: app/views/src/explorer_{pages,stats,block,main,factoids}_view.c, app/controllers/src/blog_controller.c, wallet_view_emit.c; onto existing explorer_open_readonly_db() + one shared error emitter. ~9 copy-pasted HTML error pages on the public Tor explorer; one place to fix uninit-out-buffer OOB reads (explorer_controller.c:254).

### 6. Legacy RPC oracle credential/envelope boilerplate consolidation (DRY, S-M)
Files: lib/rpc/src/legacy_chain_oracle.c (5 fns), legacy_header_client.c, client.c. One shared authenticated-oracle-call helper; also fixes client.c:198 unchecked-snprintf Content-Length/body mismatch over-read.

## NOT proposed (collision avoidance)
- MCP unsafe-JSON-builder consolidation (meta_controller.c:664) — DRY-MCP lane owns that surface.
- wallet spent-set/rescan correctness — active wallet-P0 work.
- anything in node_db_catchup_service.c.

## Launch order: #1 nullifier backfill (live-flagged, owner-gated, start early) → #2 CCoins decoder (best DRY/security ratio; audited decoder de-risks #1) → #3 blob memcpy (fast S, zero consensus risk, early green). All lead with subtraction.
