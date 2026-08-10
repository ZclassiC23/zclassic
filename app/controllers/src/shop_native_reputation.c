/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * `app shop reputation` — slice C of docs/work/SHOP_COMMAND.md: the
 * evidence readout for one ZCODE publisher, over records this node already
 * holds under <datadir>/zcode. READ-ONLY: nothing here writes, and the
 * collectors below are the same scans the package leaves already run
 * (publish replay, the reproduction scan, the attestation scan, the reward
 * ledger replay).
 *
 * THE DOCTRINE (owner-approved, non-negotiable): render only facts the
 * node can prove, with the evidence class and the counting window stated
 * on the row. Absent evidence is "no_record" — never a zero, never an
 * adjective. Concretely this means:
 *
 *   - releases/packages published: secp256k1-signed release envelopes,
 *     verified at publication, counted over the local store
 *   - reproductions: build receipts filed under <zcode>/receipts whose
 *     output sets are byte-identical across DISTINCT receipt ids — distinct
 *     build events; receipts carry no signer identity (vcs/package_reproduce.h),
 *     so nothing about WHO built them is established and the row says so
 *   - distinct signing identities: verifier pubkeys over attestations
 *     whose secp256k1 signature verifies at read time
 *   - days observed: the oldest release envelope's mtime in THIS node's
 *     store — a local observation record, unsigned, labeled as such
 *   - dependent packages: root-committed dependency declarations
 *     (zcode-package.json is a manifest member; the package root commits it)
 *   - simulated settlements: settled facts in the simulated reward ledger
 *     (placeholder token; ZC23 issuance stays simulation-only)
 *   - availability challenges: NO durable source exists (the chunk-challenge
 *     loop keeps pass/fail in per-download memory) — the row is rendered
 *     "unavailable" with the gap named, never fabricated
 *
 * Two doctrine-wanted classes have no datadir-local source and are gaps,
 * not fabrications: paid fulfillments (patronage settlement lives on the
 * scratch-workspace lane, tools/command/native_zcode_patronage_command.c)
 * and availability (above). The shop's seller identity is the publisher
 * key: releases are signed by it and the reward ledger settles to it, so
 * one 33-byte pubkey is the join key across every row.
 *
 * Bound in config/commands/store.def. Tests: lib/test/src/test_shop_reputation.c. */

#include "controllers/shop_native_handler.h"

#include "base/hex.h"
#include "command/native_command.h"
#include "json/json.h"
#include "platform/clock.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "vcs/package_attest.h"
#include "vcs/package_deps.h"
#include "vcs/package_manifest.h"
#include "vcs/package_publish.h"
#include "vcs/package_release.h"
#include "vcs/package_reproduce.h"
#include "vcs/package_reward.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SHOP_REP_TAG "native.app.shop.reputation"

/* Scan caps. The store's own caps bind releases (4096); attestation and
 * verifier lists are bounded here so a read leaf stays a read leaf. */
#define SHOP_REP_MAX_PACKAGES VCS_PACKAGE_PUBLISH_MAX_RELEASES
#define SHOP_REP_MAX_ATTEST_SCAN 4096u
#define SHOP_REP_MAX_VERIFIERS 1024u

struct shop_rep_pair {          /* one (package, recipe) reproduction key */
    uint8_t package_root[32];
    uint8_t recipe_root[32];
};

struct shop_rep_evidence {
    bool store_present;
    /* signed publication */
    uint32_t releases;
    uint32_t packages;
    uint64_t max_publisher_sequence;
    /* local observation window (unsigned, labeled on the row) */
    bool observed;
    int64_t first_observed_unix;
    int64_t days_observed;
    /* reproduction (distinct build events, unsigned-by-whom unknown) */
    uint32_t matching_receipts;
    uint32_t reproduced_packages;
    /* attestations */
    uint32_t valid_attestations;
    uint32_t distinct_verifiers;
    bool attestations_truncated;
    /* dependent packages */
    uint32_t dependent_packages;
    uint32_t declarations_read;
    uint32_t declarations_unavailable;
    /* simulated settlements (placeholder token) */
    uint32_t settled_entries;
};

/* ── small set helpers (bounded linear scans over 32-byte roots) ────── */
static bool rep_root_seen(const uint8_t (*set)[32], size_t count,
                          const uint8_t root[32])
{
    for (size_t i = 0; i < count; i++)
        if (memcmp(set[i], root, 32) == 0)
            return true;
    return false;
}

static bool rep_pair_seen(const struct shop_rep_pair *set, size_t count,
                          const uint8_t package_root[32],
                          const uint8_t recipe_root[32])
{
    for (size_t i = 0; i < count; i++)
        if (memcmp(set[i].package_root, package_root, 32) == 0 &&
            memcmp(set[i].recipe_root, recipe_root, 32) == 0)
            return true;
    return false;
}

/* ── publication + the local observation window ─────────────────────── */
static void rep_collect_publication(const char *zcode_dir,
                                    const uint8_t publisher[33],
                                    struct shop_rep_evidence *ev,
                                    uint8_t (*subject_pkgs)[32],
                                    size_t *subject_pkg_count,
                                    struct shop_rep_pair *pairs,
                                    size_t *pair_count,
                                    uint8_t (*all_pkgs)[32],
                                    size_t *all_pkg_count)
{
    struct vcs_package_release *releases = zcl_malloc(
        sizeof(*releases) * VCS_PACKAGE_PUBLISH_MAX_RELEASES,
        "shop_rep_releases");
    if (!releases)
        return;     /* logged inside zcl_malloc */
    size_t count = 0, skipped = 0;
    if (!vcs_package_publish_load_releases(
            zcode_dir, releases, VCS_PACKAGE_PUBLISH_MAX_RELEASES, &count,
            &skipped)) {
        free(releases);
        LOG_ERROR(SHOP_REP_TAG, "release load failed for %s", zcode_dir);
        return;
    }
    int64_t oldest = 0;
    for (size_t i = 0; i < count; i++) {
        const struct vcs_package_release *r = &releases[i];
        if (*all_pkg_count < SHOP_REP_MAX_PACKAGES &&
            !rep_root_seen(all_pkgs, *all_pkg_count, r->package_root))
            memcpy(all_pkgs[(*all_pkg_count)++], r->package_root, 32);
        if (memcmp(r->publisher_pubkey, publisher, 33) != 0)
            continue;
        ev->releases++;
        if (r->publisher_sequence > ev->max_publisher_sequence)
            ev->max_publisher_sequence = r->publisher_sequence;
        if (*subject_pkg_count < SHOP_REP_MAX_PACKAGES &&
            !rep_root_seen(subject_pkgs, *subject_pkg_count, r->package_root))
            memcpy(subject_pkgs[(*subject_pkg_count)++], r->package_root, 32);
        if (*pair_count < SHOP_REP_MAX_PACKAGES &&
            !rep_pair_seen(pairs, *pair_count, r->package_root,
                           r->recipe_root)) {
            memcpy(pairs[*pair_count].package_root, r->package_root, 32);
            memcpy(pairs[*pair_count].recipe_root, r->recipe_root, 32);
            (*pair_count)++;
        }
        /* The observation record: the envelope file's mtime in THIS store. */
        uint8_t id[VCS_PACKAGE_RELEASE_ID_BYTES];
        if (vcs_package_release_id(r, id) == VCS_PACKAGE_RELEASE_OK) {
            char id_hex[65];
            zcl_hex_encode(id, 32, id_hex);
            char path[4400];
            int n = snprintf(path, sizeof(path), "%s/releases/%s", zcode_dir,
                             id_hex);
            struct stat st;
            if (n > 0 && (size_t)n < sizeof(path) && stat(path, &st) == 0 &&
                (oldest == 0 || st.st_mtime < oldest))
                oldest = st.st_mtime;
        }
    }
    free(releases);
    ev->packages = (uint32_t)*subject_pkg_count;
    if (oldest > 0) {
        ev->observed = true;
        ev->first_observed_unix = (int64_t)oldest;
    }
}

/* ── reproduction: the existing receipts-dir scan per (package, recipe) ─ */
static void rep_collect_reproduction(const char *zcode_dir,
                                     const struct shop_rep_pair *pairs,
                                     size_t pair_count,
                                     struct shop_rep_evidence *ev)
{
    char dir[4400];
    int n = snprintf(dir, sizeof(dir), "%s/receipts", zcode_dir);
    if (n < 0 || (size_t)n >= sizeof(dir)) {
        LOG_ERROR(SHOP_REP_TAG, "receipts path too long: %s", zcode_dir);
        return;
    }
    for (size_t i = 0; i < pair_count; i++) {
        struct vcs_reproduce_report repro;
        if (!vcs_package_reproduce_scan(dir, pairs[i].package_root,
                                        pairs[i].recipe_root, &repro)) {
            LOG_ERROR(SHOP_REP_TAG, "receipts dir unreadable: %s", dir);
            return;
        }
        ev->matching_receipts += repro.matching;
        if (repro.reproduced)
            ev->reproduced_packages++;
    }
}

/* ── attestations: parse + verify, dedupe signer pubkeys ────────────── */
static void rep_collect_attestations(const char *zcode_dir,
                                     const uint8_t (*subject_pkgs)[32],
                                     size_t subject_pkg_count,
                                     struct shop_rep_evidence *ev)
{
    char dir[4400];
    int n = snprintf(dir, sizeof(dir), "%s/attestations", zcode_dir);
    if (n < 0 || (size_t)n >= sizeof(dir)) {
        LOG_ERROR(SHOP_REP_TAG, "attestations path too long: %s", zcode_dir);
        return;
    }
    DIR *d = opendir(dir);
    if (!d)
        return;     /* no attestations filed: an empty class, not an error */
    uint8_t *wire = zcl_malloc(VCS_PACKAGE_ATTEST_MAX_WIRE_BYTES,
                               "shop_rep_attest_wire");
    uint8_t (*verifiers)[33] = zcl_malloc(
        sizeof(*verifiers) * SHOP_REP_MAX_VERIFIERS, "shop_rep_verifiers");
    if (!wire || !verifiers) {
        free(wire);
        free(verifiers);
        closedir(d);
        return;
    }
    size_t verifier_count = 0;
    size_t scanned = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        uint8_t scratch[32];
        size_t scratch_len = 0;
        if (!zcl_hex_decode_n(de->d_name, scratch, 32, &scratch_len) ||
            scratch_len != 32)
            continue;
        if (scanned >= SHOP_REP_MAX_ATTEST_SCAN) {
            ev->attestations_truncated = true;
            break;
        }
        scanned++;
        char path[4400];
        n = snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(path))
            continue;
        FILE *f = fopen(path, "rb");
        if (!f)
            continue;
        size_t len = fread(wire, 1, VCS_PACKAGE_ATTEST_MAX_WIRE_BYTES, f);
        bool trailing = !feof(f);
        fclose(f);
        if (trailing || len == 0)
            continue;
        struct vcs_package_attest a;
        /* Only a signature-verified attestation is evidence; an unparseable
         * or bad-signature file is ignored, never counted either way. */
        if (vcs_package_attest_parse(wire, len, &a) != VCS_PACKAGE_ATTEST_OK ||
            vcs_package_attest_verify(&a) != VCS_PACKAGE_ATTEST_OK)
            continue;
        if (!rep_root_seen(subject_pkgs, subject_pkg_count, a.package_root))
            continue;
        ev->valid_attestations++;
        bool seen = false;
        for (size_t v = 0; v < verifier_count && !seen; v++)
            seen = memcmp(verifiers[v], a.verifier_pubkey, 33) == 0;
        if (!seen && verifier_count < SHOP_REP_MAX_VERIFIERS)
            memcpy(verifiers[verifier_count++], a.verifier_pubkey, 33);
    }
    ev->distinct_verifiers = (uint32_t)verifier_count;
    free(verifiers);
    free(wire);
    closedir(d);
}

/* ── dependent packages: root-committed declarations, read locally ──── */

/* Read the zcode-package.json declaration member of one package from the
 * local CAS. Returns false when the bytes are not held locally (the member
 * chunks were never fetched) or the read fails — the caller counts that as
 * unavailable, never as "no dependencies". */
static bool rep_read_declaration(const char *zcode_dir,
                                 const uint8_t package_root[32],
                                 uint8_t *text, size_t *len_out)
{
    char root_hex[65];
    zcl_hex_encode(package_root, 32, root_hex);
    char path[4400];
    int n = snprintf(path, sizeof(path), "%s/manifests/%s", zcode_dir,
                     root_hex);
    if (n < 0 || (size_t)n >= sizeof(path))
        return false;   // raw-return-ok:path-too-long-reads-as-unavailable
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;   // raw-return-ok:absent-manifest-reads-as-unavailable
    uint8_t *wire = zcl_malloc(VCS_PACKAGE_MANIFEST_MAX_WIRE_BYTES,
                               "shop_rep_manifest_wire");
    if (!wire) {
        fclose(f);
        return false;
    }
    size_t len = fread(wire, 1, VCS_PACKAGE_MANIFEST_MAX_WIRE_BYTES, f);
    bool trailing = !feof(f);
    fclose(f);
    bool ok = false;
    struct vcs_package_manifest manifest;
    if (!trailing && len > 0 &&
        vcs_package_manifest_parse(wire, len, &manifest)) {
        /* The package root commits the manifest: a persisted manifest whose
         * root no longer matches carries no committed declaration at all. */
        uint8_t mroot[32];
        if (!vcs_package_manifest_root(&manifest, mroot) ||
            memcmp(mroot, package_root, 32) != 0) {
            vcs_package_manifest_free(&manifest);
            free(wire);
            return false;
        }
        const struct vcs_package_file *meta = NULL;
        for (size_t i = 0; i < manifest.count; i++)
            if (strcmp(manifest.files[i].path, VCS_PACKAGE_DEPS_META_PATH) == 0)
                meta = &manifest.files[i];
        if (!meta) {
            *len_out = 0;   /* no declaration file: no dependencies */
            ok = true;
        } else if (meta->size <= VCS_PACKAGE_DEPS_META_MAX_BYTES &&
                   meta->chunk_count == 1) {
            char chunk_hex[65];
            zcl_hex_encode(meta->chunk_hashes, 32, chunk_hex);
            n = snprintf(path, sizeof(path), "%s/cas/sha3/%.2s/%s",
                         zcode_dir, chunk_hex, chunk_hex);
            FILE *cf = n > 0 && (size_t)n < sizeof(path) ? fopen(path, "rb")
                                                         : NULL;
            if (cf) {
                size_t got = fread(text, 1, (size_t)meta->size, cf);
                int extra = fgetc(cf);
                fclose(cf);
                /* The chunk hash binds the bytes to the manifest (and so to
                 * the package root): verify before trusting a declaration. */
                uint8_t chash[32];
                if (got == (size_t)meta->size && extra == EOF &&
                    vcs_package_chunk_hash(text, got, chash) &&
                    memcmp(chash, meta->chunk_hashes, 32) == 0) {
                    *len_out = got;
                    ok = true;
                }
            }
        }
        vcs_package_manifest_free(&manifest);
    }
    free(wire);
    return ok;
}

static void rep_collect_dependents(const char *zcode_dir,
                                   const uint8_t (*subject_pkgs)[32],
                                   size_t subject_pkg_count,
                                   const uint8_t (*all_pkgs)[32],
                                   size_t all_pkg_count,
                                   struct shop_rep_evidence *ev)
{
    uint8_t *text = zcl_malloc(VCS_PACKAGE_DEPS_META_MAX_BYTES + 1u,
                               "shop_rep_deps_text");
    if (!text)
        return;
    for (size_t i = 0; i < all_pkg_count; i++) {
        if (rep_root_seen(subject_pkgs, subject_pkg_count, all_pkgs[i]))
            continue;   /* the subject's own packages are not "dependents" */
        size_t len = 0;
        if (!rep_read_declaration(zcode_dir, all_pkgs[i], text, &len)) {
            ev->declarations_unavailable++;
            continue;
        }
        ev->declarations_read++;
        struct vcs_package_deps deps;
        enum vcs_package_deps_error derr = vcs_package_deps_parse_meta(
            text, len, &deps, NULL, 0);
        if (derr != VCS_PACKAGE_DEPS_OK) {
            ev->declarations_read--;
            ev->declarations_unavailable++;
            continue;
        }
        for (size_t k = 0; k < deps.count; k++) {
            if (rep_root_seen(subject_pkgs, subject_pkg_count,
                              deps.items[k].root)) {
                ev->dependent_packages++;
                break;
            }
        }
    }
    free(text);
}

/* ── simulated settlements: the reward ledger's settled facts ───────── */
static void rep_collect_settlements(const char *zcode_dir,
                                    const uint8_t publisher[33],
                                    struct shop_rep_evidence *ev)
{
    struct vcs_reward_ledger *ledger = vcs_reward_ledger_load(zcode_dir);
    if (!ledger)
        return;     /* allocation failure, already logged */
    struct vcs_reward_contributor_totals totals;
    vcs_reward_contributor_totals(ledger, publisher, &totals);
    ev->settled_entries = totals.settled_entries;
    vcs_reward_ledger_free(ledger);
}

/* ── rendering ──────────────────────────────────────────────────────── */
static void rep_row(struct json_value *rows, const char *fact,
                    const char *state, bool has_value, int64_t value,
                    const char *evidence_class, const char *window,
                    const char *detail)
{
    struct json_value row;
    json_init(&row);
    json_set_object(&row);
    (void)json_push_kv_str(&row, "fact", fact);
    (void)json_push_kv_str(&row, "state", state);
    if (has_value)
        (void)json_push_kv_int(&row, "value", value);
    (void)json_push_kv_str(&row, "evidence_class", evidence_class);
    (void)json_push_kv_str(&row, "window", window);
    (void)json_push_kv_str(&row, "detail", detail);
    (void)json_push_back(rows, &row);
    json_free(&row);
}

static void rep_render(struct json_value *data, const char *publisher_hex,
                       const char *datadir, int64_t now_unix,
                       const struct shop_rep_evidence *ev)
{
    (void)json_push_kv_str(data, "publisher", publisher_hex);
    (void)json_push_kv_str(data, "datadir", datadir);
    (void)json_push_kv_int(data, "now_unix", now_unix);
    (void)json_push_kv_bool(data, "zcode_store_present", ev->store_present);

    static const char *const LOCAL_STORE = "this node's local zcode store";
    struct json_value rows;
    json_init(&rows);
    json_set_array(&rows);

    rep_row(&rows, "releases_published",
            ev->releases > 0 ? "recorded" : "no_record",
            ev->releases > 0, ev->releases,
            "secp256k1-signed release envelopes, verified at publication",
            LOCAL_STORE,
            ev->releases > 0
                ? "release envelopes naming this publisher key held locally"
                : "no release envelope naming this publisher key is held");

    rep_row(&rows, "packages_published",
            ev->packages > 0 ? "recorded" : "no_record",
            ev->packages > 0, ev->packages,
            "distinct package roots across the signed release envelopes",
            LOCAL_STORE,
            ev->packages > 0
                ? "distinct package roots this publisher has released locally"
                : "no package root released by this publisher is held");

    if (ev->observed && now_unix >= ev->first_observed_unix) {
        char detail[160];
        (void)snprintf(detail, sizeof(detail),
                       "oldest release envelope first recorded locally at "
                       "unix %lld; file mtimes are this node's own "
                       "observation record, not a signed timestamp",
                       (long long)ev->first_observed_unix);
        rep_row(&rows, "days_observed", "recorded", true, ev->days_observed,
                "local store file mtime (unsigned local observation)",
                "this node only; says nothing about anyone else", detail);
    } else {
        rep_row(&rows, "days_observed", "no_record", false, 0,
                "local store file mtime (unsigned local observation)",
                "this node only", "no release envelope is held locally");
    }

    {
        char detail[224];
        (void)snprintf(detail, sizeof(detail),
                       "%u build receipt(s) name this publisher's "
                       "package+recipe pairs, %u package(s) reproduced "
                       "byte-identically across distinct receipt ids; "
                       "receipts carry no signer identity, so these are "
                       "distinct recorded build events and nothing about "
                       "who built them is established",
                       ev->matching_receipts, ev->reproduced_packages);
        rep_row(&rows, "reproductions",
                ev->matching_receipts > 0 ? "recorded" : "no_record",
                ev->matching_receipts > 0, ev->matching_receipts,
                "build receipts filed under <datadir>/zcode/receipts with "
                "byte-identical output sets (distinct receipt ids)",
                LOCAL_STORE,
                ev->matching_receipts > 0 ? detail
                : "no build receipt naming this publisher's packages is filed");
    }

    {
        char detail[224];
        (void)snprintf(detail, sizeof(detail),
                       "%u signature-verified attestation(s) from %u "
                       "distinct verifier pubkey(s); a verified signature "
                       "proves authorship of the exact bytes only — who "
                       "the signer is, or whether any two signers are the "
                       "same operator, is not established%s",
                       ev->valid_attestations, ev->distinct_verifiers,
                       ev->attestations_truncated
                           ? "; the attestation scan hit its cap, so these "
                             "counts are a lower bound" : "");
        rep_row(&rows, "distinct_signing_identities",
                ev->distinct_verifiers > 0 ? "recorded" : "no_record",
                ev->distinct_verifiers > 0, ev->distinct_verifiers,
                "distinct secp256k1 verifier pubkeys over attestations "
                "whose signature verifies at read time",
                LOCAL_STORE,
                ev->distinct_verifiers > 0 ? detail
                : "no valid attestation naming this publisher's packages "
                  "is filed");
    }

    {
        char detail[224];
        (void)snprintf(detail, sizeof(detail),
                       "%u declaration(s) read, %u unreadable locally "
                       "(manifest or member bytes not held — counted as "
                       "unavailable, never as no-dependency)",
                       ev->declarations_read, ev->declarations_unavailable);
        rep_row(&rows, "dependent_packages",
                ev->dependent_packages > 0 ? "recorded" : "no_record",
                ev->dependent_packages > 0, ev->dependent_packages,
                "root-committed dependency declarations (zcode-package.json "
                "is a manifest member; the package root commits it)",
                LOCAL_STORE,
                ev->dependent_packages > 0 ? detail
                : "no locally readable declaration names one of this "
                  "publisher's package roots");
    }

    rep_row(&rows, "simulated_settlements",
            ev->settled_entries > 0 ? "recorded" : "no_record",
            ev->settled_entries > 0, ev->settled_entries,
            "settled facts in the simulated reward ledger under "
            "<datadir>/zcode/rewards (placeholder token; ZC23 issuance "
            "stays simulation-only)",
            LOCAL_STORE,
            ev->settled_entries > 0
                ? "simulated reward entries settled to this contributor key"
                : "no simulated settlement to this contributor key is "
                  "recorded");

    rep_row(&rows, "availability_challenges", "unavailable", false, 0,
            "none: no durable challenge record exists",
            "n/a",
            "the file-market chunk-challenge loop keeps pass/fail counts in "
            "per-download memory only; a durable record lands with the "
            "challenge loop (docs/work/SHOP_COMMAND.md slice C)");

    rep_row(&rows, "paid_fulfillments", "unavailable", false, 0,
            "none: no datadir-local settlement source exists",
            "n/a",
            "patronage settlement lives on the scratch-workspace simulation "
            "lane (zcode.patronage.*), not in the datadir store; the row "
            "appears when patronage settle lands on this lane");

    (void)json_push_kv(data, "evidence", &rows);
    json_free(&rows);

    (void)json_push_kv_str(data, "doctrine",
        "every row is a fact this node can prove from its own records, with "
        "the evidence class and counting window stated on the row; absent "
        "evidence reads 'no_record' or 'unavailable', never a zero; "
        "nothing here measures intent, quality, or honesty");
}

/* ── the handler ────────────────────────────────────────────────────── */
void zcl_native_handle_shop_reputation(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const char *datadir = json_get_str(json_get(request->input, "datadir"));
    if (!datadir || !datadir[0])
        datadir = zcl_native_command_datadir();
    if (!datadir || !datadir[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false,
                               "no datadir given and no --datadir default",
                               "datadir");
        return;
    }
    const char *publisher_hex =
        json_get_str(json_get(request->input, "publisher"));
    uint8_t publisher[33];
    if (!publisher_hex || strlen(publisher_hex) != 66 ||
        !zcl_hex_decode_lower(publisher_hex, publisher, 33)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_PUBLISHER_INPUT",
                               "validate", false, false,
                               "publisher must be the 66-hex compressed "
                               "secp256k1 pubkey the releases are signed "
                               "with (the publisher column of zcode package "
                               "search)", "publisher");
        return;
    }
    int64_t now_unix = clock_now_wall_ms() / 1000;
    const struct json_value *now = json_get(request->input, "now_unix");
    if (now) {
        if (now->type != JSON_INT || json_get_int(now) <= 0) {
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_INVALID, "BAD_NOW_UNIX",
                                   "validate", false, false,
                                   "now_unix must be a positive integer",
                                   "now_unix");
            return;
        }
        now_unix = json_get_int(now);
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

    struct shop_rep_evidence ev;
    memset(&ev, 0, sizeof(ev));
    struct stat st;
    /* Absent and unreadable are not the same answer: a present
     * non-directory at the store path — or at any of the store's known
     * subdirectories — must refuse by name, never be reported as an
     * empty store (the read-leaf doctrine, asserted by
     * test_read_leaf_no_datadir_write's payload-store case, which breaks
     * zcode/manifests for this leaf). A fresh store has none of these
     * subdirectories yet, and that absence is the legitimate empty
     * answer. */
    static const char *const subdirs[] = {
        "", "releases", "manifests", "receipts", "attestations", "rewards"
    };
    for (size_t i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
        char probe[4400];
        int pn = snprintf(probe, sizeof(probe), "%s%s%s", zcode_dir,
                          subdirs[i][0] ? "/" : "", subdirs[i]);
        if (pn < 0 || (size_t)pn >= sizeof(probe))
            continue;
        if (stat(probe, &st) == 0 && !S_ISDIR(st.st_mode)) {
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                                   ZCL_COMMAND_EXIT_BLOCKED,
                                   "ZCODE_STORE_UNREADABLE", "execute",
                                   false, false,
                                   "a path inside <datadir>/zcode exists "
                                   "but is not a readable store directory "
                                   "— inspect it before trusting any "
                                   "'no_record' row", probe);
            return;
        }
    }
    ev.store_present = stat(zcode_dir, &st) == 0 && S_ISDIR(st.st_mode);

    uint8_t (*subject_pkgs)[32] = zcl_calloc(
        SHOP_REP_MAX_PACKAGES, sizeof(*subject_pkgs), "shop_rep_subject_pkgs");
    uint8_t (*all_pkgs)[32] = zcl_calloc(
        SHOP_REP_MAX_PACKAGES, sizeof(*all_pkgs), "shop_rep_all_pkgs");
    struct shop_rep_pair *pairs = zcl_calloc(
        SHOP_REP_MAX_PACKAGES, sizeof(*pairs), "shop_rep_pairs");
    if (!subject_pkgs || !all_pkgs || !pairs) {
        free(subject_pkgs);
        free(all_pkgs);
        free(pairs);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "ALLOC",
                               "execute", false, false,
                               "evidence buffers", zcode_dir);
        return;
    }
    size_t subject_pkg_count = 0, all_pkg_count = 0, pair_count = 0;
    if (ev.store_present) {
        rep_collect_publication(zcode_dir, publisher, &ev, subject_pkgs,
                                &subject_pkg_count, pairs, &pair_count,
                                all_pkgs, &all_pkg_count);
        rep_collect_reproduction(zcode_dir, pairs, pair_count, &ev);
        rep_collect_attestations(zcode_dir, subject_pkgs, subject_pkg_count,
                                 &ev);
        rep_collect_dependents(zcode_dir, subject_pkgs, subject_pkg_count,
                               all_pkgs, all_pkg_count, &ev);
        rep_collect_settlements(zcode_dir, publisher, &ev);
    }
    free(pairs);
    free(all_pkgs);
    free(subject_pkgs);

    if (ev.observed && now_unix >= ev.first_observed_unix)
        ev.days_observed = (now_unix - ev.first_observed_unix) / 86400;

    rep_render(&reply->data, publisher_hex, datadir, now_unix, &ev);
}
