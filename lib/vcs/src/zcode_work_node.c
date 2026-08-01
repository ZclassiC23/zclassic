/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Bounded requester-owned ZCODE work state over package peers. */

#include "vcs/zcode_work_node.h"

#include "util/log_macros.h"
#include "util/safe_alloc.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct work_peer {
    bool used;
    uint64_t id;
    bool has_capability;
    struct vcs_zcode_work_capability_v1 capability;
};

struct work_track {
    bool used;
    bool inbound;
    bool finished;
    bool cancelled;
    uint64_t peer;
    struct vcs_zcode_work_request_v1 request;
};

struct work_frame {
    uint64_t peer;
    size_t len;
    uint8_t bytes[VCS_ZCODE_WORK_SWARM_MAX_WIRE_BYTES];
};

struct work_request_event {
    uint64_t peer;
    struct vcs_zcode_work_request_v1 request;
};

struct work_cancel_event {
    uint64_t peer;
    struct vcs_zcode_work_cancel_v1 cancel;
};

struct work_result_event {
    uint64_t peer;
    struct vcs_zcode_work_result_v1 result;
};

struct vcs_zcode_work_node {
    pthread_mutex_t lock;
    struct work_peer peers[VCS_ZCODE_WORK_NODE_MAX_PEERS];
    struct work_track tracks[VCS_ZCODE_WORK_NODE_MAX_REQUESTS * 2u];
    bool has_local_capability;
    struct vcs_zcode_work_capability_v1 local_capability;
    struct work_frame outbound[VCS_ZCODE_WORK_NODE_MAX_OUTBOUND];
    size_t outbound_pos, outbound_count;
    struct work_request_event requests[VCS_ZCODE_WORK_NODE_MAX_REQUESTS];
    size_t request_pos, request_count;
    struct work_cancel_event cancels[VCS_ZCODE_WORK_NODE_MAX_REQUESTS];
    size_t cancel_pos, cancel_count;
    struct work_result_event results[VCS_ZCODE_WORK_NODE_MAX_RESULTS];
    size_t result_pos, result_count;
};

static struct vcs_zcode_work_node *g_work_node;

const char *vcs_zcode_work_node_result_string(
    enum vcs_zcode_work_node_result r)
{
    switch (r) {
    case VCS_ZCODE_WORK_NODE_OK: return "ok";
    case VCS_ZCODE_WORK_NODE_MALFORMED: return "malformed-frame";
    case VCS_ZCODE_WORK_NODE_UNKNOWN_PEER: return "unknown-peer";
    case VCS_ZCODE_WORK_NODE_CAPABILITY_STALE: return "capability-stale";
    case VCS_ZCODE_WORK_NODE_CAPABILITY_MISMATCH: return "capability-mismatch";
    case VCS_ZCODE_WORK_NODE_REPLAY: return "replayed-work-frame";
    case VCS_ZCODE_WORK_NODE_UNREQUESTED: return "unrequested-result";
    case VCS_ZCODE_WORK_NODE_BINDING: return "request-result-binding";
    case VCS_ZCODE_WORK_NODE_FULL: return "bounded-queue-full";
    case VCS_ZCODE_WORK_NODE_NOT_LOCAL_WORKER: return "local-worker-disabled";
    }
    return "unknown";
}

struct vcs_zcode_work_node *vcs_zcode_work_node_create(void)
{
    struct vcs_zcode_work_node *node =
        zcl_malloc(sizeof(*node), "zcode.work_node");
    if (!node) return NULL;
    memset(node, 0, sizeof(*node));
    if (pthread_mutex_init(&node->lock, NULL) != 0) {
        free(node);
        LOG_NULL("vcs.work_node", "mutex initialization failed");
    }
    return node;
}

void vcs_zcode_work_node_free(struct vcs_zcode_work_node *node)
{
    if (!node) return;
    pthread_mutex_destroy(&node->lock);
    free(node);
}

void vcs_zcode_work_node_set_global(struct vcs_zcode_work_node *node)
{
    g_work_node = node;
}

struct vcs_zcode_work_node *vcs_zcode_work_node_global(void)
{
    return g_work_node;
}

static int work_peer_slot(const struct vcs_zcode_work_node *node,
                          uint64_t peer)
{
    for (size_t i = 0; i < VCS_ZCODE_WORK_NODE_MAX_PEERS; i++)
        if (node->peers[i].used && node->peers[i].id == peer) return (int)i;
    return -1;
}

static bool work_queue_frame(struct vcs_zcode_work_node *node, uint64_t peer,
                             const struct vcs_zcode_work_swarm_message *message)
{
    if (node->outbound_count >= VCS_ZCODE_WORK_NODE_MAX_OUTBOUND) return false;
    size_t slot = (node->outbound_pos + node->outbound_count) %
                  VCS_ZCODE_WORK_NODE_MAX_OUTBOUND;
    struct work_frame *frame = &node->outbound[slot];
    if (!vcs_zcode_work_swarm_serialize(message, frame->bytes,
                                         sizeof(frame->bytes), &frame->len))
        return false;
    frame->peer = peer;
    node->outbound_count++;
    return true;
}

static bool work_advertise_locked(struct vcs_zcode_work_node *node,
                                  uint64_t peer)
{
    if (!node->has_local_capability) return true;
    struct vcs_zcode_work_swarm_message message = {
        .type = VCS_ZCODE_WORK_SWARM_CAPABILITY,
    };
    message.body.capability = node->local_capability;
    return work_queue_frame(node, peer, &message);
}

bool vcs_zcode_work_node_peer_add(struct vcs_zcode_work_node *node,
                                  uint64_t peer)
{
    if (!node || peer == 0) LOG_FAIL("vcs.work_node", "invalid peer add");
    pthread_mutex_lock(&node->lock);
    int existing = work_peer_slot(node, peer);
    if (existing >= 0) { pthread_mutex_unlock(&node->lock); return true; }
    bool ok = false;
    for (size_t i = 0; i < VCS_ZCODE_WORK_NODE_MAX_PEERS; i++) {
        if (node->peers[i].used) continue;
        node->peers[i].used = true;
        node->peers[i].id = peer;
        ok = work_advertise_locked(node, peer);
        if (!ok) memset(&node->peers[i], 0, sizeof(node->peers[i]));
        break;
    }
    pthread_mutex_unlock(&node->lock);
    return ok;
}

void vcs_zcode_work_node_peer_drop(struct vcs_zcode_work_node *node,
                                   uint64_t peer)
{
    if (!node || peer == 0) return;
    pthread_mutex_lock(&node->lock);
    int slot = work_peer_slot(node, peer);
    if (slot >= 0) memset(&node->peers[slot], 0, sizeof(node->peers[slot]));
    for (size_t i = 0; i < sizeof(node->tracks) / sizeof(node->tracks[0]); i++)
        if (node->tracks[i].used && node->tracks[i].peer == peer)
            memset(&node->tracks[i], 0, sizeof(node->tracks[i]));
    size_t kept = 0;
    for (size_t i = 0; i < node->outbound_count; i++) {
        size_t src = (node->outbound_pos + i) %
                     VCS_ZCODE_WORK_NODE_MAX_OUTBOUND;
        if (node->outbound[src].peer == peer) continue;
        size_t dst = (node->outbound_pos + kept) %
                     VCS_ZCODE_WORK_NODE_MAX_OUTBOUND;
        if (dst != src) node->outbound[dst] = node->outbound[src];
        kept++;
    }
    node->outbound_count = kept;
    pthread_mutex_unlock(&node->lock);
}

bool vcs_zcode_work_node_set_local_capability(
    struct vcs_zcode_work_node *node,
    const struct vcs_zcode_work_capability_v1 *capability)
{
    if (!node || !capability || !vcs_zcode_work_capability_verify(capability))
        return false;
    pthread_mutex_lock(&node->lock);
    node->local_capability = *capability;
    node->has_local_capability = true;
    bool ok = true;
    for (size_t i = 0; i < VCS_ZCODE_WORK_NODE_MAX_PEERS; i++)
        if (node->peers[i].used &&
            !work_advertise_locked(node, node->peers[i].id)) ok = false;
    pthread_mutex_unlock(&node->lock);
    return ok;
}

bool vcs_zcode_work_node_peer_capability(
    struct vcs_zcode_work_node *node, uint64_t peer, int64_t now,
    struct vcs_zcode_work_capability_v1 *out)
{
    if (!node || !out) return false;
    pthread_mutex_lock(&node->lock);
    int slot = work_peer_slot(node, peer);
    bool ok = slot >= 0 && node->peers[slot].has_capability &&
              now < node->peers[slot].capability.expires_unix;
    if (ok) *out = node->peers[slot].capability;
    pthread_mutex_unlock(&node->lock);
    return ok;
}

static bool work_capability_allows(
    const struct vcs_zcode_work_capability_v1 *cap,
    const struct vcs_zcode_work_request_v1 *request, int64_t now)
{
    return cap && request && now < cap->expires_unix &&
           request->deadline_unix > now && cap->queue_headroom > 0 &&
           request->deadline_unix - now <= cap->max_lease_seconds &&
           (cap->work_kinds & (UINT32_C(1) << request->work_kind)) != 0 &&
           (cap->confinement & VCS_ZCODE_WORK_CONFINEMENT_V1_MASK) ==
               VCS_ZCODE_WORK_CONFINEMENT_V1_MASK &&
           cap->target == request->target &&
           memcmp(cap->toolchain_capsule_root,
                  request->toolchain_capsule_root, 32) == 0 &&
           request->max_cpu_seconds <= cap->max_cpu_seconds &&
           request->max_memory_bytes <= cap->max_memory_bytes &&
           request->max_output_bytes <= cap->max_output_bytes;
}

static struct work_track *work_find_track(struct vcs_zcode_work_node *node,
                                          uint64_t peer, uint64_t request_id,
                                          bool inbound)
{
    for (size_t i = 0; i < sizeof(node->tracks) / sizeof(node->tracks[0]); i++)
        if (node->tracks[i].used && node->tracks[i].peer == peer &&
            node->tracks[i].inbound == inbound &&
            node->tracks[i].request.request_id == request_id)
            return &node->tracks[i];
    return NULL;
}

static struct work_track *work_add_track(struct vcs_zcode_work_node *node)
{
    for (size_t i = 0; i < sizeof(node->tracks) / sizeof(node->tracks[0]); i++)
        if (!node->tracks[i].used) return &node->tracks[i];
    return NULL;
}

enum vcs_zcode_work_node_result vcs_zcode_work_node_submit(
    struct vcs_zcode_work_node *node, uint64_t peer,
    const struct vcs_zcode_work_request_v1 *request, int64_t now)
{
    if (!node || !request || !vcs_zcode_work_request_verify(request))
        return VCS_ZCODE_WORK_NODE_MALFORMED;
    pthread_mutex_lock(&node->lock);
    int peer_at = work_peer_slot(node, peer);
    enum vcs_zcode_work_node_result result = VCS_ZCODE_WORK_NODE_OK;
    if (peer_at < 0) result = VCS_ZCODE_WORK_NODE_UNKNOWN_PEER;
    else if (!node->peers[peer_at].has_capability ||
             now >= node->peers[peer_at].capability.expires_unix)
        result = VCS_ZCODE_WORK_NODE_CAPABILITY_STALE;
    else if (!work_capability_allows(&node->peers[peer_at].capability,
                                     request, now))
        result = VCS_ZCODE_WORK_NODE_CAPABILITY_MISMATCH;
    else if (work_find_track(node, peer, request->request_id, false))
        result = VCS_ZCODE_WORK_NODE_REPLAY;
    struct work_track *track = result == VCS_ZCODE_WORK_NODE_OK
                                   ? work_add_track(node) : NULL;
    if (result == VCS_ZCODE_WORK_NODE_OK && !track)
        result = VCS_ZCODE_WORK_NODE_FULL;
    if (result == VCS_ZCODE_WORK_NODE_OK) {
        struct vcs_zcode_work_swarm_message message = {
            .type = VCS_ZCODE_WORK_SWARM_REQUEST, .body.request = *request,
        };
        if (!work_queue_frame(node, peer, &message))
            result = VCS_ZCODE_WORK_NODE_FULL;
        else {
            memset(track, 0, sizeof(*track));
            track->used = true; track->peer = peer; track->request = *request;
            node->peers[peer_at].capability.queue_headroom--;
        }
    }
    pthread_mutex_unlock(&node->lock);
    return result;
}

enum vcs_zcode_work_node_result vcs_zcode_work_node_cancel(
    struct vcs_zcode_work_node *node, uint64_t peer,
    const struct vcs_zcode_work_cancel_v1 *cancel)
{
    if (!node || !cancel || !vcs_zcode_work_cancel_verify(cancel))
        return VCS_ZCODE_WORK_NODE_MALFORMED;
    pthread_mutex_lock(&node->lock);
    struct work_track *track = work_find_track(node, peer, cancel->request_id,
                                                false);
    enum vcs_zcode_work_node_result result = VCS_ZCODE_WORK_NODE_OK;
    if (!track) result = VCS_ZCODE_WORK_NODE_UNREQUESTED;
    else if (track->cancelled || track->finished)
        result = VCS_ZCODE_WORK_NODE_REPLAY;
    else if (memcmp(track->request.task_root, cancel->task_root, 32) != 0 ||
             memcmp(track->request.requester_pubkey,
                    cancel->requester_pubkey, 32) != 0)
        result = VCS_ZCODE_WORK_NODE_BINDING;
    if (result == VCS_ZCODE_WORK_NODE_OK) {
        struct vcs_zcode_work_swarm_message message = {
            .type = VCS_ZCODE_WORK_SWARM_CANCEL, .body.cancel = *cancel,
        };
        if (!work_queue_frame(node, peer, &message))
            result = VCS_ZCODE_WORK_NODE_FULL;
        else {
            track->cancelled = true;
            int peer_at = work_peer_slot(node, peer);
            if (peer_at >= 0 && node->peers[peer_at].has_capability &&
                node->peers[peer_at].capability.queue_headroom <
                    node->peers[peer_at].capability.slots)
                node->peers[peer_at].capability.queue_headroom++;
        }
    }
    pthread_mutex_unlock(&node->lock);
    return result;
}

enum vcs_zcode_work_node_result vcs_zcode_work_node_publish_result(
    struct vcs_zcode_work_node *node, uint64_t peer,
    const struct vcs_zcode_work_result_v1 *result_row)
{
    if (!node || !result_row) return VCS_ZCODE_WORK_NODE_MALFORMED;
    pthread_mutex_lock(&node->lock);
    struct work_track *track = work_find_track(
        node, peer, result_row->request_id, true);
    enum vcs_zcode_work_node_result result = VCS_ZCODE_WORK_NODE_OK;
    if (!node->has_local_capability)
        result = VCS_ZCODE_WORK_NODE_NOT_LOCAL_WORKER;
    else if (!track) result = VCS_ZCODE_WORK_NODE_UNREQUESTED;
    else if (track->finished || track->cancelled)
        result = VCS_ZCODE_WORK_NODE_REPLAY;
    else if (!vcs_zcode_work_result_verify(
                 &track->request, result_row,
                 node->local_capability.signer_pubkey))
        result = VCS_ZCODE_WORK_NODE_BINDING;
    if (result == VCS_ZCODE_WORK_NODE_OK) {
        struct vcs_zcode_work_swarm_message message = {
            .type = VCS_ZCODE_WORK_SWARM_RESULT, .body.result = *result_row,
        };
        if (!work_queue_frame(node, peer, &message))
            result = VCS_ZCODE_WORK_NODE_FULL;
        else {
            track->finished = true;
            if (node->local_capability.queue_headroom <
                node->local_capability.slots)
                node->local_capability.queue_headroom++;
        }
    }
    pthread_mutex_unlock(&node->lock);
    return result;
}

static enum vcs_zcode_work_node_result work_handle_capability(
    struct vcs_zcode_work_node *node, int peer_at,
    const struct vcs_zcode_work_capability_v1 *capability, int64_t now)
{
    if (capability->expires_unix <= now)
        return VCS_ZCODE_WORK_NODE_CAPABILITY_STALE;
    node->peers[peer_at].capability = *capability;
    node->peers[peer_at].has_capability = true;
    return VCS_ZCODE_WORK_NODE_OK;
}

static enum vcs_zcode_work_node_result work_handle_request(
    struct vcs_zcode_work_node *node, uint64_t peer,
    const struct vcs_zcode_work_request_v1 *request, int64_t now)
{
    if (!node->has_local_capability)
        return VCS_ZCODE_WORK_NODE_NOT_LOCAL_WORKER;
    if (!work_capability_allows(&node->local_capability, request, now) ||
        (node->local_capability.confinement &
         VCS_ZCODE_WORK_CONFINEMENT_V1_MASK) !=
            VCS_ZCODE_WORK_CONFINEMENT_V1_MASK)
        return VCS_ZCODE_WORK_NODE_CAPABILITY_MISMATCH;
    if (work_find_track(node, peer, request->request_id, true))
        return VCS_ZCODE_WORK_NODE_REPLAY;
    if (node->request_count >= VCS_ZCODE_WORK_NODE_MAX_REQUESTS)
        return VCS_ZCODE_WORK_NODE_FULL;
    struct work_track *track = work_add_track(node);
    if (!track) return VCS_ZCODE_WORK_NODE_FULL;
    size_t slot = (node->request_pos + node->request_count) %
                  VCS_ZCODE_WORK_NODE_MAX_REQUESTS;
    node->requests[slot].peer = peer;
    node->requests[slot].request = *request;
    node->request_count++;
    memset(track, 0, sizeof(*track));
    track->used = true; track->inbound = true; track->peer = peer;
    track->request = *request;
    node->local_capability.queue_headroom--;
    return VCS_ZCODE_WORK_NODE_OK;
}

static enum vcs_zcode_work_node_result work_handle_cancel(
    struct vcs_zcode_work_node *node, uint64_t peer,
    const struct vcs_zcode_work_cancel_v1 *cancel)
{
    struct work_track *track = work_find_track(node, peer, cancel->request_id,
                                                true);
    if (!track) return VCS_ZCODE_WORK_NODE_UNREQUESTED;
    if (track->cancelled || track->finished)
        return VCS_ZCODE_WORK_NODE_REPLAY;
    if (memcmp(track->request.task_root, cancel->task_root, 32) != 0 ||
        memcmp(track->request.requester_pubkey,
               cancel->requester_pubkey, 32) != 0)
        return VCS_ZCODE_WORK_NODE_BINDING;
    if (node->cancel_count >= VCS_ZCODE_WORK_NODE_MAX_REQUESTS)
        return VCS_ZCODE_WORK_NODE_FULL;
    size_t slot = (node->cancel_pos + node->cancel_count) %
                  VCS_ZCODE_WORK_NODE_MAX_REQUESTS;
    node->cancels[slot].peer = peer;
    node->cancels[slot].cancel = *cancel;
    node->cancel_count++;
    track->cancelled = true;
    if (node->local_capability.queue_headroom <
        node->local_capability.slots)
        node->local_capability.queue_headroom++;
    return VCS_ZCODE_WORK_NODE_OK;
}

static enum vcs_zcode_work_node_result work_handle_result(
    struct vcs_zcode_work_node *node, int peer_at, uint64_t peer,
    const struct vcs_zcode_work_result_v1 *result_row, int64_t now)
{
    struct work_track *track = work_find_track(
        node, peer, result_row->request_id, false);
    if (!track) return VCS_ZCODE_WORK_NODE_UNREQUESTED;
    if (track->finished || track->cancelled)
        return VCS_ZCODE_WORK_NODE_REPLAY;
    if (!node->peers[peer_at].has_capability ||
        now >= node->peers[peer_at].capability.expires_unix)
        return VCS_ZCODE_WORK_NODE_CAPABILITY_STALE;
    if (!vcs_zcode_work_result_verify(
            &track->request, result_row,
            node->peers[peer_at].capability.signer_pubkey))
        return VCS_ZCODE_WORK_NODE_BINDING;
    if (node->result_count >= VCS_ZCODE_WORK_NODE_MAX_RESULTS)
        return VCS_ZCODE_WORK_NODE_FULL;
    size_t slot = (node->result_pos + node->result_count) %
                  VCS_ZCODE_WORK_NODE_MAX_RESULTS;
    node->results[slot].peer = peer;
    node->results[slot].result = *result_row;
    node->result_count++;
    track->finished = true;
    if (node->peers[peer_at].capability.queue_headroom <
        node->peers[peer_at].capability.slots)
        node->peers[peer_at].capability.queue_headroom++;
    return VCS_ZCODE_WORK_NODE_OK;
}

enum vcs_zcode_work_node_result vcs_zcode_work_node_handle_frame(
    struct vcs_zcode_work_node *node, uint64_t peer,
    const uint8_t *wire, size_t wire_len, int64_t now)
{
    if (!node || !wire || peer == 0 || now < 0)
        return VCS_ZCODE_WORK_NODE_MALFORMED;
    struct vcs_zcode_work_swarm_message message;
    if (!vcs_zcode_work_swarm_parse(wire, wire_len, &message))
        return VCS_ZCODE_WORK_NODE_MALFORMED;
    pthread_mutex_lock(&node->lock);
    int peer_at = work_peer_slot(node, peer);
    enum vcs_zcode_work_node_result result = VCS_ZCODE_WORK_NODE_OK;
    if (peer_at < 0) {
        result = VCS_ZCODE_WORK_NODE_UNKNOWN_PEER;
    } else if (message.type == VCS_ZCODE_WORK_SWARM_CAPABILITY) {
        result = work_handle_capability(node, peer_at,
                                        &message.body.capability, now);
    } else if (message.type == VCS_ZCODE_WORK_SWARM_REQUEST) {
        result = work_handle_request(node, peer, &message.body.request, now);
    } else if (message.type == VCS_ZCODE_WORK_SWARM_CANCEL) {
        result = work_handle_cancel(node, peer, &message.body.cancel);
    } else if (message.type == VCS_ZCODE_WORK_SWARM_RESULT) {
        result = work_handle_result(node, peer_at, peer,
                                    &message.body.result, now);
    } else {
        result = VCS_ZCODE_WORK_NODE_MALFORMED;
    }
    pthread_mutex_unlock(&node->lock);
    return result;
}

bool vcs_zcode_work_node_next_outbound(
    struct vcs_zcode_work_node *node, uint64_t peer_filter,
    uint64_t *peer_out, uint8_t out[VCS_ZCODE_WORK_SWARM_MAX_WIRE_BYTES],
    size_t *out_len)
{
    if (!node || !peer_out || !out || !out_len) return false;
    *peer_out = 0; *out_len = 0;
    pthread_mutex_lock(&node->lock);
    size_t found = node->outbound_count;
    for (size_t i = 0; i < node->outbound_count; i++) {
        size_t at = (node->outbound_pos + i) % VCS_ZCODE_WORK_NODE_MAX_OUTBOUND;
        if (!peer_filter || node->outbound[at].peer == peer_filter) {
            found = i; break;
        }
    }
    if (found == node->outbound_count) {
        pthread_mutex_unlock(&node->lock); return false;
    }
    size_t at = (node->outbound_pos + found) % VCS_ZCODE_WORK_NODE_MAX_OUTBOUND;
    struct work_frame selected = node->outbound[at];
    for (size_t i = found; i + 1u < node->outbound_count; i++) {
        size_t dst = (node->outbound_pos + i) % VCS_ZCODE_WORK_NODE_MAX_OUTBOUND;
        size_t src = (node->outbound_pos + i + 1u) % VCS_ZCODE_WORK_NODE_MAX_OUTBOUND;
        node->outbound[dst] = node->outbound[src];
    }
    node->outbound_count--;
    memcpy(out, selected.bytes, selected.len);
    *peer_out = selected.peer; *out_len = selected.len;
    pthread_mutex_unlock(&node->lock);
    return true;
}

#define WORK_DRAIN(name, field, type, max_count) \
bool name(struct vcs_zcode_work_node *node, uint64_t *peer_out, type *out) \
{ \
    if (!node || !peer_out || !out) return false; \
    pthread_mutex_lock(&node->lock); \
    if (node->field##_count == 0) { \
        pthread_mutex_unlock(&node->lock); return false; \
    } \
    *peer_out = node->field##s[node->field##_pos].peer; \
    *out = node->field##s[node->field##_pos].field; \
    node->field##_pos = (node->field##_pos + 1u) % (max_count); \
    node->field##_count--; \
    pthread_mutex_unlock(&node->lock); \
    return true; \
}

WORK_DRAIN(vcs_zcode_work_node_next_request, request,
           struct vcs_zcode_work_request_v1, VCS_ZCODE_WORK_NODE_MAX_REQUESTS)
WORK_DRAIN(vcs_zcode_work_node_next_cancel, cancel,
           struct vcs_zcode_work_cancel_v1, VCS_ZCODE_WORK_NODE_MAX_REQUESTS)
WORK_DRAIN(vcs_zcode_work_node_next_result, result,
           struct vcs_zcode_work_result_v1, VCS_ZCODE_WORK_NODE_MAX_RESULTS)
