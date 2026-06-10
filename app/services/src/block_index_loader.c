/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block Index Loader: read/write block_index.bin flat file, SQLite cache, and
 * LevelDB block tree compatibility. */

#include "platform/time_compat.h"
#include "services/block_index_loader.h"
#include "services/block_index_integrity.h"
#include "services/chain_state_service.h"
#include "services/chain_tip.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/pow.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "storage/block_index_db.h"
#include "models/database.h"
#include "core/uint256.h"
#include "core/arith_uint256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sqlite3.h>

#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

/* ── Flat file format ────────────────────────────────────── */

/* Compact on-disk format: height-sorted, 192 bytes per entry */
struct __attribute__((packed)) block_index_flat {
    uint8_t  hash[32];
    uint8_t  prev_hash[32];
    int32_t  height;
    uint32_t n_bits;
    uint32_t n_time;
    int32_t  n_version;
    uint32_t n_status;
    int32_t  n_file;
    uint32_t n_data_pos;
    uint32_t n_undo_pos;
    uint32_t n_tx;
    uint32_t n_chain_tx;
    uint8_t  chain_work[32];
    uint32_t n_cached_branch_id;
    uint8_t  sapling_root[32];
};

static int cmp_height(const void *a, const void *b)
{
    const struct block_index *pa = *(const struct block_index *const *)a;
    const struct block_index *pb = *(const struct block_index *const *)b;
    if (pa->nHeight < pb->nHeight) return -1; // raw-return-ok:qsort-comparator
    if (pa->nHeight > pb->nHeight) return 1;
    return 0;
}

/* Forward pass over a height-sorted block_index array: recompute nChainWork,
 * nChainTx, skip links, cached branch id, and failed-child propagation from
 * each entry's already-linked pprev. Shared by the legacy LevelDB loader and
 * the event-log projection rebuild so both compute pointer-graph-derived
 * fields through one helper. Declared in services/block_index_loader.h. */
void block_index_forward_pass(struct block_index **sorted,
                              size_t count)
{
    for (size_t i = 0; i < count; i++) {
        struct block_index *pindex = sorted[i];

        struct arith_uint256 proof = GetBlockProof(pindex);
        if (pindex->pprev)
            arith_uint256_add(&pindex->nChainWork,
                              &pindex->pprev->nChainWork, &proof);
        else
            pindex->nChainWork = proof;

        if (pindex->nTx > 0) {
            if (pindex->pprev) {
                if (pindex->pprev->nChainTx)
                    pindex->nChainTx = pindex->pprev->nChainTx + pindex->nTx;
                else
                    pindex->nChainTx = 0;
            } else {
                pindex->nChainTx = pindex->nTx;
            }
        }

        block_index_build_skip(pindex);

        if (pindex->pprev) {
            if (block_index_is_valid(pindex, BLOCK_VALID_CONSENSUS) &&
                !pindex->nCachedBranchId)
                pindex->nCachedBranchId = pindex->pprev->nCachedBranchId;
        }

        if (!(pindex->nStatus & BLOCK_FAILED_MASK) && pindex->pprev &&
            (pindex->pprev->nStatus & BLOCK_FAILED_MASK))
            pindex->nStatus |= BLOCK_FAILED_CHILD;
    }
}

/* ── save_block_index_flat ───────────────────────────────── */

void save_block_index_flat(const char *datadir, struct main_state *ms)
{
    char path[1024];
    char tmp_path[1056];
    snprintf(path, sizeof(path), "%s/block_index.bin", datadir);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    (void)unlink(tmp_path);

    size_t count = ms->map_block_index.size;
    struct block_index **sorted = zcl_malloc(count * sizeof(void *), "block_index sorted save");
    if (!sorted) {
        fprintf(stderr, "save_block_index_flat: malloc failed for %zu entries\n",
                count);
        return;
    }

    size_t idx = 0, iter = 0;
    struct block_index *p;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (p && idx < count) sorted[idx++] = p;
    }
    count = idx;

    qsort(sorted, count, sizeof(struct block_index *), cmp_height);

    int64_t t0 = (int64_t)platform_time_wall_time_t();
    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        fprintf(stderr, "save_block_index_flat: cannot create %s: %s\n",
                tmp_path, strerror(errno));
        free(sorted); return;
    }

    uint32_t magic = 0x5A434C49; /* "ZCLI" */
    if (fwrite(&magic, 4, 1, f) != 1 || // disk-io-lock: private-fd (block index flat file)
        fwrite(&(uint32_t){(uint32_t)count}, 4, 1, f) != 1) {
        fprintf(stderr, "save_block_index_flat: header write failed\n");
        fclose(f); unlink(tmp_path); free(sorted); return;
    }

    for (size_t i = 0; i < count; i++) {
        struct block_index_flat entry;
        memset(&entry, 0, sizeof(entry));
        if (sorted[i]->phashBlock)
            memcpy(entry.hash, sorted[i]->phashBlock->data, 32);
        if (sorted[i]->pprev && sorted[i]->pprev->phashBlock)
            memcpy(entry.prev_hash, sorted[i]->pprev->phashBlock->data, 32);
        entry.height = sorted[i]->nHeight;
        entry.n_bits = sorted[i]->nBits;
        entry.n_time = sorted[i]->nTime;
        entry.n_version = sorted[i]->nVersion;
        entry.n_status = sorted[i]->nStatus;
        entry.n_file = sorted[i]->nFile;
        entry.n_data_pos = sorted[i]->nDataPos;
        entry.n_undo_pos = sorted[i]->nUndoPos;
        entry.n_tx = sorted[i]->nTx;
        entry.n_chain_tx = sorted[i]->nChainTx;
        memcpy(entry.chain_work, sorted[i]->nChainWork.pn, 32);
        entry.n_cached_branch_id = (uint32_t)sorted[i]->nCachedBranchId;
        memcpy(entry.sapling_root, sorted[i]->hashFinalSaplingRoot.data, 32);
        if (fwrite(&entry, sizeof(entry), 1, f) != 1) { // disk-io-lock: private-fd
            fprintf(stderr, "save_block_index_flat: write failed at entry "
                    "%zu/%zu: %s\n", i, count, strerror(errno));
            fclose(f); unlink(tmp_path); free(sorted); return;
        }
    }
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0)
        (void)fsync(fd);
    fclose(f);
    free(sorted);

    if (rename(tmp_path, path) != 0) {
        fprintf(stderr, "save_block_index_flat: rename %s -> %s failed: %s\n",
                tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return;
    }

    {
        struct zcl_result br = bii_write_sidecar(datadir);
        if (!br.ok) {
            LOG_WARN("save_block_index_flat", "save_block_index_flat: sidecar write failed for %s: %s", path, br.message);
        }
    }

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
    printf("Block index flat file: %zu entries, %zuMB (%llds)\n",
           count, count * sizeof(struct block_index_flat) / (1024*1024),
           (long long)elapsed);
}

/* ── load_block_index_flat ───────────────────────────────── */

bool load_block_index_flat(const char *datadir, struct main_state *ms)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/block_index.bin", datadir);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "block_index_flat: cannot open %s: %s\n",
                path, strerror(errno));
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "block_index_flat: fstat failed: %s\n", strerror(errno));
        close(fd); return false;
    }
    size_t file_size = (size_t)st.st_size;
    if (file_size < 8) {
        fprintf(stderr, "block_index_flat: file too small (%zu bytes)\n", file_size);
        close(fd); return false;
    }

    uint8_t *data = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) {
        fprintf(stderr, "block_index_flat: mmap failed (%zu bytes): %s\n",
                file_size, strerror(errno));
        return false;
    }

    uint32_t magic, count;
    memcpy(&magic, data, 4);
    memcpy(&count, data + 4, 4);
    if (magic != 0x5A434C49) {
        fprintf(stderr, "block_index_flat: bad magic 0x%08x (expected 0x5A434C49)\n",
                magic);
        munmap(data, file_size); return false;
    }
    if (count > 10000000) {
        fprintf(stderr, "block_index_flat: count %u too large (max 10M)\n", count);
        munmap(data, file_size); return false;
    }

    size_t expected = 8 + (size_t)count * sizeof(struct block_index_flat);
    if (file_size < expected) {
        fprintf(stderr, "block_index_flat: truncated — %zu bytes < %zu expected "
                "(%u entries)\n", file_size, expected, count);
        munmap(data, file_size); return false;
    }

    int64_t t0 = (int64_t)platform_time_wall_time_t();
    const struct block_index_flat *entries =
        (const struct block_index_flat *)(data + 8);

    /* Pre-size hash map + arena. Pre-fault memory. */
    block_map_reserve(&ms->map_block_index, count);
    struct block_index *arena = zcl_calloc(count, sizeof(struct block_index), "block_index arena");
    if (!arena) {
        fprintf(stderr, "block_index_flat: calloc failed for %u entries "
                "(%zu bytes)\n", count, (size_t)count * sizeof(struct block_index));
        munmap(data, file_size); return false;
    }
    memset(arena, 0, count * sizeof(struct block_index)); /* pre-fault */

    /* Height→index for O(1) pprev linking */
    int32_t max_h = entries[count - 1].height;
    struct block_index **by_height = NULL;
    if (max_h > 0 && max_h < 10000000) {
        by_height = zcl_calloc((size_t)(max_h + 1), sizeof(struct block_index *), "block_index by_height");
        if (!by_height)
            LOG_WARN("block_index_flat", "block_index_flat: by_height calloc failed " "(%d entries) — pprev linking will be slow", max_h + 1);
    }

    /* Bulk insert directly into the hash table; loader is single-threaded. */
    struct block_map *bm = &ms->map_block_index;
    for (uint32_t i = 0; i < count; i++) {
        if (uint256_is_null((const struct uint256 *)entries[i].hash))
            continue;

        struct block_index *pindex = &arena[i];
        block_index_init(pindex);

        uint64_t h;
        memcpy(&h, entries[i].hash, 8);
        size_t slot = h & (bm->capacity - 1);
        bool duplicate = false;
        while (bm->buckets[slot].occupied) {
            if (uint256_eq(&bm->buckets[slot].hash,
                           (const struct uint256 *)entries[i].hash)) {
                duplicate = true;
                break;
            }
            slot = (slot + 1) & (bm->capacity - 1);
        }
        if (duplicate) continue;
        memcpy(bm->buckets[slot].hash.data, entries[i].hash, 32);
        bm->buckets[slot].index = pindex;
        bm->buckets[slot].occupied = true;
        bm->size++;

        /* Option A: point phashBlock at per-node storage, not the bucket.
         * The bucket keeps its own .hash key (memcpy above) for lookups. */
        memcpy(pindex->hashBlock.data, entries[i].hash, 32);
        pindex->phashBlock = &pindex->hashBlock;
        pindex->nHeight = entries[i].height;
        pindex->nBits = entries[i].n_bits;
        pindex->nTime = entries[i].n_time;
        pindex->nVersion = entries[i].n_version;
        pindex->nStatus = entries[i].n_status;
        pindex->nFile = entries[i].n_file;
        pindex->nDataPos = entries[i].n_data_pos;
        pindex->nUndoPos = entries[i].n_undo_pos;
        pindex->nTx = entries[i].n_tx;
        pindex->nChainTx = entries[i].n_chain_tx;
        memcpy(pindex->nChainWork.pn, entries[i].chain_work, 32);
        pindex->nCachedBranchId = entries[i].n_cached_branch_id;
        memcpy(pindex->hashFinalSaplingRoot.data, entries[i].sapling_root, 32);

        if (by_height && pindex->nHeight >= 0 && pindex->nHeight <= max_h)
            by_height[pindex->nHeight] = pindex;
    }

    /* Link pprev via prev_hash lookup (handles orphans correctly).
     * Height-based linking breaks when orphan blocks at the same height
     * overwrite the main chain entry in by_height[], causing the pprev
     * chain to follow orphan forks instead of the main chain. */
    for (uint32_t i = 0; i < count; i++) {
        struct block_index *pindex = &arena[i];
        if (pindex->nHeight > 0) {
            /* Look up prev_hash in block_map */
            struct uint256 prev;
            memcpy(prev.data, entries[i].prev_hash, 32);
            struct block_index *pp = block_map_find(bm, &prev);
            if (pp)
                pindex->pprev = pp;
            else if (by_height && pindex->nHeight - 1 >= 0 &&
                     pindex->nHeight - 1 <= max_h) {
                pindex->pprev = by_height[pindex->nHeight - 1]; /* fallback */
                printf("WARNING: pprev for height %d resolved via height fallback "
                       "(prev_hash not found in block_map)\n", pindex->nHeight);
            }
        }
    }
    free(by_height);

    /* Recompute nChainWork and nChainTx from pprev chain.
     * The flat file may have stale values for blocks that were connected
     * via P2P after the LevelDB post-load but before the file was saved.
     * These blocks can have truncated chain_work (only low 32 bits set)
     * because an earlier validation pass computed them with a pprev that had
     * wrong state. Recomputing from the sorted array with correct pprev fixes
     * this. */
    {
        int fixed_work = 0, fixed_tx = 0;
        for (uint32_t i = 0; i < count; i++) {
            struct block_index *pi = &arena[i];
            if (!pi->pprev) continue;

            /* Recompute chain_work from pprev */
            struct arith_uint256 proof = GetBlockProof(pi);
            struct arith_uint256 expected;
            arith_uint256_add(&expected, &pi->pprev->nChainWork, &proof);
            if (arith_uint256_compare(&expected, &pi->nChainWork) != 0) {
                pi->nChainWork = expected;
                fixed_work++;
            }

            /* Fix nChainTx */
            if (pi->nTx > 0 && pi->pprev->nChainTx > 0) {
                uint32_t expected_ctx = pi->pprev->nChainTx + pi->nTx;
                if (pi->nChainTx != expected_ctx) {
                    pi->nChainTx = expected_ctx;
                    fixed_tx++;
                }
            }
        }
        if (fixed_work > 0 || fixed_tx > 0)
            printf("Block index flat: fixed %d chain_work, %d chain_tx\n",
                   fixed_work, fixed_tx);
    }

    munmap(data, file_size);

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
    printf("Block index flat: loaded %u entries in %llds\n",
           count, (long long)elapsed);

    return count > 0;
}

/* ── save_block_index_recent ─────────────────────────────── */

void save_block_index_recent(struct node_db *ndb, struct main_state *ms)
{
    if (!ndb || !ndb->open) return;

    size_t total = ms->map_block_index.size;
    if (total == 0) return;

    int64_t t0 = (int64_t)platform_time_wall_time_t();
    bool tx_open = false;
    int exec_rc = sqlite3_exec(ndb->db, "DELETE FROM block_index_cache",
                               NULL, NULL, NULL);
    if (exec_rc != SQLITE_OK) {
        fprintf(stderr, "boot-index: failed to clear block_index_cache: %s\n",
                sqlite3_errmsg(ndb->db));
        return;
    }
    exec_rc = sqlite3_exec(ndb->db, "BEGIN", NULL, NULL, NULL);
    if (exec_rc != SQLITE_OK) {
        fprintf(stderr, "boot-index: failed to begin block_index_cache save: %s\n",
                sqlite3_errmsg(ndb->db));
        return;
    }
    tx_open = true;

    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO block_index_cache "
        "(hash,prev_hash,height,n_bits,n_time,n_version,n_status,"
        "n_file,n_data_pos,n_undo_pos,n_tx,chain_work,"
        "n_cached_branch_id,n_chain_tx) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        -1, &ins, NULL) != SQLITE_OK || !ins) {
        fprintf(stderr, "boot-index: failed to prepare block_index_cache insert: %s\n",
                sqlite3_errmsg(ndb->db));
        sqlite3_exec(ndb->db, "ROLLBACK", NULL, NULL, NULL);
        return;
    }

    static const unsigned char zero32[32] = {0};
    size_t iter = 0, count = 0;
    struct block_index *p;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (!p || !p->phashBlock) continue;

        sqlite3_reset(ins);
        if (sqlite3_bind_blob(ins, 1, p->phashBlock->data, 32, SQLITE_STATIC) != SQLITE_OK)
            goto fail;
        const unsigned char *prev = (p->pprev && p->pprev->phashBlock)
            ? p->pprev->phashBlock->data : zero32;
        if (sqlite3_bind_blob(ins, 2, prev, 32, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_int(ins, 3, p->nHeight) != SQLITE_OK ||
            sqlite3_bind_int(ins, 4, (int)p->nBits) != SQLITE_OK ||
            sqlite3_bind_int(ins, 5, (int)p->nTime) != SQLITE_OK ||
            sqlite3_bind_int(ins, 6, p->nVersion) != SQLITE_OK ||
            sqlite3_bind_int(ins, 7, (int)p->nStatus) != SQLITE_OK ||
            sqlite3_bind_int(ins, 8, p->nFile) != SQLITE_OK ||
            sqlite3_bind_int(ins, 9, (int)p->nDataPos) != SQLITE_OK ||
            sqlite3_bind_int(ins, 10, (int)p->nUndoPos) != SQLITE_OK ||
            sqlite3_bind_int(ins, 11, (int)p->nTx) != SQLITE_OK ||
            sqlite3_bind_blob(ins, 12, p->nChainWork.pn, 32, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_int(ins, 13, (int)p->nCachedBranchId) != SQLITE_OK ||
            sqlite3_bind_int(ins, 14, (int)p->nChainTx) != SQLITE_OK ||
            AR_STEP_ROW_READONLY(ins) != SQLITE_DONE)
            goto fail;
        count++;

        if (count % 50000 == 0) {
            if (sqlite3_exec(ndb->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
                goto fail;
            tx_open = false;
            if (sqlite3_exec(ndb->db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK)
                goto fail;
            tx_open = true;
        }
    }
    sqlite3_finalize(ins);
    ins = NULL;
    if (sqlite3_exec(ndb->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "boot-index: failed to commit block_index_cache save: %s\n",
                sqlite3_errmsg(ndb->db));
        sqlite3_exec(ndb->db, "ROLLBACK", NULL, NULL, NULL);
        return;
    }

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
    printf("Block index: cached %zu/%zu entries in SQLite (%llds)\n",
           count, total, (long long)elapsed);
    return;

fail:
    LOG_WARN("chain", "boot-index: block_index_cache save aborted: %s", sqlite3_errmsg(ndb->db));
    if (ins)
        sqlite3_finalize(ins);
    if (tx_open)
        sqlite3_exec(ndb->db, "ROLLBACK", NULL, NULL, NULL);
}

/* ── load_block_index_sqlite ─────────────────────────────── */

bool load_block_index_sqlite(struct node_db *ndb, struct main_state *ms)
{
    if (!ndb || !ndb->open) LOG_FAIL("block_index", "load_block_index_sqlite called with null or closed db");

    int64_t cached_count = 0;
    sqlite3_stmt *cnt = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT COUNT(*) FROM block_index_cache", -1, &cnt, NULL) == SQLITE_OK && cnt) {
        if (AR_STEP_ROW_READONLY(cnt) == SQLITE_ROW)
            cached_count = sqlite3_column_int64(cnt, 0);
        sqlite3_finalize(cnt);
    }
    if (cached_count < 1000) LOG_FAIL("block_index", "SQLite block_index_cache too small: %lld entries", (long long)cached_count);

    int64_t t0 = (int64_t)platform_time_wall_time_t();
    printf("Loading block index from SQLite (%lld entries)...\n",
           (long long)cached_count);

    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT hash,prev_hash,height,n_bits,n_time,n_version,n_status,"
            "n_file,n_data_pos,n_undo_pos,n_tx,chain_work,"
            "n_cached_branch_id,n_chain_tx "
            "FROM block_index_cache ORDER BY height",
            -1, &sel, NULL) != SQLITE_OK || !sel)
        LOG_FAIL("block_index", "failed to prepare SQLite SELECT for block_index_cache");

    size_t loaded = 0;
    while (AR_STEP_ROW_READONLY(sel) == SQLITE_ROW) {
        const void *hash_blob = sqlite3_column_blob(sel, 0);
        if (!hash_blob || sqlite3_column_bytes(sel, 0) < 32) continue;

        struct uint256 hash;
        memcpy(hash.data, hash_blob, 32);
        struct block_index *pindex = chainstate_insert_block_index(
            (struct chainstate *)ms, &hash);
        if (!pindex) continue;

        pindex->nHeight         = sqlite3_column_int(sel, 2);
        pindex->nBits           = (uint32_t)sqlite3_column_int(sel, 3);
        pindex->nTime           = (uint32_t)sqlite3_column_int(sel, 4);
        pindex->nVersion        = sqlite3_column_int(sel, 5);
        pindex->nStatus         = (uint32_t)sqlite3_column_int(sel, 6);
        pindex->nFile           = sqlite3_column_int(sel, 7);
        pindex->nDataPos        = (uint32_t)sqlite3_column_int(sel, 8);
        pindex->nUndoPos        = (uint32_t)sqlite3_column_int(sel, 9);
        pindex->nTx             = (uint32_t)sqlite3_column_int(sel, 10);

        const void *cw = sqlite3_column_blob(sel, 11);
        if (cw && sqlite3_column_bytes(sel, 11) >= 32)
            memcpy(pindex->nChainWork.pn, cw, 32);

        pindex->nCachedBranchId = (uint32_t)sqlite3_column_int(sel, 12);
        pindex->nChainTx        = (uint32_t)sqlite3_column_int(sel, 13);
        loaded++;
    }
    sqlite3_finalize(sel);

    /* Link pprev pointers after every cached index entry is loaded. */
    sel = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT hash,prev_hash FROM block_index_cache "
            "WHERE prev_hash != X'0000000000000000000000000000000000000000000000000000000000000000'",
            -1, &sel, NULL) == SQLITE_OK && sel) {
        while (AR_STEP_ROW_READONLY(sel) == SQLITE_ROW) {
            const void *h = sqlite3_column_blob(sel, 0);
            const void *ph = sqlite3_column_blob(sel, 1);
            if (!h || !ph) continue;
            if (sqlite3_column_bytes(sel, 0) < 32 ||
                sqlite3_column_bytes(sel, 1) < 32) continue;

            struct uint256 hash, prev;
            memcpy(hash.data, h, 32);
            memcpy(prev.data, ph, 32);

            struct block_index *pindex = block_map_find(&ms->map_block_index, &hash);
            struct block_index *pprev = block_map_find(&ms->map_block_index, &prev);
            if (pindex && pprev)
                pindex->pprev = pprev;
        }
        sqlite3_finalize(sel);
    }

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
    printf("Block index SQLite: loaded %zu entries in %llds\n",
           loaded, (long long)elapsed);

    return loaded > 0;
}

/* ── load_block_index (LevelDB + post-process) ──────────── */

static struct block_index *insert_block_index_cb(void *ctx_ptr,
                                                  const struct uint256 *hash)
{
    struct main_state *ms = (struct main_state *)ctx_ptr;
    return chainstate_insert_block_index(
        (struct chainstate *)ms, hash);
}

struct zcl_result load_block_index(struct main_state *ms,
                       const struct chain_params *params,
                       struct block_tree_db *btdb, bool btdb_open)
{
    if (btdb_open) {
        if (!block_tree_db_load_block_index_guts(btdb,
                                                  insert_block_index_cb, ms))
            return ZCL_ERR(-1, "load_block_index: LevelDB block-tree "
                           "deserialization failed (corrupt block tree db)");
    }

    /* Option A: ensure every node owns its hash in per-node storage and
     * phashBlock references it (not the reallocatable bucket array).
     * The inner loader loop and chainstate_insert_block_index already do
     * this at insert; this pass is a belt-and-suspenders re-seed that is
     * also safe under concurrent grow (it writes per-node storage, never
     * re-points into buckets). */
    {
        size_t iter = 0;
        struct block_index *pi;
        const struct uint256 *hash;
        while (block_map_next(&ms->map_block_index, &iter, &hash, &pi)) {
            if (pi && hash) {
                pi->hashBlock = *hash;
                pi->phashBlock = &pi->hashBlock;
            }
        }
    }

    if (ms->map_block_index.size == 0) {
        struct block_index *genesis = chainstate_insert_block_index(
            (struct chainstate *)ms,
            &params->consensus.hashGenesisBlock);
        if (genesis) {
            genesis->nHeight = 0;
            genesis->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
            genesis->nTx = 1;
            genesis->nChainTx = 1;
            genesis->nBits = 0x1f07ffff;
            genesis->nChainWork = GetBlockProof(genesis);
            struct chain_state_rollback_authorization rollback_auth = {
                .source = CSR_ROLLBACK_SOURCE_RESTORE,
                .decision = POLICY_ALLOW,
                .from_height = active_chain_height(&ms->chain_active),
                .to_height = genesis->nHeight,
                .max_depth = INT64_MAX,
                .evidence_class = "block_index_loader_genesis_verified",
                .reason = "loader_init_genesis",
            };
            struct chain_state_commit commit = {
                .new_tip = genesis,
                .new_coins_best = *genesis->phashBlock,
                .expected_utxo_count = 0,
                .update_header_tip = true,
                .rollback_auth = &rollback_auth,
                .wallet_scan_height = -1,
                .reason = "loader_init_genesis",
            };
            enum csr_result rc = csr_commit_tip(csr_instance(), &commit);
            if (rc == CSR_OK) {
                return ZCL_OK;
            }
#ifdef ZCL_TESTING
            if (rc == CSR_REJECTED_NOT_INITIALIZED) {
                (void)chain_set_active_tip(ms, genesis, TIP_FROM_RESTORE,
                                      "loader_init_genesis_csr_uninit");
                ms->pindex_best_header = genesis;
                return ZCL_OK;
            }
#endif
            return ZCL_ERR(-3, "load_block_index: csr rejected genesis tip "
                           "commit (%s)", csr_result_name(rc));
        }
        return ZCL_OK;
    }

    /* Post-load: compute nChainWork, nChainTx, skip links */
    size_t count = ms->map_block_index.size;
    struct block_index **sorted = zcl_malloc(count * sizeof(struct block_index *), "block_index sorted load");
    if (!sorted)
        return ZCL_ERR(-2, "load_block_index: out of memory allocating %zu "
                       "sorted block_index pointers", count);

    size_t idx = 0;
    size_t iter = 0;
    struct block_index *pindex;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &pindex)) {
        if (pindex && idx < count)
            sorted[idx++] = pindex;
    }
    count = idx;

    qsort(sorted, count, sizeof(struct block_index *), cmp_height);

    block_index_forward_pass(sorted, count);

    free(sorted);
    return ZCL_OK;
}
