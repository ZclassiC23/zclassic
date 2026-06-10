/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "sim/sim_peer.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

void sim_peer_set_init(struct sim_peer_set *set)
{
    if (!set) return;
    set->count = 0;
    set->active_count = 0;
    set->killed_count = 0;
    set->blocks_sent = 0;
    set->block_bytes_sent = 0;
    set->malformed_blocks_sent = 0;
    set->malformed_blocks_rejected = 0;
}

int sim_peer_set_resize(struct sim_peer_set *set, unsigned count)
{
    if (!set) return -EINVAL;
    if (count > SIM_PEER_MAX) return -ERANGE;

    set->count = count;
    set->active_count = count;
    set->killed_count = 0;
    set->blocks_sent = 0;
    set->block_bytes_sent = 0;
    set->malformed_blocks_sent = 0;
    set->malformed_blocks_rejected = 0;
    for (unsigned i = 0; i < count; i++) {
        set->peers[i].id = i;
        set->peers[i].connected = true;
        set->peers[i].blocks_sent = 0;
        set->peers[i].block_bytes_sent = 0;
        set->peers[i].last_block_file[0] = '\0';
        set->peers[i].malformed_blocks_sent = 0;
        set->peers[i].last_malformed_type[0] = '\0';
    }
    for (unsigned i = count; i < SIM_PEER_MAX; i++) {
        set->peers[i].id = i;
        set->peers[i].connected = false;
        set->peers[i].blocks_sent = 0;
        set->peers[i].block_bytes_sent = 0;
        set->peers[i].last_block_file[0] = '\0';
        set->peers[i].malformed_blocks_sent = 0;
        set->peers[i].last_malformed_type[0] = '\0';
    }
    return 0;
}

int sim_peer_kill(struct sim_peer_set *set, unsigned id)
{
    if (!set) return -EINVAL;
    if (id >= set->count) return -ENOENT;
    if (!set->peers[id].connected) return -EALREADY;

    set->peers[id].connected = false;
    if (set->active_count > 0)
        set->active_count--;
    set->killed_count++;
    return 0;
}

int sim_peer_send_block(struct sim_peer_set *set, unsigned id,
                        const char *path, size_t *bytes_read)
{
    if (!set || !path || !*path) return -EINVAL;
    if (id >= set->count) return -ENOENT;
    if (!set->peers[id].connected) return -ENOTCONN;
    if (bytes_read) *bytes_read = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) return -errno;
    int seek_rc = fseek(fp, 0, SEEK_END);
    long size = seek_rc == 0 ? ftell(fp) : -1;
    int close_rc = fclose(fp);
    if (seek_rc != 0 || size < 0) return -EIO;
    if (close_rc != 0) return -EIO;
    if (size == 0) return -ENODATA;

    size_t bytes = (size_t)size;
    set->peers[id].blocks_sent++;
    set->peers[id].block_bytes_sent += bytes;
    snprintf(set->peers[id].last_block_file,
             sizeof(set->peers[id].last_block_file), "%s", path);
    set->blocks_sent++;
    set->block_bytes_sent += bytes;
    if (bytes_read) *bytes_read = bytes;
    return 0;
}

bool sim_peer_malformed_type_known(const char *type)
{
    static const char *const known[] = {
        "invalid_pow",
        "bad_merkle",
        "bad_timestamp",
        "bad_size",
        "bad_coinbase",
        "duplicate_tx",
        "bad_bits",
        "bad_nonce",
    };
    if (!type || !*type) return false;
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strcmp(type, known[i]) == 0)
            return true;
    }
    return false;
}

int sim_peer_send_malformed_block(struct sim_peer_set *set, unsigned id,
                                  const char *type)
{
    if (!set || !type) return -EINVAL;
    if (id >= set->count) return -ENOENT;
    if (!set->peers[id].connected) return -ENOTCONN;
    if (!sim_peer_malformed_type_known(type)) return -EINVAL;

    set->peers[id].malformed_blocks_sent++;
    snprintf(set->peers[id].last_malformed_type,
             sizeof(set->peers[id].last_malformed_type), "%s", type);
    set->malformed_blocks_sent++;
    set->malformed_blocks_rejected++;
    return 0;
}

const struct sim_peer *sim_peer_get(const struct sim_peer_set *set,
                                    unsigned id)
{
    if (!set || id >= set->count) return NULL;
    return &set->peers[id];
}
