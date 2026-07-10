/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * One typed, read-only chain-frontier contract shared by native RPC, MCP and
 * the compact agent surface.  Collection never invents a target from peers:
 * served H*, raw indexed window, and locally validated header stay distinct. */

#ifndef ZCL_SERVICES_CHAIN_FRONTIER_SNAPSHOT_SERVICE_H
#define ZCL_SERVICES_CHAIN_FRONTIER_SNAPSHOT_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

struct main_state;

enum chain_frontier_authority_source {
    CHAIN_FRONTIER_AUTHORITY_NONE = 0,
    CHAIN_FRONTIER_AUTHORITY_RUNTIME_PUBLICATION,
    CHAIN_FRONTIER_AUTHORITY_DURABLE_TIP_FINALIZE_LOG,
};

struct chain_frontier_value {
    bool height_known;
    bool binding_known;
    bool status_known;
    bool validity_sufficient;
    bool failure_free;
    int32_t height;
    uint32_t status;
    char hash[65];
    char chain_work[65];
};

struct chain_frontier_snapshot {
    bool context_known;
    bool hstar_published;
    bool authority_pair_known;
    bool durable_authority_known;
    bool authority_matches_served;
    enum chain_frontier_authority_source authority_source;
    bool ancestry_known;
    bool served_ancestor_indexed;
    bool indexed_ancestor_header;
    bool work_known;
    bool work_monotone;
    bool validity_known;
    bool validity_sufficient;
    bool failure_free;
    struct chain_frontier_value served;
    struct chain_frontier_value indexed;
    struct chain_frontier_value header;
};

void chain_frontier_snapshot_collect(struct chain_frontier_snapshot *out,
                                     struct main_state *main);
bool chain_frontier_snapshot_equal(const struct chain_frontier_snapshot *a,
                                   const struct chain_frontier_snapshot *b);
bool chain_frontier_snapshot_values_known(
    const struct chain_frontier_snapshot *snapshot);
bool chain_frontier_snapshot_bindings_known(
    const struct chain_frontier_snapshot *snapshot);
bool chain_frontier_snapshot_consistent(
    const struct chain_frontier_snapshot *snapshot);
const char *chain_frontier_authority_source_name(
    enum chain_frontier_authority_source source);

#endif
