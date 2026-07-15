/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Atomic promotion tests for external zcl.consensus_state_bundle.v1 files. */

#include "test/test_helpers.h"

#include "coins/utxo_commitment.h"
#include "config/consensus_state_replay_receipt.h"
#include "config/consensus_state_snapshot_export.h"
#include "config/consensus_state_snapshot_install.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "crypto/sha3.h"
#include "jobs/reducer_frontier.h"
#include "sapling/incremental_merkle_tree.h"
#include "services/consensus_state_publication_cas.h"
#include "storage/anchor_kv.h"
#include "storage/coins_kv.h"
#include "storage/consensus_state_bundle_codec.h"
#include "storage/nullifier_kv.h"
#include "storage/progress_store.h"

#include <sqlite3.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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
        true, true, true, false, true, true, true, false,
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
    CSI_BAD_SOURCE_RECEIPT, CSI_BAD_SOURCE_EPOCH, CSI_UNKNOWN_PROFILE,
    CSI_EMBEDDED_BUNDLE_SCHEMA, CSI_EMBEDDED_RECEIPT_SCHEMA,
    CSI_TEXT_BLOCK_HASH, CSI_TEXT_PROOF_CURSOR, CSI_TEXT_COIN_VALUE,
    CSI_REAL_ANCHOR_HEIGHT, CSI_REAL_NF_HEIGHT,
    CSI_EXTRA_SCHEMA_OBJECT, CSI_EXTRA_SCHEMA_COLUMN,
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
        0xb8,0x57,0xf6,0x79,0xdb,0xb2,0xe5,0x4b,
        0x00,0xa5,0x05,0xc5,0x43,0xeb,0xb6,0x95,
        0xec,0xdf,0x24,0xa2,0xb4,0x53,0x7d,0xd1,
        0x6f,0x2b,0x67,0x62,0x9f,0x10,0x93,0x47,
    };
    static const uint8_t want_epoch[32] = {
        0x9c,0x27,0xe6,0x7b,0x5b,0x02,0x1d,0x09,
        0x3f,0x6d,0x5b,0x8d,0x66,0x04,0xb3,0x17,
        0x0b,0xe0,0xda,0x01,0xfc,0xce,0x13,0xdb,
        0xee,0xbc,0x1b,0x53,0xc8,0x56,0x96,0xf9,
    };
    static const uint8_t want_receipt[32] = {
        0x60,0x19,0xf1,0x3c,0xd3,0xd2,0xab,0xff,
        0x4c,0x76,0xc4,0x38,0x17,0xa2,0xda,0xcc,
        0x51,0x53,0x5b,0xf5,0x9a,0x4b,0x31,0x58,
        0x0b,0x0a,0x08,0xa2,0x65,0xba,0x4c,0x13,
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
        receipt.build_inputs_digest[i] = (uint8_t)(96u + i);
        receipt.chain_corpus_digest[i] = (uint8_t)(128u + i);
    }
    snprintf(receipt.producer_commit, sizeof(receipt.producer_commit),
             "0123456789abcdef0123456789abcdef01234567");
    receipt.source_clean = true;
    receipt.validation_profile = CONSENSUS_STATE_VALIDATION_FULL;
    receipt.fold_cursor = 7;
    consensus_state_source_epoch_digest(&receipt,
                                        receipt.source_epoch_digest);
    if (memcmp(receipt.source_epoch_digest, want_epoch, 32) != 0)
        return false;
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
    m.source_clean = true;
    m.validation_profile = CONSENSUS_STATE_VALIDATION_FULL;
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
    m.source_clean = f->receipt.source_clean;
    m.validation_profile = f->receipt.validation_profile;
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
        f->receipt.build_inputs_digest[i] = (uint8_t)(seed + i + 101u);
        f->receipt.chain_corpus_digest[i] = (uint8_t)(seed + i + 133u);
    }
    snprintf(f->receipt.producer_commit,
             sizeof(f->receipt.producer_commit),
             "0123456789abcdef0123456789abcdef01234567");
    f->receipt.source_clean = true;
    f->receipt.validation_profile = CONSENSUS_STATE_VALIDATION_FULL;
    f->receipt.fold_cursor = f->fold_cursor;
    consensus_state_source_epoch_digest(&f->receipt,
                                        f->receipt.source_epoch_digest);
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
    if (fault == CSI_BAD_SOURCE_EPOCH) {
        f->receipt.source_epoch_digest[0] ^= 1;
        consensus_state_source_receipt_digest(&f->receipt,
                                              f->receipt.receipt_digest);
        memcpy(f->source, f->receipt.receipt_digest, 32);
    }
    if (fault == CSI_UNKNOWN_PROFILE) {
        f->receipt.validation_profile = 3;
        consensus_state_source_receipt_digest(&f->receipt,
                                              f->receipt.receipt_digest);
        memcpy(f->source, f->receipt.receipt_digest, 32);
    }
    fixture_artifact_digest(f);

    sqlite3 *db = NULL;
    bool ok = sqlite3_open(f->path, &db) == SQLITE_OK && exec_ok(db,
        "CREATE TABLE bundle_meta("
        "singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
        "schema TEXT NOT NULL,height INTEGER NOT NULL,block_hash BLOB NOT NULL,"
        "history_complete INTEGER NOT NULL,source_clean INTEGER NOT NULL,"
        "validation_profile INTEGER NOT NULL,activation_boundary INTEGER NOT NULL,"
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
        "source_epoch_digest BLOB NOT NULL,source_tree_root BLOB NOT NULL,"
        "running_binary_digest BLOB NOT NULL,toolchain_digest BLOB NOT NULL,"
        "build_inputs_digest BLOB NOT NULL,chain_corpus_digest BLOB NOT NULL,"
        "source_clean INTEGER NOT NULL,validation_profile INTEGER NOT NULL,"
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
        if (fault == CSI_TEXT_COIN_VALUE && i == 0) {
            char numeric_prefix[48];
            snprintf(numeric_prefix, sizeof(numeric_prefix), "%lldx",
                     (long long)f->coins[i].value);
            sqlite3_bind_text(st,3,numeric_prefix,-1,SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_int64(st,3,f->coins[i].value);
        }
        sqlite3_bind_blob(st,4,f->coins[i].script,3,SQLITE_STATIC);
        sqlite3_bind_int(st,5,f->coins[i].height);
        sqlite3_bind_int(st,6,f->coins[i].coinbase?1:0);
        ok = sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:test-fixture-seeding
    }
    if (st)
        sqlite3_finalize(st);
    st = NULL;
    if (ok) ok = sqlite3_prepare_v2(db,
        "INSERT INTO source_receipt VALUES(1,?,?,?,?,?,?,?,?,?,?,?,?)",
        -1,&st,NULL)==SQLITE_OK;
    if (ok) {
        int i=1;
        static const char bad_receipt_schema[] =
            CONSENSUS_STATE_SOURCE_RECEIPT_SCHEMA "\0junk";
        if (fault == CSI_EMBEDDED_RECEIPT_SCHEMA)
            sqlite3_bind_text(st,i++,bad_receipt_schema,
                              (int)sizeof(bad_receipt_schema)-1,
                              SQLITE_STATIC);
        else
            sqlite3_bind_text(st,i++,CONSENSUS_STATE_SOURCE_RECEIPT_SCHEMA,-1,
                              SQLITE_STATIC);
        sqlite3_bind_blob(st,i++,f->receipt.source_epoch_digest,32,SQLITE_STATIC);
        sqlite3_bind_blob(st,i++,f->receipt.source_tree_root,32,SQLITE_STATIC);
        sqlite3_bind_blob(st,i++,f->receipt.running_binary_digest,32,
                          SQLITE_STATIC);
        sqlite3_bind_blob(st,i++,f->receipt.toolchain_digest,32,SQLITE_STATIC);
        sqlite3_bind_blob(st,i++,f->receipt.build_inputs_digest,32,
                          SQLITE_STATIC);
        sqlite3_bind_blob(st,i++,f->receipt.chain_corpus_digest,32,
                          SQLITE_STATIC);
        sqlite3_bind_int(st,i++,f->receipt.source_clean?1:0);
        sqlite3_bind_int(st,i++,f->receipt.validation_profile);
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
        if (fault == CSI_TEXT_PROOF_CURSOR && row == 0) {
            char numeric_prefix[48];
            snprintf(numeric_prefix, sizeof(numeric_prefix), "%llux",
                     (unsigned long long)p->cursor);
            sqlite3_bind_text(st,3,numeric_prefix,-1,SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_int64(st,3,(sqlite3_int64)p->cursor);
        }
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
            sqlite3_bind_text(st,1,"0x",-1,SQLITE_STATIC);
        else
            sqlite3_bind_int(st,1,f->anchors[i].pool);
        sqlite3_bind_blob(st,2,f->anchors[i].root,32,SQLITE_STATIC);
        if (fault == CSI_REAL_ANCHOR_HEIGHT && i == 0)
            sqlite3_bind_double(st,3,(double)f->anchors[i].height + 0.25);
        else
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
            sqlite3_bind_text(st,1,"0x",-1,SQLITE_STATIC);
        else
            sqlite3_bind_int(st,1,f->nfs[i].pool);
        sqlite3_bind_blob(st,2,f->nfs[i].nf,32,SQLITE_STATIC);
        if (fault == CSI_REAL_NF_HEIGHT && i == 0)
            sqlite3_bind_double(st,3,(double)f->nfs[i].height + 0.25);
        else
            sqlite3_bind_int64(st,3,f->nfs[i].height);
        ok = sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:test-fixture-seeding
    }
    if (st)
        sqlite3_finalize(st);
    st = NULL;
    if (ok) ok = sqlite3_prepare_v2(db,
        "INSERT INTO bundle_meta VALUES(1,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        -1,&st,NULL)==SQLITE_OK;
    if (ok) {
        int i=1;
        static const char bad_bundle_schema[] =
            CONSENSUS_STATE_BUNDLE_SCHEMA "\0junk";
        if (fault == CSI_EMBEDDED_BUNDLE_SCHEMA)
            sqlite3_bind_text(st,i++,bad_bundle_schema,
                              (int)sizeof(bad_bundle_schema)-1,
                              SQLITE_STATIC);
        else
            sqlite3_bind_text(st,i++,CONSENSUS_STATE_BUNDLE_SCHEMA,-1,
                              SQLITE_STATIC);
        sqlite3_bind_int(st,i++,f->height);
        if (fault == CSI_TEXT_BLOCK_HASH)
            sqlite3_bind_text(st,i++,(const char *)f->block_hash,32,
                              SQLITE_STATIC);
        else
            sqlite3_bind_blob(st,i++,f->block_hash,32,SQLITE_STATIC);
        sqlite3_bind_int(st,i++,f->complete?1:0);
        sqlite3_bind_int(st,i++,f->receipt.source_clean?1:0);
        sqlite3_bind_int(st,i++,f->receipt.validation_profile);
        sqlite3_bind_int64(st,i++,f->boundary);
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

static bool digest_nonzero(const uint8_t digest[32])
{
    uint8_t any=0;
    for(size_t i=0;i<32;i++) any|=digest[i];
    return any!=0;
}

static bool candidate_file_digest(const char *path,uint8_t out[32])
{
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) return false;
    struct sha3_256_ctx context; sha3_256_init(&context);
    uint8_t buffer[4096]; bool ok=true;
    for(;;) {
        ssize_t n=read(fd,buffer,sizeof(buffer));
        if(n>0) { sha3_256_write(&context,buffer,(size_t)n); continue; }
        if(n==0) break;
        if(errno==EINTR) continue;
        ok=false; break;
    }
    if(close(fd)!=0) ok=false;
    if(ok) sha3_256_finalize(&context,out);
    return ok;
}

static int candidate_staging_count(const char *dir)
{
    DIR *stream=opendir(dir);
    if(!stream) return -1;
    int count=0;
    struct dirent *entry;
    while((entry=readdir(stream))!=NULL)
        if(strncmp(entry->d_name,".zcl-consensus-candidate-",25)==0)
            count++;
    closedir(stream);
    return count;
}

static bool candidate_output_absent(int dirfd,const char *name)
{
    struct stat st; errno=0;
    return fstatat(dirfd,name,&st,AT_SYMLINK_NOFOLLOW)!=0&&errno==ENOENT;
}

struct candidate_sidecar_hook {
    int dirfd;
    const char *name;
    bool created;
};

struct candidate_retained_writer_hook {
    int dirfd;
    int writer_fd;
    bool ran;
};

static void candidate_create_sidecar(void *opaque)
{
    struct candidate_sidecar_hook *hook=opaque;
    if(!hook) return;
    int fd=openat(hook->dirfd,hook->name,
                  O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    hook->created=fd>=0;
    if(fd>=0) close(fd);
}

static void candidate_retain_staging_writer(void *opaque)
{
    struct candidate_retained_writer_hook *hook=opaque;
    hook->ran=true;
    struct stat dir_st;
    if(fstat(hook->dirfd,&dir_st)!=0) return;
    for(int fd=3;fd<1024;fd++) {
        struct stat st;
        int flags=fcntl(fd,F_GETFL);
        if(flags<0||(flags&O_ACCMODE)!=O_RDWR||fstat(fd,&st)!=0||
           !S_ISREG(st.st_mode)||st.st_nlink!=0||st.st_dev!=dir_st.st_dev)
            continue;
        hook->writer_fd=fcntl(fd,F_DUPFD_CLOEXEC,1024);
        break;
    }
}

static bool candidate_build(
    const struct consensus_state_artifact_evidence *artifact,int dirfd,
    const char *name,enum consensus_state_candidate_failpoint failpoint,
    struct consensus_state_candidate_result *result)
{
    struct consensus_state_candidate_request request;
    memset(&request,0,sizeof(request));
    request.output_dir_fd=dirfd;
    request.output_name=name;
    request.failpoint=failpoint;
    return consensus_state_snapshot_candidate_build(artifact,&request,result);
}

struct candidate_thread_fixture {
    const struct consensus_state_artifact_evidence *artifact;
    int dirfd;
    const char *name;
    struct consensus_state_candidate_result result;
    bool built;
};

static void *candidate_build_thread(void *opaque)
{
    struct candidate_thread_fixture *thread=opaque;
    thread->built=candidate_build(thread->artifact,thread->dirfd,thread->name,
                                  CONSENSUS_CANDIDATE_FAIL_NONE,
                                  &thread->result);
    return NULL;
}

static bool candidate_query_i64(sqlite3 *db,const char *sql,int64_t *out)
{
    sqlite3_stmt *stmt=NULL;
    if(sqlite3_prepare_v2(db,sql,-1,&stmt,NULL)!=SQLITE_OK) return false;
    bool ok=sqlite3_step(stmt)==SQLITE_ROW; // raw-sql-ok:test-read-only-assertion
    if(ok) *out=sqlite3_column_int64(stmt,0);
    if(ok) ok=sqlite3_step(stmt)==SQLITE_DONE; // raw-sql-ok:test-read-only-assertion
    sqlite3_finalize(stmt);
    return ok;
}

static bool candidate_progress_parity(
    sqlite3 *db,const struct csi_fixture *f,const uint8_t admission[32])
{
    int32_t applied=-1; bool found=false;
    if(!coins_kv_get_applied_height(db,&applied,&found)||!found||
       applied!=f->height+1||!coins_kv_contains_refold_marker(db))
        return false;
    for(int pool=0;pool<=1;pool++) {
        int64_t cursor=-1; bool cursor_found=false;
        if(!anchor_kv_activation_cursor(db,pool,&cursor,&cursor_found)||
           !cursor_found||cursor!=0) return false;
    }
    int64_t cursor=-1; bool cursor_found=false;
    if(!nullifier_kv_activation_cursor(db,&cursor,&cursor_found)||
       !cursor_found||cursor!=0) return false;
    int64_t count=-1;
    char sql[256];
    snprintf(sql,sizeof(sql),
        "SELECT count(*) FROM stage_cursor WHERE "
        "(name='tip_finalize' AND cursor=%d) OR "
        "(name!='tip_finalize' AND cursor=%d)",f->height,f->height+1);
    if(!candidate_query_i64(db,"SELECT count(*) FROM stage_cursor",&count)||
       count!=8||
       !candidate_query_i64(db,sql,&count)||count!=8)
        return false;
    snprintf(sql,sizeof(sql),
        "SELECT count(*) FROM utxo_apply_log WHERE height=%d AND "
        "status='anchor' AND ok=1",f->height);
    if(!candidate_query_i64(db,sql,&count)||count!=1) return false;
    snprintf(sql,sizeof(sql),
        "SELECT count(*) FROM tip_finalize_log WHERE height=%d AND "
        "status='anchor' AND ok=1 AND tip_hash IS NOT NULL",f->height);
    if(!candidate_query_i64(db,sql,&count)||count!=1) return false;
    sqlite3_stmt *stmt=NULL;
    bool ok=sqlite3_prepare_v2(db,
        "SELECT schema,receipt_digest FROM consensus_state_source_receipt "
        "WHERE singleton=1",-1,&stmt,NULL)==SQLITE_OK&&
        sqlite3_step(stmt)==SQLITE_ROW; // raw-sql-ok:test-read-only-assertion
    if(ok) {
        const char *schema=(const char *)sqlite3_column_text(stmt,0);
        const void *digest=sqlite3_column_blob(stmt,1);
        ok=schema&&strcmp(schema,CONSENSUS_STATE_SOURCE_RECEIPT_SCHEMA)==0&&
           digest&&sqlite3_column_bytes(stmt,1)==32&&
           memcmp(digest,f->receipt.receipt_digest,32)==0;
    }
    if(stmt) sqlite3_finalize(stmt);
    stmt=NULL;
    if(ok) ok=sqlite3_prepare_v2(db,
        "SELECT schema,artifact_digest,admission_receipt_digest "
        "FROM consensus_state_candidate_meta WHERE singleton=1",
        -1,&stmt,NULL)==SQLITE_OK&&
        sqlite3_step(stmt)==SQLITE_ROW; // raw-sql-ok:test-read-only-assertion
    if(ok) {
        const char *schema=(const char *)sqlite3_column_text(stmt,0);
        const void *artifact=sqlite3_column_blob(stmt,1);
        const void *receipt=sqlite3_column_blob(stmt,2);
        ok=schema&&strcmp(schema,CONSENSUS_STATE_CANDIDATE_SCHEMA)==0&&
           artifact&&receipt&&sqlite3_column_bytes(stmt,1)==32&&
           sqlite3_column_bytes(stmt,2)==32&&
           memcmp(artifact,f->artifact,32)==0&&memcmp(receipt,admission,32)==0;
    }
    if(stmt) sqlite3_finalize(stmt);
    return ok;
}

/* Lower the compiled SHA3-checkpoint finality floor so the H*-climb leg below
 * can drive the reducer at a small fixture height (the production floor is the
 * mainnet checkpoint at 3,056,758). Mirror-declared, exactly as the sibling
 * export test does. */
void reducer_frontier_test_set_compiled_anchor(int32_t height);

/* ── H*-climb leg fixture helpers ─────────────────────────────────────────
 * After ACTIVATE forces the reducer cursors to the anchor, prove the cure is
 * a LIVE fold-resume point (not just forced cursors): seed the anchor's trust
 * row, then fold real per-height stage evidence forward and watch
 * reducer_frontier_compute_hstar CLIMB. These write the durable stage-log image
 * directly (test scaffolding building the progress.kv, not production reducer
 * code — the same exemption the sibling reducer_frontier tests use), so they
 * carry the raw-sql-ok markers rather than routing through the AR lifecycle. */

/* A per-height 32-byte hash all stage receipts at that height AGREE on, so the
 * C3 hash-binding split scan never caps the climb. */
static void hs_synth_hash(uint8_t out[32], int32_t h)
{
    memset(out, 0, 32);
    out[0] = (uint8_t)(h & 0xff);
    out[1] = (uint8_t)((h >> 8) & 0xff);
    out[2] = (uint8_t)((h >> 16) & 0xff);
    out[3] = (uint8_t)((h >> 24) & 0xff);
    out[31] = 0x5c;
}

/* Materialize the stage-log tables with the EXACT production DDL. ACTIVATE
 * clears (and never creates) these derived logs, so on the cured store they are
 * absent; IF NOT EXISTS keeps this safe if a table is already present. */
static bool hs_ensure_forward_schema(sqlite3 *db)
{
    return exec_ok(db,
        "CREATE TABLE IF NOT EXISTS header_admit_log("
        "height INTEGER PRIMARY KEY,hash BLOB NOT NULL,parent_hash BLOB,"
        "admitted_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS validate_headers_log("
        "height INTEGER PRIMARY KEY,hash BLOB NOT NULL,ok INTEGER NOT NULL,"
        "fail_reason TEXT,validated_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS script_validate_log("
        "height INTEGER PRIMARY KEY,status TEXT NOT NULL,ok INTEGER NOT NULL,"
        "tx_count INTEGER NOT NULL,input_count INTEGER NOT NULL,"
        "first_failure_txid BLOB,first_failure_vin INTEGER,"
        "first_failure_serror INTEGER,validated_at INTEGER NOT NULL,"
        "block_hash BLOB,source_epoch_digest BLOB);"
        "CREATE TABLE IF NOT EXISTS body_persist_log("
        "height INTEGER PRIMARY KEY,source TEXT NOT NULL,ok INTEGER NOT NULL,"
        "persisted_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS proof_validate_log("
        "height INTEGER PRIMARY KEY,status TEXT NOT NULL,ok INTEGER NOT NULL,"
        "sapling_spends_total INTEGER NOT NULL,"
        "sapling_outputs_total INTEGER NOT NULL,"
        "sprout_joinsplits_total INTEGER NOT NULL,block_hash BLOB,"
        "source_epoch_digest BLOB,first_failure_txid BLOB,"
        "first_failure_proof_type TEXT,validated_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS utxo_apply_log("
        "height INTEGER PRIMARY KEY,status TEXT NOT NULL,ok INTEGER NOT NULL,"
        "spent_count INTEGER NOT NULL,added_count INTEGER NOT NULL,"
        "total_value_delta INTEGER NOT NULL,first_failure_kind TEXT,"
        "first_failure_detail BLOB,applied_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS utxo_apply_delta("
        "height INTEGER PRIMARY KEY,branch_hash BLOB NOT NULL,"
        "spent_blob BLOB NOT NULL,added_blob BLOB NOT NULL);"
        "CREATE TABLE IF NOT EXISTS tip_finalize_log("
        "height INTEGER PRIMARY KEY,status TEXT NOT NULL,ok INTEGER NOT NULL,"
        "work_delta_high INTEGER NOT NULL,work_delta_low INTEGER NOT NULL,"
        "utxo_size_after INTEGER NOT NULL,reorg_depth INTEGER NOT NULL,"
        "finalized_at INTEGER NOT NULL,tip_hash BLOB);");
}

/* Insert one row binding ?1=height and (if hash!=NULL) ?2=the 32-byte hash. */
static bool hs_ins(sqlite3 *db, const char *sql, int32_t h,
                   const uint8_t *hash)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "[hs] prepare failed: %s\n", sqlite3_errmsg(db));
        return false;
    }
    sqlite3_bind_int(st, 1, h);
    if (hash)
        sqlite3_bind_blob(st, 2, hash, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:test-fixture-seeding
    if (!ok)
        fprintf(stderr, "[hs] step h=%d failed: %s\n", h, sqlite3_errmsg(db));
    sqlite3_finalize(st);
    return ok;
}

/* The tip_finalize status="anchor" row the boot seed stamps — the trust base
 * reducer_frontier_compute_hstar clamps to. OR REPLACE so it is authoritative
 * whether or not ACTIVATE's tip_finalize_stage_seed_anchor already wrote a row
 * at this height (it does when the seed integrity gate passes). */
static bool hs_put_anchor(sqlite3 *db, int32_t h)
{
    uint8_t hh[32];
    hs_synth_hash(hh, h);
    return hs_ins(db,
        "INSERT OR REPLACE INTO tip_finalize_log(height,status,ok,"
        "work_delta_high,work_delta_low,utxo_size_after,reorg_depth,"
        "finalized_at,tip_hash) VALUES(?1,'anchor',1,0,0,0,0,0,?2)", h, hh);
}

/* Fold one fully-consistent ok=1 height across every contiguity + C3 log, all
 * bound to the same per-height hash. */
static bool hs_put_forward_height(sqlite3 *db, int32_t h)
{
    uint8_t hh[32];
    hs_synth_hash(hh, h);
    return hs_ins(db,
            "INSERT INTO header_admit_log(height,hash,admitted_at) "
            "VALUES(?1,?2,0)", h, hh) &&
        hs_ins(db,
            "INSERT INTO validate_headers_log(height,hash,ok,validated_at) "
            "VALUES(?1,?2,1,0)", h, hh) &&
        hs_ins(db,
            "INSERT INTO script_validate_log(height,status,ok,tx_count,"
            "input_count,validated_at,block_hash) "
            "VALUES(?1,'verified',1,0,0,0,?2)", h, hh) &&
        hs_ins(db,
            "INSERT INTO body_persist_log(height,source,ok,persisted_at) "
            "VALUES(?1,'test',1,0)", h, NULL) &&
        hs_ins(db,
            "INSERT INTO proof_validate_log(height,status,ok,"
            "sapling_spends_total,sapling_outputs_total,"
            "sprout_joinsplits_total,validated_at,block_hash) "
            "VALUES(?1,'verified',1,0,0,0,0,?2)", h, hh) &&
        hs_ins(db,
            "INSERT INTO utxo_apply_log(height,status,ok,spent_count,"
            "added_count,total_value_delta,applied_at) "
            "VALUES(?1,'verified',1,0,0,0,0)", h, NULL) &&
        hs_ins(db,
            "INSERT INTO utxo_apply_delta(height,branch_hash,spent_blob,"
            "added_blob) VALUES(?1,?2,x'',x'')", h, hh) &&
        hs_ins(db,
            "INSERT INTO tip_finalize_log(height,status,ok,work_delta_high,"
            "work_delta_low,utxo_size_after,reorg_depth,finalized_at,tip_hash) "
            "VALUES(?1,'ok',1,0,0,0,0,0,?2)", h, hh);
}

/* Set one reducer stage cursor (upsert). */
static bool hs_set_cursor(sqlite3 *db, const char *name, int64_t cursor)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO stage_cursor(name,cursor,updated_at) VALUES(?,?,0) "
            "ON CONFLICT(name) DO UPDATE SET cursor=excluded.cursor",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, cursor);
    bool ok = sqlite3_step(st) == SQLITE_DONE; // raw-sql-ok:test-fixture-seeding
    sqlite3_finalize(st);
    return ok;
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
        CSI_BAD_SOURCE_RECEIPT,CSI_BAD_SOURCE_EPOCH,CSI_UNKNOWN_PROFILE,
        CSI_EMBEDDED_BUNDLE_SCHEMA,CSI_EMBEDDED_RECEIPT_SCHEMA,
        CSI_TEXT_BLOCK_HASH,CSI_TEXT_PROOF_CURSOR,CSI_TEXT_COIN_VALUE,
        CSI_REAL_ANCHOR_HEIGHT,CSI_REAL_NF_HEIGHT,
        CSI_EXTRA_SCHEMA_OBJECT,
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
    CSI_CHECK("inode rename invalidates admitted metadata receipt",
              !consensus_state_artifact_evidence_manifest_copy(
                  artifact, &admitted_manifest));
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

    /* A retained writer can restore bytes and mtime after a temporary rewrite,
     * but Linux advances ctime. Admission must bind ctime so that semantic
     * reads cannot be detached from the final byte image this way. */
    int retained_fd = -1;
    bool retained_ready = chmod(b.path, 0600) == 0;
    if (retained_ready)
        retained_fd = open(b.path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (retained_fd < 0 || chmod(b.path, 0400) != 0)
        retained_ready = false;
    artifact_opened = retained_ready
        ? consensus_state_artifact_evidence_open(b.path, &artifact)
        : ZCL_ERR(-1, "retained writer fixture setup failed");
    CSI_CHECK("retained-writer fixture admits stable original",
              artifact_opened.ok && artifact != NULL);
    struct stat retained_before;
    uint8_t original_byte = 0;
    bool restored = artifact && retained_fd >= 0 &&
        fstat(retained_fd, &retained_before) == 0 &&
        pread(retained_fd, &original_byte, 1, 100) == 1;
    uint8_t temporary_byte = (uint8_t)(original_byte ^ 1u);
    if (restored)
        restored = pwrite(retained_fd, &temporary_byte, 1, 100) == 1 &&
                   fsync(retained_fd) == 0 &&
                   pwrite(retained_fd, &original_byte, 1, 100) == 1 &&
                   fsync(retained_fd) == 0;
    struct timespec restore_times[2] = {
        retained_before.st_atim, retained_before.st_mtim
    };
    if (restored)
        restored = futimens(retained_fd, restore_times) == 0;
    CSI_CHECK("retained writer restores exact bytes and mtime", restored);
    CSI_CHECK("ctime still invalidates restored retained-writer receipt",
              artifact &&
              !consensus_state_artifact_evidence_revalidate(artifact));
    consensus_state_artifact_evidence_free(artifact);
    artifact = NULL;
    if (retained_fd >= 0)
        close(retained_fd);
    CSI_CHECK("valid artifact restores after retained-writer test",
              write_bundle(&b, CSI_VALID));

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

    int candidate_dirfd=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    CSI_CHECK("candidate output directory capability opens",candidate_dirfd>=0);
    artifact_opened=consensus_state_artifact_evidence_open(b.path,&artifact);
    CSI_CHECK("candidate source admits immutable complete artifact",
              artifact_opened.ok&&artifact!=NULL);
    uint8_t candidate_admission[32];
    CSI_CHECK("candidate source admission receipt is available",
              consensus_state_artifact_evidence_receipt_digest(
                  artifact,candidate_admission));
    struct consensus_state_candidate_result reserved;
    CSI_CHECK("candidate API cannot name the active progress store",
              !candidate_build(artifact,candidate_dirfd,"progress.kv",
                               CONSENSUS_CANDIDATE_FAIL_NONE,&reserved)&&
              reserved.status==CONSENSUS_CANDIDATE_REFUSED&&
              candidate_output_absent(candidate_dirfd,"progress.kv"));
    CSI_CHECK("candidate active-store family refusal is case-independent",
              !candidate_build(artifact,candidate_dirfd,"Progress.KV.next",
                               CONSENSUS_CANDIDATE_FAIL_NONE,&reserved)&&
              reserved.status==CONSENSUS_CANDIDATE_REFUSED&&
              candidate_output_absent(candidate_dirfd,"Progress.KV.next"));
    struct consensus_state_candidate_result short_name;
    CSI_CHECK("candidate short normalized name is handled safely",
              candidate_build(artifact,candidate_dirfd,"x",
                              CONSENSUS_CANDIDATE_FAIL_NONE,&short_name)&&
              short_name.status==CONSENSUS_CANDIDATE_VERIFIED_CONTAINED);
    CSI_CHECK("candidate short-name fixture removes",
              unlinkat(candidate_dirfd,"x",0)==0);
    for(int fp=CONSENSUS_CANDIDATE_FAIL_AFTER_STAGING_OPEN;
        fp<=CONSENSUS_CANDIDATE_FAIL_AFTER_REOPEN;fp++) {
        char name[64],label[128];
        snprintf(name,sizeof(name),"failed-%d.progress.kv",fp);
        struct consensus_state_candidate_result failed;
        bool built=candidate_build(
            artifact,candidate_dirfd,name,
            (enum consensus_state_candidate_failpoint)fp,&failed);
        snprintf(label,sizeof(label),"candidate failpoint %d is injected",fp);
        CSI_CHECK(label,!built&&
            failed.status==CONSENSUS_CANDIDATE_INJECTED_FAILURE);
        snprintf(label,sizeof(label),"candidate failpoint %d has no final",fp);
        CSI_CHECK(label,candidate_output_absent(candidate_dirfd,name));
        snprintf(label,sizeof(label),"candidate failpoint %d cleans staging",fp);
        CSI_CHECK(label,candidate_staging_count(dir)==0);
        snprintf(label,sizeof(label),"candidate failpoint %d preserves active",fp);
        CSI_CHECK(label,active_is(db,&a));
    }
    struct candidate_sidecar_hook sidecar_hook={
        .dirfd=candidate_dirfd,
        .name="raced.progress.kv-wal",
    };
    consensus_state_snapshot_candidate_test_set_before_link_hook(
        candidate_create_sidecar,&sidecar_hook);
    struct consensus_state_candidate_result sidecar_race;
    CSI_CHECK("candidate late sidecar race refuses publication",
              !candidate_build(artifact,candidate_dirfd,"raced.progress.kv",
                               CONSENSUS_CANDIDATE_FAIL_NONE,&sidecar_race)&&
              sidecar_race.status==CONSENSUS_CANDIDATE_OUTPUT_ERROR&&
              sidecar_hook.created&&
              candidate_output_absent(candidate_dirfd,"raced.progress.kv"));
    CSI_CHECK("candidate sidecar race preserves active generation",
              active_is(db,&a));
    CSI_CHECK("candidate sidecar race fixture removes",
              unlinkat(candidate_dirfd,sidecar_hook.name,0)==0);

    struct candidate_retained_writer_hook retained_writer={
        .dirfd=candidate_dirfd,
        .writer_fd=-1,
    };
    consensus_state_snapshot_export_test_set_after_staging_create_hook(
        candidate_retain_staging_writer,&retained_writer);
    struct consensus_state_candidate_result retained_writer_result;
    CSI_CHECK("candidate retained writable descriptor refuses publication",
              !candidate_build(artifact,candidate_dirfd,
                               "retained-writer.progress.kv",
                               CONSENSUS_CANDIDATE_FAIL_NONE,
                               &retained_writer_result)&&
              retained_writer_result.status==
                  CONSENSUS_CANDIDATE_OUTPUT_ERROR&&
              retained_writer.ran&&retained_writer.writer_fd>=0&&
              candidate_output_absent(candidate_dirfd,
                                      "retained-writer.progress.kv")&&
              candidate_staging_count(dir)==0);
    if(retained_writer.writer_fd>=0) close(retained_writer.writer_fd);

    struct candidate_thread_fixture concurrent[2]={
        {.artifact=artifact,.dirfd=candidate_dirfd,
         .name="concurrent-a.progress.kv"},
        {.artifact=artifact,.dirfd=candidate_dirfd,
         .name="concurrent-b.progress.kv"},
    };
    pthread_t candidate_threads[2];
    bool thread_a_started=pthread_create(
        &candidate_threads[0],NULL,candidate_build_thread,&concurrent[0])==0;
    bool thread_b_started=pthread_create(
        &candidate_threads[1],NULL,candidate_build_thread,&concurrent[1])==0;
    if(thread_a_started)
        pthread_join(candidate_threads[0],NULL);
    if(thread_b_started)
        pthread_join(candidate_threads[1],NULL);
    CSI_CHECK("shared evidence serializes concurrent candidate builds",
              thread_a_started&&thread_b_started&&
              concurrent[0].built&&concurrent[1].built&&
              concurrent[0].result.status==
                  CONSENSUS_CANDIDATE_VERIFIED_CONTAINED&&
              concurrent[1].result.status==
                  CONSENSUS_CANDIDATE_VERIFIED_CONTAINED);
    CSI_CHECK("concurrent candidates publish complete immutable files",
              !candidate_output_absent(candidate_dirfd,concurrent[0].name)&&
              !candidate_output_absent(candidate_dirfd,concurrent[1].name)&&
              candidate_staging_count(dir)==0);
    (void)unlinkat(candidate_dirfd,concurrent[0].name,0);
    (void)unlinkat(candidate_dirfd,concurrent[1].name,0);

    struct consensus_state_candidate_result candidate_result;
    CSI_CHECK("complete evidence builds contained candidate generation",
              candidate_build(artifact,candidate_dirfd,"candidate.progress.kv",
                              CONSENSUS_CANDIDATE_FAIL_NONE,
                              &candidate_result)&&
              candidate_result.status==
                  CONSENSUS_CANDIDATE_VERIFIED_CONTAINED&&
              candidate_result.source_clean&&
              candidate_result.validation_profile==
                  CONSENSUS_STATE_VALIDATION_FULL&&
              candidate_result.height==b.height&&
              candidate_result.utxo_count==2&&
              candidate_result.anchor_count==2&&
              candidate_result.nullifier_count==2&&
              memcmp(candidate_result.artifact_digest,b.artifact,32)==0&&
              digest_nonzero(candidate_result.candidate_file_digest));
    CSI_CHECK("successful candidate leaves no private staging",
              candidate_staging_count(dir)==0);
    char candidate_path[512];
    snprintf(candidate_path,sizeof(candidate_path),"%s/%s",dir,
             "candidate.progress.kv");
    struct stat candidate_stat;
    CSI_CHECK("candidate output is immutable regular file",
              lstat(candidate_path,&candidate_stat)==0&&
              S_ISREG(candidate_stat.st_mode)&&
              (candidate_stat.st_mode&(S_IWUSR|S_IWGRP|S_IWOTH))==0);
    uint8_t candidate_disk_digest[32];
    CSI_CHECK("candidate result binds exact final file bytes",
              candidate_file_digest(candidate_path,candidate_disk_digest)&&
              memcmp(candidate_disk_digest,
                     candidate_result.candidate_file_digest,32)==0);
    sqlite3 *candidate_db=NULL;
    CSI_CHECK("candidate independently opens read-only",
              sqlite3_open_v2(candidate_path,&candidate_db,
                              SQLITE_OPEN_READONLY|SQLITE_OPEN_NOMUTEX,
                              NULL)==SQLITE_OK);
    CSI_CHECK("candidate complete components equal admitted bundle",
              candidate_db&&active_is(candidate_db,&b));
    CSI_CHECK("candidate cursors, logs, base, and provenance are exact",
              candidate_db&&candidate_progress_parity(
                  candidate_db,&b,candidate_admission));
    if(candidate_db) sqlite3_close(candidate_db);
    struct consensus_state_candidate_result collision;
    CSI_CHECK("candidate final name collision refuses no-replace",
              !candidate_build(artifact,candidate_dirfd,"candidate.progress.kv",
                               CONSENSUS_CANDIDATE_FAIL_NONE,&collision)&&
              collision.status==CONSENSUS_CANDIDATE_REFUSED);
    struct stat candidate_after_collision;
    CSI_CHECK("candidate collision preserves exact published inode",
              lstat(candidate_path,&candidate_after_collision)==0&&
              candidate_after_collision.st_dev==candidate_stat.st_dev&&
              candidate_after_collision.st_ino==candidate_stat.st_ino&&
              candidate_after_collision.st_size==candidate_stat.st_size);
    char active_candidate_path[512];
    snprintf(active_candidate_path,sizeof(active_candidate_path),"%s/%s",dir,
             "progress.kv");
    CSI_CHECK("contained candidate can be moved only for boot refusal test",
              rename(candidate_path,active_candidate_path)==0&&
              chmod(active_candidate_path,0600)==0);
    CSI_CHECK("progress store refuses admission-only candidate authority",
              !progress_store_open(dir)&&progress_store_db()==NULL);
    CSI_CHECK("contained candidate restores after boot refusal test",
              chmod(active_candidate_path,0400)==0&&
              rename(active_candidate_path,candidate_path)==0);
    CSI_CHECK("candidate construction never mutates active generation",
              active_is(db,&a));
    consensus_state_artifact_evidence_free(artifact);
    artifact=NULL;
    unlink(candidate_path);
    if(candidate_dirfd>=0) close(candidate_dirfd);

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
    int incomplete_dirfd=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    artifact_opened=consensus_state_artifact_evidence_open(inc.path,&artifact);
    CSI_CHECK("incomplete artifact can be admitted for contained inspection",
              artifact_opened.ok&&artifact!=NULL&&incomplete_dirfd>=0);
    struct consensus_state_candidate_result incomplete_candidate;
    CSI_CHECK("incomplete shielded history cannot become a candidate",
              !candidate_build(artifact,incomplete_dirfd,
                               "incomplete.progress.kv",
                               CONSENSUS_CANDIDATE_FAIL_NONE,
                               &incomplete_candidate)&&
              incomplete_candidate.status==CONSENSUS_CANDIDATE_REFUSED&&
              candidate_output_absent(incomplete_dirfd,
                                      "incomplete.progress.kv")&&
              candidate_staging_count(dir)==0);
    CSI_CHECK("incomplete candidate refusal preserves generation A",
              active_is(db,&a));
    consensus_state_artifact_evidence_free(artifact);
    artifact=NULL;
    if(incomplete_dirfd>=0) close(incomplete_dirfd);

    /* ── A3/A2/D3: ACTIVATE-mode install into a live (wedged) progress store ──
     * Open a REAL process-singleton progress store, seed it into the exact
     * anchor_backfill_gap WEDGE (a borrowed coin + EMPTY shielded tables with a
     * positive activation cursor), then prove: (i) a tampered bundle refuses and
     * leaves the wedge intact; (ii) a non-ADMIT publication-CAS decision gates
     * the install out; (iii) an ADMIT complete bundle installs atomically and
     * CURES the wedge, forcing the reducer cursors to the anchor so the fold
     * resumes there; (iv) a physically restorable prior generation is captured. */
    {
        /* An earlier leg left b's anchor heights at b.height-1; restore them so
         * the complete-frontier invariant (anchor height == block height) holds
         * for active_is below. */
        b.anchors[0].height = b.height;
        b.anchors[1].height = b.height;
        char active_dir[320];
        snprintf(active_dir, sizeof(active_dir), "%s/activate", dir);
        CSI_CHECK("activate: datadir", mkdir(active_dir, 0700) == 0);
        CSI_CHECK("activate: live progress store opens",
                  progress_store_open(active_dir));
        sqlite3 *pdb = progress_store_db();

        uint8_t borrowed_txid[32];
        memset(borrowed_txid, 0x5a, sizeof(borrowed_txid));
        const uint8_t borrowed_script[] = {0x51};
        CSI_CHECK("activate: wedged store seeds (borrowed coin, empty anchors, "
                  "positive activation cursor)",
                  pdb && coins_kv_ensure_schema(pdb) &&
                  progress_meta_table_ensure(pdb) &&
                  anchor_kv_ensure_schema(pdb) &&
                  nullifier_kv_ensure_schema(pdb) &&
                  anchor_kv_initialize_history(pdb, b.height) &&
                  nullifier_kv_initialize_history(pdb, b.height) &&
                  coins_kv_add(pdb, borrowed_txid, 0, 4242, b.height, false,
                               borrowed_script, sizeof(borrowed_script)));
        struct incremental_merkle_tree gate_tree;
        struct uint256 gate_root;
        int64_t gate_h = -1;
        sapling_tree_init(&gate_tree);
        CSI_CHECK("activate: pre-install sapling gate is HISTORY_INCOMPLETE "
                  "(the wedge)",
                  anchor_kv_latest_tree(pdb, ANCHOR_POOL_SAPLING, &gate_tree,
                                        &gate_root, &gate_h) ==
                      ANCHOR_KV_HISTORY_INCOMPLETE);

        struct consensus_state_activate_request areq;
        memset(&areq, 0, sizeof(areq));
        areq.expected_height = b.height;
        memcpy(areq.expected_block_hash, b.block_hash, 32);
        areq.datadir = active_dir;
        struct consensus_state_activate_result ares;

        /* (i) Negative — a tampered (bad UTXO root) bundle must refuse. */
        CSI_CHECK("activate: tampered bundle writes",
                  write_bundle(&b, CSI_WRONG_UTXO_ROOT));
        areq.bundle_path = b.path;
        CSI_CHECK("activate: tampered bundle refuses with a typed reason",
                  !consensus_state_snapshot_install_activate(pdb, &areq, &ares) &&
                  !ares.activated &&
                  ares.status != CONSENSUS_INSTALL_ACTIVATED &&
                  ares.reason[0] != '\0');
        sapling_tree_init(&gate_tree);
        CSI_CHECK("activate: tampered refusal leaves the store wedged/intact",
                  coins_kv_count(pdb) == 1 &&
                  anchor_kv_latest_tree(pdb, ANCHOR_POOL_SAPLING, &gate_tree,
                                        &gate_root, &gate_h) ==
                      ANCHOR_KV_HISTORY_INCOMPLETE);

        /* (ii) Negative — a non-ADMIT publication-CAS decision gates out the
         * install. Build cas_decide inputs from the validated artifact; a
         * durable frontier BEHIND the bundle height yields REFUSED. */
        CSI_CHECK("activate: valid complete bundle restored",
                  write_bundle(&b, CSI_VALID));
        struct consensus_state_artifact_evidence *ev = NULL;
        struct zcl_result evr =
            consensus_state_artifact_evidence_open(b.path, &ev);
        struct consensus_state_publication_cas_inputs cin;
        memset(&cin, 0, sizeof(cin));
        bool cin_ok = evr.ok && ev &&
            consensus_state_artifact_evidence_manifest_copy(ev, &cin.manifest) &&
            consensus_state_artifact_evidence_digest(
                ev, cin.artifact_logical_digest) &&
            consensus_state_artifact_evidence_receipt_digest(
                ev, cin.artifact_receipt_digest);
        cin.chain_evidence_present = true;
        cin.chain_bound_to_artifact = true;
        memset(cin.chain_evidence_digest, 0xa5, 32);
        cin.source_receipt_present = true;
        cin.source_receipt = b.receipt;
        cin.target_lane = CONSENSUS_STATE_TARGET_LANE_COPY_PROOF;
        cin.frontier_known = true;
        memset(cin.frontier_hash, 0x11, 32);
        struct consensus_state_publication_decision_record dec;
        cin.frontier_height = b.height;
        consensus_state_publication_cas_decide(&cin, &dec);
        CSI_CHECK("activate: complete bundle + bound evidence CAS-decides ADMIT",
                  cin_ok && dec.decision == CONSENSUS_PUBLICATION_ADMIT);
        cin.frontier_height = b.height - 1;
        consensus_state_publication_cas_decide(&cin, &dec);
        bool refused = dec.decision != CONSENSUS_PUBLICATION_ADMIT &&
                       dec.refusal ==
                           CONSENSUS_PUBLICATION_REFUSAL_FRONTIER_BEHIND;
        if (dec.decision == CONSENSUS_PUBLICATION_ADMIT) /* gate: never reached */
            (void)consensus_state_snapshot_install_activate(pdb, &areq, &ares);
        sapling_tree_init(&gate_tree);
        CSI_CHECK("activate: non-ADMIT CAS gates out the install (store "
                  "untouched, still wedged)",
                  refused && coins_kv_count(pdb) == 1 &&
                  anchor_kv_latest_tree(pdb, ANCHOR_POOL_SAPLING, &gate_tree,
                                        &gate_root, &gate_h) ==
                      ANCHOR_KV_HISTORY_INCOMPLETE);
        if (ev)
            consensus_state_artifact_evidence_free(ev);

        /* (ii-b) Negative — WITHOUT independent-replay authority the ADMIT-worthy
         * bundle stays CONTAINED and writes NOTHING (the production posture: the
         * bundle's self-asserted digests do not authenticate its contents). */
        areq.bundle_path = b.path;
        consensus_state_activate_test_force_independent_authority(false);
        struct consensus_state_activate_result ares_contained;
        CSI_CHECK("activate: no replay receipt keeps the install VERIFIED_"
                  "CONTAINED (store untouched, still wedged)",
                  !consensus_state_snapshot_install_activate(pdb, &areq,
                                                             &ares_contained) &&
                  !ares_contained.activated &&
                  ares_contained.status ==
                      CONSENSUS_INSTALL_VERIFIED_CONTAINED &&
                  coins_kv_count(pdb) == 1);

        /* (iii) Positive — ADMIT path with authority granted (the ZCL_TESTING
         * hook stands in for a valid replay receipt): install atomically. */
        consensus_state_activate_test_force_independent_authority(true);
        CSI_CHECK("activate: complete bundle installs atomically",
                  consensus_state_snapshot_install_activate(pdb, &areq, &ares) &&
                  ares.activated &&
                  ares.status == CONSENSUS_INSTALL_ACTIVATED &&
                  ares.height == b.height && ares.utxo_count == 2 &&
                  ares.anchor_count == 2 && ares.nullifier_count == 2);
        CSI_CHECK("activate: installed coins/anchors/nullifiers match the bundle "
                  "with COMPLETE (cursor 0) history",
                  active_is(pdb, &b));

        /* (iii-b) INDEPENDENT REPLAY RECEIPT — the store is now folded to the
         * anchor (coins_applied == anchor+1), so the offline verifier can
         * re-derive every component digest from THIS datadir's own tables (never
         * the bundle) and, on a full match, persist the receipt that authorizes
         * ACTIVATE with the ZCL_TESTING hook OFF. */
        consensus_state_activate_test_force_independent_authority(false);
        {
            struct consensus_state_replay_result vres;
            /* A tampered bundle cannot even be admitted, so no receipt is made. */
            CSI_CHECK("replay: verifier persists no receipt for a tampered bundle",
                      write_bundle(&b, CSI_WRONG_UTXO_ROOT) &&
                      !consensus_state_replay_verify_and_write_receipt(
                          pdb, b.path, active_dir, &vres) &&
                      !vres.verified);
            CSI_CHECK("replay: valid bundle restored", write_bundle(&b, CSI_VALID));
            CSI_CHECK("replay: verifier re-derives from the datadir's own tables "
                      "and writes a receipt",
                      consensus_state_replay_verify_and_write_receipt(
                          pdb, b.path, active_dir, &vres) &&
                      vres.verified && vres.height == b.height &&
                      vres.utxo_count == 2 && vres.anchor_count == 2 &&
                      vres.nullifier_count == 2);

            struct consensus_state_artifact_evidence *rev = NULL;
            struct consensus_state_bundle_manifest rman;
            uint8_t rfile[32];
            struct zcl_result ro =
                consensus_state_artifact_evidence_open(b.path, &rev);
            bool rgot = ro.ok && rev &&
                consensus_state_artifact_evidence_manifest_copy(rev, &rman) &&
                consensus_state_artifact_evidence_file_digest(rev, rfile);
            CSI_CHECK("replay: receipt authorizes THIS exact bundle + anchor",
                      rgot &&
                      consensus_state_replay_receipt_authority_available(
                          &rman, rfile, active_dir));
            uint8_t wrong_file[32];
            memcpy(wrong_file, rfile, 32);
            wrong_file[0] ^= 0xff;
            CSI_CHECK("replay: receipt refuses a foreign bundle-file digest",
                      rgot &&
                      !consensus_state_replay_receipt_authority_available(
                          &rman, wrong_file, active_dir));
            struct consensus_state_bundle_manifest wrong_man = rman;
            wrong_man.utxo_root[0] ^= 0xff;
            CSI_CHECK("replay: receipt refuses a foreign component digest",
                      rgot &&
                      !consensus_state_replay_receipt_authority_available(
                          &wrong_man, rfile, active_dir));
            if (rev)
                consensus_state_artifact_evidence_free(rev);
        }

        sapling_tree_init(&gate_tree);
        CSI_CHECK("activate: post-install sapling gate is FOUND (wedge cured)",
                  anchor_kv_latest_tree(pdb, ANCHOR_POOL_SAPLING, &gate_tree,
                                        &gate_root, &gate_h) == ANCHOR_KV_FOUND);
        /* Reducer cursors forced to the anchor so the fold resumes there. */
        int64_t applied_cursor = -1, tip_cursor = -1;
        CSI_CHECK("activate: reducer stage cursors forced to the anchor frontier "
                  "(upstream=anchor+1, tip_finalize=anchor)",
                  candidate_query_i64(pdb,
                      "SELECT cursor FROM stage_cursor WHERE name='utxo_apply'",
                      &applied_cursor) && applied_cursor == b.height + 1 &&
                  candidate_query_i64(pdb,
                      "SELECT cursor FROM stage_cursor WHERE name='tip_finalize'",
                      &tip_cursor) && tip_cursor == b.height);
        int32_t applied = -1;
        bool afound = false;
        CSI_CHECK("activate: coins_applied_height == anchor+1 (fold resume point)",
                  coins_kv_get_applied_height(pdb, &applied, &afound) && afound &&
                  applied == b.height + 1 &&
                  ares.coins_applied_height == b.height + 1);
        CSI_CHECK("activate: self-folded (checkpoint-bound) provenance marker set",
                  coins_kv_contains_refold_marker(pdb));
        /* (v) Re-export readiness: the cured datadir must satisfy BOTH
         * predicates consensus_export_prove_source() gates on, so this node can
         * later re-serve the state it installed (coins_kv_is_proven_authority +
         * the self-folded refold marker above). */
        int32_t proven_applied = -1;
        CSI_CHECK("activate: installed datadir is coins_kv proven authority "
                  "(re-export ready)",
                  coins_kv_is_proven_authority(pdb, &proven_applied) &&
                  proven_applied == b.height + 1);

        /* (vi) H*-CLIMB — prove the cure is a LIVE fold-resume point, not just
         * forced cursors: seed the anchor's trust row, confirm H* sits AT the
         * wedge/anchor, then fold three consistent heights forward over stage
         * evidence and assert reducer_frontier_compute_hstar CLIMBS past the
         * wedge. Lower the compiled finality floor so the small fixture height
         * is a valid anchor (restored to the production default after). */
        reducer_frontier_test_set_compiled_anchor(0);
        CSI_CHECK("activate: forward stage-log schema materializes",
                  hs_ensure_forward_schema(pdb));
        CSI_CHECK("activate: anchor trust row seeds at the cured height",
                  hs_put_anchor(pdb, b.height));
        int32_t hstar_base = -1, served_base = -1;
        progress_store_tx_lock();
        bool base_ok = reducer_frontier_compute_hstar(pdb, &hstar_base,
                                                      &served_base);
        progress_store_tx_unlock();
        CSI_CHECK("activate: H* rests AT the anchor before any forward fold "
                  "(the wedge floor)",
                  base_ok && hstar_base == b.height);

        bool folded = true;
        for (int32_t h = b.height + 1; h <= b.height + 3; h++)
            folded = folded && hs_put_forward_height(pdb, h);
        CSI_CHECK("activate: three consistent heights fold forward", folded);
        /* Advance the reducer cursors past the folded tip (upstream count the
         * NEXT height; tip_finalize uses the served-tip frame). */
        bool cursors = hs_set_cursor(pdb, "validate_headers", b.height + 4) &&
                       hs_set_cursor(pdb, "body_fetch", b.height + 4) &&
                       hs_set_cursor(pdb, "body_persist", b.height + 4) &&
                       hs_set_cursor(pdb, "script_validate", b.height + 4) &&
                       hs_set_cursor(pdb, "proof_validate", b.height + 4) &&
                       hs_set_cursor(pdb, "utxo_apply", b.height + 4) &&
                       hs_set_cursor(pdb, "tip_finalize", b.height + 3);
        CSI_CHECK("activate: reducer cursors advance past the folded tip",
                  cursors);
        int32_t hstar_climb = -1, served_climb = -1;
        progress_store_tx_lock();
        bool climb_ok = reducer_frontier_compute_hstar(pdb, &hstar_climb,
                                                       &served_climb);
        progress_store_tx_unlock();
        CSI_CHECK("activate: H* CLIMBS past the wedge over folded bodies "
                  "(cure enables forward progress, not just forced cursors)",
                  climb_ok && hstar_climb == b.height + 3 &&
                  hstar_climb > hstar_base && served_climb == b.height + 3);
        reducer_frontier_test_set_compiled_anchor(-1);

        /* Physically restorable prior generation. */
        sqlite3 *prior = NULL;
        CSI_CHECK("activate: restorable prior generation is a valid standalone "
                  "store",
                  ares.prior_generation_path[0] != '\0' &&
                  sqlite3_open_v2(ares.prior_generation_path, &prior,
                                  SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
                  prior);
        if (prior)
            sqlite3_close(prior);
        if (ares.prior_generation_path[0])
            unlink(ares.prior_generation_path);

        /* (vii) The real replay receipt written above lifts the production
         * containment with the ZCL_TESTING hook OFF: activate consults the
         * on-disk receipt, finds it binds THIS bundle, and installs. */
        consensus_state_activate_test_force_independent_authority(false);
        struct consensus_state_activate_result ares_auth;
        CSI_CHECK("activate: a valid on-disk replay receipt lifts the production "
                  "containment (real authority, no test hook)",
                  consensus_state_snapshot_install_activate(pdb, &areq,
                                                            &ares_auth) &&
                  ares_auth.activated &&
                  ares_auth.status == CONSENSUS_INSTALL_ACTIVATED &&
                  active_is(pdb, &b));
        if (ares_auth.prior_generation_path[0])
            unlink(ares_auth.prior_generation_path);

        progress_store_close();
        test_cleanup_tmpdir(active_dir);
    }

    fixture_free(&a); fixture_free(&b); fixture_free(&inc);
    if (db)
        sqlite3_close(db);
    test_cleanup_tmpdir(dir);
    printf("=== consensus_state_snapshot_install: %d failure(s) ===\n",failures);
    return failures;
}
