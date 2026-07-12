/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_shielded_seed — implementation. See config/boot_shielded_seed.h. */

#include "config/boot_shielded_seed.h"

#include "chain/chain.h"                     /* struct block_index */
#include "chain/utxo_snapshot_loader.h"      /* uss_version/uss_shielded */
#include "core/serialize.h"                  /* struct byte_stream */
#include "core/uint256.h"
#include "sapling/incremental_merkle_tree.h" /* sapling/sprout tree */
#include "storage/anchor_kv.h"
#include "storage/nullifier_kv.h"
#include "storage/snapshot_shielded.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "validation/chainstate.h"           /* active_chain_at */
#include "validation/main_state.h"           /* struct main_state */

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void boot_capture_shielded(struct uss_handle *h, bool load_ok,
                           uint8_t **sapling, uint32_t *sapling_len,
                           bool *shielded_v3,
                           uint8_t **sprout, uint32_t *sprout_len,
                           uint8_t **nfs, uint64_t *nf_count)
{
    if (!h || !load_ok)
        return;

    /* v2: Sapling frontier only. Capture it and return — no Sprout/nullifiers,
     * so the v3 cure does NOT engage (the old cursor-reset path runs). */
    if (uss_version(h) == 2) {
        const uint8_t *fblob = NULL;
        uint32_t flen = 0;
        if (uss_frontier(h, &fblob, &flen) && fblob && flen > 0 && !*sapling) {
            *sapling = zcl_malloc(flen, "boot.embedded_frontier");
            if (*sapling) {
                memcpy(*sapling, fblob, flen);
                *sapling_len = flen;
                LOG_INFO("boot", "[boot] -load-snapshot-at-own-height: v2 "
                         "snapshot carries a %u-byte Sapling frontier", flen);
            } else {
                LOG_WARN("boot", "[boot] -load-snapshot-at-own-height: OOM "
                         "copying the embedded frontier (%u B) — falling back to "
                         "the block-replay rebuild", flen);
            }
        } else if (!(fblob && flen > 0)) {
            LOG_WARN("boot", "[boot] -load-snapshot-at-own-height: v2 header but "
                     "no readable frontier section — falling back to the "
                     "block-replay rebuild");
        }
        return;
    }

    if (uss_version(h) != 3)
        return;

    const uint8_t *sap = NULL, *spr = NULL, *nf = NULL;
    uint32_t sap_len = 0, spr_len = 0;
    uint64_t nfc = 0;
    if (!uss_shielded(h, &sap, &sap_len, &spr, &spr_len, &nf, &nfc)) {
        LOG_WARN("boot", "[boot] -load-snapshot-at-own-height: v3 header but no "
                 "readable shielded section — falling back to the block-replay "
                 "rebuild");
        return;
    }

    bool cap_ok = true;
    if (sap_len > 0 && !*sapling) {
        *sapling = zcl_malloc(sap_len, "boot.embedded_frontier");
        if (*sapling) { memcpy(*sapling, sap, sap_len); *sapling_len = sap_len; }
        else cap_ok = false;
    }
    if (cap_ok && spr_len > 0) {
        *sprout = zcl_malloc(spr_len, "boot.embedded_sprout");
        if (*sprout) { memcpy(*sprout, spr, spr_len); *sprout_len = spr_len; }
        else cap_ok = false;
    }
    if (cap_ok && nfc > 0) {
        size_t nf_bytes = (size_t)nfc * SNAPSHOT_NF_RECORD_BYTES;
        *nfs = zcl_malloc(nf_bytes, "boot.embedded_nfs");
        if (*nfs) { memcpy(*nfs, nf, nf_bytes); *nf_count = nfc; }
        else cap_ok = false;
    }
    if (cap_ok) {
        *shielded_v3 = true;
        LOG_INFO("boot", "[boot] -load-snapshot-at-own-height: v3 snapshot "
                 "carries Sapling(%uB)+Sprout(%uB) frontiers + %llu nullifiers",
                 sap_len, spr_len, (unsigned long long)nfc);
    } else {
        LOG_WARN("boot", "[boot] -load-snapshot-at-own-height: OOM copying the "
                 "v3 shielded section — falling back to the cursor-reset "
                 "(block-replay backfill) path");
        free(*sprout); *sprout = NULL; *sprout_len = 0;
        free(*nfs);    *nfs = NULL;    *nf_count = 0;
    }
}

/* The birth-defect cure proper: reset the adoption cursor to 0 (history
 * complete), seed the root-verified Sapling frontier, add the Sprout frontier,
 * and bulk-add the nullifier set — all in the caller's open transaction. Only
 * the LATEST (seed-height) frontier is seeded, so an old-anchor Sapling spend
 * below the seed reads MISSING under cursor=0; the nullifier set and current
 * frontier ARE complete. Sprout + nullifiers have no header commitment — their
 * trust bottoms at the snapshot body SHA3 (cured later by self-derivation). */
static bool seed_shielded_from_snapshot(
    struct sqlite3 *rpdb, int seed_h,
    const uint8_t *sap, uint32_t sap_len,
    const struct uint256 *sap_expected_root,
    const uint8_t *spr, uint32_t spr_len,
    const uint8_t *nfs, uint64_t nf_count)
{
    if (!rpdb || !sap || sap_len == 0 || !sap_expected_root) {
        LOG_WARN("boot", "[boot] seed_shielded: missing Sapling frontier args");
        return false;
    }
    if (!anchor_kv_reset_in_tx(rpdb, 0))   /* cursor=0 = history complete */
        return false;

    /* Sapling — verified against the header-committed root; fail-closed. */
    struct incremental_merkle_tree sap_tree;
    sapling_tree_init(&sap_tree);
    struct byte_stream ss;
    stream_init_from_data(&ss, sap, sap_len);
    if (!incremental_tree_deserialize(&sap_tree, &ss)) {
        LOG_WARN("boot", "[boot] seed_shielded: Sapling frontier decode failed");
        return false;
    }
    if (!anchor_kv_seed_frontier_row(rpdb, ANCHOR_POOL_SAPLING, &sap_tree,
                                     seed_h, sap_expected_root)) {
        LOG_WARN("boot", "[boot] seed_shielded: Sapling frontier REFUSED "
                 "(root mismatch vs hashFinalSaplingRoot) — fail-closed");
        return false;
    }

    /* Sprout — no header commitment; trust bottoms at the snapshot SHA3. */
    if (spr && spr_len > 0) {
        struct incremental_merkle_tree spr_tree;
        sprout_tree_init(&spr_tree);
        struct byte_stream ps;
        stream_init_from_data(&ps, spr, spr_len);
        if (!incremental_tree_deserialize(&spr_tree, &ps)) {
            LOG_WARN("boot", "[boot] seed_shielded: Sprout frontier decode "
                     "failed");
            return false;
        }
        if (!anchor_kv_add_tree(rpdb, ANCHOR_POOL_SPROUT, &spr_tree, seed_h)) {
            LOG_WARN("boot", "[boot] seed_shielded: Sprout frontier add failed");
            return false;
        }
    }

    /* Nullifier set — the consensus double-spend guards; a complete set makes
     * the seeded node reject a re-reveal exactly as zclassicd does. */
    if (nfs && nf_count > 0) {
        if (!nullifier_kv_ensure_schema(rpdb)) {
            LOG_WARN("boot", "[boot] seed_shielded: nullifier schema failed");
            return false;
        }
        for (uint64_t i = 0; i < nf_count; i++) {
            const uint8_t *rec = nfs + i * SNAPSHOT_NF_RECORD_BYTES;
            uint8_t pool = 0, nf[32];
            int64_t hgt = 0;
            snapshot_shielded_unpack_nf(rec, &pool, nf, &hgt);
            int kvpool = (pool == 0) ? NULLIFIER_POOL_SPROUT
                                     : NULLIFIER_POOL_SAPLING;
            if (hgt < 0) hgt = 0;
            if (!nullifier_kv_add(rpdb, nf, kvpool, hgt)) {
                LOG_WARN("boot", "[boot] seed_shielded: nullifier_kv_add failed "
                         "at record %llu", (unsigned long long)i);
                return false;
            }
        }
    }
    return true;
}

bool boot_shielded_cure_or_reset_in_tx(
    struct sqlite3 *rpdb, struct main_state *ms, int seed_h,
    bool sapling_verified, bool shielded_v3,
    const uint8_t *sapling, uint32_t sapling_len,
    const uint8_t *sprout, uint32_t sprout_len,
    const uint8_t *nfs, uint64_t nf_count)
{
    bool cure = shielded_v3 && sapling_verified && sapling && sapling_len > 0;
    if (!cure)
        return anchor_kv_reset_in_tx(rpdb, seed_h);

    const struct block_index *seed_bi =
        ms ? active_chain_at(&ms->chain_active, seed_h) : NULL;
    if (!seed_bi) {
        LOG_WARN("boot", "[boot] -load-snapshot-at-own-height: v3 cure wanted "
                 "but seed block_index missing at h=%d — falling back to cursor "
                 "reset", seed_h);
        return anchor_kv_reset_in_tx(rpdb, seed_h);
    }
    if (!seed_shielded_from_snapshot(rpdb, seed_h, sapling, sapling_len,
                                     &seed_bi->hashFinalSaplingRoot,
                                     sprout, sprout_len, nfs, nf_count)) {
        /* Fail-closed: never silently degrade to an empty anchor set at
         * cursor=0. Refuse so the boot aborts rather than seed partial state. */
        LOG_WARN("boot", "[boot] -load-snapshot-at-own-height: v3 shielded seed "
                 "FAILED — refusing to arm the fold");
        return false;
    }
    fprintf(stderr,
            "[boot] -load-snapshot-at-own-height: v3 SHIELDED seed installed at "
            "h=%d (Sapling frontier root-verified, Sprout%s, %llu nullifiers, "
            "activation_cursor=0)\n",
            seed_h, sprout_len ? " installed" : " absent",
            (unsigned long long)nf_count);
    return true;
}
