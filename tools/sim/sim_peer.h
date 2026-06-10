/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_TOOLS_SIM_SIM_PEER_H
#define ZCL_TOOLS_SIM_SIM_PEER_H

#include <stdbool.h>
#include <stddef.h>

#define SIM_PEER_MAX 1024u

struct sim_peer {
    unsigned id;
    bool connected;
    unsigned blocks_sent;
    size_t block_bytes_sent;
    char last_block_file[128];
    unsigned malformed_blocks_sent;
    char last_malformed_type[32];
};

struct sim_peer_set {
    struct sim_peer peers[SIM_PEER_MAX];
    unsigned count;
    unsigned active_count;
    unsigned killed_count;
    unsigned blocks_sent;
    size_t block_bytes_sent;
    unsigned malformed_blocks_sent;
    unsigned malformed_blocks_rejected;
};

void sim_peer_set_init(struct sim_peer_set *set);
int sim_peer_set_resize(struct sim_peer_set *set, unsigned count);
int sim_peer_kill(struct sim_peer_set *set, unsigned id);
int sim_peer_send_block(struct sim_peer_set *set, unsigned id,
                        const char *path, size_t *bytes_read);
bool sim_peer_malformed_type_known(const char *type);
int sim_peer_send_malformed_block(struct sim_peer_set *set, unsigned id,
                                  const char *type);
const struct sim_peer *sim_peer_get(const struct sim_peer_set *set,
                                    unsigned id);

#endif /* ZCL_TOOLS_SIM_SIM_PEER_H */
