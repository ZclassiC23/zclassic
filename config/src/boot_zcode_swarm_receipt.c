/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Put dual-signed receipts on zpkgswm beside the frozen types. */

#include "config/boot_zcode_swarm_receipt.h"
#include "config/boot_zcode_swarm_membership.h"

#include "net/net.h"
#include "util/log_macros.h"
#include "vcs/package_service.h"
#include "vcs/package_swarm_node.h"
#include "vcs/service_receipt.h"

#include <stdlib.h>
#include <string.h>

static struct vcs_swarm_receipt_session *s_session;
static bool s_open_attempted;

static struct vcs_swarm_receipt_session *receipt_session(
    const char *zcode_dir)
{
    if (s_session)
        return s_session;
    if (s_open_attempted || !zcode_dir || !zcode_dir[0])
        return NULL;
    s_open_attempted = true;
    s_session = vcs_swarm_receipt_session_open(zcode_dir);
    if (!s_session)
        LOG_ERROR("net.zcode_swarm",
                  "receipt identity unavailable; receipts stay off");
    return s_session;
}

void boot_zcode_swarm_receipt_close(void)
{
    vcs_swarm_receipt_session_free(s_session);
    s_session = NULL;
    s_open_attempted = false;
}

static bool is_identity(const uint8_t *payload, size_t len)
{
    return payload && len == VCS_SWARM_RECEIPT_IDENTITY_BYTES &&
           memcmp(payload, VCS_SWARM_RECEIPT_IDENTITY_MAGIC, 4) == 0;
}

static bool is_receipt(const uint8_t *payload, size_t len)
{
    return payload && len == VCS_SERVICE_RECEIPT_WIRE_BYTES &&
           memcmp(payload, VCS_SERVICE_RECEIPT_MAGIC, 4) == 0;
}

bool boot_zcode_swarm_receipt_frame(
    struct msg_processor *mp, struct p2p_node *node,
    struct vcs_swarm_engine *engine, struct vcs_service_book *book,
    const char *zcode_dir, const uint8_t *payload, size_t payload_len,
    int64_t day)
{
    if (!is_identity(payload, payload_len) && !is_receipt(payload, payload_len))
        return false;
    struct vcs_swarm_receipt_session *session = receipt_session(zcode_dir);
    if (!session || !engine || !book || !node)
        return true; /* consume; never feed ZSID/ZSR1 to the swarm codec */
    uint64_t peer = boot_zcode_swarm_peer_id(node);
    if (is_identity(payload, payload_len)) {
        if (!vcs_swarm_receipt_identity_note(session, peer, payload,
                                             payload_len))
            LOG_WARN("net.zcode_swarm",
                     "peer %llu receipt identity refused",
                     (unsigned long long)peer);
        return true;
    }
    struct vcs_swarm_transfer xfer;
    if (!vcs_swarm_engine_transfer_snapshot(engine, peer, &xfer))
        return true;
    uint8_t *reply = NULL;
    size_t reply_len = 0;
    enum vcs_swarm_receipt_status st = vcs_swarm_receipt_session_handle(
        session, book, &xfer, peer, day, payload, payload_len, &reply,
        &reply_len);
    if (st != VCS_SWARM_RECEIPT_OK && st != VCS_SWARM_RECEIPT_DUPLICATE &&
        st != VCS_SWARM_RECEIPT_STALE)
        LOG_WARN("net.zcode_swarm", "peer %llu receipt %s",
                 (unsigned long long)peer,
                 vcs_swarm_receipt_status_string(st));
    if (reply && reply_len > 0 && mp)
        boot_zcode_swarm_send(mp, node, reply, reply_len);
    free(reply);
    return true;
}

size_t boot_zcode_swarm_receipt_drain(
    struct msg_processor *mp, struct p2p_node *node,
    struct vcs_swarm_engine *engine, const char *zcode_dir, int64_t day)
{
    struct vcs_swarm_receipt_session *session = receipt_session(zcode_dir);
    if (!session || !mp || !node || !engine)
        return 0;
    uint64_t peer = boot_zcode_swarm_peer_id(node);
    size_t sent = 0;
    uint8_t ident[VCS_SWARM_RECEIPT_IDENTITY_BYTES];
    size_t ident_len = 0;
    if (vcs_swarm_receipt_identity_take(session, peer, ident, sizeof(ident),
                                        &ident_len)) {
        boot_zcode_swarm_send(mp, node, ident, ident_len);
        sent++;
    }
    struct vcs_swarm_transfer xfer;
    uint8_t offer[VCS_SERVICE_RECEIPT_WIRE_BYTES];
    if (vcs_swarm_engine_transfer_snapshot(engine, peer, &xfer) &&
        vcs_swarm_receipt_session_offer(session, &xfer, peer, day, offer)) {
        boot_zcode_swarm_send(mp, node, offer, sizeof(offer));
        sent++;
    }
    return sent;
}
