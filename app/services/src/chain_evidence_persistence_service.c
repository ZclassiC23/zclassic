/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "services/chain_evidence_persistence_service.h"

#include "models/database.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <string.h>

#define CEC_RECORD_MAGIC 0x43454345u
#define CEC_RECORD_VERSION 2u

struct persisted_evidence_record {
    uint32_t magic;
    uint32_t version;
    uint32_t source_class;
    uint32_t publish_state;
    uint8_t header_ancestry_linked;
    uint8_t chainwork_recomputed;
    uint8_t nakamoto_selected_best_work;
    uint8_t block_bytes_hash_checked;
    uint8_t utxo_sha3_verified;
    uint8_t mmb_flyclient_proof_verified;
    uint8_t chunk_hash_coverage_verified;
    uint8_t full_validation_complete;
};

struct persisted_evidence_record_v1 {
    uint32_t magic;
    uint32_t version;
    uint8_t header_ancestry_linked;
    uint8_t chainwork_recomputed;
    uint8_t nakamoto_selected_best_work;
    uint8_t block_bytes_hash_checked;
    uint8_t utxo_sha3_verified;
    uint8_t mmb_flyclient_proof_verified;
    uint8_t chunk_hash_coverage_verified;
    uint8_t full_validation_complete;
};

static void evidence_to_persisted(const struct chain_evidence_record *in,
                                  struct persisted_evidence_record *out)
{
    memset(out, 0, sizeof(*out));
    out->magic = CEC_RECORD_MAGIC;
    out->version = CEC_RECORD_VERSION;
    if (!in)
        return;
    out->source_class = (uint32_t)in->source_class;
    out->publish_state = (uint32_t)in->publish_state;
    out->header_ancestry_linked = in->header_ancestry_linked ? 1 : 0;
    out->chainwork_recomputed = in->chainwork_recomputed ? 1 : 0;
    out->nakamoto_selected_best_work =
        in->nakamoto_selected_best_work ? 1 : 0;
    out->block_bytes_hash_checked = in->block_bytes_hash_checked ? 1 : 0;
    out->utxo_sha3_verified = in->utxo_sha3_verified ? 1 : 0;
    out->mmb_flyclient_proof_verified =
        in->mmb_flyclient_proof_verified ? 1 : 0;
    out->chunk_hash_coverage_verified =
        in->chunk_hash_coverage_verified ? 1 : 0;
    out->full_validation_complete = in->full_validation_complete ? 1 : 0;
}

static bool evidence_from_persisted(const void *buf, size_t len,
                                    struct chain_evidence_record *out)
{
    const struct persisted_evidence_record *p = buf;
    if (!buf || !out || len < sizeof(struct persisted_evidence_record_v1))
        return false;
    memset(out, 0, sizeof(*out));
    if (len == sizeof(struct persisted_evidence_record_v1)) {
        const struct persisted_evidence_record_v1 *v1 = buf;
        if (v1->magic != CEC_RECORD_MAGIC || v1->version != 1u)
            return false;
        out->publish_state = CEC_PUBLISH_LOCAL_EVIDENCE;
        out->header_ancestry_linked = v1->header_ancestry_linked != 0;
        out->chainwork_recomputed = v1->chainwork_recomputed != 0;
        out->nakamoto_selected_best_work =
            v1->nakamoto_selected_best_work != 0;
        out->block_bytes_hash_checked = v1->block_bytes_hash_checked != 0;
        out->utxo_sha3_verified = v1->utxo_sha3_verified != 0;
        out->mmb_flyclient_proof_verified =
            v1->mmb_flyclient_proof_verified != 0;
        out->chunk_hash_coverage_verified =
            v1->chunk_hash_coverage_verified != 0;
        out->full_validation_complete = v1->full_validation_complete != 0;
        if (out->utxo_sha3_verified &&
            out->mmb_flyclient_proof_verified &&
            out->chunk_hash_coverage_verified)
            out->source_class = CEC_SOURCE_CLASS_SNAPSHOT;
        else if (out->block_bytes_hash_checked)
            out->source_class = CEC_SOURCE_CLASS_NATIVE_P2P;
        return true;
    }
    if (len != sizeof(*p) ||
        p->magic != CEC_RECORD_MAGIC ||
        p->version != CEC_RECORD_VERSION)
        return false;

    out->source_class = (enum chain_evidence_source_class)p->source_class;
    out->publish_state = (enum chain_evidence_publish_state)p->publish_state;
    out->header_ancestry_linked = p->header_ancestry_linked != 0;
    out->chainwork_recomputed = p->chainwork_recomputed != 0;
    out->nakamoto_selected_best_work = p->nakamoto_selected_best_work != 0;
    out->block_bytes_hash_checked = p->block_bytes_hash_checked != 0;
    out->utxo_sha3_verified = p->utxo_sha3_verified != 0;
    out->mmb_flyclient_proof_verified =
        p->mmb_flyclient_proof_verified != 0;
    out->chunk_hash_coverage_verified =
        p->chunk_hash_coverage_verified != 0;
    out->full_validation_complete = p->full_validation_complete != 0;
    return true;
}

struct zcl_result chain_evidence_store_persist(
    struct chain_evidence_controller *authority,
    const char *key,
    const struct chain_evidence_record *evidence)
{
    struct persisted_evidence_record p;
    evidence_to_persisted(evidence, &p);
    /* Preserve the original decision exactly: success iff authority &&
     * authority->ndb && node_db_state_set(...) all held. Each failure now
     * carries a reason instead of a bare false. */
    if (!authority) {
        LOG_WARN("cec.store", "persist: null authority key=%s",
                 key ? key : "(null)");
        return ZCL_ERR(-1, "persist: null authority key=%s",
                       key ? key : "(null)");
    }
    if (!authority->ndb) {
        LOG_WARN("cec.store", "persist: null ndb key=%s",
                 key ? key : "(null)");
        return ZCL_ERR(-2, "persist: null ndb key=%s",
                       key ? key : "(null)");
    }
    if (!node_db_state_set(authority->ndb, key, &p, sizeof(p))) {
        LOG_WARN("cec.store", "persist: node_db_state_set failed key=%s",
                 key ? key : "(null)");
        return ZCL_ERR(-3, "persist: node_db_state_set failed key=%s",
                       key ? key : "(null)");
    }
    return ZCL_OK;
}

struct zcl_result chain_evidence_store_load(struct node_db *ndb,
                                            const char *key,
                                            struct chain_evidence_record *out)
{
    struct persisted_evidence_record p;
    size_t len = 0;
    /* Reject null out before clearing caller storage; then load and parse.
     * Each branch reports why instead of returning a bare false. */
    if (!out) {
        LOG_WARN("cec.store", "load: null out key=%s", key ? key : "(null)");
        return ZCL_ERR(-1, "load: null out key=%s", key ? key : "(null)");
    }
    memset(out, 0, sizeof(*out));
    if (!ndb) {
        LOG_WARN("cec.store", "load: null ndb key=%s", key ? key : "(null)");
        return ZCL_ERR(-2, "load: null ndb key=%s", key ? key : "(null)");
    }
    if (!node_db_state_get(ndb, key, &p, sizeof(p), &len)) {
        /* Missing key is the common, expected case at boot; report it but
         * keep it diagnostic (out stays zeroed, exactly as before). */
        LOG_WARN("cec.store", "load: node_db_state_get miss key=%s",
                 key ? key : "(null)");
        return ZCL_ERR(-3, "load: node_db_state_get miss key=%s",
                       key ? key : "(null)");
    }
    if (!evidence_from_persisted(&p, len, out)) {
        LOG_WARN("cec.store",
                 "load: evidence_from_persisted rejected key=%s len=%zu",
                 key ? key : "(null)", len);
        return ZCL_ERR(-4,
                       "load: evidence_from_persisted rejected key=%s len=%zu",
                       key ? key : "(null)", len);
    }
    return ZCL_OK;
}

static bool block_evidence_key(const struct uint256 *hash,
                               char out[sizeof("cec.block_evidence.") + 64])
{
    char hex[65];
    if (!hash || !out)
        return false;
    uint256_get_hex(hash, hex);
    snprintf(out, sizeof("cec.block_evidence.") + 64,
             "cec.block_evidence.%s", hex);
    return true;
}

struct zcl_result chain_evidence_controller_mark_block_evidence(
    struct chain_evidence_controller *authority,
    const struct uint256 *block_hash,
    const struct chain_evidence_record *evidence)
{
    char key[sizeof("cec.block_evidence.") + 64];
    /* Same combined guard as before, now reporting which input was bad.
     * block_evidence_key only fails on the null checks already covered, so
     * the original combined `||` decision is preserved bit-for-bit. */
    if (!authority || !authority->ndb || !block_hash || !evidence ||
        !block_evidence_key(block_hash, key)) {
        LOG_WARN("cec.store",
                 "mark_block_evidence: bad args authority=%d ndb=%d "
                 "block_hash=%d evidence=%d",
                 authority != NULL, authority && authority->ndb,
                 block_hash != NULL, evidence != NULL);
        return ZCL_ERR(-1,
                       "mark_block_evidence: bad args authority=%d ndb=%d "
                       "block_hash=%d evidence=%d",
                       authority != NULL, authority && authority->ndb,
                       block_hash != NULL, evidence != NULL);
    }
    return chain_evidence_store_persist(authority, key, evidence);
}
