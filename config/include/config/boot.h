/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCL_INIT_H
#define ZCL_INIT_H

#include <stdbool.h>

enum zcl_runtime_profile {
    ZCL_RUNTIME_FULL = 0,
    ZCL_RUNTIME_ZCLASSIC_ONLY,
    ZCL_RUNTIME_EXPLORER,
    ZCL_RUNTIME_ONION_NODE,
    ZCL_RUNTIME_LEGACY_COMPAT
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
    bool no_bg_validation;     /* -nobgvalidation : skip background proof verification */
    bool no_legacy_auto_import;/* -nolegacyimport : do not auto-read ~/.zclassic */
    bool boot_from_log;        /* -rebuildfromlog : rebuild block index + tip from
                                 * the event-log block_index_projection instead of
                                 * the legacy flat/SQLite/LevelDB loaders,
                                 * zclassicd-LDB, and UTXO importer. Opt-in;
                                 * default false so the live boot is unchanged. */
    const char *external_ip;   /* -externalip=IP : advertise this address to peers */
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

bool app_init(struct app_context *ctx);
void app_shutdown(void);

/* -refold-staged (impl in config/src/boot_refold_staged.c): boot_refold_staged_init
 * caches the durable refold_in_progress signal before the reducer starts (thin
 * wrapper over refold_progress_boot_init; no-op on a normal boot). _reset wipes
 * the staged reducer's derived state to genesis so the pipeline re-folds forward
 * over on-disk block BODIES. Owner-gated; progress.kv + node.db mirror only. */
struct node_db;
void boot_refold_staged_init(bool refold_staged);
void boot_refold_staged_reset(struct node_db *ndb);

bool app_is_running(void);
void app_add_node(const char *host, int port);
void app_start_metrics(bool mining);
void app_stop_metrics(void);

#ifdef ZCL_TESTING
bool boot_postmortem_init_for_testing(const char *datadir);
void boot_postmortem_shutdown_for_testing(void);
const char *boot_postmortem_dir_for_testing(void);
#endif

/* Background UTXO replay status (after fast file sync).
 * Node is usable immediately; replay builds UTXO set in background. */
#include <stdatomic.h>
extern _Atomic bool g_utxo_replay_active;
extern _Atomic int  g_utxo_replay_height;

#endif
