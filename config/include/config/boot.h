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

/* enum wallet_at_rest_policy — the pure, env-driven creation policy the
 * boot-site wallet decision folds node context into. Leaf header (only
 * stdint/stddef/stdbool), no config dependency, so no include cycle. */
#include "wallet/wallet_keystore.h"

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
    bool ratify_mint_anchor;   /* -ratify-mint-anchor : TERMINAL offline ratifier
                                 * for a COMPLETED full-validation mint producer
                                 * datadir (run against a COPY). Re-derives the
                                 * coins_kv commitment + count + applied frontier
                                 * from the datadir's OWN durable tables and, ONLY
                                 * on full agreement with the compiled SHA3 UTXO
                                 * checkpoint, stamps the migration-complete +
                                 * self-folded markers the bundle exporter demands
                                 * and re-arms the mint resume marker. Reads the
                                 * DURABLE set (never the coins_ram overlay); no
                                 * fold. Exits after RATIFIED or a typed REFUSED.
                                 * Default false. */
    bool verify_rom;           /* -verify-rom : TERMINAL read-only verifier. Re-
                                 * derives the canonical coins_kv commitment + count
                                 * (coins_kv_verify_against_checkpoint) and compares
                                 * them to the compiled SHA3 UTXO checkpoint, then
                                 * prints PASS/FAIL with the derived vs baked digests
                                 * and _exit()s (0 PASS / 1 FAIL). Reads only; stamps
                                 * NOTHING. A PASS is expected only on a datadir
                                 * positioned AT the checkpoint (applied == cp->height
                                 * + 1). Default false. */
    bool export_consensus_bundle; /* -export-consensus-bundle : TERMINAL offline
                                 * checkpoint-content exporter. Emits the
                                 * zcl.consensus_state_bundle.v1 from a finished
                                 * genesis->checkpoint datadir whose coins
                                 * reproduce the compiled SHA3 UTXO checkpoint and
                                 * whose Sapling tip frontier Pedersen-roots to
                                 * the anchor header's committed
                                 * hashFinalSaplingRoot — a cryptographic content
                                 * proof that replaces the fold-binary-identity
                                 * receipt bind, so a foreign binary that did not
                                 * itself fold this datadir can still export the
                                 * (byte-identical-shape) bundle. Exits after
                                 * EXPORTED or a typed REFUSED. Default false. */
    const char *promote_shielded_history; /* -promote-shielded-history=<producer>
                                 * : TERMINAL offline promote. Installs a finished
                                 * producer's below-checkpoint shielded history
                                 * (Sprout+Sapling anchors + nullifiers) into the
                                 * WEDGED COPY -datadir's progress.kv, atomically
                                 * flipping all three shielded activation cursors
                                 * to 0. Refuses any non-*-COPY-* / live datadir.
                                 * Binds each Sapling frontier to the in-RAM
                                 * header-committed root. Exits after PROMOTED or
                                 * a typed REFUSED. Default NULL. */
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
    bool backfill_nullifiers;  /* -backfill-nullifiers or
                                 * ZCL_NULLIFIER_BACKFILL=1 : owner-gated
                                 * one-shot remediation for the C-3 activation
                                 * gap. Re-walks already-applied block bodies
                                 * below nullifier_kv.activation_cursor and
                                 * inserts revealed Sprout/Sapling nullifiers
                                 * through the existing utxo_apply nullifier
                                 * writer, then exits before services. Populate
                                 * only: no consensus predicate changes. */
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
    bool sandbox_steady;       /* -sandbox=steady : enter the os_sandbox node
                                 * steady-state profile (no_new_privs + Landlock
                                 * datadir grant + node deny-list) at the late
                                 * SERVICES_RUNNING boundary. Default false
                                 * (-sandbox=off). REFUSED without a systemd
                                 * NOTIFY_SOCKET: the deny-set forbids execve, so
                                 * the off-systemd self-respawn (src/main.c) would
                                 * be killed — under systemd, Restart=always owns
                                 * respawn and no self-exec happens. */
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
    int  par_workers;          /* -par=N : verification-engine worker count.
                                 * 0 (default) => GetNumCores()-1, clamped >= 1.
                                 * 1 => serial. ADDITIVE foundation — not yet
                                 * wired into the staged reducer / consensus path. */
    const char *load_snapshot_at_own_height; /* -load-snapshot-at-own-height=PATH :
                                 * EXPLICIT-ONLY recovery (NEVER fires on a normal
                                 * boot — NULL unless the operator sets the flag).
                                 * Seed coins_kv from a ZCLUTXO snapshot at the
                                 * snapshot's OWN header height after verifying
                                 * the file's OWN body SHA3 (uss_open verify_full_sha3
                                 * with expected_sha3=NULL) + hdr.count == records
                                 * parsed — NOT bound to the compiled checkpoint. Then
                                 * force the 8 stage cursors to hdr.height, set
                                 * applied=hdr.height+1, seed tip_finalize at
                                 * hdr.height with hdr.anchor_block_hash, and fold
                                 * FORWARD over on-disk bodies from there. For a
                                 * snapshot taken ABOVE the compiled checkpoint (does
                                 * not cross it). Owner-gated; FATAL-refuses if the
                                 * body-digest/count checks fail. Passing those checks
                                 * proves file integrity, not state provenance. */
    const char *install_consensus_bundle; /* -install-consensus-bundle=PATH :
                                 * EXPLICIT-ONLY sovereign-cure consumer. Validate a
                                 * zcl.consensus_state_bundle.v1 FILE, gate it through
                                 * the publication CAS (must ADMIT), then atomically
                                 * install its complete coins + Sprout/Sapling anchors
                                 * + nullifiers + the 8 reducer stage cursors into the
                                 * live progress store (activation cursor 0 — the cure
                                 * for the anchor_backfill_gap wedge). Terminal: exits
                                 * after installing (cursors reported) or a typed
                                 * refusal. NULL unless the operator sets the flag.
                                 * Refuses on the canonical datadir unless
                                 * ZCL_DEPLOY_ALLOW_CANONICAL is set. */
    const char *verify_consensus_bundle; /* -verify-consensus-bundle=PATH :
                                 * EXPLICIT-ONLY offline replay verifier. Admits a
                                 * zcl.consensus_state_bundle.v1 FILE read-only, then
                                 * INDEPENDENTLY re-derives the UTXO/anchor/nullifier
                                 * component digests from THIS datadir's own
                                 * genesis->anchor folded tables (bundle tables never
                                 * used as derivation input) and, only if they all
                                 * match, writes an fsync'd replay receipt outside the
                                 * bundle. That receipt is what lifts the ACTIVATE
                                 * production containment. Terminal: exits after
                                 * writing the receipt or a typed refusal. NULL unless
                                 * the operator sets the flag. */
    bool cold_start_seed_oneshot; /* -coldstart-seed-oneshot : INTERNAL cold-start
                                 * driver handshake (config/src/boot_cold_start.c).
                                 * Set ONLY on the seed child the -cold-start
                                 * driver spawns; makes app_init apply the
                                 * -load-snapshot-at-own-height seed then return
                                 * before services so main.c shuts down cleanly
                                 * (WAL checkpoint + clean-shutdown marker) and
                                 * exits, leaving a durable clean-stopped datadir
                                 * for the driver's next stage. NEVER set on an
                                 * operator-driven boot; a normal node ignores it. */
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

/* ── First-run wallet creation: boot-site decision ────────────────── *
 *
 * wallet_at_rest_creation_policy() (lib/wallet) is the pure, env-driven
 * read of operator intent (passphrase / -allow-plaintext-wallet opt-in /
 * neither). It deliberately knows nothing about node context. This
 * boot-site helper folds in the two facts the wallet lib must NOT depend
 * on — whether this boot is the OFFLINE anchor-mint producer
 * (ctx->mint_anchor, which also covers -mint-anchor-fast since that is
 * only honored with -mint-anchor) and which operator lane the node was
 * launched in — to yield the final first-run creation action.
 *
 * Decision matrix (policy × context):
 *
 *   ENCRYPTED  (ZCL_WALLET_PASSPHRASE)   -> CREATE_ENCRYPTED  (any context)
 *   PLAINTEXT_OPTIN (-allow-plaintext-wallet)
 *                                        -> CREATE_PLAINTEXT  (any context)
 *   REFUSE + offline mint producer       -> MINT_EXEMPT (quiet, proceed)
 *   REFUSE + declared non-canonical lane
 *          (dev / soak / test / copy)    -> CREATE_PLAINTEXT (loud, proceed)
 *   REFUSE + canonical / unknown /
 *          interactive default           -> REFUSE (FATAL — no silent mint)
 *
 * Pure + reentrant: no globals, no env reads (policy is passed in), so it
 * is unit-testable without app_init. */
enum wallet_boot_wallet_action {
    WALLET_BOOT_CREATE_ENCRYPTED = 0, /* passphrase present — wrapped at rest */
    WALLET_BOOT_CREATE_PLAINTEXT,     /* proceed plaintext, LOUD warning */
    WALLET_BOOT_CREATE_MINT_EXEMPT,   /* offline throwaway mint — quiet INFO */
    WALLET_BOOT_REFUSE,               /* FATAL: refuse a silent plaintext mint */
};

enum wallet_boot_wallet_action
wallet_at_rest_boot_decision(enum wallet_at_rest_policy policy,
                             bool is_mint,
                             enum zcl_operator_lane lane);

/* Emit the operator-facing stderr notice for a first-run wallet-creation
 * action (INFO for the offline mint, loud WARNING for a plaintext create,
 * FATAL text for REFUSE). Pure stderr I/O — the caller owns exit()/events. */
void wallet_at_rest_boot_report(enum wallet_boot_wallet_action action,
                                enum zcl_operator_lane lane);

bool app_init(struct app_context *ctx);
void app_shutdown(void);
void app_shutdown_offline(void);

/* Legacy -refold-staged is contained before boot writes: its ordinary reducer
 * replay cannot own the bounded shielded-history completion transaction. The
 * init helper remains for the private offline mint driver and normal restart
 * cache restoration; `preflight` returns false when the legacy flag is set. */
struct node_db;
void boot_refold_staged_init(bool refold_staged);
bool boot_refold_staged_preflight(bool refold_staged);

/* -refold-from-anchor (B2; impl in config/src/boot_refold_staged.c): FULL-resets
 * coins_kv, RE-SEEDS the SHA3-verified
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

/* Read-only probe: is a SHA3-checkpoint-bound anchor snapshot artifact reachable
 * for boot_refold_from_anchor_reset to load? True iff the compiled SHA3 UTXO
 * checkpoint exists AND a verified minted snapshot reproduces it (the exact
 * "usable verified anchor" predicate the reset applies before it trusts a
 * snapshot). On true, *anchor_height_out (when non-NULL) receives the checkpoint
 * height. Used by the sticky escalator's terminal refold rung to gate: fire only
 * when an anchor artifact exists, else name the missing clue. No coins_kv
 * mutation. */
bool boot_refold_from_anchor_artifact_available(struct node_db *ndb,
                                                int32_t *anchor_height_out);

/* -load-snapshot-at-own-height=PATH (impl in config/src/boot_refold_staged.c):
 * EXPLICIT-ONLY recovery loader. Sibling of boot_refold_from_anchor_reset EXCEPT
 * the snapshot body is digest-verified against its OWN header SHA3 (uss_open
 * verify_full_sha3=true, expected_sha3=NULL) — NOT bound to the compiled
 * checkpoint — and the fold resumes at the snapshot's OWN header height, not the
 * compiled anchor. Used to seed coins_kv from a SHA3-internally-consistent
 * UTXO-set dump taken at a height ABOVE the compiled checkpoint (so it never
 * crosses the checkpoint and the anchor self-mint hook is irrelevant).
 * File-integrity gates require body SHA3 == hdr.sha3_hash AND
 * records_parsed == hdr.count; the separate active-chain check below verifies
 * only the named height/hash location. Neither proves the UTXO, Sprout, or
 * nullifier contents against ZClassic consensus. On integrity failure it
 * LOG_FAILs and FATAL-refuses. Forces the 8 stage
 * cursors to hdr.height, sets applied = hdr.height+1, seeds the tip_finalize
 * anchor at hdr.height with hdr.anchor_block_hash, then the staged pipeline folds
 * FORWARD over on-disk BODIES from hdr.height. Caller must have passed a non-NULL
 * `path` (from ctx->load_snapshot_at_own_height); a normal boot never calls it.
 *
 * CHAIN-LOCATION CROSS-CHECK: `ms` is the live main_state whose in-memory
 * active chain (populated by the prior block-index load) is consulted to match
 * the snapshot's hdr.anchor_block_hash to the validated header at seed_h. A
 * mismatch is FATAL, so state cannot be installed at a different chain
 * location. This check does not authenticate the snapshot contents because
 * ZClassic headers commit no UTXO, Sprout, or nullifier roots. `ms` may be NULL
 * only in unit tests with no chain loaded (the check is then skipped with a
 * loud warning).
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

/* -install-consensus-bundle=PATH (impl in config/src/boot_install_consensus_bundle.c):
 * the A2 consumer of the sovereign shielded-state cure. TERMINAL: this NEVER
 * returns — it _exit()s after printing a named terminal (installed + cursors
 * reported, or a typed refusal). Steps, all fail-closed:
 *   (1) containment — refuse on the canonical datadir (~/.zclassic-c23) unless
 *       ZCL_DEPLOY_ALLOW_CANONICAL is set (dev/copy datadirs proceed);
 *   (2) admit + strictly validate the immutable bundle file;
 *   (3) gate through the publication CAS (consensus_state_publication_cas) —
 *       must ADMIT (artifact + selected-chain + producer-source receipts all
 *       present and mutually binding, and the durable frontier not behind the
 *       bundle). A durable decision record is written to the datadir;
 *   (4) reload the exact durable ADMIT, retain the classified datadir
 *       capability, recapture its expected H-star/hash under the cutover lock,
 *       and
 *       atomically install via consensus_state_snapshot_install_activate.
 * `ms` is the live main_state (its selected chain is consulted to build the
 * chain-binding evidence). An ordinary pre-commit refusal installs nothing and
 * exits non-zero; the prior progress store is left intact. A distinct
 * COMMIT_OUTCOME_UNKNOWN terminal never guesses and directs the operator to the
 * independently reopened/fsynced prior generation captured before cutover. */
void boot_install_consensus_bundle(struct node_db *ndb,
                                   struct main_state *ms,
                                   const char *bundle_path,
                                   const char *datadir);

/* -verify-consensus-bundle=PATH (impl in config/src/boot_verify_consensus_bundle.c):
 * the offline replay verifier that produces the independent receipt ACTIVATE
 * requires. TERMINAL: NEVER returns — it _exit()s after printing a named
 * terminal (receipt written + components reported, or a typed refusal). Admits
 * the bundle read-only, then re-derives every component digest from the OPEN
 * progress store (this datadir's own genesis->anchor fold, parked at the anchor)
 * and writes the fsync'd receipt only if they all match the bundle. Reads the
 * bundle and the local store; never mutates progress.kv. */
void boot_verify_consensus_bundle(const char *bundle_path,
                                  const char *datadir);

/* -ratify-mint-anchor result (banner + reason surface for the terminal verb and
 * its unit test). `reason` is always set; `ratified` is true only after the
 * durable coins_kv fully agrees with the compiled checkpoint AND all three
 * markers were stamped. On any disagreement NOTHING is stamped. */
struct sqlite3;
struct sha3_utxo_checkpoint;
struct boot_ratify_result {
    bool     ratified;
    int32_t  height;         /* cp->height (the ratified anchor height) */
    uint64_t count;          /* the datadir's own coins_kv count */
    uint8_t  sha3[32];       /* the datadir's own coins_kv commitment */
    char     reason[256];
};

/* -ratify-mint-anchor=(no arg, acts on -datadir) (impl in
 * config/src/boot_ratify_mint_anchor.c): TERMINAL — NEVER returns; it _exit()s
 * after printing RATIFIED (0) or a typed REFUSED (1). Reads the OPEN progress
 * store's DURABLE coins_kv (refuses if the coins_ram overlay is active). */
void boot_ratify_mint_anchor(const char *datadir);

/* -verify-rom=(no arg, acts on -datadir) (impl in config/src/boot_verify_rom.c):
 * TERMINAL — NEVER returns; it _exit()s after printing PASS (0) or FAIL (1). It
 * re-derives the canonical coins_kv commitment + count against the compiled SHA3
 * UTXO checkpoint (coins_kv_verify_against_checkpoint) on demand and prints the
 * derived-vs-baked digests. Read-only: reads the OPEN progress store's DURABLE
 * coins_kv (refuses if the coins_ram overlay is active); stamps NOTHING. A PASS is
 * only expected on a datadir positioned AT the checkpoint height. */
void boot_verify_rom(const char *datadir);

/* -export-consensus-bundle=(no arg, acts on -datadir) (impl in
 * config/src/boot_export_consensus_bundle.c): TERMINAL — NEVER returns; it
 * _exit()s after printing EXPORTED (0) or a typed REFUSED (1). Reads the
 * compiled SHA3 UTXO checkpoint, reads the header-committed final Sapling root
 * at the checkpoint height from `ndb`'s validated block index, and runs the
 * checkpoint-content export against the OPEN progress store into the datadir. */
void boot_export_consensus_bundle(struct node_db *ndb, struct main_state *ms,
                                  const char *datadir);

/* -promote-shielded-history=<producer-datadir> (acts on -datadir as the TARGET;
 * impl in config/src/boot_promote_shielded_history.c): TERMINAL — NEVER returns;
 * it _exit()s after printing PROMOTED (0) or a typed REFUSED (1). Enforces the
 * -COPY- path-safety guard on BOTH the target and producer (never a live
 * datadir), resolves the in-RAM header tip + compiled checkpoint + Sapling
 * activation height, and runs shielded_history_promote_run against the OPEN
 * target progress store. */
void boot_promote_shielded_history(struct main_state *ms,
                                   const char *target_datadir,
                                   const char *producer_datadir);

/* Testable core of the ratify verb: re-derive commitment/count/applied-height
 * from `pdb`'s durable tables, compare against `cp`, and — only on full
 * agreement — stamp coins_kv_mark_migration_complete + coins_kv_mark_self_folded
 * and re-arm mint_anchor_progress_mark, all under one progress-store critical
 * section. Fills `*out`. Returns true iff ratified+stamped; on any mismatch it
 * stamps nothing and returns false with out->reason set. */
bool boot_ratify_mint_anchor_check_and_stamp(
    struct sqlite3 *pdb, const struct sha3_utxo_checkpoint *cp,
    struct boot_ratify_result *out);

#ifdef ZCL_TESTING
/* Unit surface for the exact production lane/owner gate. `authorization` is
 * accepted only when it is exactly "1". */
bool boot_install_consensus_bundle_gate_allows_for_test(
    const char *datadir, const char *authorization, bool *out_canonical);
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
 * digest-verified, chain-location-checked UTXO seed) — and return a malloc'd absolute
 * path to the snapshot the loader should seed from, or NULL when no usable
 * bundle is present. The highest-height snapshot wins; a snapshot with no
 * block_index.bin alongside it is declined (logged) because the loader needs the
 * header chain for its chain-location cross-check. The CALLER must gate on
 * coins_kv NOT yet being the proven authority so a synced node is never
 * re-seeded; the loader still verifies bytes + chain location, so this only
 * ever auto-selects a file the explicit -load-snapshot-at-own-height flag could
 * have loaded. Caller owns the returned string (free()). */
char *boot_autodetect_bundle_snapshot(const char *datadir);

/* FIX 3 seam — PURE chain-location cross-check for the snapshot FATAL inside
 * boot_load_snapshot_at_own_height_reset. The loaded snapshot's
 * hdr.anchor_block_hash MUST byte-equal this node's PoW-proven header hash at
 * the snapshot height; on mismatch the loader FATALs (refuses a forged /
 * wrong-chain snapshot) rather than seeding state at the wrong height/hash.
 * This does not prove the UTXO or shielded payload: ZClassic headers commit no
 * such state roots. Both inputs are 32 raw bytes (internal little-endian).
 * Returns true iff the snapshot names this chain location (the two hashes are
 * byte-identical). NULL on
 * either side is treated as a non-match (refuse). Behavior-identical to the
 * inline `memcmp(bi->hashBlock.data, hdr.anchor_block_hash, 32) == 0`. */
bool boot_snapshot_anchor_hash_matches(const unsigned char *index_block_hash,
                                       const unsigned char *snapshot_anchor_hash);

/* -mint-anchor (impl in config/src/boot_refold_staged.c): the ANCHOR-SET MINT
 * boot-time reset. Uses its private offline genesis reset and caps the fold at
 * the compiled SHA3 UTXO
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

/* Lane A1 — after a -mint-anchor fold reaches the anchor and the producer
 * source receipt is finalized, emit the contained full-history
 * zcl.consensus_state_bundle.v1 into
 * <datadir>/consensus-state-bundle-<anchor>.sqlite
 * (config/consensus_state_snapshot_export.h — the exporter's only viable
 * caller, since its proof binds the running binary to the fold). Quiesces the
 * in-RAM fold overlay first (flush the tail to durable coins_kv + release the
 * map; the exporter refuses while coins_ram_active()). Idempotent: an
 * already-present bundle from a prior run of the SAME binary is treated as
 * done. On a real failure it pages EV_OPERATOR_NEEDED, registers the PERMANENT
 * blocker mint_bundle.export_failed, and returns false (so the one-shot exits
 * non-zero) WITHOUT touching the verified anchor snapshot or the receipt.
 * Public so the consensus_state_snapshot_export test can prove the wiring
 * fires. */
bool boot_mint_anchor_export_bundle(struct sqlite3 *pdb, const char *datadir,
                                    int32_t anchor,
                                    const uint8_t block_hash[32]);

/* Stamp the EARNED sovereign-authority markers the bundle export proof requires:
 * COINS_KV_MIGRATION_COMPLETE_KEY (coins_kv provably holds the live set) + the
 * self-folded provenance bit (the set is checkpoint-verified, not a borrowed
 * node.db copy). Called by the FULL-profile mint finalize path AFTER the
 * checkpoint HARD-ASSERT (_exit on mismatch) proved the folded coins_kv
 * reproduces the compiled checkpoint, so both facts are earned — a fresh
 * full-validation producer has no other stamper. Public so the
 * consensus_state_snapshot_export test can prove the finalize path yields an
 * exporter-admissible source. Returns false (logs) if either stamp fails. */
bool boot_mint_anchor_stamp_sovereign_markers(struct sqlite3 *pdb);

/* The SHIELDED keystone hard-assert (impl in
 * config/src/boot_mint_anchor_rom_keystone.c — separate file for the E1
 * file-size ceiling). Called by the mint finalize path right AFTER the coins
 * SHA3/count hard-assert: recomputes anchor_digest/anchor_count, both pool
 * frontier roots+heights, and nullifier_digest/nullifier_count from the
 * just-folded progress.kv tables using the SAME codec primitives and SQL
 * orderings the full-history bundle export uses, and requires them ==
 * get_rom_state_checkpoint(). On a mismatch (or a recompute failure) it
 * pages EV_BOOT_VALIDATION_FAILED, unlinks `out_path`, and _exit()s — the
 * same fail posture as the coins assert; it returns only on a match (or
 * when no ROM state checkpoint is compiled in). */
void boot_mint_anchor_rom_keystone_assert(struct sqlite3 *pdb,
                                          const char *out_path);

/* Read-only producer-marker/lane refusal before node.db, wallet, or progress.kv
 * is opened for writing. */
bool boot_mint_anchor_normal_boot_preflight(const char *datadir);
bool boot_mint_anchor_normal_boot_gate(struct sqlite3 *progress_db);
void boot_mint_anchor_require_producer_lane(struct sqlite3 *progress_db,
                                            bool checkpoint_fold);
bool boot_mint_anchor_should_log_progress(int32_t applied_through,
                                          int32_t anchor);
/* mint-progress.log telemetry (impl in config/src/boot_mint_anchor_log.c —
 * S1.4). Path resolver ($ZCL_MINT_PROGRESS_LOG, else
 * <datadir>/mint-progress.log) + the throttled (~5s) best-effort append the
 * drive loop calls: height/rate/eta plus the eight stages' step-EWMAs and the
 * outer batch-COMMIT EWMA (`cm:`), the producer's only offline (RPC-less)
 * bottleneck surface. */
void boot_mint_anchor_progress_log_path(const char *datadir, char *out,
                                        size_t n);
void boot_mint_anchor_progress_log_tick(const char *path, int32_t through,
                                        int32_t anchor, int64_t start_us,
                                        bool force);
/* Fail-closed mint-fold stall diagnosis (impl in config/src/boot_mint_anchor.c):
 * the drive loop calls this when the utxo_apply frontier stops below the anchor
 * for kStallLimit consecutive kicks. Reads the eight durable stage cursors,
 * names the WALLED stage (earliest pipeline stage at the minimum cursor),
 * registers the typed PERMANENT blocker `mint_fold.frontier_walled` (owner
 * `mint_anchor`, reason carries all eight cursors), pages EV_OPERATOR_NEEDED
 * (condition=mint_fold_frontier_walled), and prints a stage-naming stderr
 * diagnosis — the bodies-gap wording ONLY when body_fetch is the wall. Public
 * so the mint-fold livelock regression test can assert the blocker payload. */
void boot_mint_anchor_report_frontier_walled(struct sqlite3 *pdb,
                                             int32_t frontier, int32_t anchor,
                                             int stall_kicks);

/* Test-only: run one mint-progress.log tick (the same throttled append the
 * drive loop uses) and force the write regardless of the 5s throttle. Exposed
 * so the reducer_step_drain_harness test group can assert the on-disk line
 * format (S1.4: per-stage step-EWMA telemetry reachable from an OFFLINE
 * -mint-anchor producer, which runs without RPC) without duplicating the
 * throttle/EWMA-collect/formatting logic living in
 * config/src/boot_mint_anchor_log.c. Not called by the mint driver itself
 * (see boot_mint_anchor_run). */
void boot_mint_anchor_progress_log_tick_for_test(const char *path,
                                                 int32_t through,
                                                 int32_t anchor,
                                                 int64_t start_us,
                                                 bool force);

struct json_value;

/* ONE preflight that names ALL unmet -mint-anchor producer preconditions
 * before any datadir mutation (impl in
 * config/src/boot_mint_anchor_preflight.c). PROBLEM this replaces: a fresh
 * -mint-anchor datadir used to surface its unmet preconditions ONE AT A TIME
 * across separate runs (missing legacy block index -> FATAL; missing bodies
 * -> silent stall; ...). Runs EVERY registered check to completion (no
 * short-circuit): datadir lock acquirable, legacy block index (node.db
 * `blocks` table) reaches the compiled anchor height, block body files
 * (blk*.dat) are present under <datadir>/blocks (sampled, not an O(chain)
 * walk), disk headroom, no leftover interrupted-run WAL/SHM artifacts, and
 * (WARN-only) the in-RAM fold's RAM estimate vs system memory. Every check is
 * READ-ONLY — no schema creation, no writes (the datadir-lock probe never
 * creates the pidfile if one is not already present; node.db is opened
 * SQLITE_OPEN_READONLY).
 *
 * Prints one stderr line per check (name + OK/FAIL + why[ + remedy]) and, on
 * any failure, ONE summary FATAL-style line naming every failed check.
 * `report` (nullable) receives {"checks":[{name,ok,why,remedy},...],
 * "all_ok":bool}. The last run's result is cached for the `mint_preflight`
 * dumpstate subsystem (boot_mint_anchor_preflight_dump_state_json). Returns
 * true iff every check passed (a WARN-only check reports ok=true with a
 * non-empty `why`). */
bool boot_mint_anchor_preflight_run_all(const char *datadir,
                                        struct json_value *report);

/* See CLAUDE.md "Adding state introspection". Reentrant-safe: reads the
 * static last-run snapshot boot_mint_anchor_preflight_run_all populated (no
 * allocation; the caller's json_value owns the buffer). */
bool boot_mint_anchor_preflight_dump_state_json(struct json_value *out,
                                                const char *key);

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

/* Pure predicate (impl in config/src/boot_legacy_import.c):
 * decide whether boot should auto-pull zclassicd's LevelDB block index.
 * Fires on the ratio trigger (local_index_size below 90% of chain_h) OR
 * the empty-datadir trigger (local_index_size == 0) — the latter exists
 * because on a genuinely fresh datadir chain_h is ALSO 0, so the ratio
 * test alone degenerates to `0 < 0` and never fires. Either trigger still
 * requires legacy_source_present (caller stat()s ~/.zclassic/blocks/index
 * and passes the result in) — -nolegacyimport short-circuits the caller
 * before this is even evaluated. */
bool boot_need_legacy_header_pull(int64_t local_index_size, int64_t chain_h,
                                  bool legacy_source_present);

bool app_is_running(void);
void app_add_node(const char *host, int port);

/* -addnode-file=PATH: one host[:port] per line, '#' comments and blank
 * lines skipped, malformed lines skipped with a logged warning. Calls
 * app_add_node() for every valid line. A missing file is a clean no-op. */
void app_add_nodes_from_file(const char *path);

/* Typed, greppable census of the bootstrap surface available at boot: the
 * static seed tiers from `params`, the operator onion-seeds file, and
 * peers.dat entries already loaded into `cm`'s addrman. Call once, right
 * after connman_load_addrman(). */
struct chain_params;
struct connman;
void app_log_bootstrap_sources(const struct chain_params *params,
                                struct connman *cm);
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
