/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "controllers/agent_security_posture.h"

#include "config/runtime.h"
#include "jobs/utxo_apply_nullifiers.h"
#include "json/json.h"
#include "services/chain_evidence_authority_service.h"
#include "storage/progress_store.h"
#include "util/blocker.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AGENT_NF_CURSOR_KEY "nullifier_kv.activation_cursor"

static void posture_str(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0)
        return;
    snprintf(dst, dst_sz, "%s", src ? src : "");
}

static int64_t posture_parse_i64(const char *buf, bool *ok)
{
    char *end = NULL;
    errno = 0;
    long long v = strtoll(buf ? buf : "", &end, 10);
    if (!buf || end == buf || errno == ERANGE) {
        if (ok)
            *ok = false;
        return 0;
    }
    while (end && (*end == ' ' || *end == '\t' || *end == '\n' ||
                   *end == '\r'))
        end++;
    if (end && *end != '\0') {
        if (ok)
            *ok = false;
        return 0;
    }
    if (ok)
        *ok = true;
    return (int64_t)v;
}

static void posture_collect_bootstrap(struct agent_security_posture *out,
                                      struct node_db *ndb)
{
    struct chain_evidence_controller cec;
    struct chain_evidence_controller_view view;

    if (!out)
        return;
    if (!ndb)
        ndb = app_runtime_node_db();
    out->node_db_available = ndb != NULL;
    if (!ndb) {
        posture_str(out->bootstrap_model, sizeof(out->bootstrap_model),
                    "unknown_no_node_db");
        posture_str(out->full_history_validation_state,
                    sizeof(out->full_history_validation_state),
                    "unknown_no_node_db");
        return;
    }

    chain_evidence_controller_init(&cec, ndb, csr_instance());
    chain_evidence_controller_snapshot(&cec, &view);
    out->snapshot_anchor_height = view.snapshot_anchor_height;
    out->snapshot_evidence_present =
        view.snapshot_anchor_height >= 0 ||
        view.snapshot_evidence.source_class == CEC_SOURCE_CLASS_SNAPSHOT;
    out->snapshot_full_validation_complete =
        view.state == CEC_FULLY_VALIDATED ||
        view.snapshot_evidence.full_validation_complete;
    out->snapshot_utxo_sha3_verified =
        view.snapshot_evidence.utxo_sha3_verified;
    out->snapshot_flyclient_verified =
        view.snapshot_evidence.mmb_flyclient_proof_verified;
    out->snapshot_chunk_hash_coverage_verified =
        view.snapshot_evidence.chunk_hash_coverage_verified;
    out->trusted_state_present =
        out->snapshot_evidence_present &&
        !out->snapshot_full_validation_complete;

    if (out->snapshot_full_validation_complete) {
        posture_str(out->bootstrap_model, sizeof(out->bootstrap_model),
                    "full_history_validated");
        posture_str(out->full_history_validation_state,
                    sizeof(out->full_history_validation_state), "complete");
    } else if (out->snapshot_evidence_present) {
        posture_str(out->bootstrap_model, sizeof(out->bootstrap_model),
                    "borrowed_but_consensus_bound_snapshot");
        posture_str(out->full_history_validation_state,
                    sizeof(out->full_history_validation_state),
                    "snapshot_seed_not_from_genesis_verified");
    } else {
        posture_str(out->bootstrap_model, sizeof(out->bootstrap_model),
                    "no_snapshot_evidence_recorded");
        posture_str(out->full_history_validation_state,
                    sizeof(out->full_history_validation_state),
                    "unknown_no_snapshot_evidence");
    }
}

static void posture_collect_nullifiers(struct agent_security_posture *out)
{
    sqlite3 *db;
    char buf[32] = {0};
    size_t len = 0;
    bool found = false;
    bool parsed = false;

    if (!out)
        return;
    db = progress_store_db();
    out->progress_store_available = db != NULL;
    if (!db) {
        posture_str(out->nullifier_history_state,
                    sizeof(out->nullifier_history_state),
                    "unknown_no_progress_store");
        return;
    }
    if (!progress_meta_get(db, AGENT_NF_CURSOR_KEY, buf, sizeof(buf) - 1,
                           &len, &found)) {
        posture_str(out->nullifier_history_state,
                    sizeof(out->nullifier_history_state), "unknown_read_failed");
        return;
    }
    if (!found) {
        out->nullifier_cursor_known = true;
        out->nullifier_activation_cursor = 0;
        out->nullifier_history_complete = true;
        posture_str(out->nullifier_history_state,
                    sizeof(out->nullifier_history_state),
                    "complete_from_genesis_or_unactivated");
        return;
    }
    buf[sizeof(buf) - 1] = '\0';
    out->nullifier_activation_cursor = posture_parse_i64(buf, &parsed);
    if (!parsed) {
        posture_str(out->nullifier_history_state,
                    sizeof(out->nullifier_history_state),
                    "unknown_malformed_activation_cursor");
        return;
    }
    out->nullifier_cursor_known = true;
    out->nullifier_backfill_gap =
        out->nullifier_activation_cursor > 0 ||
        blocker_exists(UTXO_APPLY_NF_GAP_BLOCKER_ID);
    out->nullifier_history_complete = !out->nullifier_backfill_gap;
    posture_str(out->nullifier_history_state,
                sizeof(out->nullifier_history_state),
                out->nullifier_backfill_gap
                    ? "gap_below_activation_cursor"
                    : "complete_from_genesis_or_backfilled");
}

static void posture_finalize(struct agent_security_posture *out)
{
    if (!out)
        return;
    out->review_required =
        !out->node_db_available ||
        !out->progress_store_available ||
        out->trusted_state_present ||
        !out->snapshot_full_validation_complete ||
        !out->nullifier_cursor_known ||
        out->nullifier_backfill_gap ||
        !out->nullifier_history_complete;

    if (out->nullifier_backfill_gap) {
        posture_str(out->status, sizeof(out->status),
                    "review_required_nullifier_backfill_gap");
        posture_str(out->next_action, sizeof(out->next_action),
                    "run_shielded_history_backfill_or_from_genesis_refold");
    } else if (!out->node_db_available || !out->progress_store_available ||
               !out->nullifier_cursor_known) {
        posture_str(out->status, sizeof(out->status),
                    "review_required_unknown");
        posture_str(out->next_action, sizeof(out->next_action),
                    "query_running_node_datadir_security_posture");
    } else if (out->trusted_state_present ||
               !out->snapshot_full_validation_complete) {
        posture_str(out->status, sizeof(out->status),
                    "review_required_bootstrap_trust");
        posture_str(out->next_action, sizeof(out->next_action),
                    "finish_sovereign_refold_and_full_history_validation");
    } else {
        posture_str(out->status, sizeof(out->status), "ok");
        posture_str(out->next_action, sizeof(out->next_action), "none");
    }
}

void agent_security_posture_collect(struct agent_security_posture *out,
                                    struct node_db *ndb)
{
    struct agent_security_posture empty = {0};

    if (!out)
        return;
    *out = empty;
    out->snapshot_anchor_height = -1;
    out->nullifier_activation_cursor = -1;
    posture_collect_bootstrap(out, ndb);
    posture_collect_nullifiers(out);
    posture_finalize(out);
}

void agent_push_security_posture_json(struct json_value *out, const char *key,
                                      struct node_db *ndb)
{
    struct agent_security_posture p;
    struct json_value obj = {0};

    if (!out)
        return;
    agent_security_posture_collect(&p, ndb);
    json_set_object(&obj);
    json_push_kv_str(&obj, "schema", "zcl.security_posture.v1");
    json_push_kv_int(&obj, "schema_version", 1);
    json_push_kv_str(&obj, "status", p.status);
    json_push_kv_bool(&obj, "review_required", p.review_required);
    json_push_kv_bool(&obj, "node_db_available", p.node_db_available);
    json_push_kv_bool(&obj, "progress_store_available",
                      p.progress_store_available);
    json_push_kv_str(&obj, "bootstrap_model", p.bootstrap_model);
    json_push_kv_bool(&obj, "snapshot_evidence_present",
                      p.snapshot_evidence_present);
    json_push_kv_bool(&obj, "trusted_state_present",
                      p.trusted_state_present);
    json_push_kv_int(&obj, "snapshot_anchor_height",
                     p.snapshot_anchor_height);
    json_push_kv_bool(&obj, "snapshot_full_validation_complete",
                      p.snapshot_full_validation_complete);
    json_push_kv_bool(&obj, "snapshot_utxo_sha3_verified",
                      p.snapshot_utxo_sha3_verified);
    json_push_kv_bool(&obj, "snapshot_flyclient_verified",
                      p.snapshot_flyclient_verified);
    json_push_kv_bool(&obj, "snapshot_chunk_hash_coverage_verified",
                      p.snapshot_chunk_hash_coverage_verified);
    json_push_kv_str(&obj, "full_history_validation_state",
                     p.full_history_validation_state);
    json_push_kv_bool(&obj, "nullifier_cursor_known",
                      p.nullifier_cursor_known);
    json_push_kv_int(&obj, "nullifier_activation_cursor",
                     p.nullifier_activation_cursor);
    json_push_kv_bool(&obj, "nullifier_history_complete",
                      p.nullifier_history_complete);
    json_push_kv_bool(&obj, "nullifier_backfill_gap",
                      p.nullifier_backfill_gap);
    json_push_kv_str(&obj, "nullifier_history_state",
                     p.nullifier_history_state);
    json_push_kv_str(&obj, "next_action", p.next_action);
    json_push_kv_str(&obj, "semantics",
                     "serving/healthy are liveness signals; "
                     "security_posture names bootstrap and shielded-history "
                     "trust gaps that can require review while the node stays "
                     "available");
    json_push_kv(out, key && key[0] ? key : "security_posture", &obj);
    json_free(&obj);
}
