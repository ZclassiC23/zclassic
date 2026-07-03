/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Fast wallet block scanner. Two-pass design:
 *
 * Pass 1 — Raw byte scan (no deserialization):
 *   mmap each block file and scan for P2PKH (76a914) / P2SH (a914)
 *   script patterns. Extract the 20-byte hash, check against hash table.
 *   Record file offsets of blocks that contain wallet matches.
 *   Speed: memory bandwidth limited (~10 GB/s on modern CPUs).
 *
 * Pass 2 — Selective deserialization:
 *   Only deserialize blocks identified in pass 1.
 *   Full tx parsing for correct UTXO create/spend tracking.
 *
 * Result: skips 99.9%+ of block deserialization. */

#include "platform/time_compat.h"
#include "views/format_helpers.h"
#include "controllers/wallet_scan.h"
#include "controllers/sync_controller.h"
#include "services/wallet_scan_service.h"
#include "models/wallet_tx.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "validation/chainstate.h"
#include "wallet/wallet.h"
#include "wallet/keystore.h"
#include "core/serialize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include "controllers/scan_util.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

static bool wallet_scan_begin_checked(struct node_db *ndb,
                                      const char *label)
{
    if (!ndb || !ndb->open || !node_db_begin(ndb)) {
        LOG_FAIL("wallet_scan", "wallet_scan: %s failed: %s",
                label, (ndb && ndb->db) ? sqlite3_errmsg(ndb->db)
                                        : "db unavailable");
    }
    return true;
}

static bool wallet_scan_commit_checked(struct node_db *ndb,
                                       const char *label)
{
    if (!ndb || !ndb->open || !node_db_commit(ndb)) {
        LOG_FAIL("wallet_scan", "wallet_scan: %s failed: %s",
                label, (ndb && ndb->db) ? sqlite3_errmsg(ndb->db)
                                        : "db unavailable");
    }
    return true;
}

static void wallet_scan_rollback_best_effort(struct node_db *ndb,
                                             const char *label)
{
    if (!ndb || !ndb->open)
        return;
    if (!node_db_rollback(ndb)) {
        LOG_WARN("wallet_scan", "wallet_scan: %s failed: %s", label, ndb->db ? sqlite3_errmsg(ndb->db) : "db unavailable");
    }
}

static bool wallet_scan_exec_checked(struct node_db *ndb,
                                     const char *sql,
                                     const char *label)
{
    if (!ndb || !ndb->open || !sql)
        LOG_FAIL("wallet_scan", "exec_checked: invalid args (ndb=%p sql=%p)", (void *)ndb, (void *)sql);
    if (!node_db_exec(ndb, sql))
        LOG_FAIL("wallet_scan", "wallet_scan: %s failed", label);
    return true;
}

/* --- Pass 1: Raw byte pattern scan --- */

/* Scan context for parallel file scanning */
struct file_scan_result {
    bool has_match;
};

/* Scan a single mmap'd block file for P2PKH/P2SH patterns
 * matching our address hash table. */
static bool scan_file_raw(const uint8_t *data, size_t size,
                           const struct addr_ht *ht)
{
    /* Scan for P2PKH: 76 a9 14 [20 bytes] 88 ac (25 bytes total)
     * Scan for P2SH:  a9 14 [20 bytes] 87      (23 bytes total) */
    for (size_t i = 0; i + 25 <= size; i++) {
        /* P2PKH check */
        if (data[i] == 0x76 && data[i + 1] == 0xa9 &&
            data[i + 2] == 0x14 &&
            data[i + 23] == 0x88 && data[i + 24] == 0xac) {
            if (aht_has(ht, data + i + 3))
                return true;
        }
        /* P2SH check */
        if (data[i] == 0xa9 && data[i + 1] == 0x14 &&
            i + 23 <= size && data[i + 22] == 0x87) {
            if (aht_has(ht, data + i + 2))
                return true;
        }
    }
    return false;
}

/* Parallel file scanner thread argument */
struct scan_thread_arg {
    const char *datadir;
    int file_num;
    const struct addr_ht *ht;
    bool result;
};

static void *scan_file_thread(void *arg)
{
    struct scan_thread_arg *a = (struct scan_thread_arg *)arg;
    char path[512];
    snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat",
             a->datadir, a->file_num);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { a->result = false; LOG_NULL("wallet_scan", "open failed for blk%05d.dat", a->file_num); }
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); a->result = false; LOG_NULL("wallet_scan", "fstat failed for blk%05d.dat", a->file_num); }
    size_t sz = (size_t)st.st_size;
    uint8_t *data = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) { a->result = false; LOG_NULL("wallet_scan", "mmap failed for blk%05d.dat (size=%zu)", a->file_num, sz); }
    posix_madvise(data, sz, POSIX_MADV_SEQUENTIAL);

    a->result = scan_file_raw(data, sz, a->ht);

    munmap(data, sz);
    return NULL;
}

/* --- Main entry point --- */

int wallet_scan_blocks(struct node_db *ndb,
                       const struct active_chain *chain,
                       const struct wallet *w,
                       const char *datadir,
                       int start_height,
                       int end_height)
{
    if (!ndb || !ndb->open || !chain || !w || !datadir)
        LOG_ERR("wallet_scan", "scan_blocks: invalid args (ndb=%p chain=%p w=%p datadir=%p)",
                (void *)ndb, (void *)chain, (void *)w, (void *)datadir);

    /* range fast-path. Empty range = nothing to do. */
    if (start_height > end_height) {
        printf("wallet_scan: range empty (start=%d > end=%d), "
               "skipping\n", start_height, end_height);
        return 0;
    }

    struct timespec ts_start, ts_p1;
    platform_time_monotonic_timespec(&ts_start);

    /* Build address hash table */
    struct addr_ht aht;
    aht_init(&aht);
    for (size_t i = 0; i < w->keystore.num_keys; i++)
        if (w->keystore.keys[i].used)
            aht_insert(&aht, w->keystore.keys[i].keyid.id.data);
    for (size_t i = 0; i < w->keystore.num_scripts; i++)
        if (w->keystore.scripts[i].used)
            aht_insert(&aht, w->keystore.scripts[i].script_id.data);

    printf("wallet_scan: %d address hashes loaded\n", aht.count);
    fflush(stdout);

    /* zero-keys fast-path. Without any keys to
     * match, the parallel raw scan would read every block file
     * looking for hashes that aren't in the set — minutes of
     * pointless disk I/O. */
    if (aht.count == 0) {
        printf("wallet_scan: no wallet keys, skipping block scan\n");
        return 0;
    }

    /* Determine which block files exist */
    int num_files = 0;
    for (int f = 0; f < 200; f++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/blocks/blk%05d.dat", datadir, f);
        if (access(path, R_OK) != 0) break;
        num_files = f + 1;
    }
    printf("wallet_scan: %d block files to scan\n", num_files);
    fflush(stdout);

    /* ========== PASS 1: Parallel raw byte scan ========== */
    printf("wallet_scan: pass 1 — parallel raw byte scan...\n");
    fflush(stdout);

    /* Launch threads — up to 8 at a time */
    bool *file_has_match = zcl_calloc((size_t)num_files, sizeof(bool), "wallet scan file match");
    if (!file_has_match) {
        /* zcl_calloc already logged the OOM; release the address hash table
         * and fail the scan rather than dereferencing NULL at the join loop. */
        aht_free(&aht);
        return -1; /* raw-return-ok:zcl_calloc-logged */
    }
    int batch = 8;

    for (int base = 0; base < num_files; base += batch) {
        int n = num_files - base;
        if (n > batch) n = batch;

        struct scan_thread_arg args[8];
        pthread_t threads[8];
        int started = 0;

        for (int i = 0; i < n; i++) {
            args[i].datadir = datadir;
            args[i].file_num = base + i;
            args[i].ht = &aht;
            args[i].result = false;
            /* raw-pthread-ok: short-burst-joined-immediately */
            if (pthread_create(&threads[i], NULL,
                               scan_file_thread, &args[i]) != 0) {
                LOG_WARN("wallet_scan", "wallet_scan: failed to start pass-1 scan thread");
                for (int j = 0; j < started; j++)
                    pthread_join(threads[j], NULL);
                aht_free(&aht);
                free(file_has_match);
                return -1; // raw-return-ok:logged-above
            }
            started++;
        }
        for (int i = 0; i < n; i++) {
            pthread_join(threads[i], NULL);
            file_has_match[base + i] = args[i].result;
        }
    }

    platform_time_monotonic_timespec(&ts_p1);
    double p1_ms = (double)(ts_p1.tv_sec - ts_start.tv_sec) * 1000.0 +
                   (double)(ts_p1.tv_nsec - ts_start.tv_nsec) / 1e6;

    int matched_files = 0;
    for (int i = 0; i < num_files; i++)
        if (file_has_match[i]) matched_files++;

    printf("wallet_scan: pass 1 done in %.1f ms — %d/%d files contain wallet data\n",
           p1_ms, matched_files, num_files);
    fflush(stdout);

    if (matched_files == 0) {
        printf("wallet_scan: no wallet transactions found\n");
        aht_free(&aht);
        free(file_has_match);
        /* Still write empty results to clear any stale data */
        if (!wallet_scan_begin_checked(ndb, "empty-result reset begin"))
            LOG_ERR("wallet_scan", "empty-result: failed to begin db transaction");
        if (!wallet_scan_exec_checked(ndb, "DELETE FROM wallet_utxos",
                                      "empty-result clear wallet_utxos") ||
            !wallet_scan_exec_checked(ndb, "DELETE FROM wallet_transactions",
                                      "empty-result clear wallet_transactions")) {
            wallet_scan_rollback_best_effort(ndb,
                                             "empty-result reset rollback");
            LOG_ERR("wallet_scan", "empty-result: failed to clear wallet tables");
        }
        if (!wallet_scan_commit_checked(ndb, "empty-result reset commit"))
            LOG_ERR("wallet_scan", "empty-result: failed to commit db transaction");
        return 0;
    }

    /* ========== PASS 2: Selective block deserialization ========== */
    printf("wallet_scan: pass 2 — deserializing blocks in %d matched files...\n",
           matched_files);
    fflush(stdout);

    /* Pass 2 — the selective-deserialize loop, balance compute, and the
     * SQLite result-write transaction — is a Service. The Controller owns the
     * address hash table and the Pass-1 match array; it hands them to the
     * Service by const reference and frees them afterward. */
    int found = wallet_scan_pass2_execute(ndb, chain, datadir,
                                          start_height, end_height,
                                          &aht, file_has_match,
                                          &ts_start, &ts_p1);

    /* Cleanup */
    aht_free(&aht);
    free(file_has_match);

    return found;
}
