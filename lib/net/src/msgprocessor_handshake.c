/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* P2P handshake dispatch wrappers: version, verack, sendheaders.
 * The actual handshake logic lives in msg_version.c — these are the
 * dispatch-table adapters. peer_lifecycle observability is preserved
 * exactly as it was before the split. */

#include "msgprocessor_internal.h"

bool mp_handle_version(struct msg_processor *mp, struct p2p_node *node,
                       struct byte_stream *s)
{
    return process_version(mp, node, s);
}

bool mp_handle_verack(struct msg_processor *mp, struct p2p_node *node,
                      struct byte_stream *s)
{
    (void)s;
    return process_verack(mp, node);
}

static bool process_sendheaders(struct p2p_node *node)
{
    node->prefer_headers = true;
    return true;
}

bool mp_handle_sendheaders(struct msg_processor *mp, struct p2p_node *node,
                           struct byte_stream *s)
{
    (void)mp; (void)s;
    return process_sendheaders(node);
}
