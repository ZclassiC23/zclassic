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
    bool reimport_utxos;
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
    bool allow_degraded;       /* -allow-degraded : continue past failed post-restore integrity check
                                 * (default false → boot FATALs on broken chain state). */
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
