/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Dandelion Transaction Propagation (BIP 156)
 *
 * Hides the origin of transactions by splitting relay into two phases:
 *
 *   Stem phase — a tx travels hop-by-hop along a private path. Each
 *                node picks up to 2 outbound "Dandelion destinations"
 *                per epoch and maps every inbound edge (and the local
 *                wallet) to one destination for the whole epoch.
 *                Stem txs live in the STEMPOOL, not the mempool, and
 *                are announced with inv type MSG_DANDELION_TX. A
 *                stempool tx is served only to the peer it was
 *                advertised to.
 *
 *   Fluff phase — normal inv/getdata diffusion to all peers. Entered
 *                 (a) with 10% probability at each stem hop,
 *                 (b) when a per-tx random embargo timer expires
 *                     before the tx is seen back in the mempool,
 *                 (c) when no Dandelion-capable destination exists.
 *                 On fluff the tx moves stempool -> mempool.
 *
 * Wire protocol: inv type MSG_DANDELION_TX (5) announces a stem tx;
 * the peer requests it with getdata of the same type; the payload is
 * a normal "tx" message. Dandelion support is signalled with the
 * NODE_DANDELION service bit (net/protocol.h).
 *
 * References: BIP 156; Fanti et al., "Dandelion++: Lightweight
 * Cryptocurrency Networking with Formal Anonymity Guarantees"
 * (SIGMETRICS 2018).
 */

#ifndef ZCL_DANDELION_H
#define ZCL_DANDELION_H

#include "core/uint256.h"
#include "net/net.h"
#include "util/sync.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ── Constants ─────────────────────────────────────────────────── */

/* Route re-shuffle interval: random per epoch, exponential with this
 * mean (BIP 156: "random intervals ... ten minutes on average"),
 * clamped to [DANDELION_EPOCH_MIN_SECS, DANDELION_EPOCH_MAX_SECS]. */
#define DANDELION_EPOCH_MEAN_SECS 600
#define DANDELION_EPOCH_MIN_SECS  60
#define DANDELION_EPOCH_MAX_SECS  1800

/* Embargo timer: random per tx — now + MIN + exponential(AVG_ADD).
 * If the tx isn't seen back in the mempool before expiry, this node
 * fluffs it (fail-safe against black-hole stem hops). */
#define DANDELION_EMBARGO_MIN_SECS     10
#define DANDELION_EMBARGO_AVG_ADD_SECS 20

/* Probability of transitioning from stem to fluff at each relay hop.
 * Stored as percent (0-100). BIP 156 recommends 10%. */
#define DANDELION_FLUFF_PROB      10

/* Maximum number of transactions in the stempool. Oldest entries are
 * evicted (dropped) when the limit is reached. */
#define DANDELION_MAX_STEMPOOL    1024

/* Number of outbound Dandelion destinations per epoch (BIP 156:
 * "two Dandelion destinations ... without replacement"). */
#define DANDELION_NUM_STEM_PEERS  2

/* Advertisement table: which peer each stem tx was announced to.
 * Sized to a couple of adverts per stempool entry. */
#define DANDELION_MAX_ADVERTS     2048

/* Pending getdata(MSG_DANDELION_TX) requests we have issued. */
#define DANDELION_MAX_REQUESTS    256

/* Stale-entry pruning horizon for adverts/requests (seconds). */
#define DANDELION_TABLE_TTL_SECS  120

/* ── Stempool entry ────────────────────────────────────────────── */

struct dandelion_stem_entry {
    struct uint256 txhash;       /* tx hash */
    int64_t        embargo_time; /* unix timestamp when embargo expires */
    node_id_t      from_peer;    /* peer that sent this tx (loop detect) */
    node_id_t      dest_peer;    /* destination we forwarded/announced to,
                                    or DANDELION_NODE_ID_NONE */
    uint8_t       *tx_bytes;     /* owned serialized tx (zcl_malloc) */
    size_t         tx_size;
    bool           active;       /* slot in use */
};

/* A stempool tx handed back to the caller for fluffing. The caller
 * takes ownership of tx_bytes and must free() it. */
struct dandelion_fluff_item {
    struct uint256 txhash;
    uint8_t       *tx_bytes;
    size_t         tx_size;
};

/* (txhash, peer) pair used for both the advertisement table and the
 * pending-request table. */
struct dandelion_tx_peer {
    struct uint256 txhash;
    node_id_t      peer;
    int64_t        time;
    bool           active;
};

/* ── Dandelion state ───────────────────────────────────────────── */

struct dandelion_state {
    /* Epoch management */
    int64_t    epoch_start;                            /* unix time */
    int64_t    epoch_len_secs;                         /* random per epoch */
    uint64_t   route_salt;                             /* per-epoch salt */
    node_id_t  stem_peers[DANDELION_NUM_STEM_PEERS];   /* destinations */
    int        num_stem_peers;

    /* Stempool: full stem txs under embargo, NOT in the mempool */
    struct dandelion_stem_entry stempool[DANDELION_MAX_STEMPOOL];
    int        stempool_count;

    /* Which peer each stem tx was advertised to (serve gate) */
    struct dandelion_tx_peer adverts[DANDELION_MAX_ADVERTS];

    /* getdata(MSG_DANDELION_TX) we sent and await a tx reply for */
    struct dandelion_tx_peer requests[DANDELION_MAX_REQUESTS];

    /* Thread safety */
    zcl_mutex_t cs;

    /* Enabled flag (can be toggled at runtime) */
    bool       enabled;

    /* Stats */
    uint64_t   stat_stem_sent;     /* stem announcements forwarded */
    uint64_t   stat_stem_recv;     /* stem txs received via dandelion getdata */
    uint64_t   stat_fluffed;       /* txs transitioned to fluff (coin/inv) */
    uint64_t   stat_embargo_fluff; /* txs fluffed due to embargo timeout */
    uint64_t   stat_served;        /* getdata(MSG_DANDELION_TX) served */
    uint64_t   stat_refused;       /* dandelion getdata refused (not advertised) */
};

/* ── Process-wide enable switch (set from -dandelion=0|1) ──────── */

/* Default true. Gates the NODE_DANDELION service bit and all stem
 * routing; when false every tx fluffs immediately (legacy relay). */
bool dandelion_enabled(void);
void dandelion_set_enabled(bool enabled);

/* ── Lifecycle ─────────────────────────────────────────────────── */

void dandelion_init(struct dandelion_state *ds);
void dandelion_free(struct dandelion_state *ds);

/* ── Epoch management ──────────────────────────────────────────── */

/* Check if epoch has expired and rotate destinations if needed.
 * Must be called periodically (e.g., from message send loop).
 * nm is used to enumerate connected peers; only outbound peers
 * advertising NODE_DANDELION are eligible. */
void dandelion_maybe_rotate_epoch(struct dandelion_state *ds,
                                  struct net_manager *nm);

/* ── Core routing ──────────────────────────────────────────────── */

/* Per-hop fluff coin for a RECEIVED dandelion tx. Returns true if the
 * tx should continue along the stem, false if it should fluff.
 * Locally originated txs always stem when a destination exists (the
 * coin applies to relay hops only, per BIP 156). On RNG failure:
 * fluff (safe-fail). */
bool dandelion_should_stem(struct dandelion_state *ds, node_id_t from_peer);

/* Per-inbound-edge destination: returns the Dandelion destination this
 * epoch for txs arriving from from_peer (DANDELION_NODE_ID_NONE for
 * local txs). Stable within an epoch (salt-keyed), never returns
 * from_peer itself; DANDELION_NODE_ID_NONE if no destination is
 * available (caller should fluff). */
node_id_t dandelion_get_stem_peer(struct dandelion_state *ds,
                                  node_id_t from_peer);

/* ── Stempool ──────────────────────────────────────────────────── */

/* Add a tx (serialized bytes are COPIED) to the stempool with a
 * random embargo. dest_peer records where we forwarded it (may be
 * DANDELION_NODE_ID_NONE before announcement). Returns false on
 * duplicate or allocation failure (caller should fluff on failure). */
bool dandelion_stempool_add(struct dandelion_state *ds,
                            const struct uint256 *txhash,
                            node_id_t from_peer,
                            node_id_t dest_peer,
                            const uint8_t *tx_bytes,
                            size_t tx_size);

/* Remove a tx from the stempool and discard its bytes (called when we
 * see it fluffed by someone else). Returns true if it was present. */
bool dandelion_stempool_remove(struct dandelion_state *ds,
                               const struct uint256 *txhash);

/* Remove a tx from the stempool, transferring ownership of its bytes
 * to the caller (for accept-to-mempool on fluff). Returns true and
 * fills *out if present; caller must free(out->tx_bytes). */
bool dandelion_stempool_take(struct dandelion_state *ds,
                             const struct uint256 *txhash,
                             struct dandelion_fluff_item *out);

/* Copy a stempool tx's serialized bytes (zcl_malloc'd; caller frees)
 * without removing it — used to serve getdata(MSG_DANDELION_TX) and
 * to forward along the stem. NULL if absent. */
uint8_t *dandelion_stempool_copy(struct dandelion_state *ds,
                                 const struct uint256 *txhash,
                                 size_t *out_size);

/* Check if a tx is currently in the stempool. */
bool dandelion_stempool_contains(struct dandelion_state *ds,
                                 const struct uint256 *txhash);

/* Collect txs whose embargo expired (or whose recorded stem
 * destination is no longer connected, when nm is non-NULL), removing
 * them from the stempool. Ownership of each item's tx_bytes passes to
 * the caller, who must accept-to-mempool + relay, then free(). Also
 * prunes stale advert/request entries. Returns the item count. */
int dandelion_stempool_take_expired(struct dandelion_state *ds,
                                    struct net_manager *nm,
                                    struct dandelion_fluff_item *out,
                                    int max_out);

/* ── Advertisement gate (BIP 156 serve rule) ───────────────────── */

void dandelion_mark_advertised(struct dandelion_state *ds,
                               const struct uint256 *txhash,
                               node_id_t peer);
bool dandelion_was_advertised_to(struct dandelion_state *ds,
                                 const struct uint256 *txhash,
                                 node_id_t peer);

/* ── Pending dandelion getdata tracking ────────────────────────── */

/* Record that we sent getdata(MSG_DANDELION_TX) for txhash to peer. */
void dandelion_request_add(struct dandelion_state *ds,
                           const struct uint256 *txhash,
                           node_id_t peer);
/* True if a dandelion getdata for txhash is outstanding (any peer). */
bool dandelion_request_pending(struct dandelion_state *ds,
                               const struct uint256 *txhash);
/* If we requested txhash from peer, consume the entry and return
 * true — the arriving "tx" payload is a stem tx. */
bool dandelion_request_take(struct dandelion_state *ds,
                            const struct uint256 *txhash,
                            node_id_t peer);

/* ── Introspection ─────────────────────────────────────────────── */

/* See CLAUDE.md "Adding state introspection". Reentrant-safe. */
struct json_value;
bool dandelion_dump_state_json(struct json_value *out, const char *key);

/* Sentinel value for "no peer" / "local origin" */
#define DANDELION_NODE_ID_NONE  ((node_id_t)-1)

#endif
