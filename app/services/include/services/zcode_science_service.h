/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: CAS-authoritative ZCODE science write/read services.
 *
 * Every write is an exact, expiring plan followed by a confirm:true commit.
 * The plan row (zcode_science_plans) is the durable idempotency ledger: it
 * persists the request identity hash (sha3-256 over the request domain,
 * kind, exact wire, and auxiliary roots), the exact wire, the expiry, and
 * the result root once committed. Committing the same request twice returns
 * the same result root and stores one CAS object. Expiry gates NEW
 * submissions only — stored evidence revalidates forever.
 *
 * The SQL projection tables are rebuildable lookup keys over the canonical
 * CAS wires; zcode_science_rebuild() drops and re-derives them from the
 * workspace CAS via vcs/zcode_science_index.h. */

#ifndef ZCL_SERVICES_ZCODE_SCIENCE_SERVICE_H
#define ZCL_SERVICES_ZCODE_SCIENCE_SERVICE_H

#include "base/result.h"
#include "models/zcode_science.h"
#include "vcs/build_action.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZCODE_SCIENCE_PLAN_TTL_SECONDS 600

struct zcode_science_plan_out {
    char plan_root[65];
    char request_hash[65];
    int64_t expires_unix;
    bool already_planned; /* idempotent re-plan of the same request */
};

struct zcode_science_commit_out {
    char result_root[65];
    bool already_committed; /* durable idempotent reattach */
};

/* study.plan: validate the study_spec.v1 wire (structural + the submission
 * window must hold at now) and persist the exact expiring plan. */
struct zcl_result zcode_science_study_plan(
    struct node_db *ndb, const char *workspace,
    const uint8_t *wire, size_t wire_len, int64_t now,
    struct zcode_science_plan_out *out);

/* study.commit: confirm:true + unexpired plan + exact-wire agreement;
 * idempotent reattach returns the committed root. Writes the wire to CAS
 * addressed by root and updates the projection. */
struct zcl_result zcode_science_study_commit(
    struct node_db *ndb, const char *workspace,
    const uint8_t *wire, size_t wire_len, bool confirm, int64_t now,
    struct zcode_science_commit_out *out);

/* Projection reads for study.show / study.list. show returns false via
 * *found when the root is not projected. */
struct zcl_result zcode_science_study_show(
    struct node_db *ndb, const char *study_root_hex,
    struct db_zcode_science_entry *out, bool *found);
struct zcl_result zcode_science_study_list(
    struct node_db *ndb, struct db_zcode_science_entry *out, int max,
    int *count);

/* work.plan/commit: the wire is benchmark_result.v2 (magic "ZCBEN2") or
 * reproduction.v1 (magic "ZCREPR"). For a v2 result the caller also
 * supplies the method/profile wires (stored to CAS at plan, addressed by
 * their canonical roots) and the executed fixed action; commit re-runs the
 * hardened S1 cross-validator incl. the canonical action binding. */
struct zcl_result zcode_science_work_plan(
    struct node_db *ndb, const char *workspace,
    const uint8_t *wire, size_t wire_len,
    const uint8_t *method_wire, size_t method_len,
    const uint8_t *profile_wire, size_t profile_len,
    const struct vcs_build_action_v1 *action,
    int64_t now, struct zcode_science_plan_out *out);
struct zcl_result zcode_science_work_commit(
    struct node_db *ndb, const char *workspace,
    const uint8_t *wire, size_t wire_len,
    const struct vcs_build_action_v1 *action,
    bool confirm, int64_t now, struct zcode_science_commit_out *out);

/* work.status / work.receipt: projection reads by result or reproduction
 * root. receipt additionally re-loads the canonical CAS wire and verifies
 * the projection row against it; *kind is "result" or "reproduction". */
struct zcl_result zcode_science_work_status(
    struct node_db *ndb, const char *root_hex,
    struct db_zcode_science_entry *out, const char **kind, bool *found);
struct zcl_result zcode_science_work_receipt(
    struct node_db *ndb, const char *workspace, const char *root_hex,
    struct db_zcode_science_entry *out, const char **kind);

/* review.submit: PLAN_COMMIT contract in one leaf — without confirm the
 * call validates and persists the expiring plan; with confirm:true it
 * commits. Commit requires the findings wire in CAS and
 * review.created_unix >= findings.created_unix (H1). */
struct zcl_result zcode_science_review_submit(
    struct node_db *ndb, const char *workspace,
    const uint8_t *wire, size_t wire_len, bool confirm, int64_t now,
    struct zcode_science_plan_out *plan_out,
    struct zcode_science_commit_out *commit_out);

/* vote.submit: PLAN_COMMIT contract in one leaf. Commit seals nothing —
 * the wire arrives sealed; it runs curation_vote_verify against the
 * expected network genesis, voter zid, and signer (cross-network identity
 * rejection), rejects voter+sequence replay, and is idempotent by vote id. */
struct zcl_result zcode_science_vote_submit(
    struct node_db *ndb, const char *workspace,
    const uint8_t *wire, size_t wire_len,
    const uint8_t expected_network_genesis[32],
    const uint8_t expected_voter_zid[32],
    const uint8_t expected_signer[32],
    bool confirm, int64_t now,
    struct zcode_science_plan_out *plan_out,
    struct zcode_science_commit_out *commit_out);

/* Drop the six projection tables and rebuild them from the workspace CAS.
 * Rebuild-equivalence proof: output after rebuild is identical to output
 * before the drop. Plans are not touched. */
struct zcode_science_rebuild_out {
    size_t studies, results, reproductions, findings, votes, reviews;
};
struct zcl_result zcode_science_rebuild(
    struct node_db *ndb, const char *workspace, int64_t now,
    struct zcode_science_rebuild_out *out);

#endif /* ZCL_SERVICES_ZCODE_SCIENCE_SERVICE_H */
