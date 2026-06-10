/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * On-chain load balancer implementation. */

#include "platform/time_compat.h"
#include "net/load_balancer.h"
#include "zslp/slp.h"
#include "core/uint256.h"
#include "core/serialize.h"
#include "primitives/transaction.h"
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>

/* ── Replica discovery from blockchain ───────────────────── */

int site_discover_replicas(const char *datadir, const char *token_id,
                            struct site_replica *out, size_t max)
{
    if (!datadir || !token_id || !out || max == 0) return 0;

    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/node.db", datadir);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return 0;

    /* Scan wallet_transactions for ZSLP SEND txs matching token_id.
     * Each matching tx has .onion + capacity + version after SLP fields. */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT raw_tx, block_height FROM wallet_transactions "
        "WHERE raw_tx IS NOT NULL ORDER BY block_height DESC LIMIT 10000",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) { sqlite3_close(db); return 0; }

    int found = 0;
    while (AR_STEP_ROW_READONLY(stmt) == SQLITE_ROW && found < (int)max) {
        const void *raw = sqlite3_column_blob(stmt, 0);
        int raw_len = sqlite3_column_bytes(stmt, 0);
        int height = sqlite3_column_int(stmt, 1);
        if (!raw || raw_len < 10) continue;

        struct transaction tx;
        transaction_init(&tx);
        struct byte_stream bs;
        stream_init_from_data(&bs, (const uint8_t *)raw, (size_t)raw_len);
        if (!transaction_deserialize(&tx, &bs)) {
            transaction_free(&tx);
            continue;
        }

        /* Check vout[0] for OP_RETURN with SLP */
        if (tx.num_vout < 1 || tx.vout[0].script_pub_key.size < 10 ||
            tx.vout[0].script_pub_key.data[0] != 0x6a) {
            transaction_free(&tx);
            continue;
        }

        struct slp_message msg;
        if (!slp_parse(tx.vout[0].script_pub_key.data,
                       tx.vout[0].script_pub_key.size, &msg) ||
            msg.type != SLP_TX_SEND) {
            transaction_free(&tx);
            continue;
        }

        /* Check if token_id matches */
        char tid_hex[65];
        for (int i = 0; i < 32; i++)
            snprintf(tid_hex + i*2, 3, "%02x", msg.token_id.data[i]);
        if (strcmp(tid_hex, token_id) != 0) {
            transaction_free(&tx);
            continue;
        }

        /* Parse replica data after SLP fields:
         * [1 byte onion_len] [onion hostname]
         * [4 bytes capacity LE] [4 bytes version LE] */
        const uint8_t *script = tx.vout[0].script_pub_key.data;
        size_t slen = tx.vout[0].script_pub_key.size;

        /* Skip OP_RETURN + SLP fields to find replica data */
        const uint8_t *p = script + 1;
        const uint8_t *end = script + slen;

        /* Skip SLP push fields: lokad, type, "SEND", token_id, quantities */
        for (int skip = 0; skip < 4 && p < end; skip++) {
            if (*p >= 1 && *p <= 0x4b) { p += 1 + *p; }
            else if (*p == 0x4c && p+1 < end) { p += 2 + p[1]; }
            else break;
        }
        /* Skip output quantities (8 bytes each) */
        for (int qi = 0; qi < 19 && p < end; qi++) {
            if (*p >= 1 && *p <= 0x4b && *p == 8) { p += 9; }
            else break;
        }

        /* Now parse: [1 onion_len] [onion] [4 capacity] [4 version] */
        if (p < end && p + 1 + *p + 8 <= end) {
            size_t olen = *p++;
            if (olen > 6 && olen < 64 &&
                memcmp(p + olen - 6, ".onion", 6) == 0) {
                memcpy(out[found].onion, p, olen);
                out[found].onion[olen] = '\0';
                p += olen;

                if (p + 8 <= end) {
                    out[found].capacity = (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
                                          ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
                    p += 4;
                    out[found].version = (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
                                         ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
                } else {
                    out[found].capacity = 100;
                    out[found].version = 1;
                }
                out[found].port = 80;
                out[found].height = height;
                out[found].latency_us = 0;
                out[found].reachable = false;
                found++;
            }
        }
        transaction_free(&tx);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return found;
}

/* ── Replica selection ───────────────────────────────────── */

int site_select_replica(struct site_replica *replicas, int count)
{
    if (!replicas || count <= 0) LOG_ERR("net", "site_select_replica: null replicas or count=%d", count);

    int best = -1;
    int64_t best_score = -1;

    for (int i = 0; i < count; i++) {
        int64_t score = 0;

        /* Reachable replicas always preferred */
        if (replicas[i].reachable) score += 1000000;

        /* Lower latency = higher score (invert) */
        if (replicas[i].latency_us > 0)
            score += 500000 - (replicas[i].latency_us / 1000);
        else
            score += 250000; /* unprobed gets middle score */

        /* Higher capacity = higher score */
        score += (int64_t)replicas[i].capacity * 10;

        /* Fresher announcement = higher score */
        score += (int64_t)replicas[i].height;

        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

/* ── Replica probing ─────────────────────────────────────── */

bool site_probe_replica(struct site_replica *replica)
{
    if (!replica || !replica->onion[0]) LOG_FAIL("net", "site_probe_replica: null replica or empty onion address");

    /* Measure time to establish connection.
     * In production: connect via Tor circuit, send HTTP HEAD, measure RTT.
     * For now: mark as reachable with estimated latency based on height freshness. */
    int64_t now = (int64_t)platform_time_wall_time_t();
    /* Rough estimate: 75s per block, newer = likely still running */
    int64_t age_blocks = 3046500 - (int64_t)replica->height; /* approximate */
    if (age_blocks < 0) age_blocks = 0;

    if (age_blocks < 100) {
        /* Recent announcement — likely reachable */
        replica->reachable = true;
        replica->latency_us = 200000 + age_blocks * 1000; /* 200ms base + age penalty */
    } else if (age_blocks < 1000) {
        replica->reachable = true;
        replica->latency_us = 500000 + age_blocks * 500;
    } else {
        /* Very old announcement — probably stale */
        replica->reachable = false;
        replica->latency_us = 0;
    }

    (void)now;
    return replica->reachable;
}

/* ── Announce this node as a replica ─────────────────────── */

bool site_announce_replica(const char *datadir,
                            const char *token_id,
                            const char *onion_addr,
                            uint32_t capacity,
                            uint32_t content_version)
{
    if (!datadir || !token_id || !onion_addr) LOG_FAIL("net", "site_announce_replica: null argument (datadir=%p, token_id=%p, onion=%p)",
                 (const void *)datadir, (const void *)token_id, (const void *)onion_addr);

    /* Build the ZSLP SEND OP_RETURN with replica metadata appended.
     * Format: [SLP SEND fields] [1 onion_len] [onion] [4 capacity LE] [4 version LE] */
    struct uint256 tid;
    /* Parse hex token_id to uint256 */
    for (int i = 0; i < 32 && token_id[i*2] && token_id[i*2+1]; i++) {
        char hex[3] = {token_id[i*2], token_id[i*2+1], 0};
        tid.data[i] = (uint8_t)strtol(hex, NULL, 16);
    }

    uint8_t script[256];
    uint64_t qty = 1;
    size_t slp_len = slp_build_send(script, sizeof(script), &tid, &qty, 1);
    if (slp_len == 0) LOG_FAIL("net", "site_announce_replica: slp_build_send failed for token %s", token_id);

    /* Append replica metadata */
    size_t olen = strlen(onion_addr);
    if (slp_len + 1 + olen + 8 > sizeof(script)) LOG_FAIL("net", "site_announce_replica: script buffer overflow (need %zu, have %zu)",
                 slp_len + 1 + olen + 8, sizeof(script));

    script[slp_len] = (uint8_t)olen;
    memcpy(script + slp_len + 1, onion_addr, olen);
    size_t off = slp_len + 1 + olen;

    /* Capacity (4 bytes LE) */
    script[off++] = (uint8_t)(capacity & 0xFF);
    script[off++] = (uint8_t)((capacity >> 8) & 0xFF);
    script[off++] = (uint8_t)((capacity >> 16) & 0xFF);
    script[off++] = (uint8_t)((capacity >> 24) & 0xFF);

    /* Version (4 bytes LE) */
    script[off++] = (uint8_t)(content_version & 0xFF);
    script[off++] = (uint8_t)((content_version >> 8) & 0xFF);
    script[off++] = (uint8_t)((content_version >> 16) & 0xFF);
    script[off++] = (uint8_t)((content_version >> 24) & 0xFF);

    printf("Built replica announcement: %s capacity=%u version=%u (%zu bytes)\n",
           onion_addr, capacity, content_version, off);
    fflush(stdout);

    /* Not yet implemented: needs wallet integration to create,
     * sign, and broadcast a transaction with this OP_RETURN. */
    (void)datadir;
    LOG_FAIL("net", "site_announce_replica: wallet integration not yet implemented");
}

/* ── Connect to best replica ─────────────────────────────── */

bool site_connect_best(const char *datadir, const char *token_id,
                        char *out_onion, size_t out_max)
{
    struct site_replica replicas[32];
    int count = site_discover_replicas(datadir, token_id, replicas, 32);
    if (count == 0) LOG_FAIL("net", "site_connect_best: no replicas found for token %s", token_id);

    /* Probe top candidates (up to 5) */
    int to_probe = count < 5 ? count : 5;
    for (int i = 0; i < to_probe; i++)
        site_probe_replica(&replicas[i]);

    int best = site_select_replica(replicas, count);
    if (best < 0 || !replicas[best].reachable) LOG_FAIL("net", "site_connect_best: no reachable replica found (best=%d, count=%d)", best, count);

    snprintf(out_onion, out_max, "%s", replicas[best].onion);
    printf("Selected replica: %s (latency=%lldus, capacity=%u)\n",
           out_onion, (long long)replicas[best].latency_us,
           replicas[best].capacity);
    return true;
}
