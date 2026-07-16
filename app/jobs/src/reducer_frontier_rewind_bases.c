/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * reducer_frontier_rewind_bases — rolling self-verified rewind-base
 * observability (Pillar 3 of the always-sync spine). Split out of
 * reducer_frontier_dump.c to stay under the E1 file-size ceiling.
 *
 * A rewind base is only valid if it was SELF-DERIVED (folded from real block
 * bodies by THIS node, then cross-checked against its own commitment) — never
 * a borrowed value. Today three kinds exist:
 *
 *   - compiled_checkpoint: the baked SHA3 UTXO checkpoint
 *     (REDUCER_FRONTIER_TRUSTED_ANCHOR). self_derived=true — the self-mint
 *     producer (config/src/boot_mint_anchor.c) folds bodies from genesis and
 *     HARD-ASSERTS its own commitment equals this compiled value before ever
 *     accepting it, so the constant is a proven fold result, not a trusted
 *     literal.
 *   - sealed_coins_sha3: every self-valid slot in the seal_kv 4-slot ring
 *     (storage/seal_kv.h) — each coins_sha3 was computed by
 *     seal_candidate_emit_in_tx scanning THIS node's live, forward-folded
 *     coins_kv (app/services/src/seal_service.c). self_derived=true always;
 *     `ratified` marks whether it has additionally cleared the
 *     finality-depth + input-coverage + active-chain-agreement gates.
 *   - finalized_utxo_sha3: the node_state 'utxo_sha3' commitment
 *     (coins/utxo_commitment.h). self_derived=false — today this is written
 *     ONLY at snapshot-import (seed_integrity_stamp_utxo_sha3), never
 *     re-derived by ongoing forward fold, so it is a borrowed provenance
 *     marker, not yet a self-verified rung (see docs/HANDOFF.md and
 *     docs/work/self-verified-tip-plan.md).
 *
 * READ-ONLY composition over each subsystem's own public dump/read API — no
 * new storage, no mutation, no lock ordering surprises (every call below
 * either takes no lock or acquires the RECURSIVE progress_store_tx_lock
 * itself, the same pattern reducer_frontier_log_frontier already relies on
 * from inside reducer_frontier_dump_state_json). */

#include "reducer_frontier_rewind_bases.h"

#include "jobs/reducer_frontier.h"

#include "chain/checkpoints.h"
#include "coins/utxo_commitment.h"
#include "config/runtime.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "models/database.h"
#include "storage/seal_kv.h"

#include <stdbool.h>
#include <stdint.h>

/* Push one rewind-base entry onto `arr` and fold it into the running
 * "nearest base at or below the current tip" tracker (the O(delta) recovery
 * distance an operator/agent cares about). commitment_hex may be NULL/empty
 * when no commitment hash applies. */
static void push_rewind_base(struct json_value *arr, const char *kind,
                             int32_t height, bool self_derived, bool ratified,
                             const char *commitment_hex, int32_t hstar,
                             int32_t *nearest_height, const char **nearest_kind,
                             bool *nearest_self_derived, bool *nearest_ratified)
{
    struct json_value o = {0};
    json_set_object(&o);
    json_push_kv_str(&o, "kind", kind);
    json_push_kv_int(&o, "height", height);
    json_push_kv_bool(&o, "self_derived", self_derived);
    json_push_kv_bool(&o, "ratified", ratified);
    json_push_kv_int(&o, "distance_from_tip", (int64_t)hstar - height);
    json_push_kv_str(&o, "commitment_sha3",
                     commitment_hex ? commitment_hex : "");
    json_push_back(arr, &o);
    json_free(&o);

    if (height <= hstar &&
        (*nearest_height < 0 || height > *nearest_height)) {
        *nearest_height = height;
        *nearest_kind = kind;
        *nearest_self_derived = self_derived;
        *nearest_ratified = ratified;
    }
}

void reducer_frontier_push_rewind_bases_json(struct json_value *out,
                                             int32_t hstar)
{
    struct json_value bases = {0};
    json_set_array(&bases);

    int32_t nearest_height = -1;
    const char *nearest_kind = "";
    bool nearest_self_derived = false;
    bool nearest_ratified = false;

    /* 1) The compiled SHA3 UTXO checkpoint — always available, network gate
     * aside (get_sha3_utxo_checkpoint always returns the mainnet constant;
     * callers on non-mainnet networks already treat REDUCER_FRONTIER_
     * TRUSTED_ANCHOR as inapplicable, same as the other direct-macro call
     * sites in block_index_loader_rebuild.c / header_band_service.c). */
    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    int32_t compiled_height = cp ? cp->height : REDUCER_FRONTIER_TRUSTED_ANCHOR;
    char compiled_hex[65] = "";
    if (cp)
        HexStr(cp->sha3_hash, 32, false, compiled_hex, sizeof(compiled_hex));
    push_rewind_base(&bases, "compiled_checkpoint", compiled_height, true,
                     false, compiled_hex, hstar,
                     &nearest_height, &nearest_kind, &nearest_self_derived,
                     &nearest_ratified);

    /* 2) Every self-valid slot in the seal_kv ring. Reuses seal_kv's OWN
     * public dump (storage/seal_kv.h) rather than duplicating ring-iteration
     * logic here — seal_kv.c is a sibling lane's file. */
    struct json_value seal_state = {0};
    json_init(&seal_state);
    if (seal_kv_dump_state_json(&seal_state, NULL)) {
        const struct json_value *slots = json_get(&seal_state, "slots");
        size_t n = slots ? json_size(slots) : 0;
        for (size_t i = 0; i < n; i++) {
            const struct json_value *item = json_at(slots, i);
            if (!item)
                continue;
            if (!json_get_bool(json_get(item, "self_sha3_valid")))
                continue; /* torn/corrupt slot — not a valid rewind base */
            push_rewind_base(&bases, "sealed_coins_sha3",
                             (int32_t)json_get_int(json_get(item, "height")),
                             true,
                             json_get_bool(json_get(item, "ratified")),
                             json_get_str(json_get(item, "coins_sha3")),
                             hstar, &nearest_height, &nearest_kind,
                             &nearest_self_derived, &nearest_ratified);
        }
    }
    json_free(&seal_state);

    /* 3) The finalized 'utxo_sha3' node_state commitment, when a node_db is
     * wired and a stamp exists. Best-effort / optional: absent on a
     * from-genesis-only datadir that has never imported a snapshot. */
    struct node_db *ndb = app_runtime_node_db();
    if (ndb && ndb->open && ndb->db) {
        uint8_t hash[32];
        int32_t stamp_height = -1;
        uint64_t stamp_count = 0;
        if (utxo_commitment_sha3_load(ndb->db, hash, &stamp_height,
                                      &stamp_count) &&
            stamp_height >= 0) {
            char hex[65];
            HexStr(hash, 32, false, hex, sizeof(hex));
            push_rewind_base(&bases, "finalized_utxo_sha3", stamp_height,
                             false, false, hex, hstar,
                             &nearest_height, &nearest_kind,
                             &nearest_self_derived, &nearest_ratified);
        }
    }

    int64_t bases_count = (int64_t)json_size(&bases);
    json_push_kv(out, "rewind_bases", &bases);
    json_free(&bases);

    json_push_kv_int(out, "rewind_bases_count", bases_count);
    json_push_kv_str(out, "nearest_rewind_base_kind",
                     nearest_height >= 0 ? nearest_kind : "");
    json_push_kv_int(out, "nearest_rewind_base_height", nearest_height);
    json_push_kv_bool(out, "nearest_rewind_base_self_derived",
                      nearest_height >= 0 && nearest_self_derived);
    json_push_kv_bool(out, "nearest_rewind_base_ratified",
                      nearest_height >= 0 && nearest_ratified);
    json_push_kv_int(out, "nearest_rewind_distance",
                     nearest_height >= 0 ? (int64_t)hstar - nearest_height
                                         : -1);
}
