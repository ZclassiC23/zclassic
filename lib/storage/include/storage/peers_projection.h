/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_STORAGE_PEERS_PROJECTION_H
#define ZCL_STORAGE_PEERS_PROJECTION_H

#include "storage/event_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct peers_projection peers_projection_t;

peers_projection_t *peers_projection_open(const char *projection_path,
                                          event_log_t *log);
void peers_projection_close(peers_projection_t *p);

uint64_t peers_projection_catch_up(peers_projection_t *p);

bool peers_projection_get(peers_projection_t *p,
                          const uint8_t ip[16], uint16_t port,
                          uint64_t *services_out,
                          int64_t *last_seen_out,
                          int32_t *height_hint_out);

uint64_t peers_projection_count(peers_projection_t *p);

/* Process-global projection wiring used by the peer model. NULL log
 * disables emission and keeps legacy writes authoritative.
 */
void peers_projection_set_event_log(event_log_t *log);
event_log_t *peers_projection_event_log(void);

bool peers_projection_emit_observed(const uint8_t ip[16], uint16_t port,
                                    uint64_t services, int64_t observed_unix,
                                    int32_t height_hint);
bool peers_projection_emit_dropped(const uint8_t ip[16], uint16_t port,
                                   uint8_t reason);

/* Bank a closed peer session: its final reputation + transfer totals. Folds
 * into the append-only peer_sessions ledger AND updates the durable
 * `addresses` reputation columns (bandwidth_score/avg_latency_us/
 * sessions_count/last_useful_time/headers_delivered/blocks_delivered).
 * NULL/absent event log → no-op returning false (fail-open). */
bool peers_projection_emit_session_closed(const uint8_t ip[16], uint16_t port,
                                          uint8_t reason,
                                          uint32_t duration_secs,
                                          uint64_t bytes_in, uint64_t bytes_out,
                                          uint64_t headers_delivered,
                                          uint64_t blocks_delivered,
                                          uint32_t bandwidth_score,
                                          int64_t avg_latency_us,
                                          int64_t last_useful_time);

/* Bank a network fork observation into the append-only fork_events ledger.
 * tip_hash_a/b are NUL-terminated hex (may be NULL/empty). */
bool peers_projection_emit_fork_observed(int64_t height, int64_t observed_unix,
                                         uint32_t num_clusters,
                                         uint32_t count_a, uint32_t count_b,
                                         const char *tip_hash_a,
                                         const char *tip_hash_b);

/* Durable per-peer reputation, folded from the session ledger. All fields are
 * 0 (bandwidth_score/latency) or the accumulated totals for a known address;
 * a missing/never-banked address reads all-zero via the `false` return. */
struct peer_reputation {
    uint32_t bandwidth_score;
    int64_t  avg_latency_us;
    int64_t  sessions_count;
    int64_t  last_useful_time;
    int64_t  headers_delivered;
    int64_t  blocks_delivered;
};

bool peers_projection_get_reputation(peers_projection_t *p,
                                     const uint8_t ip[16], uint16_t port,
                                     struct peer_reputation *out);

/* Process-global convenience (uses the singleton opened at boot) — the
 * connect/dial paths in net/config read reputation without threading a
 * handle. Returns false (all-zero) when no projection is open or no row. */
bool peers_projection_get_reputation_global(const uint8_t ip[16], uint16_t port,
                                            struct peer_reputation *out);

/* Iterate every address that has banked reputation (sessions_count>0), newest
 * first, bounded by `max`. Returns the number visited. Global-singleton form
 * for the boot addrman-seed step. cb must not re-enter the projection. */
typedef void (*peers_reputation_cb)(const uint8_t ip[16], uint16_t port,
                                    const struct peer_reputation *rep,
                                    void *ctx);
size_t peers_projection_for_each_reputation_global(size_t max,
                                                   peers_reputation_cb cb,
                                                   void *ctx);

struct json_value;
bool peers_projection_dump_state_json(struct json_value *out, const char *key);

#ifdef ZCL_TESTING
/* Lower the append-only ledger retention caps so the delete-oldest path is
 * provable without inserting 50,000 rows. */
void peers_projection_test_set_retention_caps(uint64_t sessions_cap,
                                              uint64_t forks_cap);
void peers_projection_test_reset_retention_caps(void);
/* Row count of a ledger table ("peer_sessions" / "fork_events"), -1 on error. */
int64_t peers_projection_test_ledger_count(peers_projection_t *p,
                                           const char *table);
#endif

#endif /* ZCL_STORAGE_PEERS_PROJECTION_H */
