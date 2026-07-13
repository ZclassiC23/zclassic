/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "config/boot.h"
#include "wallet/wallet_keystore.h"

#include <stdio.h>
#include <string.h>

void app_context_defaults(struct app_context *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->datadir = NULL;
    ctx->params_dir = NULL;
    ctx->rpc_port = 18232;
    ctx->p2p_port = 8033;
    ctx->https_port = 8443;
    ctx->fs_port = 18034;
    ctx->listen = true;   /* accept inbound by default — be a good peer */
    ctx->checkpoints_enabled = true;
    ctx->runtime_profile = ZCL_RUNTIME_FULL;
    ctx->operator_lane = ZCL_OPERATOR_LANE_UNKNOWN;
    ctx->par_workers = 0;   /* 0 => verify engine sizes to GetNumCores()-1 */
}

const char *app_runtime_profile_name(enum zcl_runtime_profile profile)
{
    switch (profile) {
    case ZCL_RUNTIME_FULL:
        return "full";
    case ZCL_RUNTIME_ZCLASSIC_ONLY:
        return "zclassic-only";
    case ZCL_RUNTIME_EXPLORER:
        return "explorer";
    case ZCL_RUNTIME_ONION_NODE:
        return "onion-node";
    case ZCL_RUNTIME_LEGACY_COMPAT:
        return "legacy-compat";
    default:
        return "unknown";
    }
}

bool app_runtime_profile_parse(const char *name,
                               enum zcl_runtime_profile *out)
{
    if (!name || !out)
        return false;
    if (strcmp(name, "full") == 0) {
        *out = ZCL_RUNTIME_FULL;
        return true;
    }
    if (strcmp(name, "zclassic-only") == 0 ||
        strcmp(name, "zclassic") == 0) {
        *out = ZCL_RUNTIME_ZCLASSIC_ONLY;
        return true;
    }
    if (strcmp(name, "explorer") == 0) {
        *out = ZCL_RUNTIME_EXPLORER;
        return true;
    }
    if (strcmp(name, "onion-node") == 0 ||
        strcmp(name, "onion") == 0) {
        *out = ZCL_RUNTIME_ONION_NODE;
        return true;
    }
    if (strcmp(name, "legacy-compat") == 0 ||
        strcmp(name, "legacy") == 0) {
        *out = ZCL_RUNTIME_LEGACY_COMPAT;
        return true;
    }
    return false;
}

bool app_runtime_profile_has_explorer(enum zcl_runtime_profile profile)
{
    return profile == ZCL_RUNTIME_FULL ||
           profile == ZCL_RUNTIME_EXPLORER ||
           profile == ZCL_RUNTIME_ONION_NODE ||
           profile == ZCL_RUNTIME_LEGACY_COMPAT;
}

bool app_runtime_profile_has_store(enum zcl_runtime_profile profile)
{
    return profile == ZCL_RUNTIME_FULL ||
           profile == ZCL_RUNTIME_ONION_NODE ||
           profile == ZCL_RUNTIME_LEGACY_COMPAT;
}

bool app_runtime_profile_has_onion(enum zcl_runtime_profile profile,
                                   bool tor_flag)
{
    return tor_flag || profile == ZCL_RUNTIME_ONION_NODE;
}

bool app_runtime_profile_has_file_service(enum zcl_runtime_profile profile)
{
    return profile == ZCL_RUNTIME_FULL ||
           profile == ZCL_RUNTIME_ONION_NODE ||
           profile == ZCL_RUNTIME_LEGACY_COMPAT;
}

const char *app_operator_lane_name(enum zcl_operator_lane lane)
{
    switch (lane) {
    case ZCL_OPERATOR_LANE_CANONICAL:
        return "canonical";
    case ZCL_OPERATOR_LANE_SOAK:
        return "soak";
    case ZCL_OPERATOR_LANE_DEV:
        return "dev";
    case ZCL_OPERATOR_LANE_TEST:
        return "test";
    case ZCL_OPERATOR_LANE_COPY:
        return "copy";
    case ZCL_OPERATOR_LANE_UNKNOWN:
    default:
        return "unknown";
    }
}

bool app_operator_lane_parse(const char *name,
                             enum zcl_operator_lane *out)
{
    if (!name || !out)
        return false;
    if (strcmp(name, "canonical") == 0 ||
        strcmp(name, "live") == 0 ||
        strcmp(name, "main") == 0) {
        *out = ZCL_OPERATOR_LANE_CANONICAL;
        return true;
    }
    if (strcmp(name, "soak") == 0) {
        *out = ZCL_OPERATOR_LANE_SOAK;
        return true;
    }
    if (strcmp(name, "dev") == 0 ||
        strcmp(name, "development") == 0) {
        *out = ZCL_OPERATOR_LANE_DEV;
        return true;
    }
    if (strcmp(name, "test") == 0 ||
        strcmp(name, "ci") == 0) {
        *out = ZCL_OPERATOR_LANE_TEST;
        return true;
    }
    if (strcmp(name, "copy") == 0 ||
        strcmp(name, "repro") == 0) {
        *out = ZCL_OPERATOR_LANE_COPY;
        return true;
    }
    if (strcmp(name, "unknown") == 0) {
        *out = ZCL_OPERATOR_LANE_UNKNOWN;
        return true;
    }
    return false;
}

/* A declared, non-canonical operator lane (dev/soak/test/copy) is an
 * automated development/soak/CI/copy-prove datadir that boots fresh and
 * unattended: it must NOT hard-refuse on the first-run wallet gate or
 * re-baselining such a datadir would wedge. CANONICAL and UNKNOWN (the
 * default / interactive node) are NOT in this set — they keep REFUSE. */
static bool operator_lane_is_automated_noncanonical(enum zcl_operator_lane lane)
{
    switch (lane) {
    case ZCL_OPERATOR_LANE_DEV:
    case ZCL_OPERATOR_LANE_SOAK:
    case ZCL_OPERATOR_LANE_TEST:
    case ZCL_OPERATOR_LANE_COPY:
        return true;
    case ZCL_OPERATOR_LANE_CANONICAL:
    case ZCL_OPERATOR_LANE_UNKNOWN:
    default:
        return false;
    }
}

enum wallet_boot_wallet_action
wallet_at_rest_boot_decision(enum wallet_at_rest_policy policy,
                             bool is_mint,
                             enum zcl_operator_lane lane)
{
    /* Operator intent wins in every context: a passphrase encrypts, an
     * explicit -allow-plaintext-wallet opt-in proceeds (loudly). */
    switch (policy) {
    case WALLET_AT_REST_ENCRYPTED:
        return WALLET_BOOT_CREATE_ENCRYPTED;
    case WALLET_AT_REST_PLAINTEXT_OPTIN:
        return WALLET_BOOT_CREATE_PLAINTEXT;
    case WALLET_AT_REST_REFUSE:
    default:
        break;
    }

    /* policy == REFUSE (no passphrase, no opt-in). Two contexts must not
     * be blocked by the funds-safety gate: */

    /* (1) The OFFLINE anchor-mint producer (-mint-anchor / -mint-anchor-
     *     fast). Its datadir is a transient, no-spend throwaway that folds
     *     bodies and exits after writing the snapshot — a REFUSE here would
     *     break the mint/refold cure producers. Exempt silently (quiet
     *     INFO at the call site — no funds are ever held here). */
    if (is_mint)
        return WALLET_BOOT_CREATE_MINT_EXEMPT;

    /* (2) A declared, non-canonical automated lane (dev/soak/test/copy)
     *     boots fresh datadirs unattended and must not wedge. Downgrade to
     *     the loud plaintext-opt-in behavior (proceed, warn every boot). */
    if (operator_lane_is_automated_noncanonical(lane))
        return WALLET_BOOT_CREATE_PLAINTEXT;

    /* Canonical, unknown, or the interactive default: keep REFUSE — the
     * security posture. No silent plaintext wallet mint on a real node. */
    return WALLET_BOOT_REFUSE;
}

void wallet_at_rest_boot_report(enum wallet_boot_wallet_action action,
                                enum zcl_operator_lane lane)
{
    switch (action) {
    case WALLET_BOOT_CREATE_ENCRYPTED:
        break; /* passphrase supplied — keys wrapped at rest, no notice */
    case WALLET_BOOT_CREATE_MINT_EXEMPT:
        fprintf(stderr, "INFO: offline mint-anchor producer — creating a "
            "transient plaintext wallet (throwaway datadir, no funds held).\n");
        break;
    case WALLET_BOOT_CREATE_PLAINTEXT:
        fprintf(stderr, "\n*** WARNING: creating a NEW wallet with private "
            "keys stored UNENCRYPTED at rest in node.db (anyone with datadir "
            "read access can drain every coin). Proceeding because "
            "-allow-plaintext-wallet was given or this is a non-canonical "
            "operator lane (%s); set ZCL_WALLET_PASSPHRASE to encrypt "
            "instead. ***\n\n", app_operator_lane_name(lane));
        break;
    case WALLET_BOOT_REFUSE:
    default:
        fprintf(stderr, "\nFATAL: refusing to create a new PLAINTEXT wallet "
            "— private keys (transparent WIF, Sapling spending keys, HD seed) "
            "would be stored UNENCRYPTED in node.db. Set ZCL_WALLET_PASSPHRASE "
            "to encrypt at rest (recommended), or pass -allow-plaintext-wallet "
            "to accept the risk explicitly (logged loudly every boot).\n\n");
        break;
    }
}
