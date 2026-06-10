/* Copyright (c) 2018 The Zcash developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef ZCASH_CONSENSUS_UPGRADES_H
#define ZCASH_CONSENSUS_UPGRADES_H

#include "consensus/params.h"
#include <stdbool.h>
#include <stdint.h>

/* Activation status of an upgrade at a height, as decoded from its
 * nActivationHeight: DISABLED (-1, never activates), PENDING (height set
 * but the queried height is below it), ACTIVE (queried height >=
 * activation). Returned by consensus_upgrade_state(). */
enum upgrade_state {
    UPGRADE_DISABLED,
    UPGRADE_PENDING,
    UPGRADE_ACTIVE
};

/* Per-upgrade metadata, indexed by enum upgrade_index. nBranchId is the
 * consensus branch id committed to by transactions in that epoch and is
 * the value returned by consensus_current_epoch_branch_id(); strName /
 * strInfo are human-readable. */
struct nu_info {
    uint32_t nBranchId;
    const char *strName;
    const char *strInfo;
};

/* The branch-id / name table, MAX_NETWORK_UPGRADES entries, indexed by
 * enum upgrade_index. Defined in lib/consensus/src/upgrades.c. The
 * branch ids are consensus constants (Sprout=0, Overwinter=0x5ba81b19,
 * Sapling=0x76b809bb, ...); they MUST NOT change. */
extern const struct nu_info NetworkUpgradeInfo[];

/* The Sprout (pre-Overwinter) consensus branch id, 0. Used where a
 * branch id is required before any upgrade has activated. */
extern const uint32_t SPROUT_BRANCH_ID;

/* Sentinel N=K=0 in EquihashUpgradeInfo meaning "use the chain's
 * default Equihash (N,K)"; a nonzero (N,K) row overrides it for that
 * epoch (e.g. Bubbles+ carry 192,7 instead of the chain default). */
#define EQUIHASH_DEFAULT_PARAMS 0

/* Equihash (N,K) parameters, indexed by enum upgrade_index. */
struct equihash_info {
    unsigned int N;
    unsigned int K;
};

/* Per-epoch Equihash (N,K) override table, MAX_NETWORK_UPGRADES entries,
 * defined in lib/consensus/src/upgrades.c. A row of EQUIHASH_DEFAULT_PARAMS
 * (0,0) means "fall back to the chain default"; the Bubbles/DiffAdj/
 * Buttercup rows carry the consensus-active (192,7). PoW verification
 * selects the (N,K) for a block's height from this table. */
extern struct equihash_info EquihashUpgradeInfo[];

/* These are the legacy (bool / int / uint32_t) network-upgrade entry
 * points. Each is a thin wrapper over the pure
 * domain_consensus_*() arithmetic in domain/consensus/upgrades.{h,c};
 * the wrapper preserves the historic return-type and observable
 * behaviour (including LOG_FAIL chatter) while the domain layer returns
 * a typed zcl_result. The activation schedule itself is the
 * vUpgrades[] table in consensus_params: each entry has an
 * nActivationHeight that is NETWORK_UPGRADE_NO_ACTIVATION (-1, disabled)
 * or a real height. "Active at H" means activation != -1 && H >= activation. */

/* State of upgrade `idx` at `nHeight`: UPGRADE_DISABLED (activation == -1),
 * UPGRADE_ACTIVE (nHeight >= activation), or UPGRADE_PENDING (activation
 * set but nHeight below it). asserts nHeight >= 0 and idx in
 * [BASE_SPROUT, MAX_NETWORK_UPGRADES). This is the primitive every
 * other predicate here is built on. */
enum upgrade_state consensus_upgrade_state(int nHeight, const struct consensus_params *params, enum upgrade_index idx);

/* The highest-numbered upgrade_index that is ACTIVE at `nHeight`,
 * scanning from MAX_NETWORK_UPGRADES-1 down. Always returns at least
 * BASE_SPROUT (0), the always-active base epoch, so the result is a
 * valid index into NetworkUpgradeInfo / vUpgrades. */
int consensus_current_epoch(int nHeight, const struct consensus_params *params);

/* Consensus branch id of the epoch active at `nHeight` —
 * NetworkUpgradeInfo[consensus_current_epoch(nHeight)].nBranchId. This
 * is the branch id that must be committed to in Overwinter+ sighashes /
 * transaction headers at this height. */
uint32_t consensus_current_epoch_branch_id(int nHeight, const struct consensus_params *params);

/* Intended as "is `branchId` a recognised consensus branch id?" (i.e.
 * appears in NetworkUpgradeInfo). NOTE (verified against upgrades.c):
 * the wrapper ALWAYS returns true; an unrecognised id only emits a
 * LOG_FAIL and is NOT reported via the return value. Callers that need
 * the actual yes/no answer must use the domain function
 * domain_consensus_is_branch_id() (which writes out_known). Documented
 * as-is; do not rely on the return to reject an unknown branch id. */
bool consensus_is_branch_id(int branchId);

/* True iff `nHeight` is EXACTLY the activation height for upgrade `idx`
 * (nHeight == vUpgrades[idx].nActivationHeight). asserts idx in range.
 * The pure contract returns false for BASE_SPROUT (which has no
 * activation height) and for nHeight < 0; NOTE the wrapper additionally
 * LOG_FAILs on the BASE_SPROUT case but the underlying match is false.
 * Used to detect upgrade-fork blocks (where extra activation rules fire). */
bool consensus_is_activation_height(int nHeight, const struct consensus_params *params, enum upgrade_index idx);

/* Intended as "does ANY upgrade after BASE_SPROUT activate exactly at
 * `nHeight`?". NOTE (verified against upgrades.c): the wrapper ALWAYS
 * returns true, emitting a LOG_FAIL when nHeight < 0 or when no upgrade
 * activates at nHeight; the real boolean is only available via the
 * domain function domain_consensus_is_activation_height_any() (out_match).
 * Documented as-is; do not branch consensus logic on this return. */
bool consensus_is_activation_height_any(int nHeight, const struct consensus_params *params);

/* The smallest upgrade_index > BASE_SPROUT whose activation height is
 * set and strictly above `nHeight` — i.e. the next upgrade still
 * PENDING at nHeight. If none is pending the domain function returns an
 * error; the wrapper LOG_ERRs and returns the unmodified epoch value
 * (0). Inspect via domain_consensus_next_epoch() for the typed result. */
int consensus_next_epoch(int nHeight, const struct consensus_params *params);

/* Activation height of the next pending upgrade after `nHeight`
 * (vUpgrades[consensus_next_epoch(nHeight)].nActivationHeight). If no
 * upgrade is pending the wrapper LOG_ERRs and returns 0; use
 * domain_consensus_next_activation_height() for the typed result. */
int consensus_next_activation_height(int nHeight, const struct consensus_params *params);

#endif
