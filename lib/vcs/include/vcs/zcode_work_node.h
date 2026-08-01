/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Bounded requester-owned ZCODE work state over package peers. */

#ifndef ZCL_VCS_ZCODE_WORK_NODE_H
#define ZCL_VCS_ZCODE_WORK_NODE_H

#include "vcs/zcode_work_swarm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VCS_ZCODE_WORK_NODE_MAX_PEERS 64u
#define VCS_ZCODE_WORK_NODE_MAX_REQUESTS 32u
#define VCS_ZCODE_WORK_NODE_MAX_RESULTS 64u
#define VCS_ZCODE_WORK_NODE_MAX_OUTBOUND 128u

struct vcs_zcode_work_node;

enum vcs_zcode_work_node_result {
    VCS_ZCODE_WORK_NODE_OK = 0,
    VCS_ZCODE_WORK_NODE_MALFORMED,
    VCS_ZCODE_WORK_NODE_UNKNOWN_PEER,
    VCS_ZCODE_WORK_NODE_CAPABILITY_STALE,
    VCS_ZCODE_WORK_NODE_CAPABILITY_MISMATCH,
    VCS_ZCODE_WORK_NODE_REPLAY,
    VCS_ZCODE_WORK_NODE_UNREQUESTED,
    VCS_ZCODE_WORK_NODE_BINDING,
    VCS_ZCODE_WORK_NODE_FULL,
    VCS_ZCODE_WORK_NODE_NOT_LOCAL_WORKER,
};

const char *vcs_zcode_work_node_result_string(
    enum vcs_zcode_work_node_result result);

struct vcs_zcode_work_node *vcs_zcode_work_node_create(void);
void vcs_zcode_work_node_free(struct vcs_zcode_work_node *node);
void vcs_zcode_work_node_set_global(struct vcs_zcode_work_node *node);
struct vcs_zcode_work_node *vcs_zcode_work_node_global(void);

bool vcs_zcode_work_node_peer_add(struct vcs_zcode_work_node *node,
                                  uint64_t peer);
void vcs_zcode_work_node_peer_drop(struct vcs_zcode_work_node *node,
                                   uint64_t peer);

/* The caller seals this capability before installation. Setting it queues an
 * advertisement to every current peer; peer_add queues it for the new peer. */
bool vcs_zcode_work_node_set_local_capability(
    struct vcs_zcode_work_node *node,
    const struct vcs_zcode_work_capability_v1 *capability);

bool vcs_zcode_work_node_peer_capability(
    struct vcs_zcode_work_node *node, uint64_t peer, int64_t now,
    struct vcs_zcode_work_capability_v1 *out);

/* Requester coordination. No automatic peer selection exists: the requester
 * chooses one advertised peer and owns its deadline, cancellation, and quorum. */
enum vcs_zcode_work_node_result vcs_zcode_work_node_submit(
    struct vcs_zcode_work_node *node, uint64_t peer,
    const struct vcs_zcode_work_request_v1 *request, int64_t now);
enum vcs_zcode_work_node_result vcs_zcode_work_node_cancel(
    struct vcs_zcode_work_node *node, uint64_t peer,
    const struct vcs_zcode_work_cancel_v1 *cancel);

/* Worker response for a request previously drained from next_request. */
enum vcs_zcode_work_node_result vcs_zcode_work_node_publish_result(
    struct vcs_zcode_work_node *node, uint64_t peer,
    const struct vcs_zcode_work_result_v1 *result);

/* Deliver one authenticated ZCWS frame from an existing package-swarm peer. */
enum vcs_zcode_work_node_result vcs_zcode_work_node_handle_frame(
    struct vcs_zcode_work_node *node, uint64_t peer,
    const uint8_t *wire, size_t wire_len, int64_t now);

/* Transport and application drains. All are bounded FIFO operations. */
bool vcs_zcode_work_node_next_outbound(
    struct vcs_zcode_work_node *node, uint64_t peer_filter,
    uint64_t *peer_out, uint8_t out[VCS_ZCODE_WORK_SWARM_MAX_WIRE_BYTES],
    size_t *out_len);
bool vcs_zcode_work_node_next_request(
    struct vcs_zcode_work_node *node, uint64_t *peer_out,
    struct vcs_zcode_work_request_v1 *out);
bool vcs_zcode_work_node_next_cancel(
    struct vcs_zcode_work_node *node, uint64_t *peer_out,
    struct vcs_zcode_work_cancel_v1 *out);
bool vcs_zcode_work_node_next_result(
    struct vcs_zcode_work_node *node, uint64_t *peer_out,
    struct vcs_zcode_work_result_v1 *out);

#endif /* ZCL_VCS_ZCODE_WORK_NODE_H */
