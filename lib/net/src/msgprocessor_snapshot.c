/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Snapshot / fast-sync / swarm message family.
 *
 * This file owns the substantial state that ZCL23-only sync uses:
 *   - cached snapshot offer (g_cached_offer, mutex)
 *   - cached UTXO sync manifest (g_cached_manifest, mutex)
 *   - cached block piece manifest (g_cached_block_manifest, mutex)
 *   - parallel UTXO swarm coordinator (g_swarm, mutex)
 *   - parallel block swarm coordinator (g_block_swarm, mutex)
 *   - per-peer FlyClient challenge rate limiter (g_fc_rate_table)
 *
 * It implements:
 *   - the public msg_processor_*_offer / *_manifest APIs declared in
 *     net/msgprocessor.h (callers in boot.c keep working as before)
 *   - send_snapshot_offer_msg, push_manifest, push_block_manifest,
 *     push_chunk_request, push_block_piece_request (the producers)
 *   - mp_handle_zcl23_sync (the receiver dispatcher for every z-prefixed
 *     command not in the standard dispatch table)
 *   - mp_snapshot_init / mp_snapshot_send_tick / mp_snapshot_maybe_offer
 *     (lifecycle / per-peer trickle hooks invoked from msgprocessor.c) */

#include "platform/time_compat.h"
#include "msgprocessor_internal.h"

#include "net/addrman.h"
#include "net/fast_sync.h"
#include "net/flyclient.h"
#include "net/peer_scoring.h"
#include "net/peer_lifecycle.h"
#include "net/file_service.h"
#include "coins/coins_view.h"
#include "net/snapshot_sync_contract.h"
#include "validation/main_state.h"
#include "util/safe_alloc.h"
#include "util/log_macros.h"
#include "util/sync.h"
#include "event/event.h"
#include "sync/sync_state.h"
#include "core/uint256.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Cached snapshot offer — pre-computed at startup, not in message handler.
 * Protected by g_offer_mutex to prevent struct tearing between the
 * background build thread (boot.c) and the P2P message handler. */
static struct snapshot_offer g_cached_offer;
static _Atomic bool g_cached_offer_valid = false;
static _Atomic uint64_t g_cached_offer_version = 0;
static pthread_mutex_t g_offer_mutex = PTHREAD_MUTEX_INITIALIZER;
struct fast_sync_rate_limiter g_rate_limiter = {0};

/* Cached manifest for parallel chunk sync (built in background at startup). */
struct sync_manifest g_cached_manifest;
_Atomic bool g_cached_manifest_valid = false;
static _Atomic uint64_t g_cached_manifest_version = 0;
static _Atomic uint32_t g_cached_manifest_num_chunks = 0;
static pthread_mutex_t g_manifest_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Global swarm coordinator — manages parallel UTXO chunk download.
 * Only active when we are syncing from multiple ZCL23 peers.
 * All access to g_swarm fields protected by g_swarm_mutex. */
static struct swarm_sync g_swarm __attribute__((used));
static _Atomic bool g_swarm_active = false;
static pthread_mutex_t g_swarm_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Snapshot sync service — global singleton in snapshot_sync_service.c */
static int64_t g_swarm_last_progress_time = 0;

/* Timeout for inflight chunk requests (30 seconds). */
#define SWARM_CHUNK_TIMEOUT_SECS 30

/* Progress display interval (5 seconds). */
#define SWARM_PROGRESS_INTERVAL_SECS 5

struct snapshot_sync_service *msg_snapshot_sync(
    const struct msg_processor *mp)
{
    if (mp && mp->runtime && mp->runtime->snapshot_sync)
        return mp->runtime->snapshot_sync;
    if (snapsync_global_initialized())
        return snapsync_global();
    LOG_NULL("net", "no snapshot sync service available");
}

struct snapshot_sync_service *msg_snapshot_sync_ensure(
    const struct msg_processor *mp)
{
    struct snapshot_sync_service *svc = msg_snapshot_sync(mp);
    struct node_db *ndb;

    if (svc)
        return svc;
    ndb = msg_node_db(mp);
    if (!ndb)
        LOG_NULL("net", "node_db unavailable for snapshot sync init");
    snapsync_global_ensure_init(ndb);
    return snapsync_global();
}

/* ── Block swarm: parallel block download coordinator ───────── */
/* Manages BitTorrent-style block piece download across multiple
 * ZCL23 peers. Legacy peers contribute blocks via normal getdata/block
 * which the coordinator assembles into verified pieces. */
static struct block_swarm g_block_swarm __attribute__((used));
static _Atomic bool g_block_swarm_active = false;
static pthread_mutex_t g_block_swarm_mutex = PTHREAD_MUTEX_INITIALIZER;
static int64_t g_block_swarm_last_progress = 0;

/* Cached block piece manifest (built in background).
 * Non-static: accessed from boot.c via extern. */
struct block_piece_manifest g_cached_block_manifest;
_Atomic bool g_cached_block_manifest_valid = false;
int32_t g_manifest_built_at_height = 0; /* height when manifest was last built */
static _Atomic uint64_t g_cached_block_manifest_version = 0;
static pthread_mutex_t g_block_manifest_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Rebuild manifest when chain grows this many blocks beyond the cached one.
 * This ensures new peers always get a reasonably fresh manifest. */
#define MANIFEST_REFRESH_BLOCKS 1000

/* per-peer FlyClient challenge rate limit.
 *
 * Every zfcchallenge forces snapsync_build_fc_response() to reconstruct
 * 50 MMB proofs over our full block index, which on a ~3M-block chain
 * pins a CPU for tens of milliseconds. Left unchecked, a single hostile
 * peer can throttle header sync for everyone — 1000 challenges/sec and
 * the message thread falls behind its queue.
 *
 * We use a small side table (no changes to struct p2p_node) mapping
 * node_id → token bucket. FC_CHALLENGE_RATE_PER_SEC tokens/sec refill,
 * burst FC_CHALLENGE_BURST — a legit IBD client issues exactly one
 * challenge per snapshot offer, so this comfortably absorbs reconnect
 * churn while still dropping sustained floods. When the bucket hits
 * zero we drop silently (no proof reply) and register PEER_OFFENCE_FLOOD
 * once per flood episode (resets when the peer next consumes a token),
 * so the sustained offender eventually crosses the ban threshold
 * without legitimate bursts costing anyone. Table size is fixed, and
 * an LRU slot is reused when full — a peer churning connections can't
 * force unbounded memory growth. Constants exposed via msgprocessor.h
 * so tests and operators can see the chosen thresholds. */
#define FC_RATE_TABLE_SIZE        64u

struct fc_rate_entry {
    bool      in_use;
    node_id_t peer_id;
    int64_t   refill_time_ms;
    uint32_t  tokens;
    uint32_t  dropped_count;
    bool      flood_scored;
};

static struct fc_rate_entry g_fc_rate_table[FC_RATE_TABLE_SIZE];
static pthread_mutex_t g_fc_rate_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Acquire the token for peer_id at now_ms, returning true on success.
 * New entries start full (burst=30) so a freshly-connected peer can
 * issue its single legitimate challenge without waiting. On a miss we
 * bump dropped_count and keep flood_scored pinned at true until the
 * next successful consume, so ban-score accrues once per flood episode
 * not per dropped challenge. */
static bool fc_rate_acquire(node_id_t peer_id, int64_t now_ms)
{
    pthread_mutex_lock(&g_fc_rate_mutex);

    struct fc_rate_entry *e = NULL;
    struct fc_rate_entry *lru = &g_fc_rate_table[0];

    for (size_t i = 0; i < FC_RATE_TABLE_SIZE; i++) {
        struct fc_rate_entry *slot = &g_fc_rate_table[i];
        if (slot->in_use && slot->peer_id == peer_id) { e = slot; break; }
        if (!slot->in_use || slot->refill_time_ms < lru->refill_time_ms)
            lru = slot;
    }

    if (!e) {
        e = lru;
        e->in_use = true;
        e->peer_id = peer_id;
        e->tokens = FC_CHALLENGE_BURST;
        e->refill_time_ms = now_ms;
        e->dropped_count = 0;
        e->flood_scored = false;
    } else {
        int64_t elapsed = now_ms - e->refill_time_ms;
        if (elapsed > 0) {
            uint64_t refill = ((uint64_t)elapsed
                * FC_CHALLENGE_RATE_PER_SEC) / 1000u;
            if (refill > 0) {
                uint64_t cap = (uint64_t)e->tokens + refill;
                if (cap > FC_CHALLENGE_BURST) cap = FC_CHALLENGE_BURST;
                e->tokens = (uint32_t)cap;
                e->refill_time_ms = now_ms;
            }
        }
    }

    bool granted = e->tokens > 0;
    if (granted) {
        e->tokens--;
        e->flood_scored = false;
    } else {
        e->dropped_count++;
    }
    pthread_mutex_unlock(&g_fc_rate_mutex);
    return granted;
}

/* Returns true the first time this function is called after a peer's
 * bucket empties, letting the caller register a single PEER_OFFENCE_FLOOD
 * per episode; subsequent calls within the same flood return false.
 * dropped_out is set to the current lifetime drop count for logging. */
static bool fc_rate_should_score(node_id_t peer_id, uint32_t *dropped_out)
{
    bool should = false;
    pthread_mutex_lock(&g_fc_rate_mutex);
    for (size_t i = 0; i < FC_RATE_TABLE_SIZE; i++) {
        struct fc_rate_entry *slot = &g_fc_rate_table[i];
        if (slot->in_use && slot->peer_id == peer_id) {
            if (!slot->flood_scored) {
                slot->flood_scored = true;
                should = true;
            }
            if (dropped_out) *dropped_out = slot->dropped_count;
            break;
        }
    }
    pthread_mutex_unlock(&g_fc_rate_mutex);
    return should;
}

/* Test-only handles. Declared in msgprocessor.h so the test harness can
 * drive the rate limiter with an explicit clock (real tests run faster
 * than peer_scoring_now_ms() can resolve) and then clear it between
 * cases without pulling in the whole P2P stack. */
bool msgprocessor_test_fc_rate_acquire(node_id_t peer_id, int64_t now_ms)
{
    return fc_rate_acquire(peer_id, now_ms);
}

uint32_t msgprocessor_test_fc_rate_dropped(node_id_t peer_id)
{
    uint32_t out = 0;
    pthread_mutex_lock(&g_fc_rate_mutex);
    for (size_t i = 0; i < FC_RATE_TABLE_SIZE; i++) {
        struct fc_rate_entry *slot = &g_fc_rate_table[i];
        if (slot->in_use && slot->peer_id == peer_id) {
            out = slot->dropped_count;
            break;
        }
    }
    pthread_mutex_unlock(&g_fc_rate_mutex);
    return out;
}

bool msgprocessor_test_fc_rate_should_score(node_id_t peer_id)
{
    return fc_rate_should_score(peer_id, NULL);
}

void msgprocessor_test_fc_rate_reset(void)
{
    pthread_mutex_lock(&g_fc_rate_mutex);
    memset(g_fc_rate_table, 0, sizeof(g_fc_rate_table));
    pthread_mutex_unlock(&g_fc_rate_mutex);
}

/* ── test hooks: g_swarm_active CAS drive ─────────────────
 * Expose the exact atomic primitive used by the zmanifest handler
 * so test_net.c can exercise the no-race / concurrent / reset-cycle
 * paths without a full peer-handshake setup. Body-for-body identical
 * to the production call sites around lines 2218 and 2398. */
bool msgprocessor_test_swarm_try_claim(void)
{
    bool expected = false;
    return atomic_compare_exchange_strong(&g_swarm_active,
                                          &expected, true);
}

void msgprocessor_test_swarm_release(void)
{
    atomic_store(&g_swarm_active, false);
}

bool msgprocessor_test_swarm_is_active(void)
{
    return atomic_load(&g_swarm_active);
}

static void msg_manifest_reset(struct sync_manifest *manifest)
{
    if (!manifest)
        return;
    sync_manifest_free(manifest);
    memset(manifest, 0, sizeof(*manifest));
}

static void msg_block_manifest_reset(struct block_piece_manifest *manifest)
{
    if (!manifest)
        return;
    block_piece_manifest_free(manifest);
    memset(manifest, 0, sizeof(*manifest));
}

static bool msg_manifest_is_reasonable(const struct sync_manifest *manifest)
{
    if (!manifest)
        LOG_FAIL("net", "manifest is NULL");
    if (manifest->num_chunks == 0)
        LOG_FAIL("net", "manifest has zero chunks");
    if (manifest->chunk_size == 0)
        LOG_FAIL("net", "manifest has zero chunk_size");
    if (!manifest->chunk_hashes)
        LOG_FAIL("net", "manifest chunk_hashes is NULL");
    return true;
}

static bool msg_block_manifest_is_reasonable(
    const struct block_piece_manifest *manifest)
{
    if (!manifest)
        LOG_FAIL("net", "block manifest is NULL");
    if (manifest->num_pieces == 0)
        LOG_FAIL("net", "block manifest has zero pieces");
    if (manifest->start_height > manifest->end_height)
        LOG_FAIL("net", "block manifest start_height %d > end_height %d",
                 manifest->start_height, manifest->end_height);
    if (!manifest->piece_hashes)
        LOG_FAIL("net", "block manifest piece_hashes is NULL");
    return true;
}

/* Thread-safe accessor: update cached snapshot offer from boot.c */
void msg_processor_update_offer(const struct snapshot_offer *offer)
{
    if (!offer)
        return;
    pthread_mutex_lock(&g_offer_mutex);
    g_cached_offer = *offer;
    atomic_store(&g_cached_offer_valid, true);
    g_cached_offer_version++;
    pthread_mutex_unlock(&g_offer_mutex);
}

bool msg_processor_get_offer(struct snapshot_offer *offer)
{
    if (!offer)
        LOG_FAIL("net", "offer output pointer is NULL");

    pthread_mutex_lock(&g_offer_mutex);
    bool ok = atomic_load(&g_cached_offer_valid);
    if (ok)
        *offer = g_cached_offer;
    else
        memset(offer, 0, sizeof(*offer));
    pthread_mutex_unlock(&g_offer_mutex);
    return ok;
}

void msg_processor_invalidate_offer(void)
{
    pthread_mutex_lock(&g_offer_mutex);
    memset(&g_cached_offer, 0, sizeof(g_cached_offer));
    atomic_store(&g_cached_offer_valid, false);
    g_cached_offer_version++;
    pthread_mutex_unlock(&g_offer_mutex);
}

uint64_t msg_processor_offer_cache_version(void)
{
    pthread_mutex_lock(&g_offer_mutex);
    uint64_t version = g_cached_offer_version;
    pthread_mutex_unlock(&g_offer_mutex);
    return version;
}

bool msg_processor_publish_manifest(struct sync_manifest *manifest)
{
    if (!msg_manifest_is_reasonable(manifest))
        LOG_FAIL("net", "cannot publish unreasonable manifest");

    pthread_mutex_lock(&g_manifest_mutex);
    if (atomic_load(&g_cached_manifest_valid))
        msg_manifest_reset(&g_cached_manifest);
    g_cached_manifest = *manifest;
    atomic_store(&g_cached_manifest_num_chunks, g_cached_manifest.num_chunks);
    memset(manifest, 0, sizeof(*manifest));
    atomic_store(&g_cached_manifest_valid, true);
    g_cached_manifest_version++;
    pthread_mutex_unlock(&g_manifest_mutex);
    return true;
}

void msg_processor_invalidate_manifest(void)
{
    pthread_mutex_lock(&g_manifest_mutex);
    if (atomic_load(&g_cached_manifest_valid))
        msg_manifest_reset(&g_cached_manifest);
    atomic_store(&g_cached_manifest_valid, false);
    atomic_store(&g_cached_manifest_num_chunks, 0);
    g_cached_manifest_version++;
    pthread_mutex_unlock(&g_manifest_mutex);
}

uint64_t msg_processor_manifest_cache_version(void)
{
    pthread_mutex_lock(&g_manifest_mutex);
    uint64_t version = g_cached_manifest_version;
    pthread_mutex_unlock(&g_manifest_mutex);
    return version;
}

bool msg_processor_get_manifest_header(struct sync_manifest *out)
{
    if (!out)
        LOG_FAIL("net", "manifest output pointer is NULL");

    pthread_mutex_lock(&g_manifest_mutex);
    bool ok = atomic_load(&g_cached_manifest_valid);
    if (ok) {
        *out = g_cached_manifest;
        out->chunk_hashes = NULL;
    } else {
        memset(out, 0, sizeof(*out));
    }
    pthread_mutex_unlock(&g_manifest_mutex);
    return ok;
}

bool msg_processor_copy_manifest_hashes(uint8_t (**out_hashes)[32],
                                        uint32_t *out_count)
{
    if (!out_hashes || !out_count)
        LOG_FAIL("net", "copy_manifest_hashes: NULL output");
    *out_hashes = NULL;
    *out_count = 0;

    pthread_mutex_lock(&g_manifest_mutex);
    bool ok = atomic_load(&g_cached_manifest_valid)
              && g_cached_manifest.chunk_hashes
              && g_cached_manifest.num_chunks > 0
              && g_cached_manifest.num_chunks <= MANIFEST_MAX_CHUNKS;
    if (ok) {
        uint32_t n = g_cached_manifest.num_chunks;
        uint8_t (*copy)[32] = zcl_calloc(n, 32, "manifest_hashes_copy");
        if (copy) {
            memcpy(copy, g_cached_manifest.chunk_hashes, (size_t)n * 32);
            *out_hashes = copy;
            *out_count = n;
        } else {
            ok = false;
        }
    }
    pthread_mutex_unlock(&g_manifest_mutex);
    return ok;
}

bool msg_processor_publish_block_manifest(struct block_piece_manifest *manifest,
                                         int32_t built_at_height)
{
    if (!msg_block_manifest_is_reasonable(manifest))
        LOG_FAIL("net", "cannot publish unreasonable block manifest");

    pthread_mutex_lock(&g_block_manifest_mutex);
    if (atomic_load(&g_cached_block_manifest_valid))
        msg_block_manifest_reset(&g_cached_block_manifest);
    g_cached_block_manifest = *manifest;
    memset(manifest, 0, sizeof(*manifest));
    g_manifest_built_at_height = built_at_height;
    atomic_store(&g_cached_block_manifest_valid, true);
    g_cached_block_manifest_version++;
    pthread_mutex_unlock(&g_block_manifest_mutex);
    return true;
}

void msg_processor_invalidate_block_manifest(void)
{
    pthread_mutex_lock(&g_block_manifest_mutex);
    if (atomic_load(&g_cached_block_manifest_valid))
        msg_block_manifest_reset(&g_cached_block_manifest);
    g_manifest_built_at_height = 0;
    atomic_store(&g_cached_block_manifest_valid, false);
    g_cached_block_manifest_version++;
    pthread_mutex_unlock(&g_block_manifest_mutex);
}

uint64_t msg_processor_block_manifest_cache_version(void)
{
    pthread_mutex_lock(&g_block_manifest_mutex);
    uint64_t version = g_cached_block_manifest_version;
    pthread_mutex_unlock(&g_block_manifest_mutex);
    return version;
}

bool msg_processor_get_block_manifest_header(struct block_piece_manifest *out,
                                            int32_t *built_at_height)
{
    if (!out)
        LOG_FAIL("net", "block manifest output pointer is NULL");

    pthread_mutex_lock(&g_block_manifest_mutex);
    bool ok = atomic_load(&g_cached_block_manifest_valid);
    if (ok) {
        *out = g_cached_block_manifest;
        out->piece_hashes = NULL;
        if (built_at_height)
            *built_at_height = g_manifest_built_at_height;
    } else {
        memset(out, 0, sizeof(*out));
        if (built_at_height)
            *built_at_height = 0;
    }
    pthread_mutex_unlock(&g_block_manifest_mutex);
    return ok;
}

/* Serialize and send a snapshot offer to a peer.
 * Wire prefix: height(4) + block_hash(32) + utxo_root(32) + mmr_root(32) +
 *       num_utxos(8) + total_bytes(8) + mmb_root(32) = 148 bytes.
 * V2 appends protocol/schema/peer_tip/chainwork. Older ZCL23 nodes read
 * 116 or 148 bytes and ignore the trailing fields. */
void send_snapshot_offer_msg(struct p2p_node *node,
                             const struct snapshot_offer *offer,
                             const unsigned char *msg_start)
{
    uint64_t offer_version = msg_processor_offer_cache_version();
    uint64_t snapshot_version = fast_sync_snapshot_cache_version();

    p2p_node_begin_message(node, MSG_SNAPSHOT_OFFER, msg_start);
    struct byte_stream os;
    stream_init(&os, 192);
    stream_write_i32_le(&os, offer->height);
    stream_write_bytes(&os, offer->block_hash, 32);
    stream_write_bytes(&os, offer->utxo_root, 32);
    stream_write_bytes(&os, offer->mmr_root, 32);
    stream_write_u64_le(&os, offer->num_utxos);
    stream_write_u64_le(&os, offer->total_bytes);
    stream_write_bytes(&os, offer->mmb_root, 32); /* appended: backward compat */
    stream_write_u32_le(&os, offer->protocol_version);
    stream_write_u32_le(&os, offer->snapshot_schema_version);
    stream_write_i32_le(&os, offer->peer_tip_height);
    stream_write_bytes(&os, offer->chain_work, 32);
    p2p_node_write_message_data(node, os.data, os.size);
    p2p_node_end_message(node);
    stream_free(&os);

    memcpy(node->zsync_offered_root, offer->utxo_root, 32);
    memcpy(node->zsync_offered_mmr, offer->mmr_root, 32);
    memcpy(node->zsync_offered_block, offer->block_hash, 32);
    node->zsync_offered_height = offer->height;
    node->zsync_offered_count = offer->num_utxos;
    node->zsync_offer_version = offer_version;
    node->zsync_snapshot_version = snapshot_version;
}

static bool msg_should_ignore_snapshot_offer(enum snapshot_sync_state snapsync_state,
                                             uint32_t serving_peer_id,
                                             enum peer_state peer_state,
                                             uint32_t peer_id,
                                             enum sync_state sync_state)
{
    (void)serving_peer_id;

    if (sync_state == SYNC_AT_TIP)
        return true;
    if (peer_state == PEER_SNAPSHOT_RECEIVING)
        return true;
    if (snapsync_state == SNAPSYNC_NEGOTIATING ||
        snapsync_state == SNAPSYNC_RECEIVING ||
        snapsync_state == SNAPSYNC_VERIFYING)
        return true;
    if (peer_id == 0)
        return false;
    return false;
}

bool msgprocessor_test_should_ignore_snapshot_offer(
    enum snapshot_sync_state snapsync_state,
    uint32_t serving_peer_id,
    enum peer_state peer_state,
    uint32_t peer_id,
    enum sync_state sync_state) {
    return msg_should_ignore_snapshot_offer(snapsync_state, serving_peer_id,
                                            peer_state, peer_id, sync_state);
}

/* Send our manifest to a ZCL23 peer. Called after version/verack handshake. */
void push_manifest(struct msg_processor *mp, struct p2p_node *node)
{
    struct sync_manifest m;
    uint8_t (*hashes)[32] = NULL;
    uint32_t hash_count = 0;

    if (node->swarm_manifest_sent ||
        !msg_processor_get_manifest_header(&m))
        return;
    if (m.num_chunks == 0 || m.num_chunks > MANIFEST_MAX_CHUNKS) {
        fprintf(stderr, "push_manifest: num_chunks %u out of [1,%u]\n",  // obs-ok:helper-context-logged
                m.num_chunks, MANIFEST_MAX_CHUNKS);
        return;
    }
    if (!msg_processor_copy_manifest_hashes(&hashes, &hash_count)
        || hash_count != m.num_chunks) {
        fprintf(stderr, "push_manifest: chunk_hashes unavailable "  // obs-ok:helper-context-logged
                "(count=%u vs num_chunks=%u)\n", hash_count, m.num_chunks);
        free(hashes);
        return;
    }

    struct byte_stream s;
    stream_init(&s, 116 + (size_t)m.num_chunks * 32);
    stream_write_i32_le(&s, m.height);
    stream_write_bytes(&s, m.block_hash, 32);
    stream_write_u64_le(&s, m.num_utxos);
    stream_write_u32_le(&s, m.num_chunks);
    stream_write_u32_le(&s, m.chunk_size);
    stream_write_bytes(&s, m.merkle_root, 32);
    stream_write_bytes(&s, m.utxo_sha3, 32);
    /* per-chunk SHA3-256 hashes so the receiver can reject a
     * corrupt or attacker-substituted chunk before it lands in the
     * utxos table. The receiver Merkle-reconstructs merkle_root from
     * these hashes and bans the peer on mismatch. */
    for (uint32_t i = 0; i < m.num_chunks; i++)
        stream_write_bytes(&s, hashes[i], 32);

    p2p_node_begin_message(node, MSG_MANIFEST, mp->params->pchMessageStart);
    p2p_node_write_message_data(node, s.data, s.size);
    p2p_node_end_message(node);
    stream_free(&s);
    free(hashes);

    node->swarm_manifest_sent = true;
    printf("Peer %s: sent manifest (h=%d, %u chunks)\n",
           node->addr_name, m.height, m.num_chunks);
}

/* Send a chunk request to a peer. */
static void push_chunk_request(struct msg_processor *mp,
                                struct p2p_node *node,
                                uint32_t chunk_index)
{
    struct byte_stream s;
    stream_init(&s, 4);
    stream_write_u32_le(&s, chunk_index);

    p2p_node_begin_message(node, MSG_CHUNK_REQ, mp->params->pchMessageStart);
    p2p_node_write_message_data(node, s.data, s.size);
    p2p_node_end_message(node);
    stream_free(&s);
}

/* Send our block piece manifest to a ZCL23 peer.
 * SAFETY: never call this for legacy peers — they will ignore it,
 * but we avoid sending unknown messages to be a good network citizen. */
void push_block_manifest(struct msg_processor *mp,
                                 struct p2p_node *node)
{
    struct block_piece_manifest m;

    if (node->blk_manifest_sent ||
        !msg_processor_get_block_manifest_header(&m, NULL))
        return;
    if (!peer_supports_fast_sync(node->services))
        return; /* guard: only send to ZCL23 peers */
    struct byte_stream s;
    stream_init(&s, 80);
    stream_write_i32_le(&s, m.start_height);
    stream_write_i32_le(&s, m.end_height);
    stream_write_u32_le(&s, m.num_pieces);
    stream_write_bytes(&s, m.tip_hash, 32);
    stream_write_bytes(&s, m.merkle_root, 32);

    p2p_node_begin_message(node, MSG_BLOCK_MANIFEST,
                            mp->params->pchMessageStart);
    p2p_node_write_message_data(node, s.data, s.size);
    p2p_node_end_message(node);
    stream_free(&s);

    node->blk_manifest_sent = true;
    printf("Peer %s: sent block manifest (h=%d..%d, %u pieces)\n",
           node->addr_name, m.start_height, m.end_height, m.num_pieces);
}

/* Send a block piece request to a peer. */
static void push_block_piece_request(struct msg_processor *mp,
                                      struct p2p_node *node,
                                      uint32_t piece_index)
{
    struct byte_stream s;
    stream_init(&s, 4);
    stream_write_u32_le(&s, piece_index);

    p2p_node_begin_message(node, MSG_BLOCK_REQ,
                            mp->params->pchMessageStart);
    p2p_node_write_message_data(node, s.data, s.size);
    p2p_node_end_message(node);
    stream_free(&s);
}

bool mp_snapshot_is_active(void)
{
    return snapsync_is_active();
}

bool mp_swarm_is_active(void)
{
    return atomic_load(&g_swarm_active);
}

bool mp_snapshot_check_stall(void)
{
    return snapsync_check_stall();
}

void mp_snapshot_init(struct msg_processor *mp)
{
    /* Build initial block piece manifest for swarm sync.
     * This enables serving block pieces to peers immediately. */
    if (mp->main_state && mp->datadir) {
        int tip = active_chain_height(&mp->main_state->chain_active);
        struct block_piece_manifest header;
        if (tip > 1000 &&
            !msg_processor_get_block_manifest_header(&header, NULL)) {
            struct block_piece_manifest manifest;
            memset(&manifest, 0, sizeof(manifest));
            if (block_piece_manifest_build_active_chain(&mp->main_state->chain_active, 1,
                                                        tip, &manifest) ||
                block_piece_manifest_build(mp->datadir, 1, tip, &manifest)) {
                uint32_t num_pieces = manifest.num_pieces;
                int32_t start_height = manifest.start_height;
                int32_t end_height = manifest.end_height;
                msg_processor_publish_block_manifest(&manifest, tip);
                printf("Block manifest built: h=%d..%d (%u pieces, SHA3 verified)\n",
                       start_height, end_height, num_pieces);
            }
        }
    }
}

/* ── ZCL23 Sync Message Handler ──────────────────────────────────
 * Handles all snapshot, chunk, block-piece, and FlyClient messages.
 * These share complex state (g_swarm, g_block_swarm) and are kept
 * in one function for clarity. */
bool mp_handle_zcl23_sync(struct msg_processor *mp,
                          struct p2p_node *node,
                          struct byte_stream *s,
                          const char *cmd)
{
    if (strcmp(cmd, MSG_SNAPSHOT_OFFER) == 0) {
            /* ── Route: zsnapshot → snapsync_handle_offer ──────── */
            struct snapshot_offer_params params;
            if (snapsync_parse_offer_params(&params, s).ok) {
                struct snapsync_status snap_status = {0};
                params.peer_id = (uint32_t)node->id;
                params.our_height = active_chain_height(
                    &mp->main_state->chain_active);

                {
                    struct snapshot_sync_service *svc =
                        msg_snapshot_sync_ensure(mp);
                    if (svc)
                        snapsync_get_status_snapshot(svc, &snap_status);
                }

                /* Additional gate: once snapshot sync already owns the
                 * receiver lifecycle, duplicate offers should be ignored in
                 * the router instead of trying to re-enter negotiation. */
                if (msg_should_ignore_snapshot_offer(
                        snap_status.state,
                        snap_status.serving_peer_id,
                        node->state,
                        (uint32_t)node->id,
                        sync_get_state())) {
                    /* silently ignore */
                } else {
                    struct snapshot_sync_service *svc =
                        msg_snapshot_sync_ensure(mp);
                    if (svc) {
                        enum snapsync_offer_result result =
                            snapsync_handle_offer(svc, &params);

                        switch (result) {
                        case SNAPSYNC_OFFER_ACCEPTED: {
                            struct snapsync_offer_acceptance accepted = {0};
                            snapsync_build_offer_acceptance(&accepted);
                            if (accepted.should_store_offer_details) {
                                memcpy(node->zsync_offered_root, params.utxo_root, 32);
                                memcpy(node->zsync_offered_mmr, params.mmr_root, 32);
                                memcpy(node->zsync_offered_block, params.block_hash, 32);
                                node->zsync_offered_height = params.height;
                            }
                            if (accepted.should_reset_offset)
                                node->zsync_offset = 0;
                            if (accepted.should_update_peer_state)
                                peer_set_state_checked((uint32_t)node->id, &node->state,
                                    accepted.peer_state, "accepted snapshot offer");
                            event_emitf(EV_SNAPSHOT_OFFER_RECEIVED, (uint32_t)node->id,
                                "h=%d utxos=%llu", params.height,
                                (unsigned long long)params.num_utxos);
                            if (accepted.should_set_sync_state)
                                sync_set_state(accepted.sync_state, "peer snapshot");

                            struct snapsync_offer_followup followup = {0};
                            snapsync_build_offer_followup(&followup, svc);
                            if (followup.action ==
                                SNAPSYNC_FOLLOWUP_SEND_FC_CHALLENGE) {
                                /* Send FlyClient challenge — verify chain
                                 * before requesting snapshot data */
                                p2p_node_begin_message(node, MSG_FC_CHALLENGE,
                                    mp->params->pchMessageStart);
                                struct byte_stream fc;
                                stream_init(&fc, 72);
                                snapsync_write_fc_challenge(svc, &fc);
                                p2p_node_write_message_data(node, fc.data, fc.size);
                                p2p_node_end_message(node);
                                stream_free(&fc);
                                printf("[snapsync] Sent FlyClient challenge to %s\n",
                                       node->addr_name);
                            } else if (followup.action ==
                                       SNAPSYNC_FOLLOWUP_SEND_SNAPSHOT_REQ) {
                                /* No MMB — send zsnapreq directly */
                                struct byte_stream rq;
                                stream_init(&rq, 52);
                                if (snapsync_write_snapshot_request(
                                        &rq, params.our_height,
                                        node->addr.svc.addr.ip).ok) {
                                    p2p_node_begin_message(node, MSG_SNAPSHOT_REQ,
                                        mp->params->pchMessageStart);
                                    p2p_node_write_message_data(node, rq.data, rq.size);
                                    p2p_node_end_message(node);
                                }
                                stream_free(&rq);
                            }
                            break;
                        }
                        case SNAPSYNC_OFFER_REJECTED_RANGE:
                            peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PAYLOAD,
                                "snapshot offer out of range");
                            break;
                        case SNAPSYNC_OFFER_REJECTED_NO_MMR:
                            peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_OFFER_REJECTED,
                                "snapshot without MMR proof");
                            break;
                        case SNAPSYNC_OFFER_REJECTED_STALE_SCHEMA:
                            peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PAYLOAD,
                                "snapshot offer missing v2 schema");
                            break;
                        case SNAPSYNC_OFFER_REJECTED_UNFINAL:
                            peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_OFFER_REJECTED,
                                "snapshot offer non-final anchor");
                            break;
                        case SNAPSYNC_OFFER_REJECTED_WEAK_WORK:
                            peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PAYLOAD,
                                "snapshot offer weak chainwork");
                            break;
                        case SNAPSYNC_OFFER_REJECTED_BLACKLISTED:
                            printf("[snapsync] Rejected offer from %s "
                                   "(peer %u): blacklisted after stall\n",
                                   node->addr_name, (uint32_t)node->id);
                            break;
                        case SNAPSYNC_OFFER_REJECTED_NOT_AHEAD:
                        case SNAPSYNC_OFFER_REJECTED_BUSY:
                            break; /* expected, no log needed */
                        default:
                            break;
                        }
                    }
                }
            } else {
                peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PAYLOAD,
                                    "truncated snapshot v2 offer");
            }

        } else if (strcmp(cmd, MSG_SNAPSHOT_REQ) == 0) {
            /* ── Route: zsnapreq → snapsync_validate_serve_request ─ */
            int32_t from_h = 0;
            if (!stream_read_i32_le(s, &from_h)) {
                peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_MESSAGE, "truncated zsnapreq");
                return true; /* skip — caller frees msg */
            }

            /* Pass remaining bytes (PoW data) to controller */
            size_t pow_len = s->size - s->read_pos;
            const uint8_t *pow_data = pow_len > 0 ? s->data + s->read_pos : NULL;
            enum snapsync_serve_result srv = snapsync_validate_serve_request(
                pow_data, pow_len, node->addr.svc.addr.ip);

            switch (srv) {
            case SNAPSYNC_SERVE_OK:
                {
                    struct snapshot_offer offer;
                    if (msg_processor_get_offer(&offer)) {
                    uint64_t current_offer_version =
                        msg_processor_offer_cache_version();
                    uint64_t current_snapshot_version =
                        fast_sync_snapshot_cache_version();
                    bool stale_offer =
                        node->zsync_offered_height <= 0 ||
                        node->zsync_offered_count == 0 ||
                        node->zsync_offer_version != current_offer_version ||
                        node->zsync_snapshot_version != current_snapshot_version ||
                        node->zsync_offered_height != offer.height ||
                        node->zsync_offered_count != offer.num_utxos ||
                        memcmp(node->zsync_offered_root, offer.utxo_root, 32) != 0 ||
                        memcmp(node->zsync_offered_block, offer.block_hash, 32) != 0;
                    if (stale_offer) {
                        printf("Peer %s: stale snapshot request "
                               "(offered h=%d/%llu offer_v=%llu snap_v=%llu, "
                               "current h=%d/%llu offer_v=%llu snap_v=%llu); "
                               "re-offering latest snapshot\n",
                               node->addr_name,
                               node->zsync_offered_height,
                               (unsigned long long)node->zsync_offered_count,
                               (unsigned long long)node->zsync_offer_version,
                               (unsigned long long)node->zsync_snapshot_version,
                               offer.height,
                               (unsigned long long)offer.num_utxos,
                               (unsigned long long)current_offer_version,
                               (unsigned long long)current_snapshot_version);
                        send_snapshot_offer_msg(node, &offer,
                                                mp->params->pchMessageStart);
                        break;
                    }
                    struct snapsync_serve_start serve = {0};
                    snapsync_build_serve_start(&serve, node->zsync_offered_count);
                    if (serve.should_reset_progress) {
                        node->zsync_offset = 0;
                        node->zsync_sent = 0;
                        node->zsync_file_offset = 0;
                        node->zsync_file_size = 0;
                    }
                    if (serve.should_update_peer_state)
                        peer_set_state_checked((uint32_t)node->id, &node->state,
                            serve.peer_state, "serving snapshot request");
                    if (serve.should_reset_cursor) {
                        node->zsync_cursor_valid = false;
                        memset(node->zsync_cursor_txid, 0, 32);
                        node->zsync_cursor_vout = 0;
                    }
                    node->zsync_total = node->zsync_offered_count;
                    printf("Peer %s: serving snapshot (h=%d, %llu UTXOs)\n",
                           node->addr_name, node->zsync_offered_height,
                           (unsigned long long)node->zsync_offered_count);
                    } else {
                        printf("Peer %s: snapshot not ready yet\n",
                               node->addr_name);
                    }
                }
                break;
            case SNAPSYNC_SERVE_BAD_POW:
                peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PAYLOAD,
                    "zsnapreq without valid PoW");
                break;
            case SNAPSYNC_SERVE_RATE_LIMITED:
                printf("Peer %s: rate limited\n", node->addr_name);
                break;
            default:
                break;
            }

        } else if (strcmp(cmd, MSG_SNAPSHOT_DATA) == 0) {
            /* ── Route: zsnapdata → snapsync_apply_chunk ───────── */
            struct snapshot_sync_service *svc = msg_snapshot_sync_ensure(mp);
            int applied = svc ? snapsync_apply_chunk(svc,
                s->data + s->read_pos, s->size - s->read_pos) : -1;
            if (applied < 0)
                peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_MESSAGE, "bad snapshot chunk");
            else
                node->zsync_offset += (uint64_t)applied;

        } else if (strcmp(cmd, MSG_SNAPSHOT_END) == 0) {
            /* ── Route: zsnapend → snapsync_handle_end ─────────── */
            struct snapshot_sync_service *svc = msg_snapshot_sync(mp);
            if (!svc) {
                /* nothing to finalize */
            } else {
                struct snapsync_end_result end_result = {0};
                bool verified = snapsync_handle_end(svc,
                                                    (uint32_t)node->id).ok;
                snapsync_build_end_result(&end_result, verified);
                if (end_result.verified) {
                if (end_result.should_update_peer_state) {
                    peer_set_state_checked((uint32_t)node->id, &node->state,
                        end_result.peer_state, "snapshot verified");
                }

                /* Set chain tip to snapshot height */
                if (end_result.should_activate_tip) {
                    int activated_height = snapsync_activate_verified_tip(
                        svc, mp->main_state);
                    if (activated_height >= 0) {
                        printf("[snapshot] Chain tip set to height %d\n",
                               activated_height);
                        /* Update in-memory coins view to match snapshot.
                         * snapsync_activate_verified_tip → csr_commit_tip
                         * already set coins_best_block on the singleton's
                         * coins_tip in production. This raw setter stays
                         * as a defensive fallback for the test-harness
                         * path (CSR_REJECTED_NOT_INITIALIZED — csr
                         * singleton not wired), where snapsync's helper
                         * only touches active_chain / pindex_best_header.
                         * Low-level: bypasses csr on purpose. */
#ifdef ZCL_TESTING
                        if (mp->coins_tip) {
                            struct uint256 snap_hash;
                            memcpy(snap_hash.data,
                                   svc->offered_block_hash, 32);
                            coins_view_cache_set_best_block(
                                mp->coins_tip, &snap_hash);
                        }
#endif
                    }
                }
                if (end_result.should_set_sync_state) {
                    sync_set_state(end_result.sync_state,
                        "snapshot verified, sync remaining headers");
                }
                } else {
                peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PROOF,
                    "snapshot SHA3 verification failed");
                }
            }

        /* ── FlyClient chain verification messages ────────────── */

        } else if (strcmp(cmd, MSG_FC_CHALLENGE) == 0) {
            /* ── Route: zfcchallenge → build and send proofs ───── */
            /* token-bucket rate limit *before* parsing so even a
             * spray of cheap-to-read challenges can't saturate the MMB
             * proof builder. One bucket per peer; drops silently and
             * scores PEER_OFFENCE_FLOOD once per flood episode. */
            if (!fc_rate_acquire(node->id, peer_scoring_now_ms())) {
                uint32_t dropped = 0;
                if (fc_rate_should_score(node->id, &dropped)) {
                    peer_scoring_record(mp->net_mgr, node,
                                        PEER_OFFENCE_FLOOD,
                                        "FlyClient challenge flood");
                    fprintf(stderr,  // obs-ok:helper-context-logged
                            "Peer %s: FlyClient challenge flood "
                            "(dropped=%u)\n",
                            node->addr_name, dropped);
                }
                /* Silently drop this challenge. */
            } else {
                struct fc_challenge challenge;
                memset(&challenge, 0, sizeof(challenge));
                if (stream_read_bytes(s, challenge.seed, 32) &&
                    stream_read_u64_le(s, &challenge.chain_length) &&
                    stream_read_bytes(s, challenge.mmb_root, 32)) {

                    if (mp && mp->flyclient_proof) {
                        struct fc_response resp;
                        if (mp->flyclient_proof(
                                &resp, &challenge,
                                &mp->main_state->chain_active,
                                mp->flyclient_proof_ctx)) {
                            /* Send zfcproofs */
                            p2p_node_begin_message(node, MSG_FC_PROOFS,
                                mp->params->pchMessageStart);
                            struct byte_stream fp;
                            stream_init(&fp, 4 + resp.num_samples * 2048);
                            snapsync_write_fc_response(&fp, &resp);
                            p2p_node_write_message_data(node, fp.data, fp.size);
                            p2p_node_end_message(node);
                            stream_free(&fp);
                            printf("Peer %s: sent %u FlyClient proofs\n",
                                   node->addr_name, resp.num_samples);
                        }
                    } else {
                        printf("Peer %s: FlyClient challenge but no MMB data\n",
                               node->addr_name);
                    }
                }
            }

        } else if (strcmp(cmd, MSG_FC_PROOFS) == 0) {
            /* ── Route: zfcproofs → snapsync_verify_flyclient ──── */
            struct fc_response resp;
            if (!snapsync_parse_fc_response(&resp, s).ok) {
                peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PAYLOAD,
                    "truncated FlyClient proofs");
            } else {
                struct snapsync_verify_result verify_result = {0};
                struct snapshot_sync_service *svc = msg_snapshot_sync(mp);
                if (svc) {
                    snapsync_build_verify_result(
                        &verify_result,
                        snapsync_verify_flyclient(svc, &resp).ok);
                }
                if (verify_result.should_send &&
                    verify_result.action == SNAPSYNC_FOLLOWUP_SEND_SNAPSHOT_REQ) {
                    /* FlyClient passed — now send zsnapreq */
                    int our_h = active_chain_height(
                        &mp->main_state->chain_active);
                    struct byte_stream rq;
                    stream_init(&rq, 52);
                    if (snapsync_write_snapshot_request(
                            &rq, our_h, node->addr.svc.addr.ip).ok) {
                        p2p_node_begin_message(node, MSG_SNAPSHOT_REQ,
                            mp->params->pchMessageStart);
                        p2p_node_write_message_data(node, rq.data, rq.size);
                        p2p_node_end_message(node);
                    }
                    stream_free(&rq);
                } else {
                    peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PROOF,
                        "FlyClient chain verification failed");
                }
            }

        /* ── Parallel chunk sync messages ────────────────────── */

        } else if (strcmp(cmd, MSG_MANIFEST) == 0) {
            /* Peer sends their manifest — describes available chunks. */
            int32_t height = 0;
            uint8_t block_hash[32], merkle_root[32], utxo_sha3[32];
            uint64_t num_utxos = 0;
            uint32_t num_chunks = 0, chunk_size = 0;
            memset(utxo_sha3, 0, sizeof(utxo_sha3));

            if (!(stream_read_i32_le(s, &height) &&
                  stream_read_bytes(s, block_hash, 32) &&
                  stream_read_u64_le(s, &num_utxos) &&
                  stream_read_u32_le(s, &num_chunks) &&
                  stream_read_u32_le(s, &chunk_size) &&
                  stream_read_bytes(s, merkle_root, 32))) {
                peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PAYLOAD,
                                    "truncated zmanifest header");
            } else if (num_chunks == 0 || num_chunks > MANIFEST_MAX_CHUNKS
                       || chunk_size == 0) {
                fprintf(stderr, "Peer %s: manifest out of bounds "  // obs-ok:helper-context-logged
                        "(num_chunks=%u cap=%u chunk_size=%u)\n",
                        node->addr_name, num_chunks, MANIFEST_MAX_CHUNKS,
                        chunk_size);
                peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PAYLOAD,
                                    "manifest bounds");
            } else {
                if (stream_remaining(s) >= ((size_t)num_chunks * 32 + 32)) {
                    if (!stream_read_bytes(s, utxo_sha3, 32)) {
                        peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PAYLOAD,
                                            "truncated zmanifest utxo root");
                        return true;
                    }
                }

                /* read num_chunks * 32 bytes of per-chunk SHA3 hashes
                 * and reject the manifest if they don't Merkle-reconstruct
                 * to the merkle_root the same peer advertised above. This
                 * is the commitment the receiver checks each zchunkdata
                 * response against before calling fast_sync_apply_chunk. */
                uint8_t (*hashes)[32] = zcl_calloc(num_chunks, 32,
                                                    "peer_chunk_hashes");
                bool hashes_ok = hashes != NULL;
                for (uint32_t i = 0; i < num_chunks && hashes_ok; i++) {
                    if (!stream_read_bytes(s, hashes[i], 32))
                        hashes_ok = false;
                }
                if (!hashes_ok) {
                    free(hashes);
                    peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PAYLOAD,
                                        "truncated zmanifest hashes");
                } else {
                    uint8_t computed_root[32];
                    fast_sync_merkle_root(
                        (const uint8_t (*)[32])hashes, num_chunks,
                        computed_root);
                    if (memcmp(computed_root, merkle_root, 32) != 0) {
                        free(hashes);
                        peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PROOF,
                                            "manifest merkle root mismatch");
                    } else {
                        node->swarm_manifest_received = true;
                        int our_h = active_chain_height(
                            &mp->main_state->chain_active);
                        printf("Peer %s: manifest h=%d chunks=%u "
                               "(%llu UTXOs)\n",
                               node->addr_name, height, num_chunks,
                               (unsigned long long)num_utxos);

                        /* If peer is significantly ahead and we have no
                         * active swarm, initialize the swarm coordinator
                         * from their (now-verified) manifest.
                         *
                         * atomic compare-exchange on g_swarm_active
                         * (false → true) closes the TOCTOU window between
                         * the "is a swarm already running?" check and the
                         * "claim the slot" write. Without it, two peers
                         * racing on near-simultaneous manifests could both
                         * observe false and both call swarm_sync_init, the
                         * loser overwriting the winner's chunk index. The
                         * CAS lets only one peer win the init; the loser
                         * drops its message. */
                        if (height > our_h + 100) {
                            bool expected = false;
                            if (!atomic_compare_exchange_strong(
                                    &g_swarm_active, &expected, true)) {
                                /* Another peer raced us — drop silently
                                 * rather than risk state leak. */
                                printf("Peer %s: swarm already active "
                                       "(peer raced), dropping manifest\n",
                                       node->addr_name);
                            } else {
                                struct sync_manifest peer_manifest = {
                                    .height = height,
                                    .num_utxos = num_utxos,
                                    .num_chunks = num_chunks,
                                    .chunk_size = chunk_size,
                                    .chunk_hashes = hashes
                                };
                                memcpy(peer_manifest.block_hash, block_hash, 32);
                                memcpy(peer_manifest.merkle_root, merkle_root, 32);
                                memcpy(peer_manifest.utxo_sha3, utxo_sha3, 32);

                                int32_t first_chunk = -1;
                                zcl_mutex_lock(&g_swarm_mutex);
                                if (swarm_sync_init(&g_swarm, &peer_manifest,
                                                    mp->datadir)) {
                                    g_swarm_last_progress_time =
                                        (int64_t)platform_time_wall_time_t();
                                    printf("Swarm sync started: %u chunks "
                                           "from h=%d\n", num_chunks, height);
                                    first_chunk = swarm_sync_assign_chunk(
                                        &g_swarm, node->id);
                                    if (first_chunk >= 0) {
                                        node->swarm_inflight_chunk =
                                            first_chunk;
                                        node->swarm_chunk_req_time =
                                            (int64_t)platform_time_wall_time_t();
                                    }
                                } else {
                                    /* Init failed — release the claim so
                                     * another peer's manifest can retry. */
                                    atomic_store(&g_swarm_active, false);
                                }
                                zcl_mutex_unlock(&g_swarm_mutex);
                                if (first_chunk >= 0)
                                    push_chunk_request(mp, node,
                                                       (uint32_t)first_chunk);
                            }
                        }
                        /* swarm_sync_init deep-copies the hash array, so
                         * our peer copy is ours to free regardless. */
                        free(hashes);
                    }
                }
            }

        } else if (strcmp(cmd, MSG_CHUNK_REQ) == 0) {
            /* Peer requests a specific chunk by index — serve it. */
            uint32_t chunk_index = 0;
            if (!stream_read_u32_le(s, &chunk_index)) {
                printf("Peer %s: bad zchunkreq\n", node->addr_name);
            } else {
                printf("Peer %s: zchunkreq raw %u\n",
                       node->addr_name, chunk_index);
                uint32_t num_chunks = atomic_load(&g_cached_manifest_num_chunks);
                if (!atomic_load(&g_cached_manifest_valid) ||
                    num_chunks == 0) {
                    printf("Peer %s: zchunkreq but no manifest ready\n",
                           node->addr_name);
                } else if (chunk_index >= num_chunks) {
                    printf("Peer %s: zchunkreq index %u out of range (%u)\n",
                           node->addr_name, chunk_index,
                           num_chunks);
                    peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_MESSAGE,
                                        "zchunkreq out of range");
                } else if (!fast_sync_rate_check(&g_rate_limiter,
                                                  node->addr.svc.addr.ip)) {
                    printf("Peer %s: rate limited on chunk request\n",
                           node->addr_name);
                } else {
                    printf("Peer %s: zchunkreq %u\n",
                           node->addr_name, chunk_index);
                    struct utxo_chunk *chunk = zcl_calloc(
                        1, sizeof(struct utxo_chunk), "utxo_chunk");
                    if (chunk &&
                        fast_sync_serve_chunk(mp->datadir, chunk_index, chunk)) {
                        /* Serialize chunk data into message. */
                        struct byte_stream cs;
                        stream_init(&cs, 65536);
                        stream_write_u32_le(&cs, chunk->chunk_index);
                        stream_write_u32_le(&cs, chunk->num_entries);
                        for (uint32_t i = 0; i < chunk->num_entries; i++) {
                            stream_write_bytes(&cs, chunk->entries[i].txid, 32);
                            stream_write_i32_le(&cs,
                                                (int32_t)chunk->entries[i].vout);
                            stream_write_i64_le(&cs, chunk->entries[i].value);
                            stream_write_i32_le(&cs, chunk->entries[i].height);
                            stream_write_u8(&cs,
                                            chunk->entries[i].is_coinbase
                                                ? 1 : 0);
                            stream_write_u16_le(&cs,
                                                chunk->entries[i].script_len);
                            if (chunk->entries[i].script_len > 0)
                                stream_write_bytes(&cs,
                                                   chunk->entries[i].script,
                                                   chunk->entries[i].script_len);
                        }

                        p2p_node_begin_message(node, MSG_CHUNK_DATA,
                                               mp->params->pchMessageStart);
                        p2p_node_write_message_data(node, cs.data, cs.size);
                        p2p_node_end_message(node);
                        printf("Peer %s: served zchunk %u (%u entries, %zu bytes)\n",
                               node->addr_name, chunk_index,
                               chunk->num_entries, cs.size);
                        stream_free(&cs);
                    } else {
                        printf("Peer %s: failed to serve zchunk %u\n",
                               node->addr_name, chunk_index);
                    }
                    free(chunk);
                }
            }

        } else if (strcmp(cmd, MSG_CHUNK_DATA) == 0) {
            /* Peer sends chunk data in response to our request. */
            uint32_t chunk_index = 0, num_entries = 0;
            if (!stream_read_u32_le(s, &chunk_index) ||
                !stream_read_u32_le(s, &num_entries) ||
                num_entries > 1000) {
                printf("Peer %s: bad zchunkdata header\n", node->addr_name);
                peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PAYLOAD, "bad zchunkdata");
            } else if (!g_swarm_active) {
                printf("Peer %s: zchunkdata but no swarm active\n",
                       node->addr_name);
            } else {
                struct utxo_chunk *chunk = zcl_calloc(1, sizeof(struct utxo_chunk), "utxo_chunk");
                if (chunk) {
                    chunk->chunk_index = chunk_index;
                    chunk->num_entries = num_entries;
                    bool parse_ok = true;

                    for (uint32_t i = 0; i < num_entries && parse_ok; i++) {
                        if (!stream_read_bytes(s, chunk->entries[i].txid, 32))
                            { parse_ok = false; break; }
                        int32_t vout = 0;
                        if (!stream_read_i32_le(s, &vout))
                            { parse_ok = false; break; }
                        chunk->entries[i].vout = (uint32_t)vout;
                        if (!stream_read_i64_le(s, &chunk->entries[i].value))
                            { parse_ok = false; break; }
                        if (!stream_read_i32_le(s, &chunk->entries[i].height))
                            { parse_ok = false; break; }
                        uint8_t is_coinbase = 0;
                        if (!stream_read_u8(s, &is_coinbase))
                            { parse_ok = false; break; }
                        chunk->entries[i].is_coinbase = is_coinbase != 0;
                        uint16_t slen = 0;
                        if (!stream_read_u16_le(s, &slen))
                            { parse_ok = false; break; }
                        if (slen > sizeof(chunk->entries[i].script)) {
                            /* Script too large for entry — reject chunk.
                             * Don't silently truncate, that corrupts UTXOs. */
                            parse_ok = false; break;
                        }
                        chunk->entries[i].script_len = slen;
                        if (slen > 0 &&
                            !stream_read_bytes(s, chunk->entries[i].script, slen))
                            { parse_ok = false; break; }
                    }

                    if (parse_ok) {
                        zcl_mutex_lock(&g_swarm_mutex);
                        bool verified = swarm_sync_receive_chunk(
                            &g_swarm, chunk, node->id);
                        node->swarm_inflight_chunk = -1;

                        if (!verified) {
                            zcl_mutex_unlock(&g_swarm_mutex);
                            fprintf(stderr, "Peer %s: chunk %u failed verification\n",  // obs-ok:helper-context-logged
                                   node->addr_name, chunk_index);
                            peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_CHUNK,
                                                "bad chunk hash");
                        } else if (swarm_sync_is_complete(&g_swarm)) {
                            printf("Swarm sync complete: %u/%u chunks\n",
                                   g_swarm.chunks_complete,
                                   g_swarm.manifest.num_chunks);

                            /* Verify SHA3 UTXO commitment matches the
                             * snapshot offer's root hash. This catches
                             * any data corruption during transfer. */
                            if (mp->utxo_sha3_compute) {
                                uint8_t local_root[32];
                                uint64_t local_count = 0;

                                if (mp->utxo_sha3_compute(
                                        local_root, &local_count,
                                        mp->utxo_sha3_compute_ctx)) {
                                    const uint8_t *expected_root =
                                        g_swarm.manifest.utxo_sha3;
                                    if (memcmp(expected_root,
                                               (const uint8_t[32]){0}, 32) == 0)
                                        expected_root =
                                            g_swarm.manifest.merkle_root;
                                    if (memcmp(local_root, expected_root,
                                               32) == 0) {
                                        printf("SHA3 UTXO verification: PASSED "
                                               "(%lu UTXOs)\n",
                                               (unsigned long)local_count);
                                    } else {
                                        printf("SHA3 UTXO verification: FAILED "
                                               "— snapshot data corrupted!\n");
                                    }
                                }
                            }

                            swarm_sync_free(&g_swarm);
                            /* explicit atomic_store for symmetry with
                             * the CAS at the init site. Functionally
                             * equivalent to `g_swarm_active = false` on
                             * _Atomic bool, but documents the pairing. */
                            atomic_store(&g_swarm_active, false);
                            zcl_mutex_unlock(&g_swarm_mutex);
                        } else {
                            zcl_mutex_unlock(&g_swarm_mutex);
                        }
                    } else {
                        printf("Peer %s: truncated zchunkdata\n",
                               node->addr_name);
                        peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PAYLOAD,
                                            "truncated zchunkdata");
                    }
                    free(chunk);
                }
            }

        /* ── Block swarm messages (parallel block download) ──── */

        } else if (strcmp(cmd, MSG_BLOCK_MANIFEST) == 0) {
            /* Peer sends their block piece manifest.
             * DEFENSIVE: validate all fields before trusting any data. */
            int32_t start_h = 0, end_h = 0;
            uint32_t num_pieces = 0;
            uint8_t tip_hash[32], merkle_root[32];

            if (stream_read_i32_le(s, &start_h) &&
                stream_read_i32_le(s, &end_h) &&
                stream_read_u32_le(s, &num_pieces) &&
                stream_read_bytes(s, tip_hash, 32) &&
                stream_read_bytes(s, merkle_root, 32)) {

                /* Sanity: heights must be positive and consistent */
                if (start_h < 0 || end_h < start_h || num_pieces == 0 ||
                    num_pieces > 100000) {
                    printf("Peer %s: invalid block manifest "
                           "(start=%d end=%d pieces=%u)\n",
                           node->addr_name, start_h, end_h, num_pieces);
                    peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PAYLOAD,
                                        "invalid block manifest params");
                } else {
                    /* Verify piece count is consistent with height range */
                    uint32_t expected = (uint32_t)((end_h - start_h +
                        BLOCKS_PER_PIECE) / BLOCKS_PER_PIECE);
                    if (num_pieces != expected) {
                        fprintf(stderr, "Peer %s: block manifest piece count mismatch "  // obs-ok:helper-context-logged
                               "(got %u, expected %u for h=%d..%d)\n",
                               node->addr_name, num_pieces, expected,
                               start_h, end_h);
                        peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_MESSAGE,
                                            "block manifest piece count wrong");
                    } else {
                        node->blk_manifest_received = true;
                        node->blk_peer_height = end_h;
                    }
                }

                int our_h = active_chain_height(&mp->main_state->chain_active);
                if (node->blk_manifest_received)
                    printf("Peer %s: block manifest h=%d..%d (%u pieces)\n",
                           node->addr_name, start_h, end_h, num_pieces);

                /* If peer is ahead and no active block swarm, start one. */
                if (node->blk_manifest_received &&
                    end_h > our_h + BLOCKS_PER_PIECE &&
                    !g_block_swarm_active && num_pieces > 0) {
                    struct block_piece_manifest pm = {
                        .start_height = start_h,
                        .end_height = end_h,
                        .num_pieces = num_pieces,
                        .piece_hashes = NULL
                    };
                    memcpy(pm.tip_hash, tip_hash, 32);
                    memcpy(pm.merkle_root, merkle_root, 32);

                    pthread_mutex_lock(&g_block_swarm_mutex);
                    if (block_swarm_init(&g_block_swarm, &pm, mp->datadir)) {
                        g_block_swarm_active = true;
                        g_block_swarm_last_progress = (int64_t)platform_time_wall_time_t();
                        printf("Block swarm started: %u pieces, h=%d..%d\n",
                               num_pieces, start_h, end_h);
                    }
                    pthread_mutex_unlock(&g_block_swarm_mutex);
                }
            }

        } else if (strcmp(cmd, MSG_BLOCK_REQ) == 0) {
            /* Peer requests a specific block piece by index.
             * SAFETY: only serve if we have a valid manifest and the
             * piece index is in range. Rate-limited like chunk requests. */
            uint32_t piece_index = 0;
            struct block_piece_manifest bm;
            if (!stream_read_u32_le(s, &piece_index)) {
                printf("Peer %s: bad zblkreq\n", node->addr_name);
            } else if (!msg_processor_get_block_manifest_header(&bm, NULL)) {
                printf("Peer %s: zblkreq but no block manifest\n",
                       node->addr_name);
            } else if (piece_index >= bm.num_pieces) {
                printf("Peer %s: zblkreq %u out of range (%u)\n",
                       node->addr_name, piece_index,
                       bm.num_pieces);
                peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_MESSAGE,
                                    "zblkreq out of range");
            } else if (!fast_sync_rate_check(&g_rate_limiter,
                                              node->addr.svc.addr.ip)) {
                printf("Peer %s: rate limited on block piece\n",
                       node->addr_name);
            } else {
                /* Serve the piece: send block hashes for this piece range.
                 * The requester can then verify against their manifest. */
                int32_t piece_start = bm.start_height
                    + (int32_t)(piece_index * BLOCKS_PER_PIECE);
                int32_t piece_end = piece_start + BLOCKS_PER_PIECE - 1;
                if (piece_end > bm.end_height)
                    piece_end = bm.end_height;

                uint8_t piece_hashes[BLOCKS_PER_PIECE][32];
                int block_count = mp->block_hashes_range
                    ? mp->block_hashes_range(piece_start, piece_end,
                                             piece_hashes, BLOCKS_PER_PIECE,
                                             mp->block_hashes_range_ctx)
                    : 0;
                if (block_count > 0) {
                    struct byte_stream bs_msg;
                    stream_init(&bs_msg, 4 + 4 + 32 * (size_t)block_count);
                    stream_write_u32_le(&bs_msg, piece_index);
                    stream_write_u32_le(&bs_msg, (uint32_t)block_count);
                    for (int bi = 0; bi < block_count; bi++)
                        stream_write_bytes(&bs_msg, piece_hashes[bi], 32);
                    p2p_node_begin_message(node, MSG_BLOCK_DATA,
                                            mp->params->pchMessageStart);
                    p2p_node_write_message_data(node, bs_msg.data, bs_msg.size);
                    p2p_node_end_message(node);
                    stream_free(&bs_msg);
                }
            }

        } else if (strcmp(cmd, MSG_BLOCK_DATA) == 0) {
            /* Peer sends block piece data (block hashes for a piece).
             * DEFENSIVE: validate piece_index, block_count, and hash. */
            uint32_t piece_index = 0, block_count = 0;
            if (!stream_read_u32_le(s, &piece_index) ||
                !stream_read_u32_le(s, &block_count) ||
                block_count == 0 || block_count > BLOCKS_PER_PIECE) {
                printf("Peer %s: bad zblkdata (piece=%u count=%u)\n",
                       node->addr_name, piece_index, block_count);
                peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PAYLOAD,
                                    "bad zblkdata header");
            } else if (!g_block_swarm_active) {
                printf("Peer %s: zblkdata piece=%u but no block swarm\n",
                       node->addr_name, piece_index);
            } else {
                /* Read block hashes */
                uint8_t (*blk_hashes)[32] = zcl_calloc(block_count, 32, "blk_piece_hashes");
                bool parse_ok = true;
                if (blk_hashes) {
                    for (uint32_t i = 0; i < block_count && parse_ok; i++) {
                        if (!stream_read_bytes(s, blk_hashes[i], 32))
                            parse_ok = false;
                    }
                }

                if (parse_ok && blk_hashes) {
                    /* DEFENSIVE: bounds check before touching swarm */
                    pthread_mutex_lock(&g_block_swarm_mutex);
                    if (piece_index >= g_block_swarm.manifest.num_pieces) {
                        pthread_mutex_unlock(&g_block_swarm_mutex);
                        printf("Peer %s: zblkdata piece %u out of range "
                               "(max %u)\n", node->addr_name, piece_index,
                               g_block_swarm.manifest.num_pieces);
                        peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PAYLOAD,
                                            "zblkdata piece out of range");
                        free(blk_hashes);
                        goto _blkdata_done;
                    }

                    /* Compute piece hash and verify against manifest.
                     * SHA3-256 of (piece_index || count || block_hashes[]).
                     * This is the core integrity check — if the hash doesn't
                     * match the manifest, the peer sent bad data. */
                    uint8_t computed_hash[32];
                    block_piece_hash(
                        (const uint8_t (*)[32])blk_hashes,
                        block_count, piece_index, computed_hash);

                    bool verified = false;
                    if (g_block_swarm.manifest.piece_hashes) {
                        verified = memcmp(computed_hash,
                            g_block_swarm.manifest.piece_hashes[piece_index],
                            32) == 0;
                    }

                    if (verified) {
                        block_swarm_receive_piece(&g_block_swarm,
                                                   piece_index, node->id);
                        /* Clear pipeline slot */
                        for (int pi = 0; pi < 4; pi++) {
                            if (node->blk_pipeline[pi].piece_index ==
                                (int32_t)piece_index) {
                                node->blk_pipeline[pi].piece_index = -1;
                                break;
                            }
                        }

                        if (block_swarm_is_complete(&g_block_swarm)) {
                            printf("Block swarm complete: %u/%u pieces\n",
                                   g_block_swarm.pieces_complete,
                                   g_block_swarm.manifest.num_pieces);
                            block_swarm_free(&g_block_swarm);
                            g_block_swarm_active = false;
                        }
                    } else {
                        block_swarm_fail_piece(&g_block_swarm, piece_index);
                        fprintf(stderr, "Peer %s: block piece %u failed verification\n",  // obs-ok:helper-context-logged
                               node->addr_name, piece_index);
                        peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_CHUNK,
                                            "bad block piece hash");
                    }
                    pthread_mutex_unlock(&g_block_swarm_mutex);
                } else {
                    printf("Peer %s: truncated zblkdata\n", node->addr_name);
                    peer_scoring_record(mp->net_mgr, node, PEER_OFFENCE_INVALID_PAYLOAD,
                                        "truncated zblkdata");
                }
                free(blk_hashes);
            }
            _blkdata_done:;

        } else if (strcmp(cmd, MSG_BLOCK_BITMAP) == 0) {
            /* Peer sends their piece availability bitmap.
             * DEFENSIVE: validate length is reasonable. */
            uint32_t bitmap_len = 0;
            if (!stream_read_u32_le(s, &bitmap_len) ||
                bitmap_len == 0 || bitmap_len > 65536) {
                printf("Peer %s: bad zblkbitmap len=%u\n",
                       node->addr_name, bitmap_len);
            } else {
                uint8_t *bitmap = zcl_calloc(bitmap_len, 1, "blk_bitmap");
                if (bitmap && stream_read_bytes(s, bitmap, bitmap_len)) {
                    /* Store on peer for rarest-first selection */
                    free(node->blk_bitmap);
                    node->blk_bitmap = bitmap;
                    node->blk_bitmap_len = bitmap_len;

                    /* Update global availability counts */
                    if (g_block_swarm_active) {
                        pthread_mutex_lock(&g_block_swarm_mutex);
                        block_swarm_update_availability(&g_block_swarm,
                                                         bitmap, bitmap_len);
                        pthread_mutex_unlock(&g_block_swarm_mutex);
                    }
                } else {
                    free(bitmap);
                    printf("Peer %s: truncated zblkbitmap\n",
                           node->addr_name);
                }
            }

        }
    return true;
}

/* Offer a snapshot to a ZCL23 peer if we're significantly ahead.
 * Called from the per-peer trickle in msg_send_messages. */
void mp_snapshot_maybe_offer(struct msg_processor *mp,
                              struct p2p_node *node)
{
    if (!peer_supports_fast_sync(node->services))
        return;
    if (node->state == PEER_SNAPSHOT_SERVING ||
        node->state == PEER_SNAPSHOT_RECEIVING)
        return;

    int our_h = msg_get_height(mp);
    if (our_h <= 100)
        return;
    if (node->starting_height >= 0 &&
        our_h <= node->starting_height + 100)
        return;

    struct snapshot_offer offer;
    if (!msg_processor_get_offer(&offer))
        return;

    uint64_t offer_version =
        msg_processor_offer_cache_version();
    uint64_t snapshot_version =
        fast_sync_snapshot_cache_version();
    bool stale_offer =
        node->zsync_sent == 0 ||
        node->zsync_offered_height <= 0 ||
        node->zsync_offered_count == 0 ||
        node->zsync_offer_version != offer_version ||
        node->zsync_snapshot_version != snapshot_version ||
        node->zsync_offered_height != offer.height ||
        node->zsync_offered_count != offer.num_utxos ||
        memcmp(node->zsync_offered_root,
               offer.utxo_root, 32) != 0 ||
        memcmp(node->zsync_offered_block,
               offer.block_hash, 32) != 0;

    if (stale_offer) {
        node->zsync_sent = UINT64_MAX; /* mark: offered */
        event_emitf(EV_SNAPSHOT_OFFER_SENT, (uint32_t)node->id,
                    "h=%d utxos=%llu", offer.height,
                    (unsigned long long)offer.num_utxos);
        printf("Peer %s: offering snapshot (us=%d, peer=%d)\n",
               node->addr_name, our_h, node->starting_height);
        send_snapshot_offer_msg(node, &offer,
                                mp->params->pchMessageStart);
    }
}

/* Per-peer send-side coordinator: serve the snapshot stream + drive
 * the swarm/block-swarm coordinators. */
void mp_snapshot_send_tick(struct msg_processor *mp,
                            struct p2p_node *node)
{
    /* Stream fast sync UTXO chunks if serving this peer.
     * Zero-copy from in-memory buffer — no file I/O, no SQL.
     * Snapshot pre-loaded into RAM at startup (~97 MB). */
    if (node->state == PEER_SNAPSHOT_SERVING) {
        struct snapshot_offer offer;
        int64_t buf_size = 0;
        const uint8_t *buf = fast_sync_get_snapshot_buf(&buf_size);
        if (buf && buf_size > 0 && msg_processor_get_offer(&offer)) {
            uint64_t current_offer_version = msg_processor_offer_cache_version();
            uint64_t current_snapshot_version =
                fast_sync_snapshot_cache_version();
            bool stale_offer =
                node->zsync_offered_height <= 0 ||
                node->zsync_offered_count == 0 ||
                node->zsync_offer_version != current_offer_version ||
                node->zsync_snapshot_version != current_snapshot_version ||
                node->zsync_offered_height != offer.height ||
                node->zsync_offered_count != offer.num_utxos ||
                memcmp(node->zsync_offered_root, offer.utxo_root, 32) != 0 ||
                memcmp(node->zsync_offered_block, offer.block_hash, 32) != 0;
            if (stale_offer) {
                printf("Peer %s: snapshot changed while serving; "
                       "resetting to re-offer latest snapshot\n",
                       node->addr_name);
                node->zsync_offset = 0;
                node->zsync_sent = 0;
                node->zsync_file_offset = 0;
                node->zsync_file_size = 0;
                peer_set_state_checked((uint32_t)node->id, &node->state,
                                       PEER_ACTIVE,
                                       "snapshot serve stale offer");
                return;
            }
            /* Send chunks from memory, respecting TCP flow control.
             * Stop when send buffer exceeds 8MB to avoid unbounded
             * backlog that stalls the receiver.  The receiver's stall
             * detector fires at 120s — we must not queue more than
             * the receiver can process in that window. */
            for (int batch = 0; batch < 200; batch++) {
                if (node->send_size > 8 * 1024 * 1024)
                    break;  /* backpressure: wait for drain */
                struct snapsync_serve_step step;
                if (!snapsync_prepare_serve_step(&step, node, buf, buf_size))
                    break;
                if (step.action == SNAPSYNC_SERVE_ACTION_NONE)
                    break;
                if (step.action == SNAPSYNC_SERVE_ACTION_SEND_END) {
                    /* EOF — all UTXOs sent */
                    struct snapsync_serve_complete complete = {0};
                    snapsync_build_serve_complete(&complete);
                    p2p_node_begin_message(node, MSG_SNAPSHOT_END,
                                            mp->params->pchMessageStart);
                    p2p_node_end_message(node);
                    if (complete.should_update_peer_state) {
                        peer_set_state_checked((uint32_t)node->id, &node->state,
                                               complete.peer_state,
                                               "snapshot serve done");
                    }
                    printf("Peer %s: snapshot complete (%llu UTXOs, "
                           "%llu chunks sent)\n",
                           node->addr_name,
                           (unsigned long long)node->zsync_offset,
                           (unsigned long long)node->zsync_sent);
                    break;
                }

                /* Send chunk directly from memory — true zero-copy */
                p2p_node_begin_message(node, MSG_SNAPSHOT_DATA,
                                        mp->params->pchMessageStart);
                p2p_node_write_message_data(node, buf + step.chunk_offset,
                                            step.chunk_len);
                p2p_node_end_message(node);

                if (node->zsync_sent % 100 == 0) {
                    printf("Peer %s: sent %llu/%llu UTXOs (%.0f%%)\n",
                           node->addr_name,
                           (unsigned long long)node->zsync_offset,
                           (unsigned long long)node->zsync_total,
                           node->zsync_total > 0 ?
                               100.0 * (double)node->zsync_offset / (double)node->zsync_total : 0);
                }
            }
        } else {
            fprintf(stderr, "Peer %s: no snapshot in memory\n", node->addr_name);  // obs-ok:helper-context-logged
            peer_set_state_checked((uint32_t)node->id, &node->state,
                                   PEER_ACTIVE, "no snapshot buffer");
        }
    }

    /* ── Swarm parallel chunk sync coordinator ────────────── */
    /* For each connected ZCL23 peer with no inflight chunk, assign one
     * and send a zchunkreq. Also handle timeouts on stale requests. */
    if (g_swarm_active && node->swarm_manifest_received &&
        node->state >= PEER_HANDSHAKE_COMPLETE) {

        zcl_mutex_lock(&g_swarm_mutex);

        /* Requeue globally stale inflight chunks. A peer can disconnect and
         * lose node->swarm_inflight_chunk while g_swarm still marks that
         * chunk inflight; without the global sweep the final chunk can sit
         * at 2679/2680 forever with no peer able to claim it. */
        swarm_sync_handle_timeouts(&g_swarm, SWARM_CHUNK_TIMEOUT_SECS);

        /* Handle timeout: if this peer's chunk is stale, re-queue it */
        if (node->swarm_inflight_chunk >= 0) {
            int64_t now_sw = (int64_t)platform_time_wall_time_t();
            if (now_sw - node->swarm_chunk_req_time > SWARM_CHUNK_TIMEOUT_SECS) {
                uint32_t ci = (uint32_t)node->swarm_inflight_chunk;
                if (ci < g_swarm.manifest.num_chunks &&
                    g_swarm.chunk_states[ci] == CHUNK_INFLIGHT) {
                    g_swarm.chunk_states[ci] = CHUNK_NEEDED;
                    g_swarm.chunk_peer[ci] = -1;
                    if (g_swarm.chunks_inflight > 0)
                        g_swarm.chunks_inflight--;
                    printf("Peer %s: chunk %u timed out, re-queuing\n",
                           node->addr_name, ci);
                }
                node->swarm_inflight_chunk = -1;
            }
        }

        /* If peer has no inflight chunk, assign the next needed one */
        if (node->swarm_inflight_chunk < 0) {
            int32_t ci = swarm_sync_assign_chunk(&g_swarm, node->id);
            if (ci >= 0) {
                node->swarm_inflight_chunk = ci;
                node->swarm_chunk_req_time = (int64_t)platform_time_wall_time_t();
                push_chunk_request(mp, node, (uint32_t)ci);
            }
        }

        /* Progress display (rate-limited to every 5 seconds) */
        int64_t now_prog = (int64_t)platform_time_wall_time_t();
        if (now_prog - g_swarm_last_progress_time >= SWARM_PROGRESS_INTERVAL_SECS) {
            g_swarm_last_progress_time = now_prog;

            int progress = swarm_sync_progress(&g_swarm);
            uint32_t complete = g_swarm.chunks_complete;
            uint32_t total = g_swarm.manifest.num_chunks;
            uint32_t inflight = g_swarm.chunks_inflight;
            zcl_mutex_unlock(&g_swarm_mutex);

            /* Count serving peers — under cs_nodes: the socket-thread
             * disconnect sweep frees nodes at refcount 0. g_swarm_mutex
             * was released above, so no lock-order hazard. */
            int serving_peers = 0;
            if (mp->net_mgr) {
                zcl_mutex_lock(&mp->net_mgr->cs_nodes);
                for (size_t i = 0; i < mp->net_mgr->num_nodes; i++) {
                    struct p2p_node *n = mp->net_mgr->nodes[i];
                    if (n && n->swarm_manifest_received)
                        serving_peers++;
                }
                zcl_mutex_unlock(&mp->net_mgr->cs_nodes);
            }

            printf("Sync: %d%% (%u/%u chunks, %u inflight, %d peers serving)\n",
                   progress, complete, total, inflight, serving_peers);
        } else {
            zcl_mutex_unlock(&g_swarm_mutex);
        }
    }

    /* ── Block swarm coordinator: parallel block piece download ── */
    /* Only for ZCL23 peers with completed handshake. Legacy peers
     * contribute via normal getdata/block (handled by download manager). */
    if (g_block_swarm_active && peer_supports_fast_sync(node->services) &&
        node->blk_manifest_received &&
        node->state >= PEER_HANDSHAKE_COMPLETE) {

        pthread_mutex_lock(&g_block_swarm_mutex);

        /* Handle timeouts on this peer's pipeline */
        int64_t now_bs = (int64_t)platform_time_wall_time_t();
        for (int pi = 0; pi < PIECE_PIPELINE_DEPTH; pi++) {
            int32_t pidx = node->blk_pipeline[pi].piece_index;
            if (pidx >= 0 &&
                now_bs - node->blk_pipeline[pi].request_time >
                    SWARM_CHUNK_TIMEOUT_SECS) {
                if ((uint32_t)pidx < g_block_swarm.manifest.num_pieces &&
                    g_block_swarm.piece_states[pidx] == CHUNK_INFLIGHT) {
                    g_block_swarm.piece_states[pidx] = CHUNK_NEEDED;
                    g_block_swarm.piece_peer[pidx] = -1;
                    if (g_block_swarm.pieces_inflight > 0)
                        g_block_swarm.pieces_inflight--;
                }
                node->blk_pipeline[pi].piece_index = -1;
            }
        }

        /* Fill empty pipeline slots with new piece assignments */
        for (int pi = 0; pi < PIECE_PIPELINE_DEPTH; pi++) {
            if (node->blk_pipeline[pi].piece_index >= 0)
                continue; /* slot occupied */

            int32_t pidx = block_swarm_assign_piece(
                &g_block_swarm, node->id, node->blk_bitmap);
            if (pidx < 0)
                break; /* no more pieces to assign */

            node->blk_pipeline[pi].piece_index = pidx;
            node->blk_pipeline[pi].request_time = now_bs;

            pthread_mutex_unlock(&g_block_swarm_mutex);
            push_block_piece_request(mp, node, (uint32_t)pidx);
            pthread_mutex_lock(&g_block_swarm_mutex);
        }

        /* Progress display (rate-limited) */
        int64_t now_bp = (int64_t)platform_time_wall_time_t();
        if (now_bp - g_block_swarm_last_progress >=
            SWARM_PROGRESS_INTERVAL_SECS) {
            g_block_swarm_last_progress = now_bp;
            int bprog = block_swarm_progress(&g_block_swarm);
            uint32_t bcomplete = g_block_swarm.pieces_complete;
            uint32_t btotal = g_block_swarm.manifest.num_pieces;
            uint32_t binflight = g_block_swarm.pieces_inflight;
            bool endgame = g_block_swarm.endgame;
            pthread_mutex_unlock(&g_block_swarm_mutex);

            printf("BlockSync: %d%% (%u/%u pieces, %u inflight%s)\n",
                   bprog, bcomplete, btotal, binflight,
                   endgame ? " [endgame]" : "");
        } else {
            pthread_mutex_unlock(&g_block_swarm_mutex);
        }
    }
}
