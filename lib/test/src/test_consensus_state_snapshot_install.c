/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Atomic promotion tests for external zcl.consensus_state_bundle.v1 files. */

#include "test/test_helpers.h"

#include "coins/utxo_commitment.h"
#include "config/consensus_state_snapshot_install.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "crypto/sha3.h"
#include "sapling/incremental_merkle_tree.h"
#include "storage/anchor_kv.h"
#include "storage/coins_kv.h"
#include "storage/consensus_state_bundle_codec.h"
#include "storage/nullifier_kv.h"
#include "storage/progress_store.h"

#include <sqlite3.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CSI_CHECK(label, expr) do {                                      \
    printf("consensus_state_install: %s... ", (label));                  \
    if (expr) printf("OK\n");                                           \
    else { printf("FAIL\n"); failures++; }                              \
} while (0)

struct csi_coin {
    uint8_t txid[32]; uint32_t vout; int64_t value; int32_t height;
    bool coinbase; uint8_t script[3];
};
struct csi_anchor {
    int pool; uint8_t root[32]; int64_t height;
    struct byte_stream blob;
};
struct csi_nf { int pool; uint8_t nf[32]; int64_t height; };
struct csi_fixture {
    char path[512];
    int32_t height; uint8_t block_hash[32]; bool complete; int64_t boundary;
    struct csi_coin coins[2]; uint8_t utxo_root[32]; int64_t supply;
    struct csi_anchor anchors[2]; uint8_t anchor_digest[32];
    uint8_t frontier_root[2][32]; int64_t frontier_height[2];
    struct csi_nf nfs[2]; uint8_t nf_digest[32];
    int64_t sprout_cursor, sapling_cursor, nf_cursor, fold_cursor;
    struct consensus_state_source_receipt receipt;
    struct consensus_state_bundle_proof_summary
        proofs[CONSENSUS_STATE_BUNDLE_PROOF_COUNT];
    uint8_t proof[32], source[32], artifact[32];
};

static void fixture_proof_summaries(struct csi_fixture *f, uint8_t seed)
{
    static const char *const names[CONSENSUS_STATE_BUNDLE_PROOF_COUNT] = {
        "header_admit", "validate_headers", "body_fetch", "body_persist",
        "script_validate", "proof_validate", "utxo_apply", "tip_finalize",
    };
    static const bool hash_bound[CONSENSUS_STATE_BUNDLE_PROOF_COUNT] = {
        true, true, true, false, true, false, false, false,
    };
    for (size_t i = 0; i < CONSENSUS_STATE_BUNDLE_PROOF_COUNT; i++) {
        struct consensus_state_bundle_proof_summary *proof = &f->proofs[i];
        snprintf(proof->component, sizeof(proof->component), "%s", names[i]);
        proof->cursor = i == CONSENSUS_STATE_BUNDLE_PROOF_COUNT - 1
                            ? (uint64_t)f->height
                            : (uint64_t)f->height + 1;
        proof->first_height = 0;
        proof->last_height = f->height;
        proof->row_count = (uint64_t)f->height + 1;
        proof->hash_bound_count = hash_bound[i] ? proof->row_count : 0;
        for (size_t j = 0; j < 32; j++)
            proof->component_digest[j] =
                (uint8_t)(seed + 13u * i + j + 1u);
    }
    memcpy(f->proofs[0].component_digest,
           f->receipt.chain_corpus_digest, 32);
    consensus_state_bundle_proof_manifest_digest(
        f->proofs, CONSENSUS_STATE_BUNDLE_PROOF_COUNT, f->proof);
}

enum csi_fault {
    CSI_VALID = 0, CSI_WRONG_UTXO_ROOT, CSI_WRONG_SUPPLY,
    CSI_WRONG_ANCHOR_DIGEST, CSI_WRONG_NF_DIGEST, CSI_BAD_TREE_ROOT,
    CSI_BAD_COMPLETE_CURSOR, CSI_ZERO_PROOF, CSI_TEXT_ANCHOR_POOL,
    CSI_TEXT_NF_POOL, CSI_WRONG_FRONTIER_ROOT, CSI_WRONG_FRONTIER_HEIGHT,
    CSI_BAD_SOURCE_RECEIPT, CSI_EXTRA_SCHEMA_OBJECT, CSI_EXTRA_SCHEMA_COLUMN,
};

static bool bundle_codec_kat(void)
{
    static const uint8_t want_anchor[32] = {
        0xd3,0xfc,0xa6,0xc8,0x82,0xbb,0x5c,0x6b,
        0x19,0x41,0xd6,0xce,0x64,0x18,0xb5,0x8d,
        0x02,0x60,0x40,0xbb,0x4e,0x6b,0xb4,0xd0,
        0x2c,0x5b,0x01,0x55,0xd3,0x60,0x35,0x77,
    };
    static const uint8_t want_nf[32] = {
        0xc3,0x71,0x92,0x64,0xe8,0x75,0xde,0x6a,
        0xba,0x37,0x05,0x2a,0xeb,0xc9,0x8f,0x1e,
        0x5f,0x5b,0x71,0x18,0xd7,0xf4,0xd8,0x40,
        0x42,0x3b,0x6d,0x32,0xee,0x3a,0xc6,0x81,
    };
    static const uint8_t want_artifact[32] = {
        0x1a,0x5c,0x1b,0x31,0x50,0x0b,0x86,0x08,
        0x2d,0xf8,0x93,0x53,0x90,0x78,0x4d,0x37,
        0xa7,0x7c,0x6f,0xe1,0xf2,0xb0,0x20,0x80,
        0xec,0x51,0x15,0xad,0xf8,0x39,0xd6,0xc3,
    };
    static const uint8_t want_receipt[32] = {
        0xbb,0xd1,0xf6,0xfa,0xdc,0xa1,0x0c,0x7a,
        0xc9,0x72,0x70,0x2c,0x7f,0x8f,0x80,0xc0,
        0xe7,0x06,0x7b,0x4e,0xf3,0xcb,0xc1,0xca,
        0x43,0x15,0x47,0x52,0x1e,0xcb,0x18,0xf7,
    };
    static const uint8_t want_proof[32] = {
        0x86,0x5a,0x26,0x90,0xa7,0xe0,0x41,0xa8,
        0x87,0x8f,0xdf,0xeb,0xbf,0xcb,0x1c,0x01,
        0x02,0x1c,0xe8,0x52,0xe9,0x3b,0x14,0xf7,
        0x9b,0xff,0x2e,0x8d,0xa7,0x46,0x01,0xd5,
    };
    uint8_t root[32], nf[32], tree[3] = {0xaa,0xbb,0xcc}, out[32];
    for (size_t i = 0; i < 32; i++) {
        root[i] = (uint8_t)i;
        nf[i] = (uint8_t)(32u + i);
    }
    struct sha3_256_ctx c;
    consensus_state_bundle_anchor_digest_begin(&c);
    consensus_state_bundle_anchor_digest_row(&c, 1, root, 9, tree, 3);
    sha3_256_finalize(&c, out);
    if (memcmp(out, want_anchor, 32) != 0)
        return false;
    consensus_state_bundle_nullifier_digest_begin(&c);
    consensus_state_bundle_nullifier_digest_row(&c, 0, nf, 11);
    sha3_256_finalize(&c, out);
    if (memcmp(out, want_nf, 32) != 0)
        return false;

    struct consensus_state_source_receipt receipt;
    memset(&receipt, 0, sizeof(receipt));
    for (size_t i = 0; i < 32; i++) {
        receipt.source_tree_root[i] = (uint8_t)i;
        receipt.running_binary_digest[i] = (uint8_t)(32u + i);
        receipt.toolchain_digest[i] = (uint8_t)(64u + i);
        receipt.chain_corpus_digest[i] = (uint8_t)(96u + i);
    }
    snprintf(receipt.producer_commit, sizeof(receipt.producer_commit),
             "0123456789abcdef0123456789abcdef01234567");
    receipt.fold_cursor = 7;
    consensus_state_source_receipt_digest(&receipt, out);
    if (memcmp(out, want_receipt, 32) != 0)
        return false;
    struct consensus_state_bundle_proof_summary proof;
    memset(&proof, 0, sizeof(proof));
    snprintf(proof.component, sizeof(proof.component), "header_admit");
    proof.cursor = 8;
    proof.last_height = 7;
    proof.row_count = 8;
    proof.hash_bound_count = 8;
    for (size_t i = 0; i < 32; i++)
        proof.component_digest[i] = (uint8_t)i;
    consensus_state_bundle_proof_manifest_digest(&proof, 1, out);
    if (memcmp(out, want_proof, 32) != 0)
        return false;

    struct consensus_state_bundle_manifest m;
    memset(&m, 0, sizeof(m));
    m.height = 7;
    m.history_complete = true;
    m.utxo_count = 3;
    m.total_supply = 99;
    m.anchor_count = 4;
    m.sprout_frontier_height = 5;
    m.sapling_frontier_height = 6;
    m.nullifier_count = 5;
    m.source_fold_cursor = 8;
    for (size_t i = 0; i < 32; i++) {
        m.block_hash[i] = (uint8_t)i;
        m.utxo_root[i] = (uint8_t)(32u + i);
        m.anchor_digest[i] = (uint8_t)(64u + i);
        m.sprout_frontier_root[i] = (uint8_t)(96u + i);
        m.sapling_frontier_root[i] = (uint8_t)(128u + i);
        m.nullifier_digest[i] = (uint8_t)(160u + i);
        m.proof_manifest_digest[i] = (uint8_t)(192u + i);
        m.source_digest[i] = (uint8_t)(224u + i);
    }
    consensus_state_bundle_artifact_digest(&m, out);
    return memcmp(out, want_artifact, 32) == 0;
}

static void fixture_component_digests(struct csi_fixture *f)
{
    struct sha3_256_ctx c;
    sha3_256_init(&c);
    f->supply = 0;
    for (size_t i = 0; i < 2; i++) {
        struct csi_coin *coin = &f->coins[i];
        utxo_commitment_sha3_write_record(
            &c, coin->txid, coin->vout, coin->value, coin->script,
            sizeof(coin->script), (uint32_t)coin->height,
            coin->coinbase ? 1 : 0);
        f->supply += coin->value;
    }
    sha3_256_finalize(&c, f->utxo_root);

    consensus_state_bundle_anchor_digest_begin(&c);
    for (size_t i = 0; i < 2; i++) {
        consensus_state_bundle_anchor_digest_row(
            &c, (uint8_t)f->anchors[i].pool, f->anchors[i].root,
            (uint64_t)f->anchors[i].height, f->anchors[i].blob.data,
            (uint32_t)f->anchors[i].blob.size);
    }
    sha3_256_finalize(&c, f->anchor_digest);
    for (size_t i = 0; i < 2; i++) {
        memcpy(f->frontier_root[i], f->anchors[i].root, 32);
        f->frontier_height[i] = f->anchors[i].height;
    }

    consensus_state_bundle_nullifier_digest_begin(&c);
    for (size_t i = 0; i < 2; i++) {
        consensus_state_bundle_nullifier_digest_row(
            &c, (uint8_t)f->nfs[i].pool, f->nfs[i].nf,
            (uint64_t)f->nfs[i].height);
    }
    sha3_256_finalize(&c, f->nf_digest);
}

static void fixture_artifact_digest(struct csi_fixture *f)
{
    struct consensus_state_bundle_manifest m;
    memset(&m, 0, sizeof(m));
    m.height = f->height;
    memcpy(m.block_hash, f->block_hash, 32);
    m.history_complete = f->complete;
    m.activation_boundary = f->boundary;
    memcpy(m.utxo_root, f->utxo_root, 32);
    m.utxo_count = 2;
    m.total_supply = f->supply;
    memcpy(m.anchor_digest, f->anchor_digest, 32);
    m.anchor_count = 2;
    memcpy(m.sprout_frontier_root,
           f->frontier_root[ANCHOR_POOL_SPROUT], 32);
    m.sprout_frontier_height = f->frontier_height[ANCHOR_POOL_SPROUT];
    memcpy(m.sapling_frontier_root,
           f->frontier_root[ANCHOR_POOL_SAPLING], 32);
    m.sapling_frontier_height = f->frontier_height[ANCHOR_POOL_SAPLING];
    memcpy(m.nullifier_digest, f->nf_digest, 32);
    m.nullifier_count = 2;
    m.sprout_source_cursor = f->sprout_cursor;
    m.sapling_source_cursor = f->sapling_cursor;
    m.nullifier_source_cursor = f->nf_cursor;
    m.source_fold_cursor = f->fold_cursor;
    memcpy(m.proof_manifest_digest, f->proof, 32);
    memcpy(m.source_digest, f->source, 32);
    consensus_state_bundle_artifact_digest(&m, f->artifact);
}

static bool fixture_init(struct csi_fixture *f, const char *dir,
                         const char *name, uint8_t seed, int height,
                         bool complete)
{
    memset(f, 0, sizeof(*f));
    snprintf(f->path, sizeof(f->path), "%s/%s.bundle-v1.db", dir, name);
    f->height = height; f->complete = complete;
    f->boundary = complete ? 0 : height;
    f->sprout_cursor = f->sapling_cursor = f->nf_cursor = complete ? 0 : height;
    f->fold_cursor = height + 1;
    for (size_t i = 0; i < 32; i++) {
        f->block_hash[i] = (uint8_t)(seed + i + 1u);
        f->receipt.source_tree_root[i] = (uint8_t)(seed + i + 5u);
        f->receipt.running_binary_digest[i] = (uint8_t)(seed + i + 37u);
        f->receipt.toolchain_digest[i] = (uint8_t)(seed + i + 69u);
        f->receipt.chain_corpus_digest[i] = (uint8_t)(seed + i + 101u);
    }
    snprintf(f->receipt.producer_commit,
             sizeof(f->receipt.producer_commit),
             "0123456789abcdef0123456789abcdef01234567");
    f->receipt.fold_cursor = f->fold_cursor;
    consensus_state_source_receipt_digest(&f->receipt,
                                          f->receipt.receipt_digest);
    memcpy(f->source, f->receipt.receipt_digest, 32);
    fixture_proof_summaries(f, seed);
    for (size_t i = 0; i < 2; i++) {
        for (size_t j = 0; j < 32; j++)
            f->coins[i].txid[j] = (uint8_t)(seed + i * 64u + j);
        f->coins[i].vout = (uint32_t)i;
        f->coins[i].value = 1000 + seed + (int64_t)i;
        f->coins[i].height = height - (int)i;
        f->coins[i].coinbase = i == 0;
        f->coins[i].script[0] = 0x51;
        f->coins[i].script[1] = seed;
        f->coins[i].script[2] = (uint8_t)i;
    }
    struct incremental_merkle_tree trees[2];
    struct uint256 leaf, root;
    sprout_tree_init(&trees[0]); sapling_tree_init(&trees[1]);
    for (size_t j = 0; j < 32; j++)
        leaf.data[j] = (uint8_t)(seed ^ (uint8_t)(j * 3u + 7u));
    for (int pool = 0; pool <= 1; pool++) {
        stream_init(&f->anchors[pool].blob, 128);
        incremental_tree_append(&trees[pool], &leaf);
        if (!incremental_tree_serialize(&trees[pool],
                                        &f->anchors[pool].blob))
            return false;
        incremental_tree_root(&trees[pool], &root);
        f->anchors[pool].pool = pool;
        memcpy(f->anchors[pool].root, root.data, 32);
        f->anchors[pool].height = height;
        f->nfs[pool].pool = pool;
        f->nfs[pool].height = height - 2 + pool;
        for (size_t j = 0; j < 32; j++)
            f->nfs[pool].nf[j] = (uint8_t)(seed + pool * 48u + j + 9u);
    }
    fixture_component_digests(f);
    fixture_artifact_digest(f);
    return true;
}

static void fixture_free(struct csi_fixture *f)
{
    stream_free(&f->anchors[0].blob);
    stream_free(&f->anchors[1].blob);
    unlink(f->path);
}

static bool exec_ok(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    bool ok = sqlite3_exec(db, sql, NULL, NULL, &err) == SQLITE_OK;
    if (err) sqlite3_free(err);
    return ok;
}

static bool write_bundle(struct csi_fixture *f, enum csi_fault fault)
{
    chmod(f->path, 0600); unlink(f->path);
    fixture_component_digests(f);
    if (fault == CSI_WRONG_UTXO_ROOT) f->utxo_root[0] ^= 0xff;
    if (fault == CSI_WRONG_SUPPLY) f->supply++;
    if (fault == CSI_WRONG_ANCHOR_DIGEST) f->anchor_digest[0] ^= 0xff;
    if (fault == CSI_WRONG_NF_DIGEST) f->nf_digest[0] ^= 0xff;
    if (fault == CSI_BAD_TREE_ROOT) {
        f->anchors[0].root[0] ^= 1;
        fixture_component_digests(f); /* digest matches the bad declared root */
    }
    if (fault == CSI_BAD_COMPLETE_CURSOR) f->sprout_cursor = 1;
    if (fault == CSI_ZERO_PROOF) memset(f->proof, 0, 32);
    if (fault == CSI_WRONG_FRONTIER_ROOT)
        f->frontier_root[ANCHOR_POOL_SAPLING][0] ^= 1;
    if (fault == CSI_WRONG_FRONTIER_HEIGHT)
        f->frontier_height[ANCHOR_POOL_SPROUT]--;
    if (fault == CSI_BAD_SOURCE_RECEIPT)
        f->receipt.receipt_digest[0] ^= 1;
    fixture_artifact_digest(f);

    sqlite3 *db = NULL;
    bool ok = sqlite3_open(f->path, &db) == SQLITE_OK && exec_ok(db,
        "CREATE TABLE bundle_meta("
        "singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
        "schema TEXT NOT NULL,height INTEGER NOT NULL,block_hash BLOB NOT NULL,"
        "history_complete INTEGER NOT NULL,activation_boundary INTEGER NOT NULL,"
        "utxo_root BLOB NOT NULL,utxo_count INTEGER NOT NULL,"
        "total_supply INTEGER NOT NULL,anchor_digest BLOB NOT NULL,"
        "anchor_count INTEGER NOT NULL,sprout_frontier_root BLOB NOT NULL,"
        "sprout_frontier_height INTEGER NOT NULL,"
        "sapling_frontier_root BLOB NOT NULL,"
        "sapling_frontier_height INTEGER NOT NULL,"
        "nullifier_digest BLOB NOT NULL,nullifier_count INTEGER NOT NULL,"
        "sprout_source_cursor INTEGER NOT NULL,"
        "sapling_source_cursor INTEGER NOT NULL,"
        "nullifier_source_cursor INTEGER NOT NULL,"
        "source_fold_cursor INTEGER NOT NULL,"
        "proof_manifest_digest BLOB NOT NULL,source_digest BLOB NOT NULL,"
        "artifact_digest BLOB NOT NULL);"
        "CREATE TABLE source_receipt("
        "singleton INTEGER PRIMARY KEY CHECK(singleton=1),schema TEXT NOT NULL,"
        "source_tree_root BLOB NOT NULL,running_binary_digest BLOB NOT NULL,"
        "toolchain_digest BLOB NOT NULL,chain_corpus_digest BLOB NOT NULL,"
        "producer_commit TEXT NOT NULL,fold_cursor INTEGER NOT NULL,"
        "receipt_digest BLOB NOT NULL);"
        "CREATE TABLE bundle_proof("
        "ordinal INTEGER PRIMARY KEY,component TEXT NOT NULL UNIQUE,"
        "cursor INTEGER NOT NULL,first_height INTEGER NOT NULL,"
        "last_height INTEGER NOT NULL,row_count INTEGER NOT NULL,"
        "hash_bound_count INTEGER NOT NULL,component_digest BLOB NOT NULL);"
        "CREATE TABLE coins("
        "txid BLOB NOT NULL,vout INTEGER NOT NULL,value INTEGER NOT NULL,"
        "script BLOB NOT NULL,height INTEGER NOT NULL,is_coinbase INTEGER NOT NULL,"
        "PRIMARY KEY(txid,vout)) WITHOUT ROWID;"
        "CREATE TABLE anchors("
        "pool INTEGER NOT NULL CHECK(pool IN(0,1)),anchor BLOB NOT NULL,"
        "height INTEGER NOT NULL,tree BLOB NOT NULL,"
        "PRIMARY KEY(pool,anchor)) WITHOUT ROWID;"
        "CREATE TABLE nullifiers("
        "pool INTEGER NOT NULL CHECK(pool IN(0,1)),nf BLOB NOT NULL,"
        "height INTEGER NOT NULL,PRIMARY KEY(pool,nf)) WITHOUT ROWID;BEGIN");
    if (ok && (fault == CSI_TEXT_ANCHOR_POOL ||
               fault == CSI_TEXT_NF_POOL))
        ok = exec_ok(db, "PRAGMA ignore_check_constraints=ON");
    sqlite3_stmt *st = NULL;
    if (ok) ok = sqlite3_prepare_v2(db,
        "INSERT INTO coins VALUES(?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK;
    for (size_t i = 0; ok && i < 2; i++) {
        sqlite3_reset(st); sqlite3_clear_bindings(st);
        sqlite3_bind_blob(st,1,f->coins[i].txid,32,SQLITE_STATIC);
        sqlite3_bind_int64(st,2,f->coins[i].vout);
        sqlite3_bind_int64(st,3,f->coins[i].value);
        sqlite3_bind_blob(st,4,f->coins[i].script,3,SQLITE_STATIC);
        sqlite3_bind_int(st,5,f->coins[i].height);
        sqlite3_bind_int(st,6,f->coins[i].coinbase?1:0);
        ok = sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:test-fixture-seeding
    }
    if (st)
        sqlite3_finalize(st);
    st = NULL;
    if (ok) ok = sqlite3_prepare_v2(db,
        "INSERT INTO source_receipt VALUES(1,?,?,?,?,?,?,?,?)",
        -1,&st,NULL)==SQLITE_OK;
    if (ok) {
        int i=1;
        sqlite3_bind_text(st,i++,CONSENSUS_STATE_SOURCE_RECEIPT_SCHEMA,-1,
                          SQLITE_STATIC);
        sqlite3_bind_blob(st,i++,f->receipt.source_tree_root,32,SQLITE_STATIC);
        sqlite3_bind_blob(st,i++,f->receipt.running_binary_digest,32,
                          SQLITE_STATIC);
        sqlite3_bind_blob(st,i++,f->receipt.toolchain_digest,32,SQLITE_STATIC);
        sqlite3_bind_blob(st,i++,f->receipt.chain_corpus_digest,32,
                          SQLITE_STATIC);
        sqlite3_bind_text(st,i++,f->receipt.producer_commit,40,SQLITE_STATIC);
        sqlite3_bind_int64(st,i++,f->receipt.fold_cursor);
        sqlite3_bind_blob(st,i++,f->receipt.receipt_digest,32,SQLITE_STATIC);
        ok=sqlite3_step(st)==SQLITE_DONE; // raw-sql-ok:test-fixture-seeding
    }
    if (st) sqlite3_finalize(st);
    st = NULL;
    if (ok) ok = sqlite3_prepare_v2(db,
        "INSERT INTO bundle_proof VALUES(?,?,?,?,?,?,?,?)",
        -1,&st,NULL)==SQLITE_OK;
    for (size_t row=0;ok&&row<CONSENSUS_STATE_BUNDLE_PROOF_COUNT;row++) {
        struct consensus_state_bundle_proof_summary *p=&f->proofs[row];
        sqlite3_reset(st); sqlite3_clear_bindings(st);
        sqlite3_bind_int(st,1,(int)row);
        sqlite3_bind_text(st,2,p->component,-1,SQLITE_STATIC);
        sqlite3_bind_int64(st,3,(sqlite3_int64)p->cursor);
        sqlite3_bind_int64(st,4,p->first_height);
        sqlite3_bind_int64(st,5,p->last_height);
        sqlite3_bind_int64(st,6,(sqlite3_int64)p->row_count);
        sqlite3_bind_int64(st,7,(sqlite3_int64)p->hash_bound_count);
        sqlite3_bind_blob(st,8,p->component_digest,32,SQLITE_STATIC);
        ok=sqlite3_step(st)==SQLITE_DONE; // raw-sql-ok:test-fixture-seeding
    }
    if (st) sqlite3_finalize(st);
    st = NULL;
    if (ok) ok = sqlite3_prepare_v2(db,
        "INSERT INTO anchors VALUES(?,?,?,?)", -1, &st, NULL) == SQLITE_OK;
    for (size_t i = 0; ok && i < 2; i++) {
        sqlite3_reset(st); sqlite3_clear_bindings(st);
        if (fault == CSI_TEXT_ANCHOR_POOL && i == 0)
            sqlite3_bind_text(st,1,"junk",-1,SQLITE_STATIC);
        else
            sqlite3_bind_int(st,1,f->anchors[i].pool);
        sqlite3_bind_blob(st,2,f->anchors[i].root,32,SQLITE_STATIC);
        sqlite3_bind_int64(st,3,f->anchors[i].height);
        sqlite3_bind_blob(st,4,f->anchors[i].blob.data,
                          (int)f->anchors[i].blob.size,SQLITE_STATIC);
        ok = sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:test-fixture-seeding
    }
    if (st)
        sqlite3_finalize(st);
    st = NULL;
    if (ok) ok = sqlite3_prepare_v2(db,
        "INSERT INTO nullifiers VALUES(?,?,?)", -1, &st, NULL) == SQLITE_OK;
    for (size_t i = 0; ok && i < 2; i++) {
        sqlite3_reset(st); sqlite3_clear_bindings(st);
        if (fault == CSI_TEXT_NF_POOL && i == 0)
            sqlite3_bind_text(st,1,"junk",-1,SQLITE_STATIC);
        else
            sqlite3_bind_int(st,1,f->nfs[i].pool);
        sqlite3_bind_blob(st,2,f->nfs[i].nf,32,SQLITE_STATIC);
        sqlite3_bind_int64(st,3,f->nfs[i].height);
        ok = sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:test-fixture-seeding
    }
    if (st)
        sqlite3_finalize(st);
    st = NULL;
    if (ok) ok = sqlite3_prepare_v2(db,
        "INSERT INTO bundle_meta VALUES(1,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        -1,&st,NULL)==SQLITE_OK;
    if (ok) {
        int i=1;
        sqlite3_bind_text(st,i++,CONSENSUS_STATE_BUNDLE_SCHEMA,-1,SQLITE_STATIC);
        sqlite3_bind_int(st,i++,f->height); sqlite3_bind_blob(st,i++,f->block_hash,32,SQLITE_STATIC);
        sqlite3_bind_int(st,i++,f->complete?1:0); sqlite3_bind_int64(st,i++,f->boundary);
        sqlite3_bind_blob(st,i++,f->utxo_root,32,SQLITE_STATIC); sqlite3_bind_int64(st,i++,2);
        sqlite3_bind_int64(st,i++,f->supply); sqlite3_bind_blob(st,i++,f->anchor_digest,32,SQLITE_STATIC);
        sqlite3_bind_int64(st,i++,2);
        sqlite3_bind_blob(st,i++,f->frontier_root[ANCHOR_POOL_SPROUT],32,SQLITE_STATIC);
        sqlite3_bind_int64(st,i++,f->frontier_height[ANCHOR_POOL_SPROUT]);
        sqlite3_bind_blob(st,i++,f->frontier_root[ANCHOR_POOL_SAPLING],32,SQLITE_STATIC);
        sqlite3_bind_int64(st,i++,f->frontier_height[ANCHOR_POOL_SAPLING]);
        sqlite3_bind_blob(st,i++,f->nf_digest,32,SQLITE_STATIC);
        sqlite3_bind_int64(st,i++,2); sqlite3_bind_int64(st,i++,f->sprout_cursor);
        sqlite3_bind_int64(st,i++,f->sapling_cursor); sqlite3_bind_int64(st,i++,f->nf_cursor);
        sqlite3_bind_int64(st,i++,f->fold_cursor); sqlite3_bind_blob(st,i++,f->proof,32,SQLITE_STATIC);
        sqlite3_bind_blob(st,i++,f->source,32,SQLITE_STATIC); sqlite3_bind_blob(st,i++,f->artifact,32,SQLITE_STATIC);
        ok = sqlite3_step(st)==SQLITE_DONE; // raw-sql-ok:test-fixture-seeding
    }
    if (st) sqlite3_finalize(st);
    if (ok && fault == CSI_EXTRA_SCHEMA_OBJECT)
        ok = exec_ok(db, "CREATE TABLE unexpected(payload BLOB)");
    if (ok && fault == CSI_EXTRA_SCHEMA_COLUMN)
        ok = exec_ok(db, "ALTER TABLE bundle_meta ADD COLUMN unexpected BLOB");
    ok = ok && exec_ok(db, ok ? "COMMIT" : "ROLLBACK");
    if (db) sqlite3_close(db);
    return ok && chmod(f->path, 0400) == 0;
}

static bool active_is(sqlite3 *db, const struct csi_fixture *f)
{
    uint8_t root[32]; int64_t txs=0,outs=0,supply=0;
    if (coins_kv_count(db)!=2 || coins_kv_commitment(db,root)!=0 ||
        memcmp(root,f->utxo_root,32)!=0 ||
        !coins_kv_setinfo(db,&txs,&outs,&supply) || supply!=f->supply)
        return false;
    for(int pool=0;pool<=1;pool++) {
        struct uint256 ar; memcpy(ar.data,f->anchors[pool].root,32);
        int64_t h=-1,c=-1; bool found=false,nff=false;
        if(anchor_kv_get(db,pool,&ar,NULL,&h)!=ANCHOR_KV_FOUND || h!=f->height ||
           !anchor_kv_activation_cursor(db,pool,&c,&found)||!found||c!=f->boundary||
           !nullifier_kv_get(db,f->nfs[pool].nf,pool,&nff,&h)||!nff||h!=f->nfs[pool].height)
            return false;
    }
    int64_t nc=-1; bool ncf=false;
    return nullifier_kv_activation_cursor(db,&nc,&ncf)&&ncf&&nc==f->boundary;
}

static bool seed_active(sqlite3 *db, const struct csi_fixture *f)
{
    if (!coins_kv_ensure_schema(db) || !progress_meta_table_ensure(db) ||
        !anchor_kv_ensure_schema(db) || !nullifier_kv_ensure_schema(db) ||
        !anchor_kv_initialize_history(db, f->boundary) ||
        !nullifier_kv_initialize_history(db, f->boundary))
        return false;
    for (size_t i = 0; i < 2; i++) {
        if (!coins_kv_add(db, f->coins[i].txid, f->coins[i].vout,
                          f->coins[i].value, f->coins[i].height,
                          f->coins[i].coinbase, f->coins[i].script,
                          sizeof(f->coins[i].script)))
            return false;
        struct incremental_merkle_tree tree;
        if (f->anchors[i].pool == ANCHOR_POOL_SPROUT)
            sprout_tree_init(&tree);
        else
            sapling_tree_init(&tree);
        struct byte_stream stream;
        stream_init_from_data(&stream, f->anchors[i].blob.data,
                              f->anchors[i].blob.size);
        if (!incremental_tree_deserialize(&tree, &stream) ||
            stream_remaining(&stream) != 0 ||
            !anchor_kv_add_tree(db, f->anchors[i].pool, &tree,
                                f->anchors[i].height) ||
            !nullifier_kv_add(db, f->nfs[i].nf, f->nfs[i].pool,
                              f->nfs[i].height))
            return false;
    }
    return true;
}

static bool install(sqlite3 *db, struct csi_fixture *f,
                    enum consensus_state_install_failpoint fp,
                    enum consensus_state_install_status want)
{
    struct consensus_state_snapshot_install_request req;
    memset(&req, 0, sizeof(req));
    req.bundle_path = f->path;
    req.expected_height = f->height;
    memcpy(req.expected_block_hash, f->block_hash, 32);
    req.failpoint = fp;
    struct consensus_state_install_result result;
    bool ok=consensus_state_snapshot_install(db,&req,&result);
    return !ok && result.status==want;
}

int test_consensus_state_snapshot_install(void)
{
    printf("\n=== consensus_state_snapshot_install ===\n");
    int failures=0; char dir[256];
    test_make_tmpdir(dir,sizeof(dir),"consensus_state_install","main");
    sqlite3 *db=NULL;
    CSI_CHECK("active DB opens",sqlite3_open(":memory:",&db)==SQLITE_OK);
    CSI_CHECK("shared bundle codec pinned SHA3 vectors",bundle_codec_kat());
    struct csi_fixture a,b,inc;
    bool fixtures=fixture_init(&a,dir,"a",0x10,40,true)&&
                  fixture_init(&b,dir,"b",0x80,60,true)&&
                  fixture_init(&inc,dir,"inc",0x40,80,false);
    CSI_CHECK("fixtures build",fixtures);
    CSI_CHECK("generation A active canary seeds",seed_active(db,&a));
    CSI_CHECK("generation A coherent",active_is(db,&a));

    CSI_CHECK("generation B bundle writes",write_bundle(&b,CSI_VALID));
    char sidecar[sizeof(b.path) + 16];
    snprintf(sidecar,sizeof(sidecar),"%s-wal",b.path);
    FILE *sidecar_file=fopen(sidecar,"wb");
    CSI_CHECK("unfinished bundle sidecar fixture writes",sidecar_file!=NULL);
    if(sidecar_file) fclose(sidecar_file);
    CSI_CHECK("unfinished bundle sidecar is refused",
              install(db,&b,CONSENSUS_INSTALL_FAIL_NONE,
                      CONSENSUS_INSTALL_REFUSED));
    CSI_CHECK("sidecar refusal preserves generation A",active_is(db,&a));
    unlink(sidecar);
    for(int fp=CONSENSUS_INSTALL_FAIL_AFTER_BUNDLE_OPEN;
        fp<=CONSENSUS_INSTALL_FAIL_AFTER_BUNDLE_VALIDATE;fp++) {
        char label[112]; snprintf(label,sizeof(label),"failpoint %d refuses",fp);
        CSI_CHECK(label,install(db,&b,(enum consensus_state_install_failpoint)fp,
                                CONSENSUS_INSTALL_INJECTED_FAILURE));
        snprintf(label,sizeof(label),"failpoint %d preserves A",fp);
        CSI_CHECK(label,active_is(db,&a));
    }
    b.height++;
    CSI_CHECK("wrong caller height assertion is refused",
              install(db,&b,CONSENSUS_INSTALL_FAIL_NONE,
                      CONSENSUS_INSTALL_REFUSED));
    b.height--;
    b.block_hash[0]^=1;
    CSI_CHECK("wrong caller hash assertion is refused",
              install(db,&b,CONSENSUS_INSTALL_FAIL_NONE,
                      CONSENSUS_INSTALL_REFUSED));
    b.block_hash[0]^=1;
    CSI_CHECK("caller assertion refusals preserve generation A",active_is(db,&a));

    enum csi_fault faults[]={CSI_WRONG_UTXO_ROOT,CSI_WRONG_SUPPLY,
        CSI_WRONG_ANCHOR_DIGEST,CSI_WRONG_NF_DIGEST,CSI_BAD_TREE_ROOT,
        CSI_BAD_COMPLETE_CURSOR,CSI_ZERO_PROOF,CSI_TEXT_ANCHOR_POOL,
        CSI_TEXT_NF_POOL,CSI_WRONG_FRONTIER_ROOT,CSI_WRONG_FRONTIER_HEIGHT,
        CSI_BAD_SOURCE_RECEIPT,CSI_EXTRA_SCHEMA_OBJECT,
        CSI_EXTRA_SCHEMA_COLUMN};
    for(size_t i=0;i<sizeof(faults)/sizeof(faults[0]);i++) {
        CSI_CHECK("malformed bundle writes",write_bundle(&b,faults[i]));
        CSI_CHECK("malformed bundle refused",install(db,&b,CONSENSUS_INSTALL_FAIL_NONE,
                                                      CONSENSUS_INSTALL_REFUSED));
        CSI_CHECK("malformed bundle publishes nothing",active_is(db,&a));
        /* Reinitialize mutated in-memory metadata/tree roots for next case. */
        fixture_free(&b);
        CSI_CHECK("candidate fixture reinitialized",fixture_init(&b,dir,"b",0x80,60,true));
    }

    CSI_CHECK("generation B final bundle writes",write_bundle(&b,CSI_VALID));
    struct consensus_state_artifact_evidence *artifact = NULL;
    struct zcl_result artifact_opened =
        consensus_state_artifact_evidence_open(b.path, &artifact);
    CSI_CHECK("valid artifact creates opaque evidence",
              artifact_opened.ok && artifact != NULL);
    struct consensus_state_bundle_manifest admitted_manifest;
    uint8_t admitted_digest[32];
    CSI_CHECK("opaque evidence exposes only validated manifest copy",
              consensus_state_artifact_evidence_manifest_copy(
                  artifact, &admitted_manifest) &&
              admitted_manifest.height == b.height &&
              memcmp(admitted_manifest.block_hash, b.block_hash, 32) == 0);
    CSI_CHECK("opaque evidence digest is the admitted artifact digest",
              consensus_state_artifact_evidence_digest(
                  artifact, admitted_digest) &&
              memcmp(admitted_digest, b.artifact, 32) == 0);

    char moved_path[sizeof(b.path) + 16];
    snprintf(moved_path, sizeof(moved_path), "%s-moved", b.path);
    unlink(moved_path);
    CSI_CHECK("admitted artifact can be renamed after descriptor pin",
              rename(b.path, moved_path) == 0);
    FILE *replacement = fopen(b.path, "wb");
    CSI_CHECK("path replacement fixture writes", replacement != NULL);
    if (replacement) {
        fputs("not the admitted artifact", replacement);
        fclose(replacement);
    }
    CSI_CHECK("evidence remains bound to original descriptor after path swap",
              consensus_state_artifact_evidence_manifest_copy(
                  artifact, &admitted_manifest) &&
              admitted_manifest.height == b.height &&
              memcmp(admitted_manifest.artifact_digest, b.artifact, 32) == 0);
    consensus_state_artifact_evidence_free(artifact);
    artifact = NULL;
    unlink(b.path);
    CSI_CHECK("fixture restores admitted artifact path",
              rename(moved_path, b.path) == 0);

    char symlink_path[sizeof(b.path) + 16];
    snprintf(symlink_path, sizeof(symlink_path), "%s-link", b.path);
    unlink(symlink_path);
    CSI_CHECK("bundle symlink fixture writes",
              symlink(b.path, symlink_path) == 0);
    artifact_opened = consensus_state_artifact_evidence_open(
        symlink_path, &artifact);
    CSI_CHECK("symlink artifact admission refuses",
              !artifact_opened.ok && artifact == NULL);
    unlink(symlink_path);
    CSI_CHECK("writable artifact fixture enables owner write",
              chmod(b.path, 0600) == 0);
    artifact_opened = consensus_state_artifact_evidence_open(b.path, &artifact);
    CSI_CHECK("writable artifact admission refuses",
              !artifact_opened.ok && artifact == NULL);
    CSI_CHECK("artifact fixture returns immutable",
              chmod(b.path, 0400) == 0);

    artifact_opened = consensus_state_artifact_evidence_open(b.path, &artifact);
    CSI_CHECK("mutation fixture admits original artifact",
              artifact_opened.ok && artifact != NULL);
    uint8_t receipt_digest[32];
    CSI_CHECK("validation receipt binds full file and local identity",
              consensus_state_artifact_evidence_receipt_digest(
                  artifact, receipt_digest));
    int mutate_fd = -1;
    uint8_t changed_byte = 0;
    bool mutated = chmod(b.path, 0600) == 0;
    if (mutated)
        mutate_fd = open(b.path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (mutate_fd < 0 || pread(mutate_fd, &changed_byte, 1, 100) != 1)
        mutated = false;
    changed_byte ^= 1u;
    if (mutated && (pwrite(mutate_fd, &changed_byte, 1, 100) != 1 ||
                    fsync(mutate_fd) != 0))
        mutated = false;
    if (mutate_fd >= 0)
        close(mutate_fd);
    if (chmod(b.path, 0400) != 0)
        mutated = false;
    CSI_CHECK("post-validation mutation fixture writes", mutated);
    CSI_CHECK("complete-file mutation makes receipt stale",
              !consensus_state_artifact_evidence_revalidate(artifact) &&
              !consensus_state_artifact_evidence_receipt_digest(
                  artifact, receipt_digest));
    consensus_state_artifact_evidence_free(artifact);
    artifact = NULL;
    CSI_CHECK("valid artifact restores after mutation test",
              write_bundle(&b, CSI_VALID));

    CSI_CHECK("valid complete claim is verified but contained",
              install(db,&b,CONSENSUS_INSTALL_FAIL_NONE,
                      CONSENSUS_INSTALL_VERIFIED_CONTAINED));
    CSI_CHECK("contained complete claim preserves generation A",active_is(db,&a));
    b.anchors[0].height = b.height - 1;
    b.anchors[1].height = b.height - 1;
    CSI_CHECK("unchanged-frontier bundle writes with latest roots below H",
              write_bundle(&b,CSI_VALID));
    CSI_CHECK("latest roots below H validate without fabricating anchor rows",
              install(db,&b,CONSENSUS_INSTALL_FAIL_NONE,
                      CONSENSUS_INSTALL_VERIFIED_CONTAINED));
    CSI_CHECK("contained older-frontier claim preserves generation A",
              active_is(db,&a));
    CSI_CHECK("incomplete bundle writes",write_bundle(&inc,CSI_VALID));
    CSI_CHECK("valid incomplete bundle is verified but contained",
              install(db,&inc,CONSENSUS_INSTALL_FAIL_NONE,
                      CONSENSUS_INSTALL_VERIFIED_CONTAINED));
    CSI_CHECK("contained incomplete bundle preserves generation A",active_is(db,&a));

    fixture_free(&a); fixture_free(&b); fixture_free(&inc);
    if (db)
        sqlite3_close(db);
    test_cleanup_tmpdir(dir);
    printf("=== consensus_state_snapshot_install: %d failure(s) ===\n",failures);
    return failures;
}
