/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZCL Messaging RPC controller.
 *
 * Commands:
 *   msg_send   — send P2P message to a connected peer
 *   msg_inbox  — list messages (all or unread only)
 *   msg_read   — mark message as read, return content */

#include "platform/time_compat.h"
#include "net/zmsg.h"
#include "net/net.h"
#include "net/connman.h"
#include "chain/chainparams.h"
#include "models/znam.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "rpc/server.h"
#include "models/database.h"
#include "crypto/sha3.h"
#include "core/serialize.h"
#include "views/format_helpers.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <inttypes.h>
#include "util/log_macros.h"

/* ── Context ────────────────────────────────────────────────────── */

static struct node_db *g_msg_ndb = NULL;
static struct connman *g_msg_connman = NULL;

void rpc_msg_set_state(struct node_db *ndb, struct connman *cm)
{
    g_msg_ndb = ndb;
    g_msg_connman = cm;
}

/* ── Helpers ────────────────────────────────────────────────────── */

static void msg_to_json(const struct zmsg_message *msg, struct json_value *obj)
{
    json_set_object(obj);
    char hex[65];
    HexStr(msg->msg_id, 32, false, hex, sizeof(hex));
    json_push_kv_str(obj, "msg_id", hex);
    json_push_kv_str(obj, "direction",
                     msg->direction == ZMSG_OUTBOUND ? "outbound" : "inbound");
    json_push_kv_str(obj, "channel",
                     msg->channel == ZMSG_CHANNEL_P2P ? "p2p" : "onchain");
    json_push_kv_str(obj, "sender", msg->sender);
    json_push_kv_str(obj, "recipient", msg->recipient);
    json_push_kv_str(obj, "body", msg->body);
    json_push_kv_int(obj, "timestamp", msg->timestamp);
    json_push_kv_bool(obj, "read", msg->read);
}

/* ── msg_send ───────────────────────────────────────────────────── */

static bool rpc_msg_send(const struct json_value *params, bool help,
                         struct json_value *result)
{
    if (help || !params || json_size(params) < 2) {
        json_set_str(result,
            "msg_send peer_id \"message\"\n"
            "\nSend a P2P message to a connected peer.\n"
            "\nArguments:\n"
            "1. peer_id  (number) Connected peer ID\n"
            "2. message  (string) Message text\n"
            "\nResult: message ID and delivery status.\n");
        return true;
    }

    int64_t peer_id = json_get_int(json_at(params, 0));
    const char *body = json_get_str(json_at(params, 1));

    if (!body || !body[0]) {
        json_set_str(result, "Empty message");
        return false;
    }

    if (!g_msg_connman) {
        json_set_str(result, "Network not available");
        return false;
    }

    /* Find the peer */
    struct p2p_node *target = NULL;
    zcl_mutex_lock(&g_msg_connman->manager.cs_nodes);
    for (size_t i = 0; i < g_msg_connman->manager.num_nodes; i++) {
        struct p2p_node *node = g_msg_connman->manager.nodes[i];
        if (node->id == peer_id && !node->disconnect) {
            target = node;
            break;
        }
    }

    if (!target) {
        zcl_mutex_unlock(&g_msg_connman->manager.cs_nodes);
        json_set_str(result, "Peer not found or disconnected");
        return false;
    }

    /* Build the message */
    struct zmsg_message msg;
    memset(&msg, 0, sizeof(msg));
    msg.direction = ZMSG_OUTBOUND;
    msg.channel = ZMSG_CHANNEL_P2P;
    msg.timestamp = (int64_t)platform_time_wall_time_t();
    snprintf(msg.sender, sizeof(msg.sender), "self");
    snprintf(msg.recipient, sizeof(msg.recipient), "peer:%lld",
             (long long)peer_id);
    snprintf(msg.body, sizeof(msg.body), "%s", body);

    /* Compute msg_id */
    zmsg_compute_id(&msg, msg.msg_id);

    /* Serialize and send */
    struct byte_stream os;
    stream_init(&os, 512);
    zmsg_serialize(&msg, &os);

    p2p_node_begin_message(target, MSG_ZMSG,
                           g_msg_connman->params->pchMessageStart);
    p2p_node_write_message_data(target, os.data, os.size);
    p2p_node_end_message(target);
    stream_free(&os);

    zcl_mutex_unlock(&g_msg_connman->manager.cs_nodes);

    /* Store locally */
    zmsg_store_add(&msg);
    if (g_msg_ndb)
        db_zmsg_save(g_msg_ndb, &msg);

    /* Return result */
    json_set_object(result);
    char hex[65];
    HexStr(msg.msg_id, 32, false, hex, sizeof(hex));
    json_push_kv_str(result, "msg_id", hex);
    json_push_kv_int(result, "peer_id", peer_id);
    json_push_kv_str(result, "status", "sent");

    printf("zmsg: sent message to peer %lld\n", (long long)peer_id);
    return true;
}

/* ── msg_inbox ──────────────────────────────────────────────────── */

static bool rpc_msg_inbox(const struct json_value *params, bool help,
                          struct json_value *result)
{
    if (help) {
        json_set_str(result,
            "msg_inbox [unread_only=false]\n"
            "\nList messages in the inbox.\n"
            "\nArguments:\n"
            "1. unread_only (bool, optional) Only show unread messages\n");
        return true;
    }

    bool unread_only = false;
    if (params && json_size(params) > 0) {
        const struct json_value *arg0 = json_at(params, 0);
        if (arg0) unread_only = json_get_int(arg0) != 0;
    }

    json_set_array(result);

    struct zmsg_message msgs[50];
    int count = 0;

    /* Try SQLite first, fall back to in-memory store */
    if (g_msg_ndb)
        count = db_zmsg_list(g_msg_ndb, msgs, 50, unread_only);
    if (count == 0)
        count = zmsg_store_list(msgs, 50, unread_only);

    for (int i = 0; i < count; i++) {
        struct json_value e = {0};
        msg_to_json(&msgs[i], &e);
        json_push_back(result, &e);
        json_free(&e);
    }

    return true;
}

/* ── msg_read ───────────────────────────────────────────────────── */

static bool rpc_msg_read(const struct json_value *params, bool help,
                         struct json_value *result)
{
    if (help || !params || json_size(params) < 1) {
        json_set_str(result,
            "msg_read \"msg_id\"\n"
            "\nMark a message as read and return its content.\n"
            "\nArguments:\n"
            "1. msg_id (string) 64-char hex message ID\n");
        return true;
    }

    const char *hex = json_get_str(json_at(params, 0));
    if (!zcl_is_hex_string(hex, 64)) {
        json_set_str(result, "Invalid msg_id (64-char hex)");
        return false;
    }

    uint8_t msg_id[32];
    (void)ParseHex(hex, msg_id, 32);  /* hex already validated by zcl_is_hex_string */

    zmsg_store_mark_read(msg_id);
    if (g_msg_ndb)
        db_zmsg_mark_read(g_msg_ndb, msg_id);

    json_set_object(result);
    json_push_kv_str(result, "msg_id", hex);
    json_push_kv_str(result, "status", "read");
    return true;
}

/* ── msg_send_named ─────────────────────────────────────────────── */

static bool rpc_msg_send_named(const struct json_value *params, bool help,
                               struct json_value *result)
{
    if (help || !params || json_size(params) < 2) {
        json_set_str(result,
            "msg_send_named \"name\" \"message\"\n"
            "\nSend a message using a ZCL Name. Resolves the name first.\n"
            "\nArguments:\n"
            "1. name    (string) ZCL Name to resolve (e.g. \"alice\")\n"
            "2. message (string) Message text\n"
            "\nThe name is resolved to find the recipient's address/onion.\n"
            "The message is stored locally with the resolved identity.\n");
        return true;
    }

    const char *name = json_get_str(json_at(params, 0));
    const char *body = json_get_str(json_at(params, 1));

    if (!name || !name[0] || !body || !body[0]) {
        json_set_str(result, "name and message required");
        return false;
    }

    /* Resolve the ZCL Name */
    struct znam_entry entry;
    if (!g_msg_ndb || !db_znam_find(g_msg_ndb, name, &entry)) {
        json_set_str(result, "Name not found");
        return false;
    }

    /* Build and store the message */
    struct zmsg_message msg;
    memset(&msg, 0, sizeof(msg));
    msg.direction = ZMSG_OUTBOUND;
    msg.channel = ZMSG_CHANNEL_P2P;
    msg.timestamp = (int64_t)platform_time_wall_time_t();
    snprintf(msg.sender, sizeof(msg.sender), "self");
    snprintf(msg.recipient, sizeof(msg.recipient), "%s (%s)",
             name, entry.target_value);
    snprintf(msg.body, sizeof(msg.body), "%s", body);
    zmsg_compute_id(&msg, msg.msg_id);

    zmsg_store_add(&msg);
    if (g_msg_ndb)
        db_zmsg_save(g_msg_ndb, &msg);

    json_set_object(result);
    char hex[65];
    HexStr(msg.msg_id, 32, false, hex, sizeof(hex));
    json_push_kv_str(result, "msg_id", hex);
    json_push_kv_str(result, "resolved_name", name);
    json_push_kv_str(result, "resolved_to", entry.target_value);

    const char *type = "unknown";
    if (entry.target_type == ZNAM_TYPE_ONION) type = "onion";
    else if (entry.target_type == ZNAM_TYPE_ZADDR) type = "z-address";
    else if (entry.target_type == ZNAM_TYPE_TADDR) type = "t-address";
    json_push_kv_str(result, "target_type", type);
    json_push_kv_str(result, "status", "queued");
    json_push_kv_str(result, "note",
        "Message stored. Delivery will occur when peer is connected.");

    printf("zmsg: message to '%s' -> %s (%s)\n",
           name, entry.target_value, type);
    return true;
}

/* ── REST API ───────────────────────────────────────────────────── */

bool api_msg_inbox(struct json_value *result)
{
    return rpc_msg_inbox(NULL, false, result);
}

/* ── Registration ───────────────────────────────────────────────── */

void register_msg_rpc_commands(struct rpc_table *t)
{
    struct rpc_command cmds[] = {
        { "messaging", "msg_send",       rpc_msg_send,       true },
        { "messaging", "msg_send_named", rpc_msg_send_named, true },
        { "messaging", "msg_inbox",      rpc_msg_inbox,      true },
        { "messaging", "msg_read",       rpc_msg_read,       true },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        rpc_table_must_append(t, &cmds[i]);
}
