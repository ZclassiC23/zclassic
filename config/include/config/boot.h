/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_INIT_H
#define ZCL_INIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum zcl_runtime_profile {
    ZCL_RUNTIME_FULL = 0,
    ZCL_RUNTIME_ZCLASSIC_ONLY,
    ZCL_RUNTIME_EXPLORER,
    ZCL_RUNTIME_ONION_NODE,
    ZCL_RUNTIME_LEGACY_COMPAT
};

enum zcl_operator_lane {
    ZCL_OPERATOR_LANE_UNKNOWN = 0,
    ZCL_OPERATOR_LANE_CANONICAL,
    ZCL_OPERATOR_LANE_SOAK,
    ZCL_OPERATOR_LANE_DEV,
    ZCL_OPERATOR_LANE_TEST,
    ZCL_OPERATOR_LANE_COPY
};

struct app_context {
    const char *datadir;
    const char *params_dir;
    bool testnet;
    bool regtest;
    bool daemon;
    bool gen;
    int gen_threads;
    const char *miner_address;
    int rpc_port;
    int p2p_port;
    int https_port;
    int fs_port;
    const char *rpc_user;
    const char *rpc_password;
    bool listen;
    bool tx_index;
    bool checkpoints_enabled;
    enum zcl_runtime_profile runtime_profile;
    enum zcl_operator_lane operator_lane;
    bool sapling_scan;
    const char *snapshot_dir;
    bool reindex_chainstate;
    bool refold_staged;        /* -refold-staged : reset the 8 staged-reducer
                                 * cursors + their per-height *_log rows DOWN to
                                 * genesis so the staged pipeline (header_admit..
                                 * tip_finalize) re-folds forward over on-disk
                                 * block BODIES, writing the per-height log rows
                                 * reducer_frontier folds into H*. Unlike
                                 * -reindex-chainstate (connect_block: rebuilds
                                 * the node.db utxos mirror but writes NO staged
                                 * logs, so H* pins at the checkpoint), this
                                 * rebuilds the folded log the frontier authority
                                 * actually reads. Also marks refold_in_progress
                                 * (progress.kv) so the L0 frontier floor drops to
                                 * 0 and the below-anchor self-repair is suspended
                                 * while the fold re-walks the frozen prefix; the
                                 * mark clears once utxo_apply crosses the anchor.
                                 * Requires -nolegacyimport. */
    bool refold_from_anchor;   /* -refold-from-anchor (B2) : like -refold-staged
                                 * EXCEPT the staged reducer is reset to the SHA3
                                 * UTXO checkpoint ANCHOR (3,056,758), not genesis.
                                 * FULL-resets coins_kv, re-seeds the anchor set
                                 * from node.db's `utxos` mirror, HARD-ASSERTS the
                                 * set against the compiled checkpoint
                                 * (sha3_hash + count; FATAL on mismatch), forces
                                 * the 8 stage cursors to the anchor, and folds
                                 * forward over on-disk BODIES from the anchor to
                                 * the active tip running the REAL
                                 * script/proof/utxo_apply/tip_finalize stages.
                                 * Marks refold_from_anchor (progress.kv) so the L0
                                 * floor HOLDS at the anchor (not 0 as -refold-staged
                                 * does) and the below-anchor self-repair is
                                 * suspended until utxo_apply reaches the resume
                                 * target. Requires -nolegacyimport. */
    bool mint_anchor;          /* -mint-anchor : the ANCHOR-SET MINT. Reset the
                                 * staged reducer to GENESIS (like -refold-staged),
                                 * cap the fold at the compiled SHA3 UTXO checkpoint
                                 * anchor (h=3,056,758) via mint_fold_ceiling, fold
                                 * genesis..anchor over on-disk BODIES through the
                                 * REAL script/proof/utxo_apply/tip_finalize stages,
                                 * then write the resulting coins_kv set to a
                                 * SHA3-committed snapshot artifact (the loader's USS
                                 * format) and HARD-ASSERT its commitment+count ==
                                 * the compiled checkpoint (FATAL on mismatch). One-
                                 * shot: exits after writing. Requires
                                 * -nolegacyimport. Default false → never set on a
                                 * normal boot (no clamp). */
    bool mint_anchor_fast;     /* -mint-anchor-fast : OFFLINE FAST-MINT — the
                                 * SAME ANCHOR-SET MINT as -mint-anchor EXCEPT the
                                 * two crypto stages (script_validate's per-input
                                 * ECDSA verify_script loop and proof_validate's
                                 * Groth16/Ed25519/PHGR13/binding-sig) PASS THROUGH
                                 * without running (jobs/mint_skip_crypto.h). The
                                 * state-transition stage (utxo_apply) is UNCHANGED,
                                 * so the folded coins_kv set is IDENTICAL to the
                                 * full-validated fold, and the SAME terminal
                                 * SHA3==checkpoint + count hard-assert certifies
                                 * it (FATAL on mismatch — never writes a wrong
                                 * snapshot). HONORED ONLY when -mint-anchor is ALSO
                                 * set (refused otherwise at the argv parse); a
                                 * normal boot never sets it, so the skip-crypto
                                 * path is unreachable on a running node. OFFLINE
                                 * PRODUCTION ONLY, fingerprint-gated, NEVER a
                                 * node-boot signature bypass. Default false. */
    bool reindex_explorer;     /* -reindex-explorer : truncate the explorer
                                 * projection + on-chain ZNAM tables and rewind
                                 * the shared node.db catchup tip to genesis so
                                 * the 0..tip walk re-emits every projection row
                                 * (INSERT OR REPLACE). node.db only; never
                                 * touches coins_kv/progress.kv/consensus. */
    bool reimport_utxos;
    bool backfill_zslp;        /* -backfill-zslp : fast one-shot — clear the
                                 * zslp_* tables and re-derive tokens+transfers
                                 * from the existing op_returns(is_slp=1) rows
                                 * (no full reindex), then exit. node.db only;
                                 * never touches coins_kv/progress.kv/consensus. */
    bool tor;
    const char *defer_proof_validation_below;  /* block hash: defer Groth16 at/below this height */
    bool no_services;          /* skip P2P, RPC, Tor — boot only (speedrun) */
    const char *file_service_peer; /* -fileservice=addr : download from this peer */
    bool connect_only;         /* -connect= mode: only connect to addnodes, no seeds */
    bool no_file_sync;         /* -nofilesync : skip file service download, use P2P only */
    bool allow_clearnet_snapshot_fetch; /* -allow-clearnet-snapshot-fetch :
                                 * OPT-IN to auto-download a chainstate
                                 * (consensus_snapshot.db) from the HARDCODED
                                 * clearnet file-service seeds. OFF by default:
                                 * those seeds are unauthenticated (no TLS, no
                                 * in-binary root binding), so a MITM/forged seed
                                 * could otherwise seed a fabricated UTXO set on a
                                 * default cold start. With this off, a fresh node
                                 * falls back to safe P2P IBD or the operator
                                 * bundle (-load-snapshot-at-own-height). An
                                 * explicit -fileservice=PEER is always honored
                                 * (the operator chose that peer). */
    bool no_bg_validation;     /* -nobgvalidation : skip background proof verification */
    bool no_legacy_auto_import;/* -nolegacyimport : do not auto-read ~/.zclassic */
    bool boot_from_log;        /* -rebuildfromlog : rebuild block index + tip from
                                 * the event-log block_index_projection instead of
                                 * the legacy flat/SQLite/LevelDB loaders,
                                 * zclassicd-LDB, and UTXO importer. Opt-in;
                                 * default false so the live boot is unchanged. */
    const char *external_ip;   /* -externalip=IP[:PORT] : advertise endpoint to peers */
    const char *https_domain;  /* -httpsdomain=DOMAIN : TLS servername / redirect host
                                 * for the clearnet explorer. Optional; with a single
                                 * cert the server presents that cert regardless of SNI,
                                 * so NULL is fine (HTTP→HTTPS redirect then falls back
                                 * to the request's Host header). */
    bool allow_degraded;       /* -allow-degraded : continue past failed post-restore integrity check
                                 * (default false → boot FATALs on broken chain state). */
    int  par_workers;          /* -par=N : verification-engine worker count
                                 * (validation/thread_pool.c). 0 (default) =>
                                 * GetNumCores()-1, clamped >= 1. 1 => serial
                                 * (no worker threads; verify_queue runs inline).
                                 * ADDITIVE foundation — not yet wired into the
                                 * staged reducer / consensus path. */
    const char *load_snapshot_at_own_height; /* -load-snapshot-at-own-height=PATH :
                                 * EXPLICIT-ONLY recovery (NEVER fires on a normal
                                 * boot — NULL unless the operator sets the flag).
                                 * Seed coins_kv from a ZCLUTXO snapshot at the
                                 * snapshot's OWN header height, SELF-verified against
                                 * the file's OWN body SHA3 (uss_open verify_full_sha3
                                 * with expected_sha3=NULL) + hdr.count == records
                                 * parsed — NOT bound to the compiled checkpoint. Then
                                 * force the 8 stage cursors to hdr.height, set
                                 * applied=hdr.height+1, seed tip_finalize at
                                 * hdr.height with hdr.anchor_block_hash, and fold
                                 * FORWARD over on-disk bodies from there. For a
                                 * snapshot taken ABOVE the compiled checkpoint (does
                                 * not cross it). Owner-gated; FATAL-refuses if the
                                 * self-verify fails (never seeds an unproven set). */
    bool load_verify_boot;     /* -load-verify-boot : on a NORMAL boot, AUTO-DETECT
                                 * a baked, SHA3-verified anchor snapshot
                                 * (<datadir>/utxo-anchor.snapshot or
                                 * $ZCL_MINT_ANCHOR_OUT) and, when present + its
                                 * recomputed body SHA3 == the compiled checkpoint
                                 * AND coins_kv is NOT already the proven authority,
                                 * LOAD+VERIFY it into coins_kv at the anchor and
                                 * fold ONLY the anchor->tip delta (same machinery
                                 * as -refold-from-anchor, but reached without that
                                 * explicit flag). ADDITIVE + SAFE-FALLBACK: when no
                                 * snapshot is present, or its SHA3 mismatches the
                                 * checkpoint, or coins_kv is already proven, the
                                 * predicate is FALSE and the CURRENT proven boot
                                 * path runs unchanged (cold-import seed). NEVER
                                 * loads an unverified/mismatched set; NEVER silently
                                 * re-folds from genesis. Default false → a normal
                                 * boot runs its current path exactly. */
};

void app_context_defaults(struct app_context *ctx);
const char *app_runtime_profile_name(enum zcl_runtime_profile profile);
bool app_runtime_profile_parse(const char *name,
                               enum zcl_runtime_profile *out);
bool app_runtime_profile_has_explorer(enum zcl_runtime_profile profile);
bool app_runtime_profile_has_store(enum zcl_runtime_profile profile);
bool app_runtime_profile_has_onion(enum zcl_runtime_profile profile,
                                   bool tor_flag);
bool app_runtime_profile_has_file_service(enum zcl_runtime_profile profile);
const char *app_operator_lane_name(enum zcl_operator_lane lane);
bool app_operator_lane_parse(const char *name,
                             enum zcl_operator_lane *out);

bool app_init(struct app_context *ctx);
void app_shutdown(void);
void app_shutdown_offline(void);

/* -refold-staged (impl in config/src/boot_refold_staged.c): boot_refold_staged_init
 * caches the durable refold_in_progress signal before the reducer starts (thin
 * wrapper over refold_progress_boot_init; no-op on a normal boot). _reset wipes
 * the staged reducer's derived state to genesis so the pipeline re-folds forward
 * over on-disk block BODIES. Owner-gated; progress.kv + node.db mirror only. */
struct node_db;
void boot_refold_staged_init(bool refold_staged);
void boot_refold_staged_reset(struct node_db *ndb);

/* -refold-from-anchor (B2; impl in config/src/boot_refold_staged.c): sibling of
 * boot_refold_staged_reset that FULL-resets coins_kv, RE-SEEDS the SHA3-verified
 * anchor coin set from node.db's `utxos` mirror, HARD-ASSERTS it against the
 * compiled checkpoint (commitment + count; FATAL + _exit on mismatch), forces the
 * 8 stage cursors to the ANCHOR (not genesis), sets coins_applied_height =
 * anchor+1, and seeds the tip_finalize anchor — so the staged pipeline re-folds
 * forward over on-disk BODIES FROM the anchor running the REAL stages. Owner-gated
 * (-refold-from-anchor); progress.kv + node.db mirror only. Marks
 * refold_from_anchor in progress.kv (refold_progress.h) so the L0 floor holds at
 * the anchor and the below-anchor self-repair is suspended until the fold reaches
 * the resume target. */
void boot_refold_from_anchor_reset(struct node_db *ndb);

/* -load-snapshot-at-own-height=PATH (impl in config/src/boot_refold_staged.c):
 * EXPLICIT-ONLY recovery loader. Sibling of boot_refold_from_anchor_reset EXCEPT
 * the snapshot is SELF-verified against its OWN header SHA3 (uss_open
 * verify_full_sha3=true, expected_sha3=NULL) — NOT bound to the compiled
 * checkpoint — and the fold resumes at the snapshot's OWN header height, not the
 * compiled anchor. Used to seed coins_kv from a SHA3-internally-consistent
 * UTXO-set dump taken at a height ABOVE the compiled checkpoint (so it never
 * crosses the checkpoint and the anchor self-mint hook is irrelevant). The ONLY
 * trust gate is: body SHA3 == hdr.sha3_hash AND records_parsed == hdr.count; on
 * failure it LOG_FAILs and FATAL-refuses (never seeds). Forces the 8 stage
 * cursors to hdr.height, sets applied = hdr.height+1, seeds the tip_finalize
 * anchor at hdr.height with hdr.anchor_block_hash, then the staged pipeline folds
 * FORWARD over on-disk BODIES from hdr.height. Caller must have passed a non-NULL
 * `path` (from ctx->load_snapshot_at_own_height); a normal boot never calls it.
 *
 * CONSENSUS CROSS-CHECK: `ms` is the live main_state whose in-memory active
 * chain (populated by the prior block-index load) is consulted to bind the
 * snapshot's hdr.anchor_block_hash to the PoW-proven header at seed_h — a
 * mismatch is FATAL, so a self-consistent-but-FORGED snapshot can never seed
 * coins_kv. `ms` may be NULL only in unit tests with no chain loaded (the
 * binding is then skipped with a loud warning).
 *
 * `trust_existing_block_files` is true for legacy-import/datadir boots, where a
 * non-empty blk file is a trusted local body source. It is false for
 * `-nolegacyimport` snapshot boots, where copied or partial blk files must pass
 * per-block read/hash verification before their BLOCK_HAVE_DATA claim survives. */
struct main_state;
void boot_load_snapshot_at_own_height_reset(struct node_db *ndb,
                                            const char *path,
                                            const char *datadir,
                                            struct main_state *ms,
                                            bool trust_existing_block_files);

#ifdef ZCL_TESTING
size_t boot_snapshot_drop_bodiless_have_data_above_seed_for_test(
    struct main_state *ms, const char *datadir, int seed_h,
    bool trust_existing_block_files);
#endif

/* Testable production transaction used by both SHA3-verified snapshot seed
 * paths. The caller owns trust: `snapshot` must already be opened by uss_open()
 * with the intended SHA3 binding. This helper only applies the verified records
 * into coins_kv under ONE BEGIN IMMEDIATE, requires exactly expected_count
 * records, stamps COINS_KV_MIGRATION_COMPLETE_KEY in the same transaction, and
 * rolls back on any mismatch or insert/stamp/commit failure. */
struct sqlite3;
struct uss_handle;
struct boot_snapshot_apply_result {
    uint64_t inserted;
    int64_t emitted;
};
bool boot_snapshot_apply_to_coins_kv(struct sqlite3 *progress_db,
                                     struct uss_handle *snapshot,
                                     uint64_t expected_count,
                                     struct boot_snapshot_apply_result *out);

/* Zero-flag starter-pack bootstrap. Scan `datadir` for a starter-pack bundle —
 * block_index.bin (the PoW header index) plus a utxo-seed-<H>.snapshot (a
 * SHA3-self-verified, anchor-bound UTXO seed) — and return a malloc'd absolute
 * path to the snapshot the loader should seed from, or NULL when no usable
 * bundle is present. The highest-height snapshot wins; a snapshot with no
 * block_index.bin alongside it is declined (logged) because the loader needs the
 * header chain for its consensus anchor cross-check. The CALLER must gate on
 * coins_kv NOT yet being the proven authority so a synced node is never
 * re-seeded; the loader itself still self-verifies + anchor-binds, so this only
 * ever auto-selects a file the explicit -load-snapshot-at-own-height flag could
 * have loaded. Caller owns the returned string (free()). */
char *boot_autodetect_bundle_snapshot(const char *datadir);

/* FIX 3 seam — PURE consensus cross-check for the forged-snapshot FATAL inside
 * boot_load_snapshot_at_own_height_reset. The loaded snapshot's
 * hdr.anchor_block_hash MUST byte-equal this node's PoW-proven header hash at
 * the snapshot height; on mismatch the loader FATALs (refuses a forged /
 * wrong-chain snapshot) rather than seeding contaminated coins. Both inputs are
 * 32 raw bytes (internal little-endian). Returns true iff the snapshot is
 * consensus-bound to this chain (the two hashes are byte-identical). NULL on
 * either side is treated as a non-match (refuse). Behavior-identical to the
 * inline `memcmp(bi->hashBlock.data, hdr.anchor_block_hash, 32) == 0`. */
bool boot_snapshot_anchor_hash_matches(const unsigned char *index_block_hash,
                                       const unsigned char *snapshot_anchor_hash);

/* -mint-anchor (impl in config/src/boot_refold_staged.c): the ANCHOR-SET MINT
 * boot-time reset. Resets the staged reducer to GENESIS (delegates to
 * boot_refold_staged_reset) AND caps the fold at the compiled SHA3 UTXO
 * checkpoint anchor (mint_fold_ceiling_set), so the staged pipeline re-folds
 * genesis..anchor over on-disk BODIES and then converges AT the anchor. Marks
 * refold_in_progress (progress.kv) so the L0 floor drops to 0 while the fold
 * re-walks the frozen prefix. Gated at the call site on ctx->mint_anchor —
 * a normal boot never calls it (no reset, no clamp).
 *
 * `fast` (the -mint-anchor-fast OFFLINE FAST-MINT): when true this ALSO flips
 * the process-global mint_skip_crypto toggle (jobs/mint_skip_crypto.h) so
 * script_validate/proof_validate PASS THROUGH their per-block crypto. The state
 * fold is unchanged, so the minted coins_kv set is identical and the same
 * terminal SHA3==checkpoint hard-assert certifies it. `fast` is only ever true
 * here when the caller already confirmed ctx->mint_anchor — this TU is the sole
 * caller of mint_skip_crypto_set (lint-fenced), so the skip-crypto path cannot
 * be armed on a running node. */
void boot_mint_anchor_reset(struct node_db *ndb, bool fast);

/* -mint-anchor driver (impl in config/src/boot_mint_anchor.c): after app_init
 * has reset to genesis + capped the fold at the anchor, this DRIVES the staged
 * reducer synchronously (reducer_kick) until the utxo_apply frontier reaches the
 * anchor, then writes the resulting coins_kv set to a SHA3-committed snapshot
 * (default <datadir>/utxo-anchor.snapshot, or $ZCL_MINT_ANCHOR_OUT) and
 * HARD-ASSERTS the written commitment+count == the compiled checkpoint (FATAL +
 * _exit on mismatch — the h=478544 class: page, never proceed). Returns true on
 * a verified mint, false on a non-fatal driver problem (e.g. the fold did not
 * reach the anchor — bodies missing). Owner-gated; reads on-disk bodies +
 * coins_kv only. `datadir` is the active data directory. */
bool boot_mint_anchor_run(const char *datadir);
bool boot_mint_anchor_should_log_progress(int32_t applied_through,
                                          int32_t anchor);

/* B2 1c — boot torn-import AUTO-ARM (impl in config/src/boot_refold_staged.c).
 * Consults the pure detect predicate block_index_loader_torn_import_detect (no
 * side-effects); on a detected tear it ARMS a from-anchor refold
 * (boot_refold_from_anchor_reset + refold_progress_mark_started_from_anchor) and
 * returns true so the caller SKIPS block_index_loader_seed_stages_from_cold_import.
 * Idempotent (returns true without re-resetting when a from-anchor refold is
 * already armed). FATALs inside the reset if the re-seeded anchor set fails the
 * SHA3/count assert. Returns false when no tear is detected → the caller runs the
 * normal seed path, whose torn-import gate stays the EV_OPERATOR_NEEDED fallback.
 * Called UNCONDITIONALLY (no flag) on every boot from the single seed-vs-anchor
 * site (config/src/boot_services.c): a normal boot of a TORN datadir self-heals,
 * while a HEALTHY datadir returns false here and runs the normal seed path. Safe
 * flag-free because the pure detect predicate only fires on a durably proven tear.
 *
 * SAFETY DECLINE (no-snapshot honest-halt guard): when a tear IS detected but NO
 * SHA3-verified anchor snapshot is reachable (no file at the mint path, or its
 * body SHA3 != the compiled checkpoint, or count mismatch), this returns FALSE
 * WITHOUT calling the reset. The reset's node.db `utxos` fallback would re-seed
 * the CONTAMINATED tip mirror and FATAL on the hard-assert; declining instead
 * defers to the caller's normal seed path → the torn gate's honest
 * operator_needed halt. (The explicit -refold-from-anchor flag path is
 * UNCHANGED: it still allows the node.db fallback + FATAL as the operator-paged
 * recovery; only this auto-arm declines.) */
struct main_state;
struct sqlite3;
bool boot_refold_from_anchor_arm_if_torn(struct main_state *ms,
                                         struct node_db *ndb,
                                         struct sqlite3 *progress_db);

/* CUTOVER DEFECT 2 — body-span contiguity gate (impl in
 * config/src/boot_refold_staged.c). PURE precondition for the from-anchor fold:
 * every block in (anchor_height, resume_target] must have its body present on
 * disk (BLOCK_HAVE_DATA in the active-chain block_index) before the fold is
 * armed. A pruned/missing body in that span would otherwise pin utxo_apply
 * mid-fold at the missing height (the prevout_unresolved wedge, relocated).
 *
 * Returns true iff the WHOLE span is body-contiguous (or the span is empty,
 * resume_target <= anchor_height). On the FIRST missing body it returns false,
 * writes that height to *out_first_missing (when non-NULL), and — unless
 * raise_blocker is false — raises the NAMED typed blocker "refold.body_gap"
 * (BLOCKER_DEPENDENCY: the body-fetch path is the dependency that fills it), so
 * the operator/peer-fetch path sees a named blocker, never a silent stall.
 * ms==NULL → returns false (cannot prove contiguity without the chain). */
bool boot_refold_body_span_contiguous(struct main_state *ms,
                                      int32_t anchor_height,
                                      int32_t resume_target,
                                      int32_t *out_first_missing,
                                      bool raise_blocker);

/* -load-verify-boot eligibility probe (impl in config/src/boot_refold_staged.c).
 * Pure, side-effect-free predicate that decides whether a NORMAL boot should
 * route the load+verify+anchor-fold path instead of the cold-import seed. Returns
 * true iff ALL hold:
 *   (1) a baked anchor snapshot exists at the mint path
 *       ($ZCL_MINT_ANCHOR_OUT else <datadir>/utxo-anchor.snapshot), AND
 *   (2) uss_open(path, verify_full_sha3=true, cp->sha3_hash) SUCCEEDS — i.e. the
 *       snapshot's recomputed body SHA3 EQUALS the compiled checkpoint root (the
 *       existing loader does the SHA3 verification; this never reimplements it),
 *       AND hdr.count == cp->utxo_count, AND
 *   (3) coins_kv is NOT already the proven authority
 *       (coins_kv_is_proven_authority == false) — a healthy forward-built /
 *       already-seeded node is NEVER reset.
 * On ANY doubt (file absent, header/body SHA3 mismatch, count mismatch, healthy
 * coins_kv, no compiled checkpoint, NULL args) it returns FALSE so the caller
 * runs today's exact proven path. SELECT-only on progress.kv; mmaps + verifies the
 * snapshot read-only via uss_open/uss_close (no coins_kv mutation here). */
bool boot_load_verify_snapshot_eligible(struct node_db *ndb,
                                        struct sqlite3 *progress_db);

bool app_is_running(void);
void app_add_node(const char *host, int port);
void app_start_metrics(bool mining);
void app_stop_metrics(void);

#ifdef ZCL_TESTING
#include "config/boot_postmortem.h"
bool boot_test_params_thread_failure_is_fatal(bool has_params_dir,
                                              bool is_mainnet,
                                              bool mint_anchor_fast);
#endif

/* Background UTXO replay status (after fast file sync).
 * Node is usable immediately; replay builds UTXO set in background. */
#include <stdatomic.h>
extern _Atomic bool g_utxo_replay_active;
extern _Atomic int  g_utxo_replay_height;

#endif
