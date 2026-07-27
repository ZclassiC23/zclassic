/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native handlers for the registry-owned `zcode` tree (slice 3: package
 * publication and local search — LOCAL only: no P2P gossip, no reward
 * credit, no install/build/execution of published content).
 *
 *   zcode package publish plan    validate a candidate release + manifest +
 *                                 chunk source WITHOUT persisting; the reply
 *                                 names every failed rule or the plan token
 *   zcode package publish commit  re-validate, then persist manifest +
 *                                 chunks into the CAS store and the release
 *                                 into releases/ (idempotent: a redelivered
 *                                 release id reports "duplicate")
 *   zcode package search          bounded local search over the rebuildable
 *                                 package index projection
 *   zcode package show            one package's full release record +
 *                                 manifest summary
 *
 * Truth discipline: the CAS manifest/release bytes under <datadir>/zcode
 * are authoritative; the search index (lib/vcs/package_index.*) is a
 * rebuildable projection re-read from those bytes on every call, and
 * acceptance state is replayed from the persisted releases before any
 * classification, so a one-shot CLI process agrees with the node's own
 * store. No second package database exists.
 *
 * Commit opens a fresh store on the target datadir for the duration of the
 * command. The store's temp/fsync/atomic-rename discipline makes a crash
 * mid-commit resumable, never partially visible; running commit
 * CONCURRENTLY with a hosting node mid-put on the same datadir is an
 * operator-discipline boundary for v1 (the open-time recovery sweep is not
 * cross-process coordinated).
 *
 * Acceptance is node-bound (chain id, t1/t3 reward address), so plan and
 * commit select CHAIN_MAIN when nothing selected a chain — the same
 * one-shot-CLI precedent as core.sync.frontier.offline. */

#include "command/native_command.h"

#include "kernel/command_registry.h"

#include "base/log_macros.h"
#include "base/safe_alloc.h"
#include "chain/chainparams.h"
#include "json/json.h"
#include "vcs/package_index.h"
#include "vcs/package_publish.h"
#include "vcs/package_store.h"

#include <stdio.h>
#include <string.h>

#define ZC_LOG "zcode.command"

/* Render caps: search rows omit the long optional fields (show carries the
 * full record) so 16 rows stay inside the LIST budget even at maximum name
 * length; show renders at most 32 manifest files per page. */
#define ZC_SEARCH_MAX_ROWS 16u
#define ZC_SHOW_MAX_FILES 32u

/* ── small input helpers ──────────────────────────────────────────── */

static const char *zc_input_str(const struct json_value *input,
                                const char *key)
{
    const struct json_value *v = json_get(input, key);
    return v ? json_get_str(v) : NULL;
}

/* Resolve the target datadir: explicit input.datadir wins, else the CLI's
 * --datadir. NULL when neither is set (core.node.bootstatus precedent). */
static const char *zc_datadir(const struct zcl_command_request *request)
{
    const char *dd = zc_input_str(request->input, "datadir");
    if (dd && dd[0])
        return dd;
    dd = zcl_native_command_datadir();
    return (dd && dd[0]) ? dd : NULL;
}

static void zc_hex_encode(const uint8_t *in, size_t len, char *out)
{
    static const char hexd[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i]     = hexd[(in[i] >> 4) & 0xf];
        out[2 * i + 1] = hexd[in[i] & 0xf];
    }
    out[2 * len] = '\0';
}

/* Strict hex decode: even length, 1..out_cap bytes. */
static bool zc_hex_decode(const char *hex, uint8_t *out, size_t out_cap,
                          size_t *out_len)
{
    if (!hex || !out)
        return false;
    size_t n = strlen(hex);
    if (n == 0 || (n & 1u) != 0 || n / 2 > out_cap)
        return false;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v = 0;
        for (size_t j = 0; j < 2; j++) {
            char ch = hex[2 * i + j];
            v <<= 4;
            if (ch >= '0' && ch <= '9')
                v |= (unsigned)(ch - '0');
            else if (ch >= 'a' && ch <= 'f')
                v |= (unsigned)(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F')
                v |= (unsigned)(ch - 'A' + 10);
            else
                return false;
        }
        out[i] = (uint8_t)v;
    }
    if (out_len)
        *out_len = n / 2;
    return true;
}

/* ── candidate parsing + validation (shared by plan and commit) ────── */

struct zc_candidate {
    struct vcs_package_release release;
    bool release_parsed;
    struct vcs_package_manifest manifest;
    bool manifest_parsed;
    uint8_t *manifest_wire;
    size_t manifest_wire_len;
};

static void zc_candidate_free(struct zc_candidate *c)
{
    /* The manifest is init'd at the top of zc_validate and parse re-inits
     * on rejection, so this is always a balanced free. */
    vcs_package_manifest_free(&c->manifest);
    free(c->manifest_wire);
    memset(c, 0, sizeof(*c));
}

/* Parse the release_hex and manifest_hex inputs and run every publication
 * rule (envelope, manifest, structure, license text, acceptance replay,
 * chunk verification when dir is given). The report collects every failed
 * rule. Returns false only on a hard error already reported via
 * reply_fail (missing/undecodable inputs, I/O failure); a true return can
 * still carry a non-empty failure list — that is the plan report. */
static bool zc_validate(const struct zcl_command_request *request,
                        struct zcl_command_reply *reply,
                        struct zc_candidate *cand,
                        struct vcs_package_publish_report *report,
                        const char *datadir, const char *dir,
                        size_t *replayed_out)
{
    memset(cand, 0, sizeof(*cand));
    vcs_package_manifest_init(&cand->manifest);

    const char *release_hex = zc_input_str(request->input, "release_hex");
    if (!release_hex || !release_hex[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_RELEASE",
                               "normalize", false, false,
                               "no release_hex given",
                               "zcode.package.publish");
        return false;
    }
    uint8_t *release_wire =
        zcl_malloc(VCS_PACKAGE_RELEASE_MAX_WIRE_BYTES, "zc_release_wire");
    if (!release_wire) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "ALLOC",
                               "normalize", false, false,
                               "release wire buffer", "zcode.package.publish");
        return false;
    }
    size_t release_wire_len = 0;
    bool decoded = zc_hex_decode(release_hex, release_wire,
                                 VCS_PACKAGE_RELEASE_MAX_WIRE_BYTES,
                                 &release_wire_len);
    enum vcs_package_release_error perr = VCS_PACKAGE_RELEASE_OK;
    if (decoded)
        perr = vcs_package_release_parse(release_wire, release_wire_len,
                                         &cand->release);
    free(release_wire);
    if (!decoded || perr != VCS_PACKAGE_RELEASE_OK) {
        vcs_package_publish_fail(
            report, VCS_PACKAGE_PUBLISH_RULE_RELEASE_PARSE,
            decoded ? vcs_package_release_error_string(perr)
                    : "release_hex is not bounded strict hex");
    } else {
        cand->release_parsed = true;
    }

    const char *manifest_hex = zc_input_str(request->input, "manifest_hex");
    if (!manifest_hex || !manifest_hex[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_MANIFEST",
                               "normalize", false, false,
                               "no manifest_hex given",
                               "zcode.package.publish");
        return false;
    }
    cand->manifest_wire =
        zcl_malloc(VCS_PACKAGE_MANIFEST_MAX_WIRE_BYTES, "zc_manifest_wire");
    if (!cand->manifest_wire) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "ALLOC",
                               "normalize", false, false,
                               "manifest wire buffer",
                               "zcode.package.publish");
        return false;
    }
    if (!zc_hex_decode(manifest_hex, cand->manifest_wire,
                       VCS_PACKAGE_MANIFEST_MAX_WIRE_BYTES,
                       &cand->manifest_wire_len) ||
        !vcs_package_manifest_parse(cand->manifest_wire,
                                    cand->manifest_wire_len,
                                    &cand->manifest)) {
        vcs_package_publish_fail(
            report, VCS_PACKAGE_PUBLISH_RULE_MANIFEST_PARSE,
            "manifest wire violates the content.v2 grammar (paths, modes, "
            "counts, order, or the 1 MiB wire bound)");
    } else {
        cand->manifest_parsed = true;
    }

    if (!cand->release_parsed || !cand->manifest_parsed)
        return true; /* the report already names the failed rules */

    vcs_package_publish_validate(&cand->release, &cand->manifest, report);
    if (!report->release_ok || !report->manifest_ok)
        return true;

    /* Acceptance (rule 7): replay the persisted releases, then classify.
     * Node-bound: select a chain for the one-shot CLI process first. */
    chain_params_select(CHAIN_MAIN);
    char zcode_dir[4400];
    int n = snprintf(zcode_dir, sizeof(zcode_dir), "%s/zcode", datadir);
    if (n < 0 || (size_t)n >= sizeof(zcode_dir)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "DATADIR_TOO_LONG",
                               "normalize", false, false,
                               "datadir path too long", datadir);
        return false;
    }
    struct vcs_package_accept *accept = vcs_package_accept_new();
    if (!accept) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "ALLOC",
                               "validate", false, false,
                               "acceptance context", "zcode.package.publish");
        return false;
    }
    size_t replayed = 0;
    if (!vcs_package_publish_replay(zcode_dir, accept, &replayed)) {
        vcs_package_accept_free(accept);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "REPLAY_IO",
                               "validate", false, false,
                               "persisted releases could not be replayed",
                               zcode_dir);
        return false;
    }
    report->accept = vcs_package_accept(accept, &cand->release);
    vcs_package_accept_free(accept);
    if (replayed_out)
        *replayed_out = replayed;
    if (report->accept != VCS_PACKAGE_ACCEPT_OK &&
        report->accept != VCS_PACKAGE_ACCEPT_DUPLICATE) {
        vcs_package_publish_fail(
            report, VCS_PACKAGE_PUBLISH_RULE_ACCEPT,
            vcs_package_accept_result_string(report->accept));
        return true; /* acceptance failure: chunk checks are moot */
    }

    /* Chunks (rule 8): only when a source directory is given. */
    if (dir && dir[0])
        vcs_package_publish_verify_chunks(&cand->manifest, dir, report);
    return true;
}

/* ── reply projection ───────────────────────────────────────────────── */

static void zc_failures_json(struct json_value *out,
                             const struct vcs_package_publish_report *report)
{
    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    for (size_t i = 0; i < report->failure_count; i++) {
        struct json_value f;
        json_init(&f);
        json_set_object(&f);
        (void)json_push_kv_str(
            &f, "rule",
            vcs_package_publish_rule_string(report->failures[i].rule));
        (void)json_push_kv_str(&f, "detail", report->failures[i].detail);
        (void)json_push_back(&arr, &f);
        json_free(&f);
    }
    (void)json_push_kv(out, "failures", &arr);
    json_free(&arr);
    (void)json_push_kv_bool(out, "failures_truncated",
                            report->failures_truncated);
}

static void zc_summary_json(struct json_value *out,
                            const struct zc_candidate *cand,
                            const struct vcs_package_publish_report *report,
                            bool chunks_checked, size_t replayed)
{
    char hex[65];
    if (report->release_ok) {
        struct json_value rel;
        json_init(&rel);
        json_set_object(&rel);
        zc_hex_encode(report->release_id, 32, hex);
        (void)json_push_kv_str(&rel, "release_id", hex);
        (void)json_push_kv_str(&rel, "name", cand->release.name);
        (void)json_push_kv_str(&rel, "semver", cand->release.semver);
        (void)json_push_kv_str(&rel, "license", cand->release.license);
        char pub[2 * VCS_PACKAGE_RELEASE_PUBKEY_BYTES + 1];
        zc_hex_encode(cand->release.publisher_pubkey,
                      VCS_PACKAGE_RELEASE_PUBKEY_BYTES, pub);
        (void)json_push_kv_str(&rel, "publisher", pub);
        (void)json_push_kv_int(&rel, "publisher_sequence",
                               (int64_t)cand->release.publisher_sequence);
        (void)json_push_kv_str(&rel, "acceptance",
                               vcs_package_accept_result_string(
                                   report->accept));
        (void)json_push_kv_int(&rel, "replayed_releases",
                               (int64_t)replayed);
        (void)json_push_kv(out, "release", &rel);
        json_free(&rel);
    }
    if (report->manifest_ok) {
        struct json_value pkg;
        json_init(&pkg);
        json_set_object(&pkg);
        zc_hex_encode(cand->release.package_root, 32, hex);
        (void)json_push_kv_str(&pkg, "package_root", hex);
        (void)json_push_kv_int(&pkg, "files", (int64_t)report->file_count);
        (void)json_push_kv_int(&pkg, "bytes", (int64_t)report->total_bytes);
        (void)json_push_kv_int(&pkg, "chunks",
                               (int64_t)report->chunk_count);
        (void)json_push_kv_bool(&pkg, "chunks_checked", chunks_checked);
        if (chunks_checked)
            (void)json_push_kv_int(&pkg, "chunks_verified",
                                   (int64_t)report->chunks_verified);
        (void)json_push_kv(out, "package", &pkg);
        json_free(&pkg);
    }
}

/* ── zcode package publish plan ─────────────────────────────────────── */

void zcl_native_handle_zcode_package_publish_plan(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *datadir = zc_datadir(request);
    if (!datadir) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false,
                               "no datadir given (input datadir or --datadir)",
                               "zcode.package.publish.plan");
        return;
    }
    const char *dir = zc_input_str(request->input, "dir");
    struct zc_candidate cand;
    struct vcs_package_publish_report report;
    vcs_package_publish_report_init(&report);
    size_t replayed = 0;
    if (!zc_validate(request, reply, &cand, &report, datadir, dir,
                     &replayed)) {
        zc_candidate_free(&cand);
        return; /* hard failure: error body already set */
    }

    bool valid = report.failure_count == 0;
    (void)json_push_kv_str(&reply->data, "stage", "plan");
    (void)json_push_kv_bool(&reply->data, "valid", valid);
    if (report.release_ok) {
        char token[65];
        zc_hex_encode(report.release_id, 32, token);
        (void)json_push_kv_str(&reply->data, "plan_token", token);
    }
    zc_summary_json(&reply->data, &cand, &report, report.chunks_checked,
                    replayed);
    zc_failures_json(&reply->data, &report);
    zc_candidate_free(&cand);
}

/* ── zcode package publish commit ───────────────────────────────────── */

void zcl_native_handle_zcode_package_publish_commit(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *datadir = zc_datadir(request);
    if (!datadir) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false,
                               "no datadir given (input datadir or --datadir)",
                               "zcode.package.publish.commit");
        return;
    }
    const char *dir = zc_input_str(request->input, "dir");
    if (!dir || !dir[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DIR",
                               "normalize", false, false,
                               "commit requires the chunk source dir",
                               "zcode.package.publish.commit");
        return;
    }
    struct zc_candidate cand;
    struct vcs_package_publish_report report;
    vcs_package_publish_report_init(&report);
    size_t replayed = 0;
    if (!zc_validate(request, reply, &cand, &report, datadir, dir,
                     &replayed)) {
        zc_candidate_free(&cand);
        return;
    }
    if (report.failure_count > 0) {
        /* The exact failed rule leads; the rest fit the evidence budget. */
        const struct vcs_package_publish_failure *first =
            &report.failures[0];
        char evidence[256];
        snprintf(evidence, sizeof(evidence), "%s (%zu rule%s failed)",
                 first->detail, report.failure_count,
                 report.failure_count == 1 ? "" : "s");
        zcl_command_reply_fail(
            reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INVALID,
            vcs_package_publish_rule_string(first->rule), "validate", false,
            false, "candidate release failed publication validation",
            evidence);
        zc_candidate_free(&cand);
        return;
    }

    char zcode_dir[4400];
    int n = snprintf(zcode_dir, sizeof(zcode_dir), "%s/zcode", datadir);
    if (n < 0 || (size_t)n >= sizeof(zcode_dir)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "DATADIR_TOO_LONG",
                               "normalize", false, false,
                               "datadir path too long", datadir);
        zc_candidate_free(&cand);
        return;
    }
    struct vcs_package_store *store =
        vcs_package_store_open(datadir, vcs_package_store_quota_bytes());
    if (!store) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "STORE_OPEN",
                               "persist", false, false,
                               "the package store failed to open", zcode_dir);
        zc_candidate_free(&cand);
        return;
    }

    uint8_t root[32];
    enum vcs_package_store_result sres = vcs_package_store_put_manifest(
        store, cand.manifest_wire, cand.manifest_wire_len, root);
    if (sres != VCS_PACKAGE_STORE_OK) {
        vcs_package_store_close(store);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL,
                               vcs_package_store_result_string(sres),
                               "persist", false, false,
                               "manifest admission failed",
                               cand.release.name);
        zc_candidate_free(&cand);
        return;
    }

    uint8_t *buf = zcl_malloc(VCS_PACKAGE_CHUNK_BYTES, "zc_chunk_buf");
    if (!buf) {
        vcs_package_store_close(store);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "ALLOC",
                               "persist", false, false, "chunk buffer",
                               "zcode.package.publish.commit");
        zc_candidate_free(&cand);
        return;
    }
    uint64_t chunks_stored = 0;
    bool io_failed = false;
    char evidence[256] = "";
    for (size_t i = 0; i < cand.manifest.count && !io_failed; i++) {
        const struct vcs_package_file *f = &cand.manifest.files[i];
        for (uint32_t c = 0; c < f->chunk_count; c++) {
            size_t len = 0;
            enum vcs_package_publish_rule rule;
            if (!vcs_package_publish_read_chunk(dir, f, c, buf, &len,
                                                &rule)) {
                snprintf(evidence, sizeof(evidence), "%s#%u: %s", f->path,
                         c, vcs_package_publish_rule_string(rule));
                io_failed = true;
                break;
            }
            sres = vcs_package_store_put_chunk(store, root, f->path, c, buf,
                                               len);
            if (sres != VCS_PACKAGE_STORE_OK) {
                snprintf(evidence, sizeof(evidence), "%s#%u: %s", f->path,
                         c, vcs_package_store_result_string(sres));
                io_failed = true;
                break;
            }
            chunks_stored++;
        }
    }
    free(buf);
    if (io_failed) {
        /* Staging survives: a later commit of the same candidate resumes. */
        vcs_package_store_close(store);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL,
                               "CHUNK_PERSIST", "persist", true, true,
                               "chunk admission failed; staged bytes are "
                               "resumable on retry", evidence);
        zc_candidate_free(&cand);
        return;
    }

    bool published = report.accept == VCS_PACKAGE_ACCEPT_OK;
    if (published) {
        enum vcs_package_accept_result ar;
        sres = vcs_package_store_put_release(store, &cand.release, &ar);
        if (sres != VCS_PACKAGE_STORE_OK) {
            vcs_package_store_close(store);
            zcl_command_reply_fail(
                reply, ZCL_COMMAND_STATUS_FAILED,
                ZCL_COMMAND_EXIT_INTERNAL,
                vcs_package_store_result_string(sres), "persist", false,
                true, "release persistence failed",
                vcs_package_accept_result_string(ar));
            zc_candidate_free(&cand);
            return;
        }
    }
    vcs_package_store_close(store);

    char hex[65];
    (void)json_push_kv_str(&reply->data, "stage", "commit");
    (void)json_push_kv_str(&reply->data, "result",
                           published ? "published" : "duplicate");
    zc_hex_encode(report.release_id, 32, hex);
    (void)json_push_kv_str(&reply->data, "release_id", hex);
    (void)json_push_kv_str(&reply->data, "plan_token", hex);
    zc_hex_encode(root, 32, hex);
    (void)json_push_kv_str(&reply->data, "package_root", hex);
    (void)json_push_kv_str(&reply->data, "name", cand.release.name);
    (void)json_push_kv_int(&reply->data, "files",
                           (int64_t)report.file_count);
    (void)json_push_kv_int(&reply->data, "bytes",
                           (int64_t)report.total_bytes);
    (void)json_push_kv_int(&reply->data, "chunks_stored",
                           (int64_t)chunks_stored);
    (void)json_push_kv_int(&reply->data, "replayed_releases",
                           (int64_t)replayed);
    reply->error.mutated = published;
    zc_candidate_free(&cand);
}

/* ── zcode package search ───────────────────────────────────────────── */

static void zc_search_row_json(struct json_value *row,
                               const struct vcs_package_index_entry *e)
{
    json_set_object(row);
    (void)json_push_kv_str(row, "name", e->name);
    (void)json_push_kv_str(row, "semver", e->semver);
    (void)json_push_kv_str(row, "license", e->license);
    (void)json_push_kv_str(row, "publisher", e->publisher_hex);
    (void)json_push_kv_int(row, "publisher_sequence",
                           (int64_t)e->publisher_sequence);
    (void)json_push_kv_str(row, "release_id", e->release_id_hex);
    (void)json_push_kv_str(row, "package_root", e->package_root_hex);
    (void)json_push_kv_bool(row, "manifest_present", e->manifest_present);
    (void)json_push_kv_int(row, "files", (int64_t)e->file_count);
    (void)json_push_kv_int(row, "bytes", (int64_t)e->total_bytes);
}

void zcl_native_handle_zcode_package_search(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *datadir = zc_datadir(request);
    if (!datadir) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false,
                               "no datadir given (input datadir or --datadir)",
                               "zcode.package.search");
        return;
    }
    int64_t limit = 0;
    const struct json_value *lv = json_get(request->input, "limit");
    if (lv)
        limit = json_get_int(lv);
    if (limit <= 0)
        limit = ZC_SEARCH_MAX_ROWS;
    if (limit > (int64_t)ZC_SEARCH_MAX_ROWS)
        limit = ZC_SEARCH_MAX_ROWS;

    char zcode_dir[4400];
    int n = snprintf(zcode_dir, sizeof(zcode_dir), "%s/zcode", datadir);
    if (n < 0 || (size_t)n >= sizeof(zcode_dir)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "DATADIR_TOO_LONG",
                               "normalize", false, false,
                               "datadir path too long", datadir);
        return;
    }
    struct vcs_package_index *index = vcs_package_index_build(zcode_dir);
    if (!index) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "INDEX_BUILD",
                               "execute", false, false,
                               "the package index could not be built",
                               zcode_dir);
        return;
    }
    struct vcs_package_search search = {
        .publisher = zc_input_str(request->input, "publisher"),
        .name_prefix = zc_input_str(request->input, "name_prefix"),
        .license = zc_input_str(request->input, "license"),
        .keyword = zc_input_str(request->input, "keyword"),
    };
    const struct vcs_package_index_entry *rows[ZC_SEARCH_MAX_ROWS];
    size_t total = vcs_package_index_search(index, &search, rows,
                                            (size_t)limit);
    size_t rendered = total < (size_t)limit ? total : (size_t)limit;
    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    for (size_t i = 0; i < rendered; i++) {
        struct json_value row;
        json_init(&row);
        zc_search_row_json(&row, rows[i]);
        (void)json_push_back(&arr, &row);
        json_free(&row);
    }
    (void)json_push_kv(&reply->data, "results", &arr);
    json_free(&arr);
    (void)json_push_kv_int(&reply->data, "total_matches", (int64_t)total);
    (void)json_push_kv_int(&reply->data, "rendered", (int64_t)rendered);
    (void)json_push_kv_bool(&reply->data, "items_truncated",
                            total > rendered);
    (void)json_push_kv_int(&reply->data, "packages_scanned",
                           (int64_t)vcs_package_index_count(index));
    (void)json_push_kv_int(&reply->data, "limit", limit);
    vcs_package_index_free(index);
}

/* ── zcode package show ─────────────────────────────────────────────── */

void zcl_native_handle_zcode_package_show(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *datadir = zc_datadir(request);
    if (!datadir) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false,
                               "no datadir given (input datadir or --datadir)",
                               "zcode.package.show");
        return;
    }
    const char *root_hex = zc_input_str(request->input, "root");
    uint8_t root[32];
    size_t root_len = 0;
    if (!root_hex || !zc_hex_decode(root_hex, root, 32, &root_len) ||
        root_len != 32) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_ROOT",
                               "normalize", false, false,
                               "root must be a 64-hex package root",
                               root_hex ? root_hex : "");
        return;
    }
    char zcode_dir[4400];
    int n = snprintf(zcode_dir, sizeof(zcode_dir), "%s/zcode", datadir);
    if (n < 0 || (size_t)n >= sizeof(zcode_dir)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "DATADIR_TOO_LONG",
                               "normalize", false, false,
                               "datadir path too long", datadir);
        return;
    }
    struct vcs_package_index *index = vcs_package_index_build(zcode_dir);
    if (!index) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "INDEX_BUILD",
                               "execute", false, false,
                               "the package index could not be built",
                               zcode_dir);
        return;
    }
    const struct vcs_package_index_entry *e =
        vcs_package_index_find_root(index, root);
    if (!e) {
        vcs_package_index_free(index);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "UNKNOWN_PACKAGE",
                               "execute", false, false,
                               "no published release names this package root",
                               root_hex);
        return;
    }

    struct json_value rel;
    json_init(&rel);
    json_set_object(&rel);
    (void)json_push_kv_str(&rel, "release_id", e->release_id_hex);
    (void)json_push_kv_str(&rel, "name", e->name);
    (void)json_push_kv_str(&rel, "semver", e->semver);
    (void)json_push_kv_str(&rel, "license", e->license);
    (void)json_push_kv_str(&rel, "publisher", e->publisher_hex);
    (void)json_push_kv_int(&rel, "publisher_sequence",
                           (int64_t)e->publisher_sequence);
    (void)json_push_kv_str(&rel, "chain_id", e->chain_id);
    (void)json_push_kv_str(&rel, "reward_address", e->reward_address);
    (void)json_push_kv_bool(&rel, "has_parent", e->has_parent);
    if (e->has_parent)
        (void)json_push_kv_str(&rel, "parent_root", e->parent_root_hex);
    (void)json_push_kv_bool(&rel, "has_znam", e->has_znam);
    if (e->has_znam)
        (void)json_push_kv_str(&rel, "znam", e->znam);
    (void)json_push_kv(&reply->data, "release", &rel);
    json_free(&rel);

    (void)json_push_kv_str(&reply->data, "package_root",
                           e->package_root_hex);
    (void)json_push_kv_bool(&reply->data, "manifest_present",
                            e->manifest_present);
    (void)json_push_kv_int(&reply->data, "files", (int64_t)e->file_count);
    (void)json_push_kv_int(&reply->data, "bytes", (int64_t)e->total_bytes);
    (void)json_push_kv_int(&reply->data, "chunks", (int64_t)e->chunk_total);
    (void)json_push_kv_bool(&reply->data, "license_present",
                            e->license_present);
    (void)json_push_kv_int(&reply->data, "executable_files",
                           (int64_t)e->executable_count);

    /* The bounded file page: parse the persisted manifest again (the index
     * projects summaries only; the CAS wire stays the truth). */
    if (e->manifest_present) {
        char path[4400];
        n = snprintf(path, sizeof(path), "%s/manifests/%s", zcode_dir,
                     e->package_root_hex);
        uint8_t *wire = (n > 0 && (size_t)n < sizeof(path))
            ? zcl_malloc(VCS_PACKAGE_MANIFEST_MAX_WIRE_BYTES,
                         "zc_show_manifest")
            : NULL;
        struct vcs_package_manifest manifest;
        bool parsed = false;
        if (wire) {
            FILE *f = fopen(path, "rb");
            if (f) {
                size_t len = fread(wire, 1,
                                   VCS_PACKAGE_MANIFEST_MAX_WIRE_BYTES, f);
                bool trailing = !feof(f);
                fclose(f);
                parsed = !trailing &&
                    vcs_package_manifest_parse(wire, len, &manifest);
            }
            free(wire);
        }
        if (parsed) {
            size_t shown = manifest.count < ZC_SHOW_MAX_FILES
                ? manifest.count : ZC_SHOW_MAX_FILES;
            struct json_value arr;
            json_init(&arr);
            json_set_array(&arr);
            for (size_t i = 0; i < shown; i++) {
                const struct vcs_package_file *mf = &manifest.files[i];
                struct json_value row;
                json_init(&row);
                json_set_object(&row);
                (void)json_push_kv_str(&row, "path", mf->path);
                (void)json_push_kv_int(&row, "mode", (int64_t)mf->mode);
                (void)json_push_kv_int(&row, "size", (int64_t)mf->size);
                (void)json_push_kv_int(&row, "chunks",
                                       (int64_t)mf->chunk_count);
                (void)json_push_back(&arr, &row);
                json_free(&row);
            }
            (void)json_push_kv(&reply->data, "files_page", &arr);
            json_free(&arr);
            (void)json_push_kv_bool(&reply->data, "files_truncated",
                                    manifest.count > shown);
            vcs_package_manifest_free(&manifest);
        } else {
            (void)json_push_kv_str(&reply->data, "files_page_error",
                                   "persisted manifest unreadable");
        }
    }
    vcs_package_index_free(index);
}
