/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "storage/coins_db.h"
#include "coins/undo.h"
#include "core/serialize.h"
#include "util/log_macros.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char DB_COINS = 'c';
static const char DB_BEST_BLOCK = 'B';

static void make_coins_key(char *buf, size_t *len,
                            const struct uint256 *txid)
{
    buf[0] = DB_COINS;
    memcpy(buf + 1, txid->data, 32);
    *len = 33;
}

static bool cvdb_get_coins_impl(void *self, const struct uint256 *txid,
                                struct coins *out)
{
    struct coins_view_db *cvdb = (struct coins_view_db *)self;
    return coins_view_db_get_coins(cvdb, txid, out);
}

static bool cvdb_have_coins_impl(void *self, const struct uint256 *txid)
{
    struct coins_view_db *cvdb = (struct coins_view_db *)self;
    return coins_view_db_have_coins(cvdb, txid);
}

static bool cvdb_get_best_block_impl(void *self, struct uint256 *hash)
{
    struct coins_view_db *cvdb = (struct coins_view_db *)self;
    return coins_view_db_get_best_block(cvdb, hash);
}

static bool cvdb_batch_write_impl(void *self, struct coins_map *map_coins,
                                   const struct uint256 *hash_block)
{
    struct coins_view_db *cvdb = (struct coins_view_db *)self;
    return coins_view_db_batch_write(cvdb, map_coins, hash_block);
}

static struct coins_view_vtable cvdb_vtable = {
    .get_coins = cvdb_get_coins_impl,
    .have_coins = cvdb_have_coins_impl,
    .get_best_block = cvdb_get_best_block_impl,
    .batch_write = cvdb_batch_write_impl,
    .get_stats = NULL,
};

bool coins_view_db_open(struct coins_view_db *cvdb, const char *path,
                        size_t cache_size, bool memory, bool wipe)
{
    if (!db_wrapper_open(&cvdb->db, path, cache_size, memory, wipe))
        return false;
    cvdb->view.vtable = &cvdb_vtable;
    cvdb->view.impl = cvdb;
    return true;
}

void coins_view_db_close(struct coins_view_db *cvdb)
{
    db_wrapper_close(&cvdb->db);
}

bool coins_view_db_get_coins(struct coins_view_db *cvdb,
                             const struct uint256 *txid,
                             struct coins *out)
{
    char key[64];
    size_t keylen;
    make_coins_key(key, &keylen, txid);

    char *val = NULL;
    size_t vallen = 0;
    if (!db_read(&cvdb->db, key, keylen, &val, &vallen))
        return false;

    struct byte_stream s;
    stream_init_from_data(&s, (unsigned char *)val, vallen);

    uint64_t nVersion = 0;
    if (!stream_read_varint(&s, &nVersion)) {
        stream_free(&s); free(val);
        LOG_FAIL("coins_db", "get_coins: read nVersion varint failed");
    }
    out->version = (int)nVersion;

    uint64_t nCode = 0;
    if (!stream_read_varint(&s, &nCode)) {
        stream_free(&s); free(val);
        LOG_FAIL("coins_db", "get_coins: read nCode varint failed");
    }
    out->is_coinbase = (nCode & 1) != 0;
    bool vout0_present = (nCode & 2) != 0;
    bool vout1_present = (nCode & 4) != 0;
    unsigned int nMaskCode = (unsigned int)(nCode / 8) +
        ((vout0_present || vout1_present) ? 0 : 1);

    if (nMaskCode > 10000) {
        stream_free(&s); free(val);
        LOG_FAIL("coins_db", "get_coins: nMaskCode %u exceeds limit", nMaskCode);
    }

    /* Build availability vector: vAvail[0..1] from flags, rest from mask bytes.
     *
     * This decode shares the CCoins on-disk mask format with
     * chainstate_legacy_reader.c, but the two MUST NOT be folded into one
     * helper: this live read path is hardened (nMaskCode > 10000 reject above,
     * plus the fixed 4096-vout bound below) so a malformed node.db row cannot
     * amplify memory; the legacy reader grows its buffer unbounded by design
     * for trusted external-chainstate import. Unifying them would either strip
     * this DoS bound or impose truncation on the import path. Keep them
     * separate. */
    size_t num_avail = 2;
    bool avail_stack[4096];
    avail_stack[0] = vout0_present;
    avail_stack[1] = vout1_present;

    unsigned int mask_remaining = nMaskCode;
    while (mask_remaining > 0) {
        unsigned char ch = 0;
        if (!stream_read_bytes(&s, &ch, 1)) {
            stream_free(&s); free(val);
            LOG_FAIL("coins_db", "get_coins: read mask byte failed");
        }
        for (unsigned int p = 0; p < 8 && num_avail < 4096; p++)
            avail_stack[num_avail++] = (ch & (1 << p)) != 0;
        if (ch != 0)
            mask_remaining--;
    }

    if (!coins_alloc(out, num_avail)) {
        stream_free(&s); free(val);
        LOG_FAIL("coins_db",
                 "get_coins: coins_alloc failed for %zu vouts", num_avail);
    }
    for (size_t i = 0; i < num_avail; i++) {
        if (avail_stack[i]) {
            if (!compressed_txout_deserialize(&out->vout[i], &s)) {
                stream_free(&s); free(val);
                LOG_FAIL("coins_db", "get_coins: compressed txout deserialize failed at vout %zu", i);
            }
        }
    }

    uint64_t h = 0;
    if (!stream_read_varint(&s, &h)) {
        stream_free(&s); free(val);
        LOG_FAIL("coins_db", "get_coins: read height varint failed");
    }
    out->height = (int)h;

    stream_free(&s);
    free(val);
    coins_cleanup(out);
    return true;
}

bool coins_view_db_have_coins(struct coins_view_db *cvdb,
                              const struct uint256 *txid)
{
    char key[64];
    size_t keylen;
    make_coins_key(key, &keylen, txid);
    return db_exists(&cvdb->db, key, keylen);
}

bool coins_view_db_get_best_block(struct coins_view_db *cvdb,
                                  struct uint256 *hash)
{
    char key = DB_BEST_BLOCK;
    char *val = NULL;
    size_t vallen = 0;
    if (!db_read(&cvdb->db, &key, 1, &val, &vallen))
        return false;
    if (vallen >= 32)
        memcpy(hash->data, val, 32);
    else
        uint256_set_null(hash);
    free(val);
    return true;
}

bool coins_view_db_batch_write(struct coins_view_db *cvdb,
                               struct coins_map *map_coins,
                               const struct uint256 *hash_block)
{
    struct db_batch batch;
    db_batch_init(&batch);

    for (size_t i = 0; i < map_coins->num_buckets; i++) {
        struct coins_map_entry *e = &map_coins->buckets[i];
        if (!e->occupied || !(e->entry.flags & COINS_CACHE_DIRTY))
            continue;

        char key[64];
        size_t keylen;
        make_coins_key(key, &keylen, &e->txid);

        if (coins_is_pruned(&e->entry.coins)) {
            db_batch_delete(&batch, key, keylen);
        } else {
                struct byte_stream s;
                stream_init(&s, 256);
                const struct coins *cc = &e->entry.coins;

                stream_write_varint(&s, (uint64_t)cc->version);

                bool vout0 = cc->num_vout > 0 && !tx_out_is_null(&cc->vout[0]);
                bool vout1 = cc->num_vout > 1 && !tx_out_is_null(&cc->vout[1]);

                /* Compute mask per C++ CCoins::CalcMaskSize:
                 * nMaskSize = total bytes up to last non-zero byte
                 * nMaskCode = count of non-zero bytes (used in nCode) */
                unsigned int nMaskSize = 0;
                unsigned int nMaskCode = 0;
                for (size_t vi = 2; vi < cc->num_vout; vi++) {
                    if (!tx_out_is_null(&cc->vout[vi])) {
                        unsigned int byte_pos = (unsigned int)((vi - 2) / 8) + 1;
                        if (byte_pos > nMaskSize)
                            nMaskSize = byte_pos;
                    }
                }
                /* Count non-zero mask bytes */
                for (unsigned int mi = 0; mi < nMaskSize; mi++) {
                    unsigned char ch = 0;
                    for (unsigned int p = 0; p < 8; p++) {
                        size_t idx = 2 + mi * 8 + p;
                        if (idx < cc->num_vout && !tx_out_is_null(&cc->vout[idx]))
                            ch |= (1 << p);
                    }
                    if (ch != 0)
                        nMaskCode++;
                }

                uint64_t nCode = 8 * (nMaskCode - ((vout0 || vout1) ? 0 : 1))
                                 + (cc->is_coinbase ? 1 : 0)
                                 + (vout0 ? 2 : 0)
                                 + (vout1 ? 4 : 0);
                stream_write_varint(&s, nCode);

                /* Write spentness bitmask */
                for (unsigned int mi = 0; mi < nMaskSize; mi++) {
                    unsigned char ch = 0;
                    for (unsigned int p = 0; p < 8; p++) {
                        size_t idx = 2 + mi * 8 + p;
                        if (idx < cc->num_vout && !tx_out_is_null(&cc->vout[idx]))
                            ch |= (1 << p);
                    }
                    stream_write_bytes(&s, &ch, 1);
                }

                /* Write available outputs */
                for (size_t vi = 0; vi < cc->num_vout; vi++) {
                    if (!tx_out_is_null(&cc->vout[vi]))
                        compressed_txout_serialize(&cc->vout[vi], &s);
                }

                stream_write_varint(&s, (uint64_t)cc->height);

                db_batch_put(&batch, key, keylen, (char *)s.data, s.size);
                stream_free(&s);
            }
    }

    if (!uint256_is_null(hash_block)) {
        char key = DB_BEST_BLOCK;
        db_batch_put(&batch, &key, 1, (char *)hash_block->data, 32);
    }

    bool ok = db_write_batch(&cvdb->db, &batch, true);
    db_batch_free(&batch);
    return ok;
}

