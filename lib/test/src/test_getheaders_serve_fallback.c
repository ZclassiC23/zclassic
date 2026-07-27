/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_getheaders_serve_fallback — offline regression test for the
 * getheaders SERVE path on snapshot-seeded nodes (Wedge B).
 *
 * A snapshot-seeded node holds full headers in the node.db `blocks` table
 * (1344-byte Equihash nSolution included) but its hydrated in-memory block
 * index carries NO nSolution, and the flat block files below the body
 * floor are absent. Pre-fix, getheaders_index_header_servable built a
 * header with nSolutionSize=0 from such an entry, failed Equihash with
 * "invalid-solution", refused to serve, AND marked the entry
 * BLOCK_FAILED_VALID (an availability failure is not a validity verdict);
 * the successor walk then re-queried the same parent instead of advancing,
 * so the peer got a 0-header reply.
 *
 * Pins, with REAL regtest Equihash (48,5) headers mined via mine_block_pow
 * and a REAL node.db fixture behind the production node_db_runtime port
 * (db_service + app_runtime_set_current — the exact seam the live serve
 * path reads):
 *
 *   1. an index entry with no in-memory solution IS served when the
 *      node.db `blocks` row carries the full hash-bound header (fallback
 *      fires, served header hash-binds, and the index entry is healed so
 *      later serves take the in-memory hot path);
 *   2. an entry with no reachable store is refused WITHOUT gaining
 *      BLOCK_FAILED_VALID and WITHOUT a fabricated solution;
 *   3. the successor walk ADVANCES past an unservable entry and returns
 *      the next servable one instead of re-querying the same parent.
 */

#include "test/test_core.h"

#include "chain/chainparams.h"
#include "config/db_service.h"
#include "config/runtime.h"
#include "core/arith_uint256.h"
#include "core/uint256.h"
#include "mining/miner.h"
#include "models/block.h"
#include "models/database.h"
#include "net/msg_internal.h"
#include "net/msgprocessor.h"
#include "primitives/block.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdio.h>
#include <string.h>

#define GSF_CHECK(name, expr) do { \
    printf("getheaders_serve_fallback: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* Mine a consensus-valid regtest header at `height` on `prev` (mirrors
 * ph_mine_header in test_process_headers_adversarial.c). */
static bool gsf_mine_header(struct block_header *out, int height,
                            const struct uint256 *prev,
                            const struct chain_params *cp)
{
    struct block blk;
    block_init(&blk);
    blk.header.nVersion = 4;
    blk.header.hashPrevBlock = *prev;
    uint256_set_null(&blk.header.hashMerkleRoot);
    blk.header.hashMerkleRoot.data[0] = (uint8_t)height;
    uint256_set_null(&blk.header.hashFinalSaplingRoot);
    blk.header.nTime = 1600000000u + (uint32_t)height;
    struct arith_uint256 pow_limit;
    uint256_to_arith(&pow_limit, &cp->consensus.powLimit);
    blk.header.nBits = arith_uint256_get_compact(&pow_limit, false);
    bool ok = mine_block_pow(&blk, height, cp, 0);
    if (ok)
        *out = blk.header;
    block_free(&blk);
    return ok;
}

/* Store the full hash-bound header (Equihash solution included) as a
 * connected node.db `blocks` row — the row a snapshot seed has. */
static bool gsf_db_put_header(struct node_db *ndb, int height,
                              const struct block_header *h,
                              const struct uint256 *hash)
{
    if (!ndb || !h || !hash)
        return false;
    struct db_block blk;
    memset(&blk, 0, sizeof(blk));
    memcpy(blk.hash, hash->data, 32);
    blk.height = height;
    memcpy(blk.prev_hash, h->hashPrevBlock.data, 32);
    blk.version = h->nVersion;
    memcpy(blk.merkle_root, h->hashMerkleRoot.data, 32);
    blk.time = h->nTime;
    blk.bits = h->nBits;
    memcpy(blk.nonce, h->nNonce.data, 32);
    blk.solution = (uint8_t *)h->nSolution;   /* save copies; cast matches
                                               * vh_db_put_header */
    blk.solution_len = h->nSolutionSize;
    memset(blk.chain_work, 0x44, 32);
    blk.status = 3;                            /* connected floor */
    blk.num_tx = 1;
    memcpy(blk.sapling_root, h->hashFinalSaplingRoot.data, 32);
    return db_block_save(ndb, &blk);
}

/* Insert a hydrated-style index entry: every fixed header field populated
 * from the stored header, but NO nSolution (the snapshot-seed hydration
 * gap) and header-only validity (no HAVE_DATA, no FAILED bits). */
static struct block_index *gsf_seed_index(struct main_state *ms,
                                          const struct block_header *h,
                                          const struct uint256 *hash,
                                          int height,
                                          struct block_index *prev)
{
    struct block_index *bi =
        chainstate_insert_block_index((struct chainstate *)ms, hash);
    if (!bi)
        return NULL;
    bi->nHeight = height;
    bi->nVersion = h->nVersion;
    bi->hashMerkleRoot = h->hashMerkleRoot;
    bi->hashFinalSaplingRoot = h->hashFinalSaplingRoot;
    bi->nTime = h->nTime;
    bi->nBits = h->nBits;
    bi->nNonce = h->nNonce;
    bi->nStatus = BLOCK_VALID_TREE;
    bi->pprev = prev;
    return bi;
}

static struct net_manager g_gsf_nm;

int test_getheaders_serve_fallback(void);
int test_getheaders_serve_fallback(void)
{
    int failures = 0;
    printf("\n=== getheaders serve-path fallback tests ===\n");

    /* Regtest: small Equihash (48,5) mines in milliseconds. Restore
     * CHAIN_MAIN on the way out (sequential runner shares the process). */
    chain_params_select(CHAIN_REGTEST);
    const struct chain_params *cp = chain_params_get();

    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "gsf", "ok");

    /* node.db fixture behind the production port seam. */
    struct node_db ndb;
    struct db_service dbsvc;
    struct app_runtime_context runtime;
    memset(&ndb, 0, sizeof(ndb));
    memset(&dbsvc, 0, sizeof(dbsvc));
    memset(&runtime, 0, sizeof(runtime));
    GSF_CHECK("node.db fixture opens", node_db_open(&ndb, ":memory:"));
    db_service_init(&dbsvc);
    GSF_CHECK("db_service attaches", db_service_attach(&dbsvc, &ndb));
    GSF_CHECK("db_service starts", db_service_start(&dbsvc));
    runtime.db_service = &dbsvc;
    app_runtime_set_current(&runtime);

    /* Mine a 3-header chain g(0) -> A(1) -> B(2). */
    struct block_header hg, ha, hb;
    struct uint256 hash_g, hash_a, hash_b;
    struct uint256 null_hash;
    uint256_set_null(&null_hash);
    GSF_CHECK("mine g", gsf_mine_header(&hg, 0, &null_hash, cp));
    block_header_get_hash(&hg, &hash_g);
    GSF_CHECK("mine A", gsf_mine_header(&ha, 1, &hash_g, cp));
    block_header_get_hash(&ha, &hash_a);
    GSF_CHECK("mine B", gsf_mine_header(&hb, 2, &hash_a, cp));
    block_header_get_hash(&hb, &hash_b);

    struct main_state ms;
    main_state_init(&ms);
    struct block_index *bi_g = gsf_seed_index(&ms, &hg, &hash_g, 0, NULL);
    struct block_index *bi_a = gsf_seed_index(&ms, &ha, &hash_a, 1, bi_g);
    struct block_index *bi_b = gsf_seed_index(&ms, &hb, &hash_b, 2, bi_a);
    GSF_CHECK("index chain seeded", bi_g && bi_a && bi_b);
    ms.pindex_best_header = bi_b;

    /* Only B gets a node.db row: A models the entry whose store is gone. */
    GSF_CHECK("node.db row for B stored",
              gsf_db_put_header(&ndb, 2, &hb, &hash_b));

    struct msg_processor mp;
    msg_processor_init(&mp, &ms, NULL, NULL, cp, dir, &g_gsf_nm, NULL);

    /* 1. Fallback serve: no in-memory solution, node.db has the full
     *    hash-bound header -> servable, real solution, hash-bind, heal. */
    {
        struct block_header out;
        block_header_init(&out);
        bool ok = getheaders_index_header_servable(&mp, bi_b, &out);
        GSF_CHECK("fallback serves header from node.db row", ok);
        struct uint256 served_hash;
        block_header_get_hash(&out, &served_hash);
        GSF_CHECK("served header hash-binds to the index entry",
                  ok && uint256_eq(&served_hash, &hash_b));
        GSF_CHECK("served header carries the real Equihash solution",
                  ok && out.nSolutionSize == hb.nSolutionSize &&
                  out.nSolutionSize > 0 &&
                  memcmp(out.nSolution, hb.nSolution,
                         hb.nSolutionSize) == 0);
        GSF_CHECK("index entry healed with the stored solution",
                  ok && bi_b->nSolutionSize == hb.nSolutionSize);
        GSF_CHECK("served entry keeps its validity bits",
                  ok && bi_b->nStatus == BLOCK_VALID_TREE);
    }

    /* 2. Availability is not a validity verdict: A has no in-memory
     *    solution, no flat file, and no node.db row -> refuse, but do NOT
     *    mark BLOCK_FAILED_VALID and do NOT fabricate a solution. */
    {
        struct block_header out;
        block_header_init(&out);
        bool ok = getheaders_index_header_servable(&mp, bi_a, &out);
        GSF_CHECK("entry with no reachable store is refused", !ok);
        GSF_CHECK("refusal does NOT mark BLOCK_FAILED_VALID",
                  !ok && bi_a->nStatus == BLOCK_VALID_TREE);
        GSF_CHECK("refusal fabricates no in-memory solution",
                  !ok && bi_a->nSolutionSize == 0);
    }

    /* 3. The successor walk ADVANCES: from g, past unservable A, to
     *    servable B. Pre-fix the walk re-queried successor(g) — the same
     *    unservable A — until the guard gave up and returned NULL (a
     *    0-header reply to the peer). */
    {
        struct block_index *next =
            getheaders_next_servable_successor(&mp, bi_g);
        GSF_CHECK("walk advances past the unservable entry",
                  next == bi_b);
        GSF_CHECK("walk skipped entry is still not FAILED-marked",
                  next == bi_b && bi_a->nStatus == BLOCK_VALID_TREE);
    }

    app_runtime_set_current(NULL);
    db_service_stop(&dbsvc);
    node_db_close(&ndb);
    main_state_free(&ms);
    test_rm_rf(dir);
    chain_params_select(CHAIN_MAIN);

    printf("getheaders serve-path fallback tests: %s\n",
           failures ? "FAILED" : "PASSED");
    return failures;
}
