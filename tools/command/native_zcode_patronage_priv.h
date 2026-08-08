/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Internal wiring shared by the ZC23 patronage settlement adapter. Not a
 * public surface: only native_zcode_patronage_command.c and its promotion
 * test include this. It exposes the exact caller-pinned simulation context
 * the settle/refund handlers bind — active-chain anchor authority, immutable
 * ZC23 policy root, and continuity-uniqueness callbacks — so the promotion
 * tests exercise the same code the handlers run, never a reimplementation.
 * Everything here stays simulation-only: no GENESIS/MINT/SEND, wallet,
 * custody or consensus path is reachable from this context. */
#ifndef ZCL_TOOLS_NATIVE_ZCODE_PATRONAGE_PRIV_H
#define ZCL_TOOLS_NATIVE_ZCODE_PATRONAGE_PRIV_H

#include "vcs/zcode_patronage_settlement.h"

#include <stdbool.h>
#include <stdint.h>

struct json_value;

struct zpc_simulation_context {
    struct vcs_zcode_patronage_validation_context patronage;
    struct vcs_zcode_creation_validation_context creation;
    struct vcs_zcode_patronage_settlement_validation_context settlement;
    /* Owned copy of the scratch workspace path: the sub-contexts above point
     * here, so the bound context stays valid after the caller's JSON input
     * is freed (the promotion tests bind once, then probe the callbacks). */
    char workspace[4097];
    uint8_t network_root[32];
    uint8_t policy_root[32];
    uint64_t opening_height;
    uint8_t opening_hash[32];
    uint64_t maturity_height;
    uint8_t maturity_hash[32];
};

/* Parse and bind the full caller-pinned simulation context: scratch
 * workspace, expected network genesis root, immutable ZC23 policy root,
 * expected epoch/award, active-chain height/MTP, both declared simulation
 * anchors (opening and maturity height+hash) and now_unix. Fails closed with
 * a named *reason on any missing, zero, or inconsistent pin. */
bool zpc_simulation_context_bind(
    const struct json_value *input, struct zpc_simulation_context *context,
    const char **reason);

#endif /* ZCL_TOOLS_NATIVE_ZCODE_PATRONAGE_PRIV_H */
