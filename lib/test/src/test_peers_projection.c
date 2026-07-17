/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "storage/event_log.h"
#include "storage/event_log_payloads.h"
#include "storage/peers_projection.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PP_CHECK(label, cond) do { \
    bool _ok = (cond); \
    printf("peers_projection: %s... %s\n", (label), _ok ? "OK" : "FAIL"); \
    if (!_ok) failures++; \
} while (0)


static void fill_ip(uint8_t ip[16], uint8_t tail)
{
    memset(ip, 0, 16);
    ip[10] = 0xff;
    ip[11] = 0xff;
    ip[12] = 203;
    ip[13] = 0;
    ip[14] = 113;
    ip[15] = tail;
}

static bool append_observed(event_log_t *log, const uint8_t ip[16],
                            uint16_t port, uint64_t services,
                            uint32_t seen, int32_t height)
{
    struct ev_peer_observed ev;
    uint8_t payload[EV_PEER_OBSERVED_FIXED_LEN + EV_PEER_ONION_MAX];
    size_t len = 0;
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.ip_v4_or_v6, ip, 16);
    ev.port = port;
    ev.services_bitmap = services;
    ev.observed_unix = seen;
    ev.height_hint = height;
    if (!ev_peer_observed_serialize(&ev, payload, sizeof(payload), &len))
        return false;
    return event_log_append(log, EV_PEER_OBSERVED, payload, len) != UINT64_MAX;
}

static bool append_dropped(event_log_t *log, const uint8_t ip[16],
                           uint16_t port)
{
    struct ev_peer_dropped ev;
    uint8_t payload[EV_PEER_DROPPED_LEN];
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.ip_v4_or_v6, ip, 16);
    ev.port = port;
    ev.reason = 1;
    if (!ev_peer_dropped_serialize(&ev, payload))
        return false;
    return event_log_append(log, EV_PEER_DROPPED,
                            payload, sizeof(payload)) != UINT64_MAX;
}

static bool append_session(event_log_t *log, const uint8_t ip[16],
                           uint16_t port, uint64_t bytes_in, uint64_t bytes_out,
                           uint64_t headers, uint64_t blocks, uint32_t bw,
                           int64_t latency_us, int64_t last_useful)
{
    struct ev_peer_session_closed ev;
    uint8_t payload[EV_PEER_SESSION_CLOSED_LEN];
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.ip_v4_or_v6, ip, 16);
    ev.port = port;
    ev.reason = 0;
    ev.duration_secs = 42;
    ev.bytes_in = bytes_in;
    ev.bytes_out = bytes_out;
    ev.headers_delivered = headers;
    ev.blocks_delivered = blocks;
    ev.bandwidth_score = bw;
    ev.avg_latency_us = latency_us;
    ev.last_useful_time = last_useful;
    if (!ev_peer_session_closed_serialize(&ev, payload))
        return false;
    return event_log_append(log, EV_PEER_SESSION_CLOSED,
                            payload, sizeof(payload)) != UINT64_MAX;
}

static bool append_fork(event_log_t *log, int64_t height, int64_t observed,
                        uint32_t num_clusters, uint32_t ca, uint32_t cb,
                        const char *ha, const char *hb)
{
    struct ev_net_fork_observed ev;
    uint8_t payload[EV_NET_FORK_OBSERVED_FIXED_LEN +
                    2 * EV_NET_FORK_TIP_HEX_MAX];
    size_t len = 0;
    memset(&ev, 0, sizeof(ev));
    ev.height = height;
    ev.observed_unix = observed;
    ev.num_clusters = num_clusters;
    ev.count_a = ca;
    ev.count_b = cb;
    if (ha) { size_t n = strlen(ha); memcpy(ev.tip_hash_a, ha, n);
              ev.hash_a_len = (uint8_t)n; }
    if (hb) { size_t n = strlen(hb); memcpy(ev.tip_hash_b, hb, n);
              ev.hash_b_len = (uint8_t)n; }
    if (!ev_net_fork_observed_serialize(&ev, payload, sizeof(payload), &len))
        return false;
    return event_log_append(log, EV_NET_FORK_OBSERVED, payload, len)
           != UINT64_MAX;
}

static int t_payload_roundtrip(void)
{
    int failures = 0;
    uint8_t payload[EV_PEER_OBSERVED_FIXED_LEN + EV_PEER_ONION_MAX];
    struct ev_peer_observed in, out;
    size_t len = 0;
    memset(&in, 0, sizeof(in));
    fill_ip(in.ip_v4_or_v6, 9);
    in.port = 8033;
    in.services_bitmap = 0xdeadbeefULL;
    in.observed_unix = 1700000000u;
    in.height_hint = 1234;
    in.is_onion = 1;
    in.onion_len = 11;
    memcpy(in.onion, "abcde.onion", 11);
    PP_CHECK("observed serialize",
             ev_peer_observed_serialize(&in, payload, sizeof(payload),
                                        &len));
    PP_CHECK("observed parse",
             ev_peer_observed_parse(payload, len, &out));
    PP_CHECK("observed roundtrip",
             memcmp(in.ip_v4_or_v6, out.ip_v4_or_v6, 16) == 0 &&
             out.port == in.port &&
             out.services_bitmap == in.services_bitmap &&
             out.observed_unix == in.observed_unix &&
             out.height_hint == in.height_hint &&
             out.is_onion == in.is_onion &&
             out.onion_len == in.onion_len &&
             memcmp(in.onion, out.onion, in.onion_len) == 0);

    struct ev_peer_dropped din, dout;
    uint8_t dropped[EV_PEER_DROPPED_LEN];
    memset(&din, 0, sizeof(din));
    fill_ip(din.ip_v4_or_v6, 10);
    din.port = 9033;
    din.reason = 3;
    PP_CHECK("dropped serialize",
             ev_peer_dropped_serialize(&din, dropped));
    PP_CHECK("dropped parse",
             ev_peer_dropped_parse(dropped, sizeof(dropped), &dout));
    PP_CHECK("dropped roundtrip",
             memcmp(din.ip_v4_or_v6, dout.ip_v4_or_v6, 16) == 0 &&
             dout.port == din.port && dout.reason == din.reason);

    struct ev_peer_session_closed sin, sout;
    uint8_t sbuf[EV_PEER_SESSION_CLOSED_LEN];
    memset(&sin, 0, sizeof(sin));
    fill_ip(sin.ip_v4_or_v6, 20);
    sin.port = 8033;
    sin.reason = 1;
    sin.duration_secs = 3600;
    sin.bytes_in = 0x1122334455667788ULL;
    sin.bytes_out = 0x8877665544332211ULL;
    sin.headers_delivered = 987654;
    sin.blocks_delivered = 123;
    sin.bandwidth_score = 200;
    sin.avg_latency_us = -55; /* sign round-trip */
    sin.last_useful_time = 1700000123LL;
    PP_CHECK("session serialize",
             ev_peer_session_closed_serialize(&sin, sbuf));
    PP_CHECK("session parse",
             ev_peer_session_closed_parse(sbuf, sizeof(sbuf), &sout));
    PP_CHECK("session roundtrip",
             memcmp(sin.ip_v4_or_v6, sout.ip_v4_or_v6, 16) == 0 &&
             sout.port == sin.port && sout.reason == sin.reason &&
             sout.duration_secs == sin.duration_secs &&
             sout.bytes_in == sin.bytes_in &&
             sout.bytes_out == sin.bytes_out &&
             sout.headers_delivered == sin.headers_delivered &&
             sout.blocks_delivered == sin.blocks_delivered &&
             sout.bandwidth_score == sin.bandwidth_score &&
             sout.avg_latency_us == sin.avg_latency_us &&
             sout.last_useful_time == sin.last_useful_time);
    PP_CHECK("session rejects short", !ev_peer_session_closed_parse(sbuf,
             sizeof(sbuf) - 1, &sout));

    struct ev_net_fork_observed fin, fout;
    uint8_t fbuf[EV_NET_FORK_OBSERVED_FIXED_LEN + 2 * EV_NET_FORK_TIP_HEX_MAX];
    size_t flen = 0;
    memset(&fin, 0, sizeof(fin));
    fin.height = 3176325;
    fin.observed_unix = 1700000000LL;
    fin.num_clusters = 3;
    fin.count_a = 5;
    fin.count_b = 4;
    memset(fin.tip_hash_a, 'a', 64); fin.hash_a_len = 64;
    memset(fin.tip_hash_b, 'b', 64); fin.hash_b_len = 64;
    PP_CHECK("fork serialize",
             ev_net_fork_observed_serialize(&fin, fbuf, sizeof(fbuf), &flen));
    PP_CHECK("fork parse", ev_net_fork_observed_parse(fbuf, flen, &fout));
    PP_CHECK("fork roundtrip",
             fout.height == fin.height && fout.observed_unix == fin.observed_unix &&
             fout.num_clusters == fin.num_clusters &&
             fout.count_a == fin.count_a && fout.count_b == fin.count_b &&
             fout.hash_a_len == 64 && fout.hash_b_len == 64 &&
             memcmp(fout.tip_hash_a, fin.tip_hash_a, 64) == 0 &&
             memcmp(fout.tip_hash_b, fin.tip_hash_b, 64) == 0);
    return failures;
}

/* Reputation round-trips through fold + accumulates + survives reopen + a
 * later re-observation must NOT wipe it. */
static int t_reputation_roundtrip(void)
{
    int failures = 0;
    char dir[256], elog_path[300], proj_path[300];
    uint8_t ip[16];
    test_make_tmpdir(dir, sizeof(dir), "peers_projection", "reputation");
    test_projection_paths(dir, "peers", elog_path, sizeof(elog_path),
                          proj_path, sizeof(proj_path));
    fill_ip(ip, 55);
    event_log_t *log = event_log_open(elog_path);
    peers_projection_t *p = peers_projection_open(proj_path, log);

    /* Two banked sessions for the same peer: headers/blocks accumulate,
     * sessions_count increments, last_useful_time takes the MAX, and
     * bandwidth_score/latency reflect the latest close. */
    PP_CHECK("append session 1",
             append_session(log, ip, 8033, 1000, 500, 100, 5, 180, 12000, 1000));
    PP_CHECK("append session 2",
             append_session(log, ip, 8033, 2000, 900, 60, 3, 220, 9000, 2000));
    PP_CHECK("catch up sessions", peers_projection_catch_up(p) != UINT64_MAX);

    struct peer_reputation rep;
    PP_CHECK("get reputation",
             peers_projection_get_reputation(p, ip, 8033, &rep));
    PP_CHECK("sessions_count accumulates", rep.sessions_count == 2);
    PP_CHECK("headers accumulate", rep.headers_delivered == 160);
    PP_CHECK("blocks accumulate", rep.blocks_delivered == 8);
    PP_CHECK("bandwidth latest wins", rep.bandwidth_score == 220);
    PP_CHECK("latency latest wins", rep.avg_latency_us == 9000);
    PP_CHECK("last_useful is max", rep.last_useful_time == 2000);

    /* A later observation must PRESERVE the banked reputation (UPSERT, not
     * INSERT OR REPLACE). */
    PP_CHECK("append later observed",
             append_observed(log, ip, 8033, 9, 1700000005u, 999));
    PP_CHECK("catch up observed", peers_projection_catch_up(p) != UINT64_MAX);
    PP_CHECK("reputation survives re-observe",
             peers_projection_get_reputation(p, ip, 8033, &rep) &&
             rep.sessions_count == 2 && rep.headers_delivered == 160 &&
             rep.bandwidth_score == 220);

    /* Survives a close + reopen (cursor persisted, addresses table on disk). */
    peers_projection_close(p);
    event_log_close(log);
    log = event_log_open(elog_path);
    p = peers_projection_open(proj_path, log);
    PP_CHECK("catch up after reopen", peers_projection_catch_up(p) != UINT64_MAX);
    PP_CHECK("reputation survives reopen",
             peers_projection_get_reputation(p, ip, 8033, &rep) &&
             rep.sessions_count == 2 && rep.headers_delivered == 160 &&
             rep.blocks_delivered == 8 && rep.bandwidth_score == 220 &&
             rep.last_useful_time == 2000);

    peers_projection_close(p);
    event_log_close(log);
    test_cleanup_tmpdir(dir);
    return failures;
}

/* Missing / never-banked reputation reads all-zero (false), and a corrupt
 * session payload aborts its batch WITHOUT corrupting prior reputation. */
static int t_reputation_fallback_clean(void)
{
    int failures = 0;
    char dir[256], elog_path[300], proj_path[300];
    uint8_t ip[16], other[16];
    test_make_tmpdir(dir, sizeof(dir), "peers_projection", "repfallback");
    test_projection_paths(dir, "peers", elog_path, sizeof(elog_path),
                          proj_path, sizeof(proj_path));
    fill_ip(ip, 66);
    fill_ip(other, 67);
    event_log_t *log = event_log_open(elog_path);
    peers_projection_t *p = peers_projection_open(proj_path, log);

    struct peer_reputation rep;
    memset(&rep, 0xAB, sizeof(rep));
    PP_CHECK("unknown reputation false + zeroed",
             !peers_projection_get_reputation(p, ip, 8033, &rep) &&
             rep.bandwidth_score == 0 && rep.sessions_count == 0);

    /* Observed-only peer: row exists but reputation is clean zero (download
     * seed would no-op) — exactly today's behavior. */
    PP_CHECK("append observed only",
             append_observed(log, ip, 8033, 1, 100, 10));
    PP_CHECK("catch up", peers_projection_catch_up(p) != UINT64_MAX);
    PP_CHECK("observed-only reputation is zero",
             !peers_projection_get_reputation(p, ip, 8033, &rep) ||
             (rep.bandwidth_score == 0 && rep.sessions_count == 0));

    /* Bank a good session, then append a CORRUPT (truncated) session payload:
     * the batch aborts (UINT64_MAX) but the good reputation is untouched. */
    PP_CHECK("append good session",
             append_session(log, other, 8033, 1, 1, 50, 2, 150, 5000, 4242));
    PP_CHECK("catch up good", peers_projection_catch_up(p) != UINT64_MAX);
    PP_CHECK("good banked",
             peers_projection_get_reputation(p, other, 8033, &rep) &&
             rep.bandwidth_score == 150 && rep.headers_delivered == 50);

    uint8_t bad[EV_PEER_SESSION_CLOSED_LEN - 4] = {0};
    PP_CHECK("append corrupt session",
             event_log_append(log, EV_PEER_SESSION_CLOSED, bad, sizeof(bad))
             != UINT64_MAX);
    PP_CHECK("corrupt batch aborts",
             peers_projection_catch_up(p) == UINT64_MAX);
    PP_CHECK("good reputation intact after corrupt",
             peers_projection_get_reputation(p, other, 8033, &rep) &&
             rep.bandwidth_score == 150 && rep.headers_delivered == 50);

    peers_projection_close(p);
    event_log_close(log);
    test_cleanup_tmpdir(dir);
    return failures;
}

/* Session + fork ledgers append durably and honor the delete-oldest cap. */
static int t_ledger_retention(void)
{
    int failures = 0;
    char dir[256], elog_path[300], proj_path[300];
    uint8_t ip[16];
    test_make_tmpdir(dir, sizeof(dir), "peers_projection", "retention");
    test_projection_paths(dir, "peers", elog_path, sizeof(elog_path),
                          proj_path, sizeof(proj_path));
    fill_ip(ip, 88);
    event_log_t *log = event_log_open(elog_path);
    peers_projection_t *p = peers_projection_open(proj_path, log);

    peers_projection_test_set_retention_caps(3, 2);

    for (int i = 0; i < 5; i++)
        PP_CHECK("append session (retention)",
                 append_session(log, ip, (uint16_t)(8000 + i), 1, 1,
                                (uint64_t)i, 0, 100, 1000, 100 + i));
    for (int i = 0; i < 4; i++)
        PP_CHECK("append fork (retention)",
                 append_fork(log, 1000 + i, 2000 + i, 2, 2, 2, "aaaa", "bbbb"));
    PP_CHECK("catch up ledgers", peers_projection_catch_up(p) != UINT64_MAX);

    PP_CHECK("peer_sessions capped to 3",
             peers_projection_test_ledger_count(p, "peer_sessions") == 3);
    PP_CHECK("fork_events capped to 2",
             peers_projection_test_ledger_count(p, "fork_events") == 2);

    peers_projection_test_reset_retention_caps();
    peers_projection_close(p);
    event_log_close(log);
    test_cleanup_tmpdir(dir);
    return failures;
}

static int t_open_close_clean(void)
{
    int failures = 0;
    char dir[256], elog_path[300], proj_path[300];
    test_make_tmpdir(dir, sizeof(dir), "peers_projection", "open");
    test_projection_paths(dir, "peers", elog_path, sizeof(elog_path),
                          proj_path, sizeof(proj_path));
    event_log_t *log = event_log_open(elog_path);
    peers_projection_t *p = peers_projection_open(proj_path, log);
    PP_CHECK("open handles", log && p);
    PP_CHECK("empty count", peers_projection_count(p) == 0);
    peers_projection_close(p);
    event_log_close(log);
    log = event_log_open(elog_path);
    p = peers_projection_open(proj_path, log);
    PP_CHECK("reopen handles", log && p);
    PP_CHECK("reopen empty count", peers_projection_count(p) == 0);
    peers_projection_close(p);
    event_log_close(log);
    test_cleanup_tmpdir(dir);
    return failures;
}

static int t_add_drop_replay(void)
{
    int failures = 0;
    char dir[256], elog_path[300], proj_path[300];
    uint8_t ip[16];
    uint64_t services = 0;
    int64_t seen = 0;
    int32_t height = 0;
    test_make_tmpdir(dir, sizeof(dir), "peers_projection", "adddrop");
    test_projection_paths(dir, "peers", elog_path, sizeof(elog_path),
                          proj_path, sizeof(proj_path));
    fill_ip(ip, 42);
    event_log_t *log = event_log_open(elog_path);
    peers_projection_t *p = peers_projection_open(proj_path, log);
    PP_CHECK("append observed",
             append_observed(log, ip, 8033, 9, 1700000001u, 321));
    PP_CHECK("catch up observed", peers_projection_catch_up(p) != UINT64_MAX);
    PP_CHECK("count after observed", peers_projection_count(p) == 1);
    PP_CHECK("get observed",
             peers_projection_get(p, ip, 8033, &services, &seen, &height) &&
             services == 9 && seen == 1700000001LL && height == 321);
    PP_CHECK("idempotent catch up", peers_projection_catch_up(p) != UINT64_MAX);
    PP_CHECK("count still one", peers_projection_count(p) == 1);
    PP_CHECK("append dropped", append_dropped(log, ip, 8033));
    PP_CHECK("catch up dropped", peers_projection_catch_up(p) != UINT64_MAX);
    PP_CHECK("count after dropped", peers_projection_count(p) == 0);
    PP_CHECK("get absent", !peers_projection_get(p, ip, 8033, NULL, NULL, NULL));
    peers_projection_close(p);
    event_log_close(log);
    test_cleanup_tmpdir(dir);
    return failures;
}

static int t_replace_collision(void)
{
    int failures = 0;
    char dir[256], elog_path[300], proj_path[300];
    uint8_t ip[16];
    uint64_t services = 0;
    int64_t seen = 0;
    int32_t height = 0;
    test_make_tmpdir(dir, sizeof(dir), "peers_projection", "replace");
    test_projection_paths(dir, "peers", elog_path, sizeof(elog_path),
                          proj_path, sizeof(proj_path));
    fill_ip(ip, 77);
    event_log_t *log = event_log_open(elog_path);
    peers_projection_t *p = peers_projection_open(proj_path, log);
    append_observed(log, ip, 8033, 1, 100, 10);
    append_observed(log, ip, 8033, 2, 200, 20);
    PP_CHECK("catch up replace", peers_projection_catch_up(p) != UINT64_MAX);
    PP_CHECK("replace count one", peers_projection_count(p) == 1);
    PP_CHECK("replace value wins",
             peers_projection_get(p, ip, 8033, &services, &seen, &height) &&
             services == 2 && seen == 200 && height == 20);
    peers_projection_close(p);
    event_log_close(log);
    test_cleanup_tmpdir(dir);
    return failures;
}

int test_peers_projection(void)
{
    int failures = 0;
    printf("\n=== peers_projection tests ===\n");
    failures += t_payload_roundtrip();
    failures += t_open_close_clean();
    failures += t_add_drop_replay();
    failures += t_replace_collision();
    failures += t_reputation_roundtrip();
    failures += t_reputation_fallback_clean();
    failures += t_ledger_retention();
    printf("peers_projection: %d failures\n", failures);
    return failures;
}
