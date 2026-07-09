/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Private simnet_wire implementation state shared by the transport engine
 * and adversarial peer byte generators.
 */

#ifndef ZCL_SIMNET_WIRE_INTERNAL_H
#define ZCL_SIMNET_WIRE_INTERNAL_H

#include "sim/simnet_wire.h"

#include "chain/chainparams.h"
#include "coins/coins_view.h"
#include "coins/utxo_commitment.h"
#include "consensus/validation.h"
#include "core/uint256.h"
#include "event/event.h"
#include "net/msgprocessor.h"
#include "net/net.h"
#include "net/protocol.h"
#include "sim/seed_tape.h"
#include "sim/simnet.h"
#include "sync/sync_state.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIMNET_WIRE_EVENT_ENQUEUED 129u
#define SIMNET_WIRE_MIN_LATENCY_US 50u
#define SIMNET_WIRE_LATENCY_SPAN_US 5000u
#define SIMNET_WIRE_REORDER_SPAN_US 17u
#define SIMNET_WIRE_DEFAULT_RING_CAP 4096u
#define SIMNET_WIRE_MAX_PEERS 1024u
#define SIMNET_WIRE_IO_CHUNK_BASE 0x10000u
#define SIMNET_WIRE_FNV_OFFSET 1469598103934665603ULL
#define SIMNET_WIRE_FNV_PRIME 1099511628211ULL

enum wire_event_kind {
    WIRE_EVENT_DELIVER_TO_NUT = 1,
    WIRE_EVENT_OPEN = 2,
    WIRE_EVENT_CLOSE = 3,
    WIRE_EVENT_PARTITION = 4,
};

struct wire_byte_ring {
    uint8_t *data;
    size_t cap;
    size_t head;
    size_t len;
};

struct wire_link {
    struct wire_byte_ring to_nut;
    struct wire_byte_ring to_peer;
    bool open;
    size_t down_tokens;
    size_t up_tokens;
};

struct wire_event {
    size_t peer_id;
    uint64_t deliver_us;
    uint64_t seq;
    enum wire_event_kind kind;
    uint8_t *bytes;
    size_t len;
};

struct wire_peer {
    enum simnet_wire_peer_kind kind;
    struct wire_link link;
    struct net_address addr;
    bool version_sent;
    bool verack_sent;
    bool saw_nut_version;
    bool saw_nut_verack;
    bool saw_nut_sendheaders;
    bool saw_nut_pong;
    uint64_t ping_nonce_sent;
    uint64_t last_pong_nonce;

    uint64_t child_seed;
    bool adversary_started;
    bool adversary_done;
    bool not_implemented;
    bool ban_expected;
    bool flood_active;
    uint64_t flood_ticks;
    enum simnet_wire_malformed_case malformed_case;
    enum simnet_wire_bad_handshake_case bad_handshake_case;
    enum simnet_byzantine_class byz_kind;
    bool byz_kind_set;
    bool byz_injected;
    uint8_t *slowloris_frame;
    size_t slowloris_len;
    size_t slowloris_pos;
};

struct simnet_wire_monitor {
    bool recv_queue_bounded;
    bool no_unexpected_permanent_blocker;
    bool memory_plateau_ok;
    bool consensus_unchanged;
    bool failed;
    bool warned_memory_growth;
    bool saved_capsule;
    size_t max_recv_msg_count;
    size_t max_send_size;
    size_t max_inventory_to_send;
    size_t max_addr_to_send;
    struct uint256 baseline_tip;
    struct utxo_commitment baseline_coins;
};

struct simnet_wire_event_counts {
    uint64_t checksum_fail;
    uint64_t peer_misbehave;
    uint64_t backpressure_reject;
    uint64_t peer_banned;
    uint64_t block_rejected;
    uint64_t headers_rejected;
};

struct simnet_wire {
    size_t peer_count;
    struct wire_peer *peers;
    seed_tape_t *tape;
    uint64_t master_seed;

    struct net_manager nm;
    struct msg_processor mp;
    struct main_state ms;
    struct tx_mempool mempool;
    struct coins_view null_view;
    struct coins_view_cache coins_tip;
    const struct chain_params *params;
    struct p2p_node *nut;
    struct send_segment *send_sentinel;
    bool main_ready;
    bool mempool_ready;
    bool coins_ready;
    bool net_ready;

    struct wire_event *queue;
    size_t queue_count;
    size_t queue_cap;
    uint64_t next_seq;
    uint64_t fingerprint;
    uint64_t ticks;
    uint64_t delivered_to_nut_bytes;
    uint64_t delivered_to_peer_bytes;
    uint64_t not_implemented_peers;
    struct simnet_wire_monitor monitor;
    struct simnet_wire_event_counts events;

    struct simnet byz_sim;
    bool byz_sim_ready;
    struct simnet_byzantine_block_case byz_block;
    bool byz_block_ready;
    struct simnet_byzantine_header_case byz_header;
    bool byz_header_ready;
    struct simnet_wire_byzantine_observation byz_obs;
    bool byz_obs_ready;
    struct uint256 byz_baseline_tip;
    struct utxo_commitment byz_baseline_coins;
    enum sync_state byz_saved_sync_state;
    bool byz_saved_sync_state_valid;
    bool byz_honest_after_attempted;
};

struct wire_event_record {
    uint64_t peer_id;
    uint64_t deliver_us;
    uint64_t seq;
    uint64_t len;
    uint64_t bytes_hash;
    uint8_t kind;
};

uint64_t simnet_wire_splitmix64_value(uint64_t x);
uint64_t simnet_wire_fnv_bytes(const uint8_t *bytes, size_t len);

bool simnet_wire_frame(struct simnet_wire *wire, const char *command,
                       const uint8_t *payload, size_t payload_len,
                       uint8_t **out, size_t *out_len);
bool simnet_wire_enqueue_frame(struct simnet_wire *wire, size_t peer_id,
                               const char *command, const uint8_t *payload,
                               size_t payload_len);
bool simnet_wire_enqueue_raw(struct simnet_wire *wire, size_t peer_id,
                             const uint8_t *bytes, size_t len);
bool simnet_wire_deliver_raw_now(struct simnet_wire *wire, size_t peer_id,
                                 const uint8_t *bytes, size_t len);
bool simnet_wire_enqueue_version(struct simnet_wire *wire, size_t peer_id);
bool simnet_wire_peer_tick(struct simnet_wire *wire, size_t peer_id,
                           bool *progress);
void simnet_wire_fp_mix(struct simnet_wire *wire, size_t peer_id,
                        uint8_t direction, const uint8_t *bytes,
                        size_t len);
void simnet_wire_mark_monitor_failed(struct simnet_wire *wire,
                                     const char *reason);
bool simnet_wire_monitor_after_tick(struct simnet_wire *wire);
bool simnet_wire_monitor_finish(struct simnet_wire *wire);

bool simnet_wire_byzantine_start(struct simnet_wire *wire, size_t peer_id,
                                 enum simnet_byzantine_class kind,
                                 enum simnet_byzantine_tier tier);
bool simnet_wire_byzantine_tick(struct simnet_wire *wire, size_t peer_id,
                                bool *progress);
bool simnet_wire_byzantine_submit_block(struct block *block,
                                        struct validation_state *out,
                                        void *ctx);
void simnet_wire_byzantine_observe_event(struct simnet_wire *wire,
                                         enum event_type type,
                                         const void *payload,
                                         uint32_t payload_len);
bool simnet_wire_byzantine_expected_blocker(
    const struct simnet_wire *wire, const char *id, int cls);
void simnet_wire_byzantine_after_tick(struct simnet_wire *wire);
void simnet_wire_byzantine_free(struct simnet_wire *wire);
bool simnet_wire_byzantine_get_observation(
    const struct simnet_wire *wire,
    struct simnet_wire_byzantine_observation *out);

#endif /* ZCL_SIMNET_WIRE_INTERNAL_H */
