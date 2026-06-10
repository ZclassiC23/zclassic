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
    printf("peers_projection: %d failures\n", failures);
    return failures;
}
