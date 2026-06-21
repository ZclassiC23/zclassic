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

struct json_value;
bool peers_projection_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_STORAGE_PEERS_PROJECTION_H */
