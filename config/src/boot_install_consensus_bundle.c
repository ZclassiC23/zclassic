/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_install_consensus_bundle.c — the -install-consensus-bundle=PATH consumer
 * (lane A2 of the sovereign shielded-state cure). Kept in its own file so it
 * carries one focused responsibility: gate a consensus-state bundle FILE through
 * the publication compare-and-swap and, only on ADMIT, atomically activate-
 * install it into the live progress store. TERMINAL — every path _exit()s.
 *
 * Contract declared in config/boot.h. */

#include "config/boot.h"

#include "config/consensus_state_snapshot_install.h"
#include "consensus_state_snapshot_install_internal.h" /* candidate_lease_begin/end */
#include "framework/condition.h"                     /* condition_engine_*_main_state */
#include "services/consensus_state_chain_binding_service.h"
#include "services/consensus_state_publication_cas.h"
#include "storage/consensus_state_bundle_codec.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "validation/main_state.h"

#include <fcntl.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ICB_SUBSYS "install_consensus_bundle"
#define ICB_DECISION_RECORD_NAME "consensus_state_publication_decision.v1"

/* Terminal helpers: print a named terminal to stderr + node.log and _exit. */
static _Noreturn void icb_refuse(const char *fmt, ...)
{
    char reason[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, sizeof(reason), fmt, ap);
    va_end(ap);
    fprintf(stderr, "REFUSED: -install-consensus-bundle: %s\n", reason);
    LOG_WARN(ICB_SUBSYS, "%s", reason);
    _exit(EXIT_FAILURE);
}

/* The canonical daily-driver datadir (agent_lane_runtime.c topology). A live
 * cutover there is owner-gated behind ZCL_DEPLOY_ALLOW_CANONICAL; dev/copy
 * datadirs proceed. */
static bool icb_is_canonical_datadir(const char *datadir)
{
    if (!datadir || !datadir[0])
        return false;
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return false;
    char canonical[1024];
    int n = snprintf(canonical, sizeof(canonical), "%s/.zclassic-c23", home);
    if (n <= 0 || (size_t)n >= sizeof(canonical))
        return false;
    return strcmp(datadir, canonical) == 0 ||
           strcmp(datadir, "~/.zclassic-c23") == 0;
}

/* Read the producer source receipt from the pinned, already-validated bundle
 * handle. The CAS re-checks the receipt's self-consistency and its binding to
 * the manifest source digest, so a misread fails closed there. */
static bool icb_read_source_receipt(sqlite3 *bundle_db,
                                    struct consensus_state_source_receipt *out)
{
    static const char sql[] =
        "SELECT source_epoch_digest,source_tree_root,running_binary_digest,"
        "toolchain_digest,build_inputs_digest,chain_corpus_digest,source_clean,"
        "validation_profile,producer_commit,fold_cursor,receipt_digest "
        "FROM source_receipt WHERE singleton=1";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(bundle_db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    memset(out, 0, sizeof(*out));
    bool ok = sqlite3_step(st) == SQLITE_ROW; // raw-sql-ok:read-only-introspection
    const struct { int col; uint8_t *dst; } blobs[] = {
        {0, out->source_epoch_digest}, {1, out->source_tree_root},
        {2, out->running_binary_digest}, {3, out->toolchain_digest},
        {4, out->build_inputs_digest}, {5, out->chain_corpus_digest},
        {10, out->receipt_digest},
    };
    for (size_t i = 0; ok && i < sizeof(blobs) / sizeof(blobs[0]); i++) {
        ok = sqlite3_column_type(st, blobs[i].col) == SQLITE_BLOB &&
             sqlite3_column_bytes(st, blobs[i].col) == 32;
        if (ok)
            memcpy(blobs[i].dst, sqlite3_column_blob(st, blobs[i].col), 32);
    }
    if (ok) {
        const unsigned char *commit = sqlite3_column_text(st, 8);
        ok = commit && sqlite3_column_bytes(st, 8) == 40;
        if (ok) {
            memcpy(out->producer_commit, commit, 40);
            out->producer_commit[40] = '\0';
            out->source_clean = sqlite3_column_int(st, 6) == 1;
            out->validation_profile = (uint8_t)sqlite3_column_int(st, 7);
            out->fold_cursor = sqlite3_column_int64(st, 9);
        }
    }
    sqlite3_finalize(st);
    return ok;
}

void boot_install_consensus_bundle(struct node_db *ndb, struct main_state *ms,
                                   const char *bundle_path, const char *datadir)
{
    (void)ndb;
    if (!bundle_path || !bundle_path[0] || !datadir || !datadir[0])
        icb_refuse("empty bundle path or datadir");

    /* (1) Containment: the canonical daily-driver is owner-gated. */
    if (icb_is_canonical_datadir(datadir) &&
        !(getenv("ZCL_DEPLOY_ALLOW_CANONICAL") &&
          getenv("ZCL_DEPLOY_ALLOW_CANONICAL")[0]))
        icb_refuse("datadir %s is the canonical lane; set "
                   "ZCL_DEPLOY_ALLOW_CANONICAL=1 to cut over the daily-driver, "
                   "or run the install on a dev/copy datadir first", datadir);

    enum consensus_state_target_lane lane =
        icb_is_canonical_datadir(datadir)
            ? CONSENSUS_STATE_TARGET_LANE_CANONICAL
            : CONSENSUS_STATE_TARGET_LANE_COPY_PROOF;

    /* (2) Admit + strictly validate the immutable bundle. */
    struct consensus_state_artifact_evidence *artifact = NULL;
    struct zcl_result admitted =
        consensus_state_artifact_evidence_open(bundle_path, &artifact);
    if (!admitted.ok)
        icb_refuse("bundle admission/validation failed: %s", admitted.message);

    struct consensus_state_bundle_manifest manifest;
    if (!consensus_state_artifact_evidence_manifest_copy(artifact, &manifest)) {
        consensus_state_artifact_evidence_free(artifact);
        icb_refuse("artifact evidence became stale after admission");
    }

    /* (3) Read the producer source receipt from the pinned bundle handle. */
    struct consensus_state_source_receipt receipt;
    {
        struct consensus_state_bundle_manifest leased;
        uint8_t receipt_digest[32];
        sqlite3 *bundle_db = NULL;
        bool leased_ok = consensus_state_artifact_evidence_candidate_lease_begin(
            artifact, &leased, receipt_digest, &bundle_db);
        bool read_ok = leased_ok && icb_read_source_receipt(bundle_db, &receipt);
        if (leased_ok)
            consensus_state_artifact_evidence_candidate_lease_end(artifact);
        if (!read_ok) {
            consensus_state_artifact_evidence_free(artifact);
            icb_refuse("could not read the producer source receipt from the "
                       "bundle");
        }
    }

    /* (4) Gate through the publication CAS. Both the chain-evidence build and
     * the CAS run assert the process singleton (condition_engine_main_state()
     * == request->main); at this boot stage the singleton is not yet wired to
     * the reducer, so set it for the duration of the decision and restore it.
     * Single-threaded boot — no reducer thread observes the transient value. */
    int dir_fd = open(datadir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        consensus_state_artifact_evidence_free(artifact);
        icb_refuse("could not open datadir for the decision record: %s",
                   datadir);
    }

    struct main_state *prev_singleton = condition_engine_main_state();
    condition_engine_set_main_state(ms);

    struct consensus_state_chain_binding_request chain_req = {
        .main = ms, .artifact = artifact, .target_lane = lane,
    };
    struct consensus_state_chain_evidence *chain_evidence = NULL;
    struct zcl_result chain_built =
        consensus_state_chain_evidence_build(&chain_req, &chain_evidence);

    struct consensus_state_publication_decision_record decision;
    struct zcl_result cas = ZCL_ERR(-1, "chain evidence unavailable");
    if (chain_built.ok) {
        struct consensus_state_publication_cas_request cas_req = {
            .main = ms, .artifact = artifact,
            .chain_evidence = chain_evidence,
            .source_receipt = &receipt, .target_lane = lane,
            .output_dir_fd = dir_fd,
            .output_name = ICB_DECISION_RECORD_NAME,
        };
        cas = consensus_state_publication_cas_run(&cas_req, &decision);
    }

    condition_engine_set_main_state(prev_singleton);
    if (chain_evidence)
        consensus_state_chain_evidence_free(chain_evidence);
    (void)close(dir_fd);

    if (!chain_built.ok) {
        consensus_state_artifact_evidence_free(artifact);
        icb_refuse("selected-chain binding failed (the bundle's height/hash is "
                   "not on this node's validated header chain, or the node is "
                   "not the open singleton): %s", chain_built.message);
    }
    if (!cas.ok) {
        consensus_state_artifact_evidence_free(artifact);
        icb_refuse("publication CAS run failed: %s", cas.message);
    }
    if (decision.decision != CONSENSUS_PUBLICATION_ADMIT) {
        consensus_state_artifact_evidence_free(artifact);
        icb_refuse("publication CAS did not ADMIT (refusal=%s): %s",
                   consensus_state_publication_refusal_name(decision.refusal),
                   decision.reason);
    }

    /* The artifact handle is done; the activate step re-opens + re-validates the
     * immutable file itself. */
    consensus_state_artifact_evidence_free(artifact);

    /* (5) ADMIT — atomically install the complete state. */
    struct consensus_state_activate_request areq;
    memset(&areq, 0, sizeof(areq));
    areq.bundle_path = bundle_path;
    areq.expected_height = manifest.height;
    memcpy(areq.expected_block_hash, manifest.block_hash, 32);
    areq.datadir = datadir;
    struct consensus_state_activate_result ares;
    if (!consensus_state_snapshot_install_activate(progress_store_db(), &areq,
                                                   &ares))
        icb_refuse("activation install failed after ADMIT (nothing committed): "
                   "%s", ares.reason);

    fprintf(stderr,
            "INSTALLED: -install-consensus-bundle: %s\n"
            "  reboot normally; the reducer folds forward from H*=%d to tip.\n",
            ares.reason, ares.hstar);
    LOG_INFO(ICB_SUBSYS, "consensus bundle activated: %s", ares.reason);
    _exit(EXIT_SUCCESS);
}
