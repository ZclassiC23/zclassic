/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * UTXO Recovery Backfill Service
 *
 * Rehydrates per-block Sprout/Sapling value columns from the in-memory block
 * index and, when needed, block files on disk. This service owns shielded-value
 * backfill for UTXO recovery.
 */

#include "services/utxo_recovery_service.h"
#include "config/db_service.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "models/database.h"
#include "storage/disk_block_io.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/result.h"
#include <stdio.h>
#include <sqlite3.h>

struct shielded_backfill_ctx {
    int updated;
    struct main_state *state;
    const char *datadir;
};

static struct zcl_result backfill_validate_args(
    struct node_db *ndb,
    struct db_service *dbsvc,
    struct main_state *state)
{
    if (!state)
        return ZCL_ERR(-50, "backfill: invalid state=%p", (void *)state);
    if (!dbsvc && (!ndb || !ndb->open))
        return ZCL_ERR(-51, "backfill: invalid ndb=%p open=%d",
                       (void *)ndb, ndb ? ndb->open : 0);
    return ZCL_OK;
}

static bool backfill_shielded_write(struct node_db *ndb, void *ctx_ptr)
{
    struct shielded_backfill_ctx *bctx = ctx_ptr;
    if (!ndb || !ndb->open)
        LOG_FAIL("utxo_recovery",
                 "backfill_shielded called with null or closed db");

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT OR REPLACE INTO blocks"
        "(hash,height,prev_hash,version,merkle_root,"
        "time,bits,nonce,solution,chain_work,status,"
        "file_num,data_pos,undo_pos,num_tx,"
        "sapling_root,sprout_root,sapling_value,sprout_value)"
        " VALUES(?,?,?,?,?,?,?,?,X'',X'',?,?,?,0,?,NULL,NULL,?,?)";
    if (sqlite3_prepare_v2(ndb->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "backfill: prepare failed: %s\n",
                sqlite3_errmsg(ndb->db));
        return false;
    }

    int updated = 0, batch = 0;
    node_db_begin(ndb);

    size_t iter = 0;
    struct block_index *bi;
    while (block_map_next(&bctx->state->map_block_index, &iter, NULL, &bi)) {
        if (!bi) continue;
        if (bi->nFile < 0 || !(bi->nStatus & BLOCK_HAVE_DATA)) continue;
        if (bi->nDataPos == 0 && bi->nHeight > 0) continue;

        int64_t sprout_val = bi->nSproutValue;
        int64_t sapling_val = bi->nSaplingValue;

        if (sprout_val == 0 && sapling_val == 0) {
            struct block blk;
            if (!read_block_from_disk_index(&blk, bi, bctx->datadir))
                continue;
            for (size_t i = 0; i < blk.num_vtx; i++) {
                const struct transaction *tx = &blk.vtx[i];
                for (size_t j = 0; j < tx->num_joinsplit; j++) {
                    sprout_val += tx->v_joinsplit[j].vpub_old;
                    sprout_val -= tx->v_joinsplit[j].vpub_new;
                }
                sapling_val += tx->value_balance;
            }
            block_free(&blk);
            if (sprout_val == 0 && sapling_val == 0) continue;
            bi->nSproutValue = sprout_val;
            bi->nSaplingValue = sapling_val;
        }

        sqlite3_reset(stmt);
        sqlite3_bind_blob(stmt, 1, bi->phashBlock->data, 32, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, bi->nHeight);
        sqlite3_bind_blob(stmt, 3,
            bi->pprev ? bi->pprev->phashBlock->data : (const uint8_t[32]){0},
            32, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 4, bi->nVersion);
        sqlite3_bind_blob(stmt, 5, bi->hashMerkleRoot.data, 32,
                          SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 6, bi->nTime);
        sqlite3_bind_int(stmt, 7, (int)bi->nBits);
        sqlite3_bind_blob(stmt, 8, bi->nNonce.data, 32, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 9, bi->nStatus);
        sqlite3_bind_int(stmt, 10, bi->nFile);
        sqlite3_bind_int(stmt, 11, (int)bi->nDataPos);
        sqlite3_bind_int(stmt, 12, bi->nTx);
        sqlite3_bind_int64(stmt, 13, sapling_val);
        sqlite3_bind_int64(stmt, 14, sprout_val);

        if (AR_STEP_WRITE(stmt) != SQLITE_DONE) {
            static int errs = 0;
            if (++errs <= 3)
                LOG_INFO("chain", "backfill h=%d: %s", bi->nHeight,
                         sqlite3_errmsg(ndb->db));
        } else {
            updated++;
        }

        if (++batch >= 5000) {
            node_db_commit(ndb);
            node_db_begin(ndb);
            batch = 0;
            printf("  backfill: %d blocks so far...\n", updated);
            fflush(stdout);
        }
    }

    node_db_commit(ndb);
    sqlite3_finalize(stmt);
    node_db_state_set_int(ndb, "shielded_backfilled", 1);

    bctx->updated = updated;
    printf("Shielded backfill complete: %d blocks with "
           "JoinSplit/Sapling data\n", updated);
    fflush(stdout);
    return true;
}

struct zcl_result utxo_recovery_backfill_shielded(struct node_db *ndb,
                                     struct db_service *dbsvc,
                                     struct main_state *state,
                                     const char *datadir,
                                     int *out_updated)
{
    struct zcl_result valid = backfill_validate_args(ndb, dbsvc, state);
    if (!valid.ok)
        return valid;  /* zcl_result self-describes; the caller logs the reason */

    struct shielded_backfill_ctx bctx = {
        .updated = 0,
        .state = state,
        .datadir = datadir,
    };
    bool ok = false;

    printf("Backfilling shielded values from block_index...\n");
    if (dbsvc)
        ok = db_service_run_write(dbsvc, backfill_shielded_write, &bctx);
    else
        ok = backfill_shielded_write(ndb, &bctx);

    if (ok) {
        printf("Backfill: updated %d blocks with shielded values\n",
               bctx.updated);
        fflush(stdout);
        if (out_updated)
            *out_updated = bctx.updated;
        return ZCL_OK;
    }

    return ZCL_ERR(-52, "backfill: failed to persist shielded values "
                   "to blocks table");
}
