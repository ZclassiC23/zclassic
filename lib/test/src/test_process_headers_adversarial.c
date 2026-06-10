/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Adversarial tests for the inbound `headers` P2P handler
 * (lib/net/src/msg_headers.c::process_headers). A hostile peer fully
 * controls the payload bytes; these cases pin the handler's defensive
 * contract with a stub msg_processor + stack p2p_node + raw byte_stream
 * payloads (no sockets — all hooks left NULL are no-ops by design):
 *
 *   1. oversize count (> 2000)  -> rejected, peer penalized + disconnected
 *   2. truncated mid-header     -> clean failure, NO partial accept,
 *                                  no block-tree mutation, peer penalized
 *   3. valid 2-header batch with trailing garbage -> both accepted, the
 *      per-header tx-count compact-size is consumed correctly (stream
 *      stops exactly at the garbage; the garbage is never parsed)
 *   4. non-connecting batch (unknown prev) -> rejected with DoS 0:
 *      no block-tree mutation, no peer penalty (orphans are normal)
 *
 * Headers for case 3/4 are REAL regtest Equihash (48,5) blocks mined via
 * mine_block_pow, so accept_block_header's PoW gate runs for real. */

#include "test/test_helpers.h"

#include "mining/miner.h"
#include "net/msg_internal.h"
#include "net/msgprocessor.h"
#include "net/peer_scoring.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define PH_CHECK(name, expr) do { \
    printf("process_headers_adversarial: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* Non-localhost peer (is_trusted_peer matches the IPv4-mapped 127/8
 * prefix at ip[10..12]; use 1.2.3.4) so peer_misbehaving scores it. */
static void ph_setup_node(struct p2p_node *node)
{
    memset(node, 0, sizeof(*node));
    snprintf(node->addr_name, sizeof(node->addr_name), "203.0.113.9:8033");
    node->id = 9;
    node->addr.svc.addr.ip[10] = 0xff;
    node->addr.svc.addr.ip[11] = 0xff;
    node->addr.svc.addr.ip[12] = 1;
    node->addr.svc.addr.ip[13] = 2;
    node->addr.svc.addr.ip[14] = 3;
    node->addr.svc.addr.ip[15] = 4;
}

/* Mine a consensus-valid regtest header at `height` on `prev`: PoW-true
 * Equihash witness + hash <= powLimit target. Merkle root is arbitrary —
 * header acceptance never inspects it. */
static bool ph_mine_header(struct block_header *out, int height,
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

/* Serialize one wire `headers` element: header bytes + tx-count 0. */
static bool ph_write_header(struct byte_stream *s,
                            const struct block_header *hdr)
{
    return block_header_serialize(hdr, s) &&
           stream_write_compact_size(s, 0);
}

static struct net_manager g_ph_nm;

int test_process_headers_adversarial(void);
int test_process_headers_adversarial(void)
{
    int failures = 0;
    printf("\n=== process_headers adversarial tests ===\n");

    /* Regtest: small Equihash (48,5) mines in milliseconds. Restore
     * CHAIN_MAIN on the way out (sequential runner shares the process). */
    chain_params_select(CHAIN_REGTEST);
    const struct chain_params *cp = chain_params_get();
    peer_scoring_init();
    enum sync_state sync0 = sync_get_state();

    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "ph_adversarial", "main");
    SetDataDir(dir); /* hermetic datadir for mp->datadir resolution */

    struct main_state ms;
    main_state_init(&ms);
    struct uint256 gh = cp->consensus.hashGenesisBlock;
    struct block_index *gen =
        chainstate_insert_block_index((struct chainstate *)&ms, &gh);
    PH_CHECK("genesis block_index inserted", gen != NULL);
    if (gen) {
        gen->nHeight = 0;
        gen->nStatus = BLOCK_HAVE_DATA | BLOCK_VALID_SCRIPTS;
        gen->nTx = 1;
        gen->nChainTx = 1;
        active_chain_move_window_tip(&ms.chain_active, gen);
        ms.pindex_best_header = gen;
    }

    memset(&g_ph_nm, 0, sizeof(g_ph_nm));
    struct msg_processor mp;
    msg_processor_init(&mp, &ms, NULL, NULL, cp, dir, &g_ph_nm, NULL);

    struct p2p_node node;
    ph_setup_node(&node);

    /* ── 1. oversize count (> 2000): reject + penalize + disconnect ── */
    if (gen) {
        size_t map0 = ms.map_block_index.size;
        struct msg_headers_stats st0, st1;
        msg_headers_get_stats(&st0);
        struct byte_stream s;
        stream_init(&s, 64);
        stream_write_compact_size(&s, 2001);
        bool ret = process_headers(&mp, &node, &s);
        msg_headers_get_stats(&st1);
        PH_CHECK("oversize: handler returns false", ret == false);
        PH_CHECK("oversize: peer flagged for disconnect",
                 node.disconnect == true);
        PH_CHECK("oversize: peer penalized",
                 atomic_load(&node.misbehavior) > 0);
        PH_CHECK("oversize: no block-tree mutation",
                 ms.map_block_index.size == map0);
        PH_CHECK("oversize: no batch counted",
                 st1.batches_received == st0.batches_received);
        stream_free(&s);
    }

    /* ── 2. truncated mid-header: clean failure, no partial accept ── */
    if (gen) {
        node.disconnect = false;
        atomic_store(&node.misbehavior, 0);
        size_t map0 = ms.map_block_index.size;
        struct msg_headers_stats st0, st1;
        msg_headers_get_stats(&st0);
        struct byte_stream s;
        stream_init(&s, 64);
        stream_write_compact_size(&s, 2); /* promises 2 headers... */
        unsigned char garbage[20];
        memset(garbage, 0xab, sizeof(garbage));
        stream_write_bytes(&s, garbage, sizeof(garbage)); /* ...delivers 20B */
        bool ret = process_headers(&mp, &node, &s);
        msg_headers_get_stats(&st1);
        PH_CHECK("truncated: handler returns false", ret == false);
        PH_CHECK("truncated: peer penalized",
                 atomic_load(&node.misbehavior) > 0);
        PH_CHECK("truncated: no block-tree mutation",
                 ms.map_block_index.size == map0);
        PH_CHECK("truncated: nothing accepted",
                 st1.total_accepted == st0.total_accepted &&
                 st1.batches_received == st0.batches_received);
        stream_free(&s);
    }

    /* ── 3. valid 2-header batch + trailing garbage ── */
    struct block_header h1, h2;
    struct uint256 h1_hash;
    bool mined = gen &&
                 ph_mine_header(&h1, 1, &gh, cp);
    if (mined) {
        block_header_get_hash(&h1, &h1_hash);
        mined = ph_mine_header(&h2, 2, &h1_hash, cp);
    }
    PH_CHECK("two connecting regtest headers mined", mined);
    if (mined) {
        node.disconnect = false;
        atomic_store(&node.misbehavior, 0);
        struct msg_headers_stats st0, st1;
        msg_headers_get_stats(&st0);
        struct byte_stream s;
        stream_init(&s, 1024);
        stream_write_compact_size(&s, 2);
        bool wrote = ph_write_header(&s, &h1) && ph_write_header(&s, &h2);
        unsigned char garbage[16];
        memset(garbage, 0xcd, sizeof(garbage));
        wrote = wrote && stream_write_bytes(&s, garbage, sizeof(garbage));
        PH_CHECK("batch payload built", wrote);
        size_t payload_end = s.size - sizeof(garbage);
        bool ret = process_headers(&mp, &node, &s);
        msg_headers_get_stats(&st1);
        PH_CHECK("valid batch: handler returns true", ret == true);
        PH_CHECK("valid batch: both headers accepted",
                 st1.total_accepted == st0.total_accepted + 2 &&
                 st1.newly_added == st0.newly_added + 2);
        struct uint256 h2_hash;
        block_header_get_hash(&h2, &h2_hash);
        struct block_index *bi1 = block_map_find(&ms.map_block_index, &h1_hash);
        struct block_index *bi2 = block_map_find(&ms.map_block_index, &h2_hash);
        PH_CHECK("valid batch: both in block tree at h=1,2",
                 bi1 && bi1->nHeight == 1 && bi2 && bi2->nHeight == 2);
        PH_CHECK("valid batch: compact-size consumed exactly "
                 "(garbage never parsed)",
                 s.read_pos == payload_end);
        PH_CHECK("valid batch: best header promoted to h=2",
                 ms.pindex_best_header == bi2);
        PH_CHECK("valid batch: honest peer not penalized",
                 atomic_load(&node.misbehavior) == 0 && !node.disconnect);
        stream_free(&s);
    }

    /* ── 4. non-connecting batch: reject, DoS 0, no tree mutation ── */
    if (gen) {
        struct uint256 unknown_prev;
        memset(unknown_prev.data, 0xee, 32);
        struct block_header orphan;
        bool orphan_mined = ph_mine_header(&orphan, 1, &unknown_prev, cp);
        PH_CHECK("orphan header mined", orphan_mined);
        if (orphan_mined) {
            node.disconnect = false;
            atomic_store(&node.misbehavior, 0);
            size_t map0 = ms.map_block_index.size;
            struct msg_headers_stats st0, st1;
            msg_headers_get_stats(&st0);
            struct byte_stream s;
            stream_init(&s, 1024);
            stream_write_compact_size(&s, 1);
            PH_CHECK("orphan payload built", ph_write_header(&s, &orphan));
            bool ret = process_headers(&mp, &node, &s);
            msg_headers_get_stats(&st1);
            PH_CHECK("non-connecting: handler completes", ret == true);
            struct uint256 ohash;
            block_header_get_hash(&orphan, &ohash);
            PH_CHECK("non-connecting: header NOT added to block tree",
                     block_map_find(&ms.map_block_index, &ohash) == NULL &&
                     ms.map_block_index.size == map0);
            PH_CHECK("non-connecting: counted as rejected",
                     st1.total_rejected == st0.total_rejected + 1);
            PH_CHECK("non-connecting: DoS 0 — orphan peer NOT penalized",
                     atomic_load(&node.misbehavior) == 0 && !node.disconnect);
            stream_free(&s);
        }
    }

    sync_set_state(sync0, "process_headers_adversarial restore");
    main_state_free(&ms);
    SetDataDir("");
    ClearDataDirCache();
    test_rm_rf(dir);
    chain_params_select(CHAIN_MAIN);

    printf("process_headers adversarial tests: %s\n",
           failures ? "FAILED" : "PASSED");
    return failures;
}
