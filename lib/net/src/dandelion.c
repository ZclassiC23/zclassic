/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Dandelion (BIP 156) transaction propagation — stem/fluff relay for
 * tx origin privacy. See dandelion.h for protocol overview.
 *
 * Thread safety: all public functions acquire ds->cs. The caller must
 * NOT hold ds->cs when calling these functions. The caller MAY hold
 * net_manager->cs_nodes when calling dandelion_maybe_rotate_epoch()
 * (it acquires cs_nodes internally only if not rotating). */

#include "platform/time_compat.h"
#include "net/dandelion.h"
#include "net/protocol.h"
#include "net/msg_internal.h"
#include "crypto/random_secret.h"
#include "json/json.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ── Process-wide enable switch ────────────────────────────────── */

static _Atomic bool g_dandelion_enabled = true;

bool dandelion_enabled(void)
{
    return atomic_load(&g_dandelion_enabled);
}

void dandelion_set_enabled(bool enabled)
{
    atomic_store(&g_dandelion_enabled, enabled);
}

/* ── Lifecycle ─────────────────────────────────────────────────── */

void dandelion_init(struct dandelion_state *ds)
{
    memset(ds, 0, sizeof(*ds));
    zcl_mutex_init(&ds->cs);
    ds->epoch_start = 0;
    ds->epoch_len_secs = DANDELION_EPOCH_MEAN_SECS;
    ds->num_stem_peers = 0;
    ds->stempool_count = 0;
    ds->enabled = true;
    for (int i = 0; i < DANDELION_NUM_STEM_PEERS; i++)
        ds->stem_peers[i] = DANDELION_NODE_ID_NONE;
}

void dandelion_free(struct dandelion_state *ds)
{
    if (!ds)
        return;
    zcl_mutex_lock(&ds->cs);
    for (int i = 0; i < DANDELION_MAX_STEMPOOL; i++) {
        if (ds->stempool[i].active && ds->stempool[i].tx_bytes)
            free(ds->stempool[i].tx_bytes);
        ds->stempool[i].active = false;
        ds->stempool[i].tx_bytes = NULL;
    }
    zcl_mutex_unlock(&ds->cs);
    zcl_mutex_destroy(&ds->cs);
    memset(ds, 0, sizeof(*ds));
}

/* ── Cryptographic RNG for stem decisions ───────────────────
 *
 * Dandelion's stem-peer selection (Fisher-Yates), per-edge route salt,
 * per-tx fluff coin-flip, embargo delays, and epoch lengths MUST be
 * unpredictable to the network. An attacker who can replay the stream
 * could predict (a) which outbound peers we use for stem relay this
 * epoch and (b) every fluff outcome — defeating Dandelion's
 * origin-privacy property.
 *
 * All decisions route through `zcl_random_secret_bytes`, the same
 * source used for esk / Sapling rcm/rcv / Groth16 blinding. On RNG
 * failure (open(/dev/urandom) failure or all-zero output — both
 * extremely rare) callers safe-fail by aborting stem-peer selection
 * (leaves num_stem_peers=0 → next dandelion_should_stem returns false
 * → tx fluffs via normal relay) or returning false from the coin-flip
 * directly (tx fluffs).
 *
 * If a future "performance" refactor wants to swap this back to a
 * cheap PRNG: don't. The seed must be unpredictable to the network,
 * and fetching fresh per-call entropy is the simplest way to
 * guarantee that. Dandelion is not a hot path — the per-tx cost of
 * a /dev/urandom read is negligible compared to script verification.
 */

static bool dandelion_secret_u64(uint64_t *out, const char *label)
{
    uint8_t buf[8];
    if (!zcl_random_secret_bytes(buf, sizeof buf, label))
        return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)buf[i] << (i * 8);
    *out = v;
    return true;
}

/* Sample an exponential delay with the given mean from a uniform
 * 64-bit draw. Result clamped to [0, 6*mean] to bound the tail. */
static int64_t dandelion_exp_secs(double mean_secs, uint64_t r)
{
    /* (r >> 11) is uniform on [0, 2^53); +1 keeps u in (0, 1] so
     * log(u) is finite. */
    double u = ((double)((r >> 11) + 1)) / 9007199254740993.0;
    double t = -mean_secs * log(u);
    double cap = 6.0 * mean_secs;
    if (t > cap)
        t = cap;
    if (t < 0.0)
        t = 0.0;
    return (int64_t)t;
}

/* splitmix64 finalizer — deterministic mixing for the per-epoch,
 * per-inbound-edge destination mapping (NOT a randomness source; the
 * unpredictability comes from route_salt). */
static uint64_t dandelion_mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/* ── Epoch management ──────────────────────────────────────────── */

void dandelion_maybe_rotate_epoch(struct dandelion_state *ds,
                                  struct net_manager *nm)
{
    if (!ds || !nm)
        return;

    int64_t now = (int64_t)platform_time_wall_time_t();

    zcl_mutex_lock(&ds->cs);

    if (ds->epoch_start != 0 &&
        (now - ds->epoch_start) < ds->epoch_len_secs) {
        zcl_mutex_unlock(&ds->cs);
        return;
    }

    /* New epoch — pick Dandelion destinations from connected outbound
     * peers that advertise NODE_DANDELION (BIP 156 negotiation). */
    ds->epoch_start = now;
    ds->num_stem_peers = 0;
    for (int i = 0; i < DANDELION_NUM_STEM_PEERS; i++)
        ds->stem_peers[i] = DANDELION_NODE_ID_NONE;

    node_id_t candidates[MAX_OUTBOUND_CONNECTIONS];
    int num_candidates = 0;

    zcl_mutex_lock(&nm->cs_nodes);
    for (size_t i = 0; i < nm->num_nodes && num_candidates < MAX_OUTBOUND_CONNECTIONS; i++) {
        struct p2p_node *peer = nm->nodes[i];
        if (!peer->inbound &&
            peer->state >= PEER_HANDSHAKE_COMPLETE &&
            !peer->disconnect &&
            peer->relay_txes &&
            (peer->services & NODE_DANDELION)) {
            candidates[num_candidates++] = peer->id;
        }
    }
    zcl_mutex_unlock(&nm->cs_nodes);

    /* Fresh per-epoch state: route salt and randomized epoch length
     * (BIP 156: routes re-shuffle at random intervals, ~10 min mean).
     * On RNG failure: leave num_stem_peers=0 so dandelion_should_stem
     * returns false and txs fluff via normal relay (safer than picking
     * with a compromised RNG). */
    bool rng_ok = true;
    uint64_t salt = 0, rlen = 0;
    if (!dandelion_secret_u64(&salt, "dandelion-route-salt") ||
        !dandelion_secret_u64(&rlen, "dandelion-epoch-len"))
        rng_ok = false;

    /* Fisher-Yates shuffle backed by the cryptographic RNG. */
    for (int i = num_candidates - 1; rng_ok && i > 0; i--) {
        uint64_t r;
        if (!dandelion_secret_u64(&r, "dandelion-stem-shuffle")) {
            rng_ok = false;
            break;
        }
        int j = (int)(r % (uint64_t)(i + 1));
        node_id_t tmp = candidates[i];
        candidates[i] = candidates[j];
        candidates[j] = tmp;
    }

    if (!rng_ok) {
        fprintf(stderr, "[dandelion] stem-peer RNG failed; deferring "  // obs-ok:helper-context-logged
                        "selection (txs will fluff this epoch)\n");
        ds->num_stem_peers = 0;
        ds->epoch_len_secs = DANDELION_EPOCH_MIN_SECS; /* retry soon */
        for (int i = 0; i < DANDELION_NUM_STEM_PEERS; i++)
            ds->stem_peers[i] = DANDELION_NODE_ID_NONE;
        zcl_mutex_unlock(&ds->cs);
        return;
    }

    ds->route_salt = salt;
    int64_t len = dandelion_exp_secs((double)DANDELION_EPOCH_MEAN_SECS, rlen);
    if (len < DANDELION_EPOCH_MIN_SECS)
        len = DANDELION_EPOCH_MIN_SECS;
    if (len > DANDELION_EPOCH_MAX_SECS)
        len = DANDELION_EPOCH_MAX_SECS;
    ds->epoch_len_secs = len;

    int pick = num_candidates < DANDELION_NUM_STEM_PEERS
             ? num_candidates : DANDELION_NUM_STEM_PEERS;
    for (int i = 0; i < pick; i++)
        ds->stem_peers[i] = candidates[i];
    ds->num_stem_peers = pick;

    if (pick > 0) {
        fprintf(stderr, "[dandelion] new epoch: %d destination(s), "  // obs-ok:helper-context-logged
                        "%lld s\n", pick, (long long)len);
    }

    zcl_mutex_unlock(&ds->cs);
}

/* ── Core routing ──────────────────────────────────────────────── */

bool dandelion_should_stem(struct dandelion_state *ds, node_id_t from_peer)
{
    (void)from_peer;

    if (!ds || !dandelion_enabled())
        return false;

    zcl_mutex_lock(&ds->cs);

    if (!ds->enabled || ds->num_stem_peers == 0) {
        zcl_mutex_unlock(&ds->cs);
        return false;
    }

    /* Each relay hop independently fluffs with DANDELION_FLUFF_PROB%.
     * On RNG failure: fluff (safer than stemming with a compromised
     * RNG; matches the safe-fail policy in maybe_rotate_epoch). */
    uint64_t r;
    if (!dandelion_secret_u64(&r, "dandelion-fluff-coin")) {
        zcl_mutex_unlock(&ds->cs);
        return false;
    }
    bool stem = ((r % 100) >= DANDELION_FLUFF_PROB);

    zcl_mutex_unlock(&ds->cs);
    return stem;
}

node_id_t dandelion_get_stem_peer(struct dandelion_state *ds,
                                  node_id_t from_peer)
{
    if (!ds || !dandelion_enabled())
        return DANDELION_NODE_ID_NONE;

    zcl_mutex_lock(&ds->cs);

    if (!ds->enabled || ds->num_stem_peers == 0) {
        zcl_mutex_unlock(&ds->cs);
        return DANDELION_NODE_ID_NONE;
    }

    /* BIP 156 per-inbound-edge routing: each inbound edge (and the
     * local wallet, from_peer == DANDELION_NODE_ID_NONE) maps to one
     * destination for the whole epoch. Salt-keyed so the mapping is
     * stable within an epoch, unpredictable across epochs, and needs
     * no per-peer storage. */
    uint64_t k = ds->route_salt ^
                 dandelion_mix64((uint64_t)(int64_t)from_peer);
    int idx = (int)(dandelion_mix64(k) % (uint64_t)ds->num_stem_peers);

    /* Never stem back to the sender; fall over to the other
     * destination, else give up (caller fluffs). */
    node_id_t chosen = DANDELION_NODE_ID_NONE;
    for (int attempt = 0; attempt < ds->num_stem_peers; attempt++) {
        int i = (idx + attempt) % ds->num_stem_peers;
        if (ds->stem_peers[i] != from_peer &&
            ds->stem_peers[i] != DANDELION_NODE_ID_NONE) {
            chosen = ds->stem_peers[i];
            break;
        }
    }

    zcl_mutex_unlock(&ds->cs);
    return chosen;
}

/* ── Stempool ──────────────────────────────────────────────────── */

bool dandelion_stempool_add(struct dandelion_state *ds,
                            const struct uint256 *txhash,
                            node_id_t from_peer,
                            node_id_t dest_peer,
                            const uint8_t *tx_bytes,
                            size_t tx_size)
{
    if (!ds || !txhash || !tx_bytes || tx_size == 0)
        return false;

    /* Random embargo: now + MIN + exponential(AVG_ADD). On RNG failure
     * use the minimum (fluffs sooner — safe direction). */
    int64_t now = (int64_t)platform_time_wall_time_t();
    int64_t embargo = now + DANDELION_EMBARGO_MIN_SECS;
    uint64_t r;
    if (dandelion_secret_u64(&r, "dandelion-embargo"))
        embargo += dandelion_exp_secs(
            (double)DANDELION_EMBARGO_AVG_ADD_SECS, r);

    uint8_t *copy = zcl_malloc(tx_size, "dandelion.stem_tx");
    if (!copy)
        LOG_FAIL("net", "dandelion stempool: OOM copying tx (%zu bytes)",
                 tx_size);
    memcpy(copy, tx_bytes, tx_size);

    zcl_mutex_lock(&ds->cs);

    /* Check for duplicate */
    for (int i = 0; i < DANDELION_MAX_STEMPOOL; i++) {
        if (ds->stempool[i].active &&
            uint256_eq(&ds->stempool[i].txhash, txhash)) {
            zcl_mutex_unlock(&ds->cs);
            free(copy);
            return false;
        }
    }

    /* Find empty slot, or evict oldest */
    int slot = -1;
    int64_t oldest_time = INT64_MAX;
    int oldest_slot = 0;

    for (int i = 0; i < DANDELION_MAX_STEMPOOL; i++) {
        if (!ds->stempool[i].active) {
            slot = i;
            break;
        }
        if (ds->stempool[i].embargo_time < oldest_time) {
            oldest_time = ds->stempool[i].embargo_time;
            oldest_slot = i;
        }
    }

    if (slot < 0) {
        /* Evict oldest — it should have been fluffed already */
        slot = oldest_slot;
        if (ds->stempool[slot].tx_bytes)
            free(ds->stempool[slot].tx_bytes);
        ds->stempool_count--;
    }

    ds->stempool[slot].txhash = *txhash;
    ds->stempool[slot].embargo_time = embargo;
    ds->stempool[slot].from_peer = from_peer;
    ds->stempool[slot].dest_peer = dest_peer;
    ds->stempool[slot].tx_bytes = copy;
    ds->stempool[slot].tx_size = tx_size;
    ds->stempool[slot].active = true;
    ds->stempool_count++;

    zcl_mutex_unlock(&ds->cs);
    return true;
}

static int stempool_find_locked(struct dandelion_state *ds,
                                const struct uint256 *txhash)
{
    for (int i = 0; i < DANDELION_MAX_STEMPOOL; i++) {
        if (ds->stempool[i].active &&
            uint256_eq(&ds->stempool[i].txhash, txhash))
            return i;
    }
    return -1;
}

bool dandelion_stempool_remove(struct dandelion_state *ds,
                               const struct uint256 *txhash)
{
    if (!ds || !txhash)
        return false;

    zcl_mutex_lock(&ds->cs);
    int i = stempool_find_locked(ds, txhash);
    if (i < 0) {
        zcl_mutex_unlock(&ds->cs);
        return false;
    }
    if (ds->stempool[i].tx_bytes)
        free(ds->stempool[i].tx_bytes);
    ds->stempool[i].tx_bytes = NULL;
    ds->stempool[i].active = false;
    ds->stempool_count--;
    zcl_mutex_unlock(&ds->cs);
    return true;
}

bool dandelion_stempool_take(struct dandelion_state *ds,
                             const struct uint256 *txhash,
                             struct dandelion_fluff_item *out)
{
    if (!ds || !txhash || !out)
        return false;

    zcl_mutex_lock(&ds->cs);
    int i = stempool_find_locked(ds, txhash);
    if (i < 0) {
        zcl_mutex_unlock(&ds->cs);
        return false;
    }
    out->txhash = ds->stempool[i].txhash;
    out->tx_bytes = ds->stempool[i].tx_bytes; /* ownership transferred */
    out->tx_size = ds->stempool[i].tx_size;
    ds->stempool[i].tx_bytes = NULL;
    ds->stempool[i].active = false;
    ds->stempool_count--;
    zcl_mutex_unlock(&ds->cs);
    return true;
}

uint8_t *dandelion_stempool_copy(struct dandelion_state *ds,
                                 const struct uint256 *txhash,
                                 size_t *out_size)
{
    if (!ds || !txhash || !out_size)
        return NULL;

    zcl_mutex_lock(&ds->cs);
    int i = stempool_find_locked(ds, txhash);
    if (i < 0 || !ds->stempool[i].tx_bytes) {
        zcl_mutex_unlock(&ds->cs);
        return NULL;
    }
    size_t sz = ds->stempool[i].tx_size;
    uint8_t *copy = zcl_malloc(sz, "dandelion.serve_tx");
    if (!copy) {
        zcl_mutex_unlock(&ds->cs);
        LOG_NULL("net", "dandelion: OOM copying stem tx (%zu bytes)", sz);
    }
    memcpy(copy, ds->stempool[i].tx_bytes, sz);
    zcl_mutex_unlock(&ds->cs);
    *out_size = sz;
    return copy;
}

bool dandelion_stempool_contains(struct dandelion_state *ds,
                                 const struct uint256 *txhash)
{
    if (!ds || !txhash)
        return false;

    zcl_mutex_lock(&ds->cs);
    bool found = stempool_find_locked(ds, txhash) >= 0;
    zcl_mutex_unlock(&ds->cs);
    return found;
}

/* Prune advert/request entries older than DANDELION_TABLE_TTL_SECS.
 * Caller holds ds->cs. */
static void dandelion_prune_tables_locked(struct dandelion_state *ds,
                                          int64_t now)
{
    for (int i = 0; i < DANDELION_MAX_ADVERTS; i++) {
        if (ds->adverts[i].active &&
            now - ds->adverts[i].time > DANDELION_TABLE_TTL_SECS)
            ds->adverts[i].active = false;
    }
    for (int i = 0; i < DANDELION_MAX_REQUESTS; i++) {
        if (ds->requests[i].active &&
            now - ds->requests[i].time > DANDELION_TABLE_TTL_SECS)
            ds->requests[i].active = false;
    }
}

int dandelion_stempool_take_expired(struct dandelion_state *ds,
                                    struct net_manager *nm,
                                    struct dandelion_fluff_item *out,
                                    int max_out)
{
    if (!ds || !out || max_out <= 0)
        return 0;

    int64_t now = (int64_t)platform_time_wall_time_t();
    int count = 0;

    /* Snapshot connected peer ids so we can early-fluff entries whose
     * stem destination is gone (their embargo would cover it anyway,
     * but this shortens the propagation gap). Taken before ds->cs to
     * keep the cs_nodes → ds->cs order out of the picture. */
    enum { DANDELION_PEER_SNAPSHOT_MAX = 256 };
    node_id_t connected[DANDELION_PEER_SNAPSHOT_MAX];
    size_t num_connected = 0;
    bool have_peers = false;
    if (nm) {
        zcl_mutex_lock(&nm->cs_nodes);
        for (size_t i = 0; i < nm->num_nodes && num_connected < DANDELION_PEER_SNAPSHOT_MAX; i++) {
            struct p2p_node *peer = nm->nodes[i];
            if (peer->state >= PEER_HANDSHAKE_COMPLETE && !peer->disconnect)
                connected[num_connected++] = peer->id;
        }
        zcl_mutex_unlock(&nm->cs_nodes);
        have_peers = true;
    }

    zcl_mutex_lock(&ds->cs);

    dandelion_prune_tables_locked(ds, now);

    for (int i = 0; i < DANDELION_MAX_STEMPOOL && count < max_out; i++) {
        if (!ds->stempool[i].active)
            continue;

        bool expired = now >= ds->stempool[i].embargo_time;

        if (!expired && have_peers &&
            ds->stempool[i].dest_peer != DANDELION_NODE_ID_NONE) {
            bool dest_alive = false;
            for (size_t p = 0; p < num_connected; p++) {
                if (connected[p] == ds->stempool[i].dest_peer) {
                    dest_alive = true;
                    break;
                }
            }
            if (!dest_alive)
                expired = true; /* destination gone: fluff early */
        }

        if (!expired)
            continue;

        out[count].txhash = ds->stempool[i].txhash;
        out[count].tx_bytes = ds->stempool[i].tx_bytes; /* ownership out */
        out[count].tx_size = ds->stempool[i].tx_size;
        count++;

        ds->stempool[i].tx_bytes = NULL;
        ds->stempool[i].active = false;
        ds->stempool_count--;
        ds->stat_embargo_fluff++;
    }

    zcl_mutex_unlock(&ds->cs);
    return count;
}

/* ── (txhash, peer) tables ─────────────────────────────────────── */

static void tx_peer_add(struct dandelion_tx_peer *table, int cap,
                        const struct uint256 *txhash, node_id_t peer,
                        int64_t now)
{
    /* Reuse a matching slot, else first free, else oldest. */
    int slot = -1;
    int64_t oldest_time = INT64_MAX;
    int oldest_slot = 0;
    for (int i = 0; i < cap; i++) {
        if (table[i].active &&
            uint256_eq(&table[i].txhash, txhash) && table[i].peer == peer) {
            table[i].time = now;
            return;
        }
        if (!table[i].active && slot < 0)
            slot = i;
        if (table[i].time < oldest_time) {
            oldest_time = table[i].time;
            oldest_slot = i;
        }
    }
    if (slot < 0)
        slot = oldest_slot;
    table[slot].txhash = *txhash;
    table[slot].peer = peer;
    table[slot].time = now;
    table[slot].active = true;
}

void dandelion_mark_advertised(struct dandelion_state *ds,
                               const struct uint256 *txhash,
                               node_id_t peer)
{
    if (!ds || !txhash)
        return;
    int64_t now = (int64_t)platform_time_wall_time_t();
    zcl_mutex_lock(&ds->cs);
    tx_peer_add(ds->adverts, DANDELION_MAX_ADVERTS, txhash, peer, now);
    zcl_mutex_unlock(&ds->cs);
}

bool dandelion_was_advertised_to(struct dandelion_state *ds,
                                 const struct uint256 *txhash,
                                 node_id_t peer)
{
    if (!ds || !txhash)
        return false;
    zcl_mutex_lock(&ds->cs);
    bool found = false;
    for (int i = 0; i < DANDELION_MAX_ADVERTS; i++) {
        if (ds->adverts[i].active &&
            ds->adverts[i].peer == peer &&
            uint256_eq(&ds->adverts[i].txhash, txhash)) {
            found = true;
            break;
        }
    }
    zcl_mutex_unlock(&ds->cs);
    return found;
}

void dandelion_request_add(struct dandelion_state *ds,
                           const struct uint256 *txhash,
                           node_id_t peer)
{
    if (!ds || !txhash)
        return;
    int64_t now = (int64_t)platform_time_wall_time_t();
    zcl_mutex_lock(&ds->cs);
    tx_peer_add(ds->requests, DANDELION_MAX_REQUESTS, txhash, peer, now);
    zcl_mutex_unlock(&ds->cs);
}

bool dandelion_request_pending(struct dandelion_state *ds,
                               const struct uint256 *txhash)
{
    if (!ds || !txhash)
        return false;
    zcl_mutex_lock(&ds->cs);
    bool found = false;
    for (int i = 0; i < DANDELION_MAX_REQUESTS; i++) {
        if (ds->requests[i].active &&
            uint256_eq(&ds->requests[i].txhash, txhash)) {
            found = true;
            break;
        }
    }
    zcl_mutex_unlock(&ds->cs);
    return found;
}

bool dandelion_request_take(struct dandelion_state *ds,
                            const struct uint256 *txhash,
                            node_id_t peer)
{
    if (!ds || !txhash)
        return false;
    zcl_mutex_lock(&ds->cs);
    bool found = false;
    for (int i = 0; i < DANDELION_MAX_REQUESTS; i++) {
        if (ds->requests[i].active &&
            ds->requests[i].peer == peer &&
            uint256_eq(&ds->requests[i].txhash, txhash)) {
            ds->requests[i].active = false;
            found = true;
            break;
        }
    }
    zcl_mutex_unlock(&ds->cs);
    return found;
}

/* ── Introspection ─────────────────────────────────────────────── */

bool dandelion_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;

    json_push_kv_bool(out, "enabled_global", dandelion_enabled());
    json_push_kv_bool(out, "initialized", g_dandelion_init);
    if (!g_dandelion_init)
        return true;

    struct dandelion_state *ds = &g_dandelion;
    int64_t now = (int64_t)platform_time_wall_time_t();

    zcl_mutex_lock(&ds->cs);
    json_push_kv_bool(out, "enabled", ds->enabled);
    json_push_kv_int(out, "epoch_age_secs",
                     ds->epoch_start ? (now - ds->epoch_start) : -1);
    json_push_kv_int(out, "epoch_len_secs", ds->epoch_len_secs);
    json_push_kv_int(out, "num_stem_peers", ds->num_stem_peers);

    struct json_value peers = {0};
    json_set_array(&peers);
    for (int i = 0; i < ds->num_stem_peers; i++) {
        struct json_value v = {0};
        json_set_int(&v, ds->stem_peers[i]);
        json_push_back(&peers, &v);
        json_free(&v);
    }
    json_push_kv(out, "stem_peers", &peers);
    json_free(&peers);

    json_push_kv_int(out, "stempool_count", ds->stempool_count);

    int adverts = 0, requests = 0;
    for (int i = 0; i < DANDELION_MAX_ADVERTS; i++)
        if (ds->adverts[i].active)
            adverts++;
    for (int i = 0; i < DANDELION_MAX_REQUESTS; i++)
        if (ds->requests[i].active)
            requests++;
    json_push_kv_int(out, "adverts_active", adverts);
    json_push_kv_int(out, "requests_pending", requests);

    json_push_kv_int(out, "stat_stem_sent", (int64_t)ds->stat_stem_sent);
    json_push_kv_int(out, "stat_stem_recv", (int64_t)ds->stat_stem_recv);
    json_push_kv_int(out, "stat_fluffed", (int64_t)ds->stat_fluffed);
    json_push_kv_int(out, "stat_embargo_fluff",
                     (int64_t)ds->stat_embargo_fluff);
    json_push_kv_int(out, "stat_served", (int64_t)ds->stat_served);
    json_push_kv_int(out, "stat_refused", (int64_t)ds->stat_refused);
    zcl_mutex_unlock(&ds->cs);
    return true;
}

#ifdef ZCL_TESTING
/* acceptance hooks. These exercise the same RNG-driven shuffle,
 * coin-flip, and exponential sampler used by the production paths, in
 * a form that doesn't require a populated net_manager. NOT for
 * production callers. */

bool dandelion_test_shuffle(node_id_t *inout, int n)
{
    if (!inout || n < 0)
        return false;
    for (int i = n - 1; i > 0; i--) {
        uint64_t r;
        if (!dandelion_secret_u64(&r, "dandelion-test-shuffle"))
            return false;
        int j = (int)(r % (uint64_t)(i + 1));
        node_id_t tmp = inout[i];
        inout[i] = inout[j];
        inout[j] = tmp;
    }
    return true;
}

bool dandelion_test_should_stem_coin(bool *out_stem)
{
    if (!out_stem)
        return false;
    uint64_t r;
    if (!dandelion_secret_u64(&r, "dandelion-test-coin"))
        return false;
    *out_stem = ((r % 100) >= DANDELION_FLUFF_PROB);
    return true;
}

/* Sample one embargo offset (seconds past now+MIN) the same way
 * dandelion_stempool_add does. */
bool dandelion_test_embargo_offset(int64_t *out_secs)
{
    if (!out_secs)
        return false;
    uint64_t r;
    if (!dandelion_secret_u64(&r, "dandelion-test-embargo"))
        return false;
    *out_secs = dandelion_exp_secs((double)DANDELION_EMBARGO_AVG_ADD_SECS, r);
    return true;
}
#endif
