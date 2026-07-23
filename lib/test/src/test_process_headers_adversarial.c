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
#include "validation/chainstate.h"
#include "validation/process_block.h"  /* accept_block_header */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define PH_CHECK(name, expr) do { \
    printf("process_headers_adversarial: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* Snapshot-active stub for the push_getheaders_from suppression cases. */
static bool ph_snapshot_active_true(void *ctx) { (void)ctx; return true; }

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

/* Case 5 authority shims: replay the EXACT inconsistent authority pair the
 * 2026-06-11 forensic found at the finalize frontier — height = tip-1 while
 * the hash resolves to the tip block itself. */
static int64_t g_ph_auth_height = -1;
static uint8_t g_ph_auth_hash[32];
static bool ph_auth_is_authoritative(void) { return true; }
static int64_t ph_auth_get_height(void) { return g_ph_auth_height; }
static bool ph_auth_get_hash(uint8_t out[32])
{
    memcpy(out, g_ph_auth_hash, 32);
    return true;
}

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

    /* ── 5. tip-header re-delivery must NOT relabel heights (the 2026-06-11
     *       height-splice regression). Re-use h1/h2 from case 3 (accepted at
     *       h=1,2). Serve h2 as the window tip and register an authority
     *       publishing the INCONSISTENT pair the forensic captured at the
     *       finalize frontier (height = tip-1, hash = tip). The deleted
     *       label-trust install in accept_block_header would re-height the
     *       tip 2->1 and rewrite its parent 1->0, cascading a -1 splice over
     *       every header above; the derive-from-parent rule must leave the
     *       graph untouched and a successor must still land at parent+1. */
    if (mined) {
        struct uint256 h2_hash;
        block_header_get_hash(&h2, &h2_hash);
        struct block_index *bi1 = block_map_find(&ms.map_block_index, &h1_hash);
        struct block_index *bi2 = block_map_find(&ms.map_block_index, &h2_hash);
        PH_CHECK("relabel: accepted tip + parent present", bi1 && bi2);
        if (bi1 && bi2) {
            PH_CHECK("relabel: window tip installed at h=2",
                     active_chain_move_window_tip(&ms.chain_active, bi2));
            g_ph_auth_height = bi2->nHeight - 1;       /* the poisoned label */
            memcpy(g_ph_auth_hash, h2_hash.data, 32);  /* ...of the tip hash */
            struct active_chain_authority poisoned = {
                .get_height = ph_auth_get_height,
                .get_hash = ph_auth_get_hash,
                .is_authoritative = ph_auth_is_authoritative,
            };
            active_chain_register_authority(&poisoned);
            active_chain_register_block_map(&ms.map_block_index);
            PH_CHECK("relabel: simulated pair is the inconsistent one",
                     active_chain_height(&ms.chain_active) == 1 &&
                     active_chain_tip(&ms.chain_active) == bi2);

            struct validation_state state;
            validation_state_init(&state);
            struct block_index *re = NULL;
            bool ok = accept_block_header(&h2, &state, &ms, cp, &re);
            PH_CHECK("relabel: re-delivered tip header accepted",
                     ok && re == bi2);
            PH_CHECK("relabel: tip nHeight NOT mutated", bi2->nHeight == 2);
            PH_CHECK("relabel: parent nHeight NOT mutated", bi1->nHeight == 1);
            PH_CHECK("relabel: links intact",
                     bi2->pprev == bi1 && bi1->pprev == gen &&
                     gen->nHeight == 0);

            /* Overlapping successor batch: a child above the re-delivered
             * tip still derives parent+1, never authority-label+1. */
            struct block_header h3;
            bool m3 = ph_mine_header(&h3, 3, &h2_hash, cp);
            PH_CHECK("relabel: successor mined", m3);
            if (m3) {
                struct validation_state st3;
                validation_state_init(&st3);
                struct block_index *bi3 = NULL;
                bool ok3 = accept_block_header(&h3, &st3, &ms, cp, &bi3);
                PH_CHECK("relabel: successor derives parent+1",
                         ok3 && bi3 && bi3->nHeight == 3 &&
                         bi3->pprev == bi2);
                PH_CHECK("relabel: graph heights unchanged after batch",
                         bi1->nHeight == 1 && bi2->nHeight == 2);
            }

            /* Restore globals for the sequential in-process runner. */
            struct active_chain_authority none = {0};
            active_chain_register_authority(&none);
            active_chain_register_block_map(NULL);
        }
    }

    /* ── 6. push_getheaders_from continuation-suppression must be LOUD +
     *       COUNTED, never a silent header-sync stop (the header-continuation
     *       wedge class). Uses a fresh EMPTY main_state so the null-hash
     *       re-anchor finds no hashed frontier and returns after counting,
     *       and the snapshot guard returns before touching the send path —
     *       both stay off the node send mutex. */
    {
        struct main_state ms2;
        main_state_init(&ms2);
        struct msg_processor mp2;
        msg_processor_init(&mp2, &ms2, NULL, NULL, cp, dir, &g_ph_nm, NULL);

        /* (a) null-hash anchor: counted no-hash suppression (pre-fix this
         *     was a silent `return;` that killed the continuation). */
        {
            struct block_index ghost;
            memset(&ghost, 0, sizeof(ghost));
            ghost.nHeight = 5;
            ghost.phashBlock = NULL;   /* stable hash slot never populated */
            struct msg_headers_stats a, b;
            msg_headers_get_stats(&a);
            push_getheaders_from(&mp2, &node, &ghost);
            msg_headers_get_stats(&b);
            PH_CHECK("null-hash anchor: counted (no silent continuation drop)",
                     b.getheaders_suppressed_no_hash ==
                         a.getheaders_suppressed_no_hash + 1);
        }

        /* (b) active snapshot exchange: counted snapshot suppression (pre-fix
         *     this was the silent latch that wedged header sync after one
         *     in-flight batch). */
        {
            struct msg_headers_stats a, b;
            msg_headers_get_stats(&a);
            msg_processor_set_snapshot_active(&mp2, ph_snapshot_active_true,
                                              NULL);
            push_getheaders_from(&mp2, &node, NULL);
            msg_processor_set_snapshot_active(&mp2, NULL, NULL);
            msg_headers_get_stats(&b);
            PH_CHECK("snapshot active: counted getheaders suppression",
                     b.getheaders_suppressed_snapshot ==
                         a.getheaders_suppressed_snapshot + 1);
        }

        main_state_free(&ms2);
    }

    /* ── 7. sibling silent-drop sites in the same file, same defect class:
     *       inbound `headers` dropped receive-side, push_getheaders() and
     *       push_getheaders_span() dropped send-side while a snapshot
     *       exchange owns the wire, and the getheaders-serving defer while
     *       a peer snapshot transfer is in progress — all must count and
     *       (rising-edge) log instead of silently returning. */
    {
        struct main_state ms3;
        main_state_init(&ms3);
        struct msg_processor mp3;
        msg_processor_init(&mp3, &ms3, NULL, NULL, cp, dir, &g_ph_nm, NULL);

        /* (a) process_headers: inbound headers dropped while a snapshot
         *     exchange is active — counted, never a silent stop. */
        {
            struct msg_headers_stats a, b;
            msg_headers_get_stats(&a);
            msg_processor_set_snapshot_active(&mp3, ph_snapshot_active_true,
                                              NULL);
            struct byte_stream s;
            stream_init(&s, 8);
            bool ret = process_headers(&mp3, &node, &s);
            msg_processor_set_snapshot_active(&mp3, NULL, NULL);
            stream_free(&s);
            msg_headers_get_stats(&b);
            PH_CHECK("process_headers: snapshot-active drop counted",
                     ret == true &&
                     b.headers_recv_suppressed_snapshot ==
                         a.headers_recv_suppressed_snapshot + 1);
        }

        /* (b) push_getheaders: send-side request suppressed while a
         *     snapshot exchange is active — counted. */
        {
            struct msg_headers_stats a, b;
            msg_headers_get_stats(&a);
            msg_processor_set_snapshot_active(&mp3, ph_snapshot_active_true,
                                              NULL);
            push_getheaders(&mp3, &node);
            msg_processor_set_snapshot_active(&mp3, NULL, NULL);
            msg_headers_get_stats(&b);
            PH_CHECK("push_getheaders: snapshot-active drop counted",
                     b.push_getheaders_suppressed_snapshot ==
                         a.push_getheaders_suppressed_snapshot + 1);
        }

        /* (c) push_getheaders_span: span request suppressed while a
         *     snapshot exchange is active — counted. */
        {
            struct msg_headers_stats a, b;
            msg_headers_get_stats(&a);
            msg_processor_set_snapshot_active(&mp3, ph_snapshot_active_true,
                                              NULL);
            push_getheaders_span(&mp3, &node, &gh, NULL);
            msg_processor_set_snapshot_active(&mp3, NULL, NULL);
            msg_headers_get_stats(&b);
            PH_CHECK("push_getheaders_span: snapshot-active drop counted",
                     b.push_getheaders_span_suppressed_snapshot ==
                         a.push_getheaders_span_suppressed_snapshot + 1);
        }

        /* (d) process_getheaders: request deferred while we are serving a
         *     snapshot to this peer — counted (was a bare printf). */
        {
            struct msg_headers_stats a, b;
            msg_headers_get_stats(&a);
            bool saved = node.swarm_manifest_sent;
            node.swarm_manifest_sent = true;
            struct byte_stream s;
            stream_init(&s, 8);
            bool ret = process_getheaders(&mp3, &node, &s);
            stream_free(&s);
            node.swarm_manifest_sent = saved;
            msg_headers_get_stats(&b);
            PH_CHECK("process_getheaders: snapshot-serving defer counted",
                     ret == true &&
                     b.getheaders_deferred_snapshot_serving ==
                         a.getheaders_deferred_snapshot_serving + 1);
        }

        main_state_free(&ms3);
    }

    /* ── 8. getheaders SERVE side: the reply must stay under the 2 MiB wire
     *       cap. ~2000 Equihash headers (1344-byte solution ≈ 1.5 KB each)
     *       serialize to ~2.9 MB > MAX_PROTOCOL_MESSAGE_LENGTH, and the peer
     *       drops the whole oversized reply. getheaders_try_append_header()
     *       bounds the batch by bytes so the framed reply always fits. */
    {
        struct block_header big;
        block_header_init(&big);
        big.nBits = 0x1f07ffff;
        big.nSolutionSize = MAX_SOLUTION_SIZE;   /* mainnet Equihash 200,9 */
        memset(big.nSolution, 0xab, MAX_SOLUTION_SIZE);

        struct byte_stream body;
        stream_init(&body, 1u << 20);
        int served = 0;
        bool stopped_by_cap = false;
        for (int i = 0; i < 2000; i++) {
            if (!getheaders_try_append_header(&body, &big)) {
                stopped_by_cap = true;
                break;
            }
            served++;
        }
        size_t framed = compact_size_sizeof((uint64_t)served) + body.size;
        PH_CHECK("getheaders serve: byte cap stops the batch before count 2000",
                 stopped_by_cap && served > 0 && served < 2000);
        PH_CHECK("getheaders serve: framed reply within wire cap",
                 framed <= (size_t)MAX_PROTOCOL_MESSAGE_LENGTH);
        PH_CHECK("getheaders serve: one more header would overflow (tight)",
                 body.size + 1488u + 16u > (size_t)MAX_PROTOCOL_MESSAGE_LENGTH);
        stream_free(&body);
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
