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
#include "chain/chain.h"                              /* block_index, active_chain_at */
#include "chain/checkpoints.h"                        /* get_sha3_utxo_checkpoint */
#include "framework/condition.h"                     /* condition_engine_*_main_state */
#include "jobs/tip_finalize_stage.h"                 /* tip_finalize_stage_warm_authority_caches */
#include "services/consensus_state_chain_binding_service.h"
#include "services/consensus_state_publication_cas.h"
#include "storage/consensus_state_bundle_codec.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "validation/chainstate.h"                    /* active_chain_at */
#include "validation/main_state.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
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

static bool icb_same_directory(int target_fd, const char *candidate,
                               bool *same)
{
    *same = false;
    int candidate_fd = open(candidate, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (candidate_fd < 0)
        return errno == ENOENT;
    struct stat target_st;
    struct stat candidate_st;
    bool ok = fstat(target_fd, &target_st) == 0 &&
              fstat(candidate_fd, &candidate_st) == 0;
    if (ok)
        *same = target_st.st_dev == candidate_st.st_dev &&
                target_st.st_ino == candidate_st.st_ino;
    (void)close(candidate_fd);
    return ok;
}

static bool icb_canonical_candidate(int target_fd, const char *home,
                                    bool *canonical)
{
    if (!home || !home[0])
        return true;
    char candidate[PATH_MAX];
    int n = snprintf(candidate, sizeof(candidate), "%s/.zclassic-c23", home);
    if (n <= 0 || (size_t)n >= sizeof(candidate))
        return false;
    bool same = false;
    if (!icb_same_directory(target_fd, candidate, &same))
        return false;
    *canonical = *canonical || same;
    return true;
}

/* Open the target once and compare directory identities, not spelling. Both
 * the account home and HOME are considered: an altered/unset HOME must not
 * turn the real daily-driver into a copy-proof lane, while test/service homes
 * still receive the same conservative gate. The returned descriptor is also
 * the capability used for the CAS decision record. */
static struct zcl_result icb_datadir_open_classify(const char *datadir,
                                                   int *out_fd,
                                                   bool *out_canonical)
{
    if (out_fd)
        *out_fd = -1;
    if (out_canonical)
        *out_canonical = false;
    if (!datadir || !datadir[0] || !out_fd || !out_canonical)
        return ZCL_ERR(-1, "datadir classification arguments are missing");

    int target_fd = open(datadir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (target_fd < 0)
        return ZCL_ERR(-2, "could not open datadir: %s", strerror(errno));

    bool canonical = false;
    bool have_home = false;
    char pwbuf[16384];
    struct passwd pwd;
    struct passwd *pw = NULL;
    if (getpwuid_r(geteuid(), &pwd, pwbuf, sizeof(pwbuf), &pw) == 0 &&
        pw && pw->pw_dir && pw->pw_dir[0]) {
        have_home = true;
        if (!icb_canonical_candidate(target_fd, pw->pw_dir, &canonical)) {
            (void)close(target_fd);
            return ZCL_ERR(-3, "could not resolve account canonical datadir");
        }
    }

    const char *env_home = getenv("HOME");
    if (env_home && env_home[0]) {
        have_home = true;
        if (!icb_canonical_candidate(target_fd, env_home, &canonical)) {
            (void)close(target_fd);
            return ZCL_ERR(-4, "could not resolve HOME canonical datadir");
        }
    }
    if (!have_home) {
        (void)close(target_fd);
        return ZCL_ERR(-5, "no account or HOME directory for lane authority");
    }

    *out_fd = target_fd;
    *out_canonical = canonical;
    return ZCL_OK;
}

static bool icb_canonical_authorized(const char *authorization)
{
    return authorization && strcmp(authorization, "1") == 0;
}

#ifdef ZCL_TESTING
bool boot_install_consensus_bundle_gate_allows_for_test(
    const char *datadir, const char *authorization, bool *out_canonical)
{
    int dir_fd = -1;
    bool canonical = false;
    struct zcl_result classified =
        icb_datadir_open_classify(datadir, &dir_fd, &canonical);
    if (!classified.ok)
        return false;
    (void)close(dir_fd);
    if (out_canonical)
        *out_canonical = canonical;
    return !canonical || icb_canonical_authorized(authorization);
}
#endif

/* Read the producer source receipt from the pinned, already-validated bundle
 * handle. The CAS re-checks the receipt's self-consistency and its binding to
 * the manifest source digest, so a misread fails closed there. */
static bool icb_read_source_receipt(sqlite3 *bundle_db,
                                    struct consensus_state_source_receipt *out)
{
    static const char sql[] =
        "SELECT schema,source_epoch_digest,source_tree_root,running_binary_digest,"
        "toolchain_digest,build_inputs_digest,chain_corpus_digest,source_clean,"
        "validation_profile,producer_commit,fold_cursor,receipt_digest "
        "FROM source_receipt WHERE singleton=1";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(bundle_db, sql, -1, &st, NULL) != SQLITE_OK)
        return false;
    memset(out, 0, sizeof(*out));
    bool ok = sqlite3_step(st) == SQLITE_ROW; // raw-sql-ok:read-only-introspection
    const struct { int col; uint8_t *dst; } blobs[] = {
        {1, out->source_epoch_digest}, {2, out->source_tree_root},
        {3, out->running_binary_digest}, {4, out->toolchain_digest},
        {5, out->build_inputs_digest}, {6, out->chain_corpus_digest},
        {11, out->receipt_digest},
    };
    for (size_t i = 0; ok && i < sizeof(blobs) / sizeof(blobs[0]); i++) {
        ok = sqlite3_column_type(st, blobs[i].col) == SQLITE_BLOB &&
             sqlite3_column_bytes(st, blobs[i].col) == 32;
        if (ok)
            memcpy(blobs[i].dst, sqlite3_column_blob(st, blobs[i].col), 32);
    }
    if (ok) {
        const unsigned char *schema =
            sqlite3_column_type(st, 0) == SQLITE_TEXT
                ? sqlite3_column_text(st, 0) : NULL;
        int schema_len = schema ? sqlite3_column_bytes(st, 0) : -1;
        const unsigned char *commit =
            sqlite3_column_type(st, 9) == SQLITE_TEXT
                ? sqlite3_column_text(st, 9) : NULL;
        int commit_len = commit ? sqlite3_column_bytes(st, 9) : -1;
        uint8_t receipt_version = CONSENSUS_STATE_SOURCE_RECEIPT_INVALID;
        ok = schema && schema_len >= 0 &&
             consensus_state_source_receipt_schema_version(
                 (const char *)schema, (size_t)schema_len,
                 &receipt_version) &&
             commit && commit_len >= 0 &&
             consensus_state_source_receipt_commit_valid(
                 receipt_version, (const char *)commit,
                 (size_t)commit_len) &&
             sqlite3_column_type(st, 7) == SQLITE_INTEGER &&
             (sqlite3_column_int(st, 7) == 0 ||
              sqlite3_column_int(st, 7) == 1) &&
             sqlite3_column_type(st, 8) == SQLITE_INTEGER &&
             (sqlite3_column_int(st, 8) == CONSENSUS_STATE_VALIDATION_FULL ||
              sqlite3_column_int(st, 8) ==
                  CONSENSUS_STATE_VALIDATION_CHECKPOINT_FOLD) &&
             sqlite3_column_type(st, 10) == SQLITE_INTEGER;
        if (ok) {
            out->schema_version = receipt_version;
            memcpy(out->producer_commit, commit, (size_t)commit_len);
            out->producer_commit[commit_len] = '\0';
            out->source_clean = sqlite3_column_int(st, 7) == 1;
            out->validation_profile = (uint8_t)sqlite3_column_int(st, 8);
            out->fold_cursor = sqlite3_column_int64(st, 10);
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

    /* (1) Containment: descriptor-classify the target once, then retain that
     * exact directory capability through the CAS decision write. */
    int dir_fd = -1;
    bool canonical_datadir = false;
    struct zcl_result classified =
        icb_datadir_open_classify(datadir, &dir_fd, &canonical_datadir);
    if (!classified.ok)
        icb_refuse("datadir lane classification failed: %s",
                   classified.message);
    if (canonical_datadir && !icb_canonical_authorized(
            getenv("ZCL_DEPLOY_ALLOW_CANONICAL")))
        icb_refuse("datadir %s is the canonical lane; set "
                   "ZCL_DEPLOY_ALLOW_CANONICAL=1 to cut over the daily-driver, "
                   "or run the install on a dev/copy datadir first", datadir);

    enum consensus_state_target_lane lane =
        canonical_datadir
            ? CONSENSUS_STATE_TARGET_LANE_CANONICAL
            : CONSENSUS_STATE_TARGET_LANE_COPY_PROOF;

    /* (2) Admit + strictly validate the immutable bundle. */
    struct consensus_state_artifact_evidence *artifact = NULL;
    struct zcl_result admitted =
        consensus_state_artifact_evidence_open(bundle_path, dir_fd, &artifact);
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
    struct main_state *prev_singleton = condition_engine_main_state();
    condition_engine_set_main_state(ms);

    /* (3b) Boot-order warm: this terminal verb runs BEFORE
     * tip_finalize_stage_init, so the runtime authority pair + provable-tip
     * cache the chain-binding evidence consults are still unpublished and
     * every target — copy or live — would refuse "selected frontier changed
     * or is not durable" regardless of durable truth. Warm from the durable
     * store with the same primitives the stage init uses; a genuinely torn
     * target still refuses on real durable disagreement. */
    tip_finalize_stage_warm_authority_caches(
        progress_store_db(),
        ms ? active_chain_tip(&ms->chain_active) : NULL,
        "install_verb_warm");

    struct consensus_state_chain_binding_request chain_req = {
        .main = ms, .artifact = artifact, .target_lane = lane,
    };
    /* (3c) Compiled-checkpoint content authority. A snapshot-seeded target
     * floors its block index / header-pass / script-validate logs ABOVE the
     * checkpoint height, so the checkpoint block and its Sapling source are
     * never materialized there — every below-checkpoint chain-binding predicate
     * would refuse on absent index evidence even for the byte-correct compiled
     * checkpoint bundle. Feed the compiled SHA3 checkpoint (height + block_hash)
     * and the compiled ROM keystone (Sapling frontier root + height) as the
     * substitute trust root. The binding decision uses it ONLY when the bundle
     * sits at exactly this height and reproduces this content byte-for-byte;
     * a bundle at any other height, or one that disagrees, is unaffected. */
    const struct sha3_utxo_checkpoint *sha3_cp = get_sha3_utxo_checkpoint();
    const struct rom_state_checkpoint *rom_cp = get_rom_state_checkpoint();
    if (sha3_cp && rom_cp && sha3_cp->height == rom_cp->height) {
        chain_req.checkpoint_authority.available = true;
        chain_req.checkpoint_authority.height = sha3_cp->height;
        memcpy(chain_req.checkpoint_authority.block_hash,
               sha3_cp->block_hash, 32);
        chain_req.checkpoint_authority.sapling_frontier_height =
            (int32_t)rom_cp->sapling_frontier_height;
        memcpy(chain_req.checkpoint_authority.sapling_frontier_root,
               rom_cp->sapling_frontier_root, 32);
    }
    struct consensus_state_chain_evidence *chain_evidence = NULL;
    struct zcl_result chain_built =
        consensus_state_chain_evidence_build(&chain_req, &chain_evidence);
    bool used_checkpoint_authority =
        chain_built.ok &&
        consensus_state_chain_evidence_used_checkpoint_authority(chain_evidence);

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

    if (!chain_built.ok) {
        consensus_state_artifact_evidence_free(artifact);
        icb_refuse("selected-chain binding failed (the bundle's height/hash is "
                   "not on this node's validated header chain, or the node is "
                   "not the open singleton): %s", chain_built.message);
    }
    /* Auditable, not silent: state whether the below-checkpoint predicates were
     * bound from the compiled checkpoint (the sovereignty anchor) or from the
     * target's own materialized index. The same flag is folded into the chain
     * evidence digest, so this decision is also durable in the ADMIT record. */
    if (used_checkpoint_authority)
        LOG_INFO(ICB_SUBSYS,
                 "selected-chain binding used compiled-checkpoint content "
                 "authority (bundle height==checkpoint height=%d; block_hash + "
                 "Sapling frontier match the compiled keystone byte-for-byte)",
                 manifest.height);
    else
        LOG_INFO(ICB_SUBSYS,
                 "selected-chain binding used the target's materialized index "
                 "(no checkpoint-content substitution)");
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
    struct consensus_state_publication_decision_record durable_decision;
    struct zcl_result loaded = consensus_state_publication_cas_load(
        dir_fd, ICB_DECISION_RECORD_NAME, &durable_decision);
    if (!loaded.ok) {
        consensus_state_artifact_evidence_free(artifact);
        icb_refuse("durable publication ADMIT could not be reloaded exactly: "
                   "%s", loaded.message);
    }
    if (memcmp(durable_decision.decision_digest, decision.decision_digest,
               32) != 0) {
        consensus_state_artifact_evidence_free(artifact);
        icb_refuse("durable publication ADMIT changed between write and load");
    }

    /* The activate step re-opens and must reproduce the exact artifact receipt
     * bound into the ADMIT record; height/hash alone carry no state authority. */
    consensus_state_artifact_evidence_free(artifact);

    /* (4b) CHECKPOINT_CONTENT authority input. If this node's validated header
     * chain owns the compiled SHA3 UTXO checkpoint block, read that header's
     * PoW-committed hashFinalSaplingRoot so activate can bind a checkpoint-
     * content bundle's Sapling tip frontier to PoW (see the request contract in
     * config/consensus_state_snapshot_install.h). An absent / mismatched /
     * zero-root header leaves the authority unavailable: a content-matching
     * bundle then only VERIFY-CONTAINS, never activates on an unverifiable root.
     * This never gates the receipt authority, which needs no header. */
    bool checkpoint_root_from_header = false;
    uint8_t checkpoint_sapling_root[32];
    memset(checkpoint_sapling_root, 0, sizeof(checkpoint_sapling_root));
    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    if (cp && ms) {
        const struct block_index *cp_bi =
            active_chain_at(&ms->chain_active, cp->height);
        struct uint256 zero_root;
        memset(&zero_root, 0, sizeof(zero_root));
        if (cp_bi && cp_bi->phashBlock &&
            memcmp(cp_bi->phashBlock->data, cp->block_hash, 32) == 0 &&
            memcmp(cp_bi->hashFinalSaplingRoot.data, zero_root.data, 32) != 0) {
            memcpy(checkpoint_sapling_root,
                   cp_bi->hashFinalSaplingRoot.data, 32);
            checkpoint_root_from_header = true;
        }
    }

    /* (5) ADMIT — atomically install the complete state. */
    struct consensus_state_activate_request areq;
    memset(&areq, 0, sizeof(areq));
    areq.bundle_path = bundle_path;
    memcpy(areq.expected_artifact_receipt_digest,
           durable_decision.artifact_receipt_digest, 32);
    areq.expected_height = manifest.height;
    memcpy(areq.expected_block_hash, manifest.block_hash, 32);
    areq.publication_decision = &durable_decision;
    areq.datadir_fd = dir_fd;
    areq.datadir_display = datadir;
    areq.checkpoint_sapling_root_from_validated_header =
        checkpoint_root_from_header;
    memcpy(areq.checkpoint_sapling_root, checkpoint_sapling_root, 32);
    struct consensus_state_activate_result ares;
    if (!consensus_state_snapshot_install_activate(progress_store_db(), &areq,
                                                   &ares))
        icb_refuse("activation install failed after ADMIT: %s", ares.reason);
    (void)close(dir_fd);

    fprintf(stderr,
            "INSTALLED: -install-consensus-bundle: %s\n"
            "  reboot normally; the reducer folds forward from H*=%d to tip.\n",
            ares.reason, ares.hstar);
    LOG_INFO(ICB_SUBSYS, "consensus bundle activated: %s", ares.reason);
    _exit(EXIT_SUCCESS);
}
