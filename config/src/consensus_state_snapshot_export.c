/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Contained, immutable full-history consensus-state exporter. */

#define _GNU_SOURCE

#include "config/consensus_state_snapshot_export.h"

#include "config/consensus_state_bundle_validate.h"
#include "consensus_state_snapshot_export_internal.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1u << 0)
#endif

#define EXPORT_SUBSYS "consensus_bundle_export"

bool consensus_export_fail(struct consensus_state_export_result *result,
                           enum consensus_state_export_status status,
                           const char *fmt, ...)
{
    char reason[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, sizeof(reason), fmt, ap);
    va_end(ap);
    if (result) {
        result->status = status;
        snprintf(result->reason, sizeof(result->reason), "%s", reason);
    }
    LOG_WARN(EXPORT_SUBSYS, "%s", reason);
    return false;
}

static bool path_is_new_regular_target(const char *path)
{
    if (!path || !path[0] || strlen(path) >= PATH_MAX || strchr(path, '?') ||
        strchr(path, '#') || strchr(path, '%'))
        return false;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!base[0] || strcmp(base, ".") == 0 || strcmp(base, "..") == 0)
        return false;
    struct stat st;
    errno = 0;
    return lstat(path, &st) != 0 && errno == ENOENT;
}

static void remove_bundle_paths(const char *path)
{
    if (!path || !path[0])
        return;
    (void)unlink(path);
    static const char *const suffixes[] = {"-journal", "-wal", "-shm"};
    char sidecar[CONSENSUS_EXPORT_TEMP_PATH_MAX + 16];
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        int n = snprintf(sidecar, sizeof(sidecar), "%s%s", path, suffixes[i]);
        if (n > 0 && (size_t)n < sizeof(sidecar))
            (void)unlink(sidecar);
    }
}

static bool bundle_sidecars_absent(const char *path)
{
    static const char *const suffixes[] = {"-journal", "-wal", "-shm"};
    char sidecar[CONSENSUS_EXPORT_TEMP_PATH_MAX + 16];
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        int n = snprintf(sidecar, sizeof(sidecar), "%s%s", path, suffixes[i]);
        if (n <= 0 || (size_t)n >= sizeof(sidecar))
            return false;
        struct stat st;
        errno = 0;
        if (lstat(sidecar, &st) == 0 || errno != ENOENT)
            return false;
    }
    return true;
}

bool consensus_export_open_temp(const char *final_path,
                                char temp_path[CONSENSUS_EXPORT_TEMP_PATH_MAX],
                                sqlite3 **destination,
                                struct consensus_state_export_result *result)
{
    static atomic_uint_fast64_t sequence = 0;
    *destination = NULL;
    if (!path_is_new_regular_target(final_path))
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_REFUSED,
            "output path is invalid, aliased, or already exists");

    int fd = -1;
    for (unsigned attempt = 0; attempt < 64 && fd < 0; attempt++) {
        uint64_t nonce = atomic_fetch_add(&sequence, 1) + 1;
        int n = snprintf(temp_path, CONSENSUS_EXPORT_TEMP_PATH_MAX,
                         "%s.tmp.%ld.%llu", final_path, (long)getpid(),
                         (unsigned long long)nonce);
        if (n <= 0 || n >= CONSENSUS_EXPORT_TEMP_PATH_MAX)
            return consensus_export_fail(result, CONSENSUS_EXPORT_REFUSED,
                                         "output staging path is too long");
        fd = open(temp_path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW,
                  S_IRUSR | S_IWUSR);
        if (fd < 0 && errno != EEXIST)
            break;
    }
    if (fd < 0)
        return consensus_export_fail(result, CONSENSUS_EXPORT_OUTPUT_ERROR,
                                     "unique output staging file create failed");
    if (close(fd) != 0) {
        remove_bundle_paths(temp_path);
        return consensus_export_fail(result, CONSENSUS_EXPORT_OUTPUT_ERROR,
                                     "output staging file close failed");
    }

    int rc = sqlite3_open_v2(temp_path, destination,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX,
                             NULL);
    if (rc != SQLITE_OK) {
        if (*destination)
            sqlite3_close(*destination);
        *destination = NULL;
        remove_bundle_paths(temp_path);
        return consensus_export_fail(result, CONSENSUS_EXPORT_OUTPUT_ERROR,
                                     "output SQLite open failed");
    }
    int defensive = 0;
    char *error = NULL;
    bool ok = sqlite3_db_config(*destination, SQLITE_DBCONFIG_DEFENSIVE, 1,
                                &defensive) == SQLITE_OK &&
              defensive == 1 &&
              sqlite3_exec(*destination,
                  "PRAGMA journal_mode=DELETE;"
                  "PRAGMA synchronous=FULL;"
                  "PRAGMA temp_store=MEMORY;"
                  "PRAGMA foreign_keys=ON;"
                  "PRAGMA locking_mode=EXCLUSIVE;",
                  NULL, NULL, &error) == SQLITE_OK;
    if (error) {
        LOG_WARN(EXPORT_SUBSYS, "output durability configuration failed: %s",
                 error);
        sqlite3_free(error);
    }
    if (!ok) {
        sqlite3_close(*destination);
        *destination = NULL;
        remove_bundle_paths(temp_path);
        return consensus_export_fail(result, CONSENSUS_EXPORT_OUTPUT_ERROR,
                                     "FULL-durable output setup failed");
    }
    return true;
}

static bool fsync_path(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    return ok;
}

static bool fsync_parent_directory(const char *path)
{
    char parent[PATH_MAX];
    if (strlen(path) >= sizeof(parent))
        return false;
    snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
    if (!slash)
        snprintf(parent, sizeof(parent), ".");
    else if (slash == parent)
        slash[1] = '\0';
    else
        *slash = '\0';
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    return ok;
}

static bool manifests_equal(
    const struct consensus_state_bundle_manifest *a,
    const struct consensus_state_bundle_manifest *b)
{
    return a->height == b->height &&
           a->history_complete == b->history_complete &&
           a->activation_boundary == b->activation_boundary &&
           a->utxo_count == b->utxo_count &&
           a->total_supply == b->total_supply &&
           a->anchor_count == b->anchor_count &&
           a->sprout_frontier_height == b->sprout_frontier_height &&
           a->sapling_frontier_height == b->sapling_frontier_height &&
           a->nullifier_count == b->nullifier_count &&
           a->sprout_source_cursor == b->sprout_source_cursor &&
           a->sapling_source_cursor == b->sapling_source_cursor &&
           a->nullifier_source_cursor == b->nullifier_source_cursor &&
           a->source_fold_cursor == b->source_fold_cursor &&
           memcmp(a->block_hash, b->block_hash, 32) == 0 &&
           memcmp(a->utxo_root, b->utxo_root, 32) == 0 &&
           memcmp(a->anchor_digest, b->anchor_digest, 32) == 0 &&
           memcmp(a->sprout_frontier_root, b->sprout_frontier_root, 32) == 0 &&
           memcmp(a->sapling_frontier_root, b->sapling_frontier_root, 32) == 0 &&
           memcmp(a->nullifier_digest, b->nullifier_digest, 32) == 0 &&
           memcmp(a->proof_manifest_digest, b->proof_manifest_digest, 32) == 0 &&
           memcmp(a->source_digest, b->source_digest, 32) == 0 &&
           memcmp(a->artifact_digest, b->artifact_digest, 32) == 0;
}

bool consensus_export_finalize_temp(
    const char *temp_path, const char *final_path,
    const struct consensus_state_bundle_manifest *manifest,
    struct consensus_state_export_result *result)
{
    if (!bundle_sidecars_absent(temp_path))
        return consensus_export_fail(result, CONSENSUS_EXPORT_OUTPUT_ERROR,
                                     "bundle SQLite sidecar remains after close");
    if (!fsync_path(temp_path) || chmod(temp_path, S_IRUSR) != 0 ||
        !fsync_path(temp_path))
        return consensus_export_fail(result, CONSENSUS_EXPORT_OUTPUT_ERROR,
                                     "bundle fsync/immutable mode failed");

    struct stat before;
    if (lstat(temp_path, &before) != 0 || !S_ISREG(before.st_mode) ||
        (before.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) != 0)
        return consensus_export_fail(result, CONSENSUS_EXPORT_OUTPUT_ERROR,
                                     "staged bundle identity/mode invalid");
    char uri[CONSENSUS_EXPORT_TEMP_PATH_MAX + 32];
    int n = snprintf(uri, sizeof(uri), "file:%s?mode=ro&immutable=1",
                     temp_path);
    if (n <= 0 || (size_t)n >= sizeof(uri))
        return consensus_export_fail(result, CONSENSUS_EXPORT_OUTPUT_ERROR,
                                     "staged bundle URI too long");
    sqlite3 *check = NULL;
    if (sqlite3_open_v2(uri, &check,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX,
            NULL) != SQLITE_OK) {
        if (check)
            sqlite3_close(check);
        return consensus_export_fail(result, CONSENSUS_EXPORT_OUTPUT_ERROR,
                                     "immutable bundle reopen failed");
    }
    int defensive = 0;
    int trusted = 1;
    bool ok = sqlite3_db_config(check, SQLITE_DBCONFIG_DEFENSIVE, 1,
                                &defensive) == SQLITE_OK &&
              sqlite3_db_config(check, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0,
                                &trusted) == SQLITE_OK &&
              defensive == 1 && trusted == 0 &&
              sqlite3_exec(check, "PRAGMA query_only=ON", NULL, NULL, NULL) ==
                  SQLITE_OK;
    struct consensus_state_bundle_manifest reopened;
    struct consensus_state_install_result validation;
    memset(&reopened, 0, sizeof(reopened));
    memset(&validation, 0, sizeof(validation));
    if (ok)
        ok = consensus_state_bundle_validate(check, &reopened, &validation);
    if (ok)
        ok = manifests_equal(manifest, &reopened);
    if (sqlite3_close(check) != SQLITE_OK)
        ok = false;
    struct stat after;
    if (!ok || lstat(temp_path, &after) != 0 ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec)
        return consensus_export_fail(
            result, CONSENSUS_EXPORT_OUTPUT_ERROR,
            "independent immutable bundle validation failed");

    if (!path_is_new_regular_target(final_path) ||
        renameat2(AT_FDCWD, temp_path, AT_FDCWD, final_path,
                  RENAME_NOREPLACE) != 0)
        return consensus_export_fail(result, CONSENSUS_EXPORT_OUTPUT_ERROR,
                                     "atomic no-replace bundle publish failed");
    if (!fsync_parent_directory(final_path)) {
        /* The rename happened, but without a directory durability witness the
         * artifact cannot be reported as accepted. Remove it and fsync that
         * removal when possible; never return a false successful receipt. */
        (void)unlink(final_path);
        (void)fsync_parent_directory(final_path);
        return consensus_export_fail(result, CONSENSUS_EXPORT_OUTPUT_ERROR,
                                     "bundle directory fsync failed");
    }
    return true;
}

static bool digest_nonzero(const uint8_t digest[32])
{
    uint8_t any = 0;
    for (size_t i = 0; i < 32; i++)
        any |= digest[i];
    return any != 0;
}

bool consensus_state_snapshot_export(
    sqlite3 *progress_db,
    const struct consensus_state_snapshot_export_request *request,
    struct consensus_state_export_result *result)
{
    if (result)
        memset(result, 0, sizeof(*result));
    if (!progress_db || !request || !request->output_path)
        return consensus_export_fail(result, CONSENSUS_EXPORT_REFUSED,
                                     "NULL progress_db/request/output_path");
    if (progress_store_db() != progress_db)
        return consensus_export_fail(result, CONSENSUS_EXPORT_REFUSED,
                                     "source is not the owned progress.kv handle");
    if (request->expected_height < 0 ||
        !digest_nonzero(request->expected_block_hash))
        return consensus_export_fail(result, CONSENSUS_EXPORT_REFUSED,
                                     "invalid expected source height/hash");
    if (!path_is_new_regular_target(request->output_path))
        return consensus_export_fail(result, CONSENSUS_EXPORT_REFUSED,
                                     "output path is invalid or already exists");

    char temp_path[CONSENSUS_EXPORT_TEMP_PATH_MAX] = {0};
    sqlite3 *destination = NULL;
    struct consensus_state_bundle_manifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    struct consensus_state_source_receipt receipt;
    memset(&receipt, 0, sizeof(receipt));
    struct consensus_state_bundle_proof_summary
        proofs[CONSENSUS_STATE_BUNDLE_PROOF_COUNT];
    memset(proofs, 0, sizeof(proofs));
    bool source_tx = false;
    bool ok = true;

    progress_store_tx_lock();
    if (sqlite3_get_autocommit(progress_db) == 0) {
        (void)consensus_export_fail(result, CONSENSUS_EXPORT_REFUSED,
                                    "source already has an open transaction");
        ok = false;
    }
    if (ok && sqlite3_exec(progress_db, "BEGIN", NULL, NULL, NULL) !=
                  SQLITE_OK) {
        (void)consensus_export_fail(result, CONSENSUS_EXPORT_STORE_ERROR,
                                    "frozen source transaction begin failed");
        ok = false;
    } else if (ok) {
        source_tx = true;
    }
    if (ok)
        ok = consensus_export_prove_source(progress_db, request, &manifest,
                                           &receipt, proofs, result);
    if (ok)
        ok = consensus_export_open_temp(request->output_path, temp_path,
                                        &destination, result);
    if (ok)
        ok = consensus_export_write_bundle(progress_db, destination, &manifest,
                                           &receipt, proofs, result);
    if (destination) {
        if (sqlite3_close(destination) != SQLITE_OK && ok) {
            (void)consensus_export_fail(result, CONSENSUS_EXPORT_OUTPUT_ERROR,
                                        "bundle close failed");
            ok = false;
        }
        destination = NULL;
    }
    if (source_tx) {
        const char *finish = ok ? "COMMIT" : "ROLLBACK";
        if (sqlite3_exec(progress_db, finish, NULL, NULL, NULL) != SQLITE_OK) {
            if (ok)
                (void)consensus_export_fail(
                    result, CONSENSUS_EXPORT_STORE_ERROR,
                    "frozen source transaction close failed");
            ok = false;
            sqlite3_exec(progress_db, "ROLLBACK", NULL, NULL, NULL);
        }
    }
    progress_store_tx_unlock();

    if (ok)
        ok = consensus_export_finalize_temp(temp_path, request->output_path,
                                            &manifest, result);
    if (!ok) {
        remove_bundle_paths(temp_path);
        return false;
    }
    if (result) {
        result->status = CONSENSUS_EXPORT_EXPORTED;
        result->history_complete = true;
        result->height = manifest.height;
        result->utxo_count = manifest.utxo_count;
        result->anchor_count = manifest.anchor_count;
        result->nullifier_count = manifest.nullifier_count;
        memcpy(result->artifact_digest, manifest.artifact_digest, 32);
        snprintf(result->reason, sizeof(result->reason),
                 "exported immutable %s height=%d",
                 CONSENSUS_STATE_BUNDLE_SCHEMA, manifest.height);
    }
    return true;
}
