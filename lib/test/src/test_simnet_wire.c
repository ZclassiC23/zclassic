/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * simnet_wire honest loopback tests. These exercise real P2P framing,
 * receive reassembly, msgprocessor dispatch, and send_head capture without
 * starting network threads.
 */

#include "test/test_helpers.h"

#include "net/net.h"
#include "sim/simnet_byzantine.h"
#include "sim/simnet_wire.h"
#include "util/blocker.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define SW_CHECK(name, expr) do {          \
    printf("%s... ", (name));              \
    if ((expr)) printf("OK\n");            \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static bool sw_stats_monitors_ok(const struct simnet_wire_stats *st)
{
    return st &&
           st->recv_queue_bounded &&
           st->no_unexpected_permanent_blocker &&
           st->memory_plateau_ok &&
           st->consensus_unchanged &&
           !st->monitor_failed;
}

static bool sw_run_loopback(uint64_t seed, uint64_t nonce,
                            uint64_t *out_fp,
                            struct simnet_wire_stats *out_stats)
{
    struct simnet_wire *wire = simnet_wire_create(1, seed);
    if (!wire)
        return false;

    bool ok = simnet_wire_start_honest_peer(wire, 0) &&
              simnet_wire_run(wire, 1024, 64) &&
              simnet_wire_peer_handshake_complete(wire, 0) &&
              simnet_wire_peer_send_ping(wire, 0, nonce) &&
              simnet_wire_run(wire, 1024, 64) &&
              simnet_wire_peer_pong_received(wire, 0, nonce);

    struct simnet_wire_stats st;
    memset(&st, 0, sizeof(st));
    if (simnet_wire_get_stats(wire, &st)) {
        if (out_stats)
            *out_stats = st;
        if (out_fp)
            *out_fp = st.fingerprint;
    }
    ok = ok &&
         st.handshake_complete &&
         st.pong_received &&
         !st.nut_disconnected &&
         st.pending_events == 0 &&
         st.to_nut_bytes == 0 &&
         st.to_peer_bytes == 0 &&
         st.delivered_to_nut_bytes > 0 &&
         st.delivered_to_peer_bytes > 0 &&
         st.fingerprint != 0;

    simnet_wire_free(wire);
    return ok;
}

static bool sw_run_malformed(uint64_t seed, uint64_t *out_fp,
                             struct simnet_wire_stats *out_stats)
{
    blocker_reset_for_testing();
    struct simnet_wire *wire = simnet_wire_create(1, seed);
    if (!wire)
        return false;

    bool ok = simnet_wire_start_malformed_peer(
                  wire, 0, SIMNET_WIRE_MALFORMED_BAD_CHECKSUM) &&
              simnet_wire_run(wire, 1024, 128);

    struct simnet_wire_stats st;
    memset(&st, 0, sizeof(st));
    if (simnet_wire_get_stats(wire, &st)) {
        if (out_stats)
            *out_stats = st;
        if (out_fp)
            *out_fp = st.fingerprint;
    }
    ok = ok &&
         sw_stats_monitors_ok(&st) &&
         st.checksum_fail_events > 0 &&
         st.peer_misbehave_events > 0 &&
         st.max_recv_msg_count <= MAX_RECV_MESSAGES &&
         !st.nut_disconnected;

    simnet_wire_free(wire);
    return ok;
}

static bool sw_run_bad_handshake(uint64_t seed, uint64_t *out_fp,
                                 struct simnet_wire_stats *out_stats)
{
    blocker_reset_for_testing();
    struct simnet_wire *wire = simnet_wire_create(1, seed);
    if (!wire)
        return false;

    bool ok = simnet_wire_start_bad_handshake_peer(
                  wire, 0,
                  SIMNET_WIRE_BAD_HANDSHAKE_DATA_BEFORE_VERSION) &&
              simnet_wire_run(wire, 1024, 128);

    struct simnet_wire_stats st;
    memset(&st, 0, sizeof(st));
    if (simnet_wire_get_stats(wire, &st)) {
        if (out_stats)
            *out_stats = st;
        if (out_fp)
            *out_fp = st.fingerprint;
    }
    ok = ok &&
         sw_stats_monitors_ok(&st) &&
         st.peer_misbehave_events > 0 &&
         st.nut_disconnected &&
         st.max_recv_msg_count <= MAX_RECV_MESSAGES;

    simnet_wire_free(wire);
    return ok;
}

static bool sw_run_flood(uint64_t seed, uint64_t *out_fp,
                         struct simnet_wire_stats *out_stats)
{
    blocker_reset_for_testing();
    struct simnet_wire *wire = simnet_wire_create(1, seed);
    if (!wire)
        return false;

    bool ok = simnet_wire_start_peer_kind(
                  wire, 0, SIMNET_WIRE_PEER_FLOOD) &&
              simnet_wire_run(wire, 8192, 0);

    struct simnet_wire_stats st;
    memset(&st, 0, sizeof(st));
    if (simnet_wire_get_stats(wire, &st)) {
        if (out_stats)
            *out_stats = st;
        if (out_fp)
            *out_fp = st.fingerprint;
    }
    ok = ok &&
         sw_stats_monitors_ok(&st) &&
         st.backpressure_reject_events > 0 &&
         st.max_recv_msg_count <= MAX_RECV_MESSAGES &&
         !st.nut_disconnected;

    simnet_wire_free(wire);
    return ok;
}

static bool sw_run_slowloris(uint64_t seed, uint64_t *out_fp,
                             struct simnet_wire_stats *out_stats)
{
    blocker_reset_for_testing();
    struct simnet_wire *wire = simnet_wire_create(1, seed);
    if (!wire)
        return false;

    const uint64_t recovery_nonce = 0x5152535455565758ULL;
    bool ok = simnet_wire_start_peer_kind(
                  wire, 0, SIMNET_WIRE_PEER_SLOWLORIS) &&
              simnet_wire_run(wire, 14000, 0) &&
              !simnet_wire_node(wire)->disconnect &&
              simnet_wire_peer_send_ping(wire, 0, recovery_nonce) &&
              simnet_wire_run(wire, 2048, 128) &&
              simnet_wire_peer_pong_received(wire, 0, recovery_nonce);

    struct simnet_wire_stats st;
    memset(&st, 0, sizeof(st));
    if (simnet_wire_get_stats(wire, &st)) {
        if (out_stats)
            *out_stats = st;
        if (out_fp)
            *out_fp = st.fingerprint;
    }
    ok = ok &&
         sw_stats_monitors_ok(&st) &&
         st.max_recv_msg_count <= MAX_RECV_MESSAGES &&
         !st.nut_disconnected &&
         st.pong_received;

    simnet_wire_free(wire);
    return ok;
}

static bool sw_run_mixed(uint64_t seed, uint64_t *out_fp,
                         struct simnet_wire_stats *out_stats)
{
    blocker_reset_for_testing();
    struct wire_scenario_peer peers[] = {
        { SIMNET_WIRE_PEER_FLOOD, 1 },
        { SIMNET_WIRE_PEER_SLOWLORIS, 1 },
    };
    struct wire_scenario scenario = {
        .master_seed = seed,
        .peers = peers,
        .peer_kind_count = sizeof(peers) / sizeof(peers[0]),
        .honest_peer_count = 1,
        .duration_us = 15000000,
    };
    struct simnet_wire *wire = simnet_wire_create_scenario(&scenario);
    if (!wire)
        return false;

    const uint64_t recovery_nonce = 0x6d69786564504f4eULL;
    bool ok = simnet_wire_run(wire, 16000, 0) &&
              simnet_wire_peer_handshake_complete(wire, 0) &&
              simnet_wire_peer_send_ping(wire, 0, recovery_nonce) &&
              simnet_wire_run(wire, 2048, 128) &&
              simnet_wire_peer_pong_received(wire, 0, recovery_nonce);

    struct simnet_wire_stats st;
    memset(&st, 0, sizeof(st));
    if (simnet_wire_get_stats(wire, &st)) {
        if (out_stats)
            *out_stats = st;
        if (out_fp)
            *out_fp = st.fingerprint;
    }
    ok = ok &&
         sw_stats_monitors_ok(&st) &&
         st.backpressure_reject_events > 0 &&
         st.max_recv_msg_count <= MAX_RECV_MESSAGES &&
         !st.nut_disconnected &&
         st.handshake_complete &&
         st.pong_received;

    simnet_wire_free(wire);
    return ok;
}

static bool sw_byz_reason_ok(
    enum simnet_byzantine_class kind,
    const struct simnet_wire_byzantine_observation *obs)
{
    if (!obs)
        return false;
    if (kind == SIMNET_BYZ_OVERSIZE_VTX) {
        return obs->expected_reject_path_observed &&
               strcmp(obs->reject_reason, "oversized block msg") == 0;
    }
    return obs->expected_reason_observed &&
           strcmp(obs->reject_reason,
                  simnet_byzantine_expected_reason(kind)) == 0;
}

static bool sw_run_invalid_block(
    uint64_t seed, enum simnet_byzantine_class kind,
    uint64_t *out_fp, struct simnet_wire_stats *out_stats,
    struct simnet_wire_byzantine_observation *out_obs)
{
    blocker_reset_for_testing();
    blocker_set_clock_for_testing(2000000 + (int64_t)kind * 100000);
    struct simnet_wire *wire = simnet_wire_create(1, seed);
    if (!wire)
        return false;

    bool ok = simnet_wire_start_invalid_block_peer(wire, 0, kind) &&
              simnet_wire_run(wire, 8192, 512);

    struct simnet_wire_stats st;
    memset(&st, 0, sizeof(st));
    struct simnet_wire_byzantine_observation obs;
    memset(&obs, 0, sizeof(obs));
    bool got_stats = simnet_wire_get_stats(wire, &st);
    bool got_obs = simnet_wire_get_byzantine_observation(wire, &obs);
    if (got_stats) {
        if (out_stats)
            *out_stats = st;
        if (out_fp)
            *out_fp = st.fingerprint;
    }
    if (got_obs && out_obs)
        *out_obs = obs;

    ok = ok &&
         got_stats &&
         got_obs &&
         sw_stats_monitors_ok(&st) &&
         obs.kind == kind &&
         obs.tier == SIMNET_BYZ_TIER_CONNECT_BLOCK &&
         obs.injected &&
         obs.rejected &&
         sw_byz_reason_ok(kind, &obs) &&
         obs.expected_blocker_observed &&
         obs.tip_unchanged &&
         obs.coins_unchanged &&
         obs.peer_misbehaved &&
         obs.peer_banned &&
         obs.peer_disconnected &&
         obs.honest_after_accepted &&
         obs.honest_tip_after > obs.tip_after &&
         st.peer_misbehave_events > 0 &&
         st.peer_banned_events > 0 &&
         st.nut_banned &&
         st.nut_disconnected &&
         st.not_implemented_peers == 0;

    simnet_wire_free(wire);
    return ok;
}

static bool sw_run_invalid_header(
    uint64_t seed, enum simnet_byzantine_class kind,
    uint64_t *out_fp, struct simnet_wire_stats *out_stats,
    struct simnet_wire_byzantine_observation *out_obs)
{
    blocker_reset_for_testing();
    blocker_set_clock_for_testing(3000000 + (int64_t)kind * 100000);
    struct simnet_wire *wire = simnet_wire_create(1, seed);
    if (!wire)
        return false;

    bool ok = simnet_wire_start_invalid_header_peer(wire, 0, kind) &&
              simnet_wire_run(wire, 8192, 512);

    struct simnet_wire_stats st;
    memset(&st, 0, sizeof(st));
    struct simnet_wire_byzantine_observation obs;
    memset(&obs, 0, sizeof(obs));
    bool got_stats = simnet_wire_get_stats(wire, &st);
    bool got_obs = simnet_wire_get_byzantine_observation(wire, &obs);
    if (got_stats) {
        if (out_stats)
            *out_stats = st;
        if (out_fp)
            *out_fp = st.fingerprint;
    }
    if (got_obs && out_obs)
        *out_obs = obs;

    ok = ok &&
         got_stats &&
         got_obs &&
         sw_stats_monitors_ok(&st) &&
         obs.kind == kind &&
         obs.tier == SIMNET_BYZ_TIER_HEADER_ADMISSION &&
         obs.injected &&
         obs.rejected &&
         obs.expected_reason_observed &&
         strcmp(obs.reject_reason,
                simnet_byzantine_expected_reason(kind)) == 0 &&
         obs.expected_blocker_observed &&
         obs.tip_unchanged &&
         obs.coins_unchanged &&
         obs.peer_misbehaved &&
         !obs.peer_banned &&
         st.peer_misbehave_events > 0 &&
         st.not_implemented_peers == 0;

    simnet_wire_free(wire);
    return ok;
}

int test_simnet_wire_peer_malformed_frame(void)
{
    printf("\n=== simnet_wire malformed frame peer ===\n");
    int failures = 0;
    uint64_t fp_a = 0;
    uint64_t fp_b = 0;
    struct simnet_wire_stats st;
    memset(&st, 0, sizeof(st));

    bool a = sw_run_malformed(0x571A00000000b001ULL, &fp_a, &st);
    bool b = sw_run_malformed(0x571A00000000b001ULL, &fp_b, NULL);
    SW_CHECK("wire malformed: checksum fail and misbehave observed", a);
    SW_CHECK("wire malformed: same-seed fingerprint deterministic",
             a && b && fp_a == fp_b);
    printf("[wire malformed] fp=0x%016" PRIx64
           " checksum=%" PRIu64 " misbehave=%" PRIu64
           " max_recv=%zu\n",
           fp_a, st.checksum_fail_events, st.peer_misbehave_events,
           st.max_recv_msg_count);
    return failures;
}

int test_simnet_wire_peer_bad_handshake(void)
{
    printf("\n=== simnet_wire bad handshake peer ===\n");
    int failures = 0;
    uint64_t fp_a = 0;
    uint64_t fp_b = 0;
    struct simnet_wire_stats st;
    memset(&st, 0, sizeof(st));

    bool a = sw_run_bad_handshake(0x571A00000000b002ULL, &fp_a, &st);
    bool b = sw_run_bad_handshake(0x571A00000000b002ULL, &fp_b, NULL);
    SW_CHECK("wire bad_handshake: pre-version data disconnects", a);
    SW_CHECK("wire bad_handshake: same-seed fingerprint deterministic",
             a && b && fp_a == fp_b);
    printf("[wire bad_handshake] fp=0x%016" PRIx64
           " misbehave=%" PRIu64 " disconnected=%d max_recv=%zu\n",
           fp_a, st.peer_misbehave_events, st.nut_disconnected ? 1 : 0,
           st.max_recv_msg_count);
    return failures;
}

int test_simnet_wire_peer_flood(void)
{
    printf("\n=== simnet_wire flood peer ===\n");
    int failures = 0;
    uint64_t fp_a = 0;
    uint64_t fp_b = 0;
    struct simnet_wire_stats st;
    memset(&st, 0, sizeof(st));

    bool a = sw_run_flood(0x571A00000000b003ULL, &fp_a, &st);
    bool b = sw_run_flood(0x571A00000000b003ULL, &fp_b, NULL);
    SW_CHECK("wire flood: recv queue bounded and backpressure fires", a);
    SW_CHECK("wire flood: same-seed fingerprint deterministic",
             a && b && fp_a == fp_b);
    printf("[wire flood] fp=0x%016" PRIx64
           " backpressure=%" PRIu64 " max_recv=%zu send=%zu inv=%zu\n",
           fp_a, st.backpressure_reject_events, st.max_recv_msg_count,
           st.max_send_size, st.max_inventory_to_send);
    return failures;
}

int test_simnet_wire_peer_slowloris(void)
{
    printf("\n=== simnet_wire slowloris peer ===\n");
    int failures = 0;
    uint64_t fp_a = 0;
    uint64_t fp_b = 0;
    struct simnet_wire_stats st;
    memset(&st, 0, sizeof(st));

    bool a = sw_run_slowloris(0x571A00000000b004ULL, &fp_a, &st);
    bool b = sw_run_slowloris(0x571A00000000b004ULL, &fp_b, NULL);
    SW_CHECK("wire slowloris: no halt/disconnect and recovery pong works", a);
    SW_CHECK("wire slowloris: same-seed fingerprint deterministic",
             a && b && fp_a == fp_b);
    printf("[wire slowloris] fp=0x%016" PRIx64
           " ticks=%" PRIu64 " max_recv=%zu pong=%d\n",
           fp_a, st.ticks, st.max_recv_msg_count, st.pong_received ? 1 : 0);
    return failures;
}

int test_simnet_wire_mixed_scenario(void)
{
    printf("\n=== simnet_wire mixed scenario ===\n");
    int failures = 0;
    uint64_t fp_a = 0;
    uint64_t fp_b = 0;
    struct simnet_wire_stats st;
    memset(&st, 0, sizeof(st));

    bool a = sw_run_mixed(0x571A00000000b005ULL, &fp_a, &st);
    bool b = sw_run_mixed(0x571A00000000b005ULL, &fp_b, NULL);
    SW_CHECK("wire mixed: honest peer recovers under flood+slowloris", a);
    SW_CHECK("wire mixed: same-seed fingerprint deterministic",
             a && b && fp_a == fp_b);
    printf("[wire mixed] fp=0x%016" PRIx64
           " backpressure=%" PRIu64 " max_recv=%zu pong=%d\n",
           fp_a, st.backpressure_reject_events, st.max_recv_msg_count,
           st.pong_received ? 1 : 0);
    return failures;
}

int test_simnet_wire_peer_invalid_block(void)
{
    printf("\n=== simnet_wire invalid block peer ===\n");
    int failures = 0;
    static const enum simnet_byzantine_class kinds[] = {
        SIMNET_BYZ_BAD_MERKLE,
        SIMNET_BYZ_BAD_CB_AMOUNT,
        SIMNET_BYZ_BIP30_DUP_TXID,
        SIMNET_BYZ_MISSING_SPEND,
        SIMNET_BYZ_IMMATURE_SPEND,
        SIMNET_BYZ_NEGATIVE_OUTPUT,
        SIMNET_BYZ_OVERFLOW_OUTPUT,
        SIMNET_BYZ_OVERSIZE_VTX,
    };

    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        enum simnet_byzantine_class kind = kinds[i];
        uint64_t seed = 0x571A00000000c000ULL + (uint64_t)kind;
        uint64_t fp_a = 0;
        uint64_t fp_b = 0;
        struct simnet_wire_stats st;
        struct simnet_wire_byzantine_observation obs;
        memset(&st, 0, sizeof(st));
        memset(&obs, 0, sizeof(obs));

        bool a = sw_run_invalid_block(seed, kind, &fp_a, &st, &obs);
        bool b = sw_run_invalid_block(seed, kind, &fp_b, NULL, NULL);
        char label[128];
        snprintf(label, sizeof(label),
                 "wire invalid_block %s: reject/blocker/ban/recover",
                 simnet_byzantine_class_name(kind));
        SW_CHECK(label, a);
        snprintf(label, sizeof(label),
                 "wire invalid_block %s: same-seed deterministic",
                 simnet_byzantine_class_name(kind));
        SW_CHECK(label, a && b && fp_a == fp_b);
        printf("[wire invalid_block] kind=%s fp=0x%016" PRIx64
               " reason=%s banned=%d honest_h=%d\n",
               simnet_byzantine_class_name(kind), fp_a,
               obs.reject_reason, obs.peer_banned ? 1 : 0,
               obs.honest_tip_after);
    }

    blocker_reset_for_testing();
    return failures;
}

int test_simnet_wire_peer_invalid_header(void)
{
    printf("\n=== simnet_wire invalid header peer ===\n");
    int failures = 0;
    uint64_t fp_a = 0;
    uint64_t fp_b = 0;
    struct simnet_wire_stats st;
    struct simnet_wire_byzantine_observation obs;
    memset(&st, 0, sizeof(st));
    memset(&obs, 0, sizeof(obs));

    bool a = sw_run_invalid_header(0x571A00000000c100ULL,
                                   SIMNET_BYZ_INVALID_POW,
                                   &fp_a, &st, &obs);
    bool b = sw_run_invalid_header(0x571A00000000c100ULL,
                                   SIMNET_BYZ_INVALID_POW,
                                   &fp_b, NULL, NULL);
    SW_CHECK("wire invalid_header: expected reject/blocker observed", a);
    SW_CHECK("wire invalid_header: same-seed fingerprint deterministic",
             a && b && fp_a == fp_b);
    printf("[wire invalid_header] fp=0x%016" PRIx64
           " reason=%s misbehave=%" PRIu64 "\n",
           fp_a, obs.reject_reason, st.peer_misbehave_events);
    blocker_reset_for_testing();
    return failures;
}

int test_simnet_wire(void)
{
    printf("\n=== simnet_wire honest in-memory transport ===\n");
    int failures = 0;

    {
        uint64_t fp = 0;
        struct simnet_wire_stats st;
        memset(&st, 0, sizeof(st));
        bool ok = sw_run_loopback(0x571A000000000001ULL,
                                  0x70696e67504f4e47ULL,
                                  &fp, &st);
        SW_CHECK("wire: version/verack handshake + ping/pong completes", ok);
        SW_CHECK("wire: pong was captured through NUT send_head",
                 ok && st.delivered_to_peer_bytes > 0 &&
                 st.pong_received);
        SW_CHECK("wire: run is bounded and drained",
                 ok && st.ticks <= 2048 && st.pending_events == 0 &&
                 st.to_nut_bytes == 0 && st.to_peer_bytes == 0);
        printf("[wire] seed=0x%016" PRIx64 " fp=0x%016" PRIx64
               " ticks=%" PRIu64 " nut_bytes=%" PRIu64
               " peer_bytes=%" PRIu64 " rng=%" PRIu64 "\n",
               (uint64_t)0x571A000000000001ULL, fp, st.ticks,
               st.delivered_to_nut_bytes, st.delivered_to_peer_bytes,
               st.rng_count);
    }

    {
        uint64_t fp_a = 0;
        uint64_t fp_b = 0;
        uint64_t fp_c = 0;
        bool a = sw_run_loopback(0x571A000000000002ULL,
                                 0x1111222233334444ULL,
                                 &fp_a, NULL);
        bool b = sw_run_loopback(0x571A000000000002ULL,
                                 0x1111222233334444ULL,
                                 &fp_b, NULL);
        bool c = sw_run_loopback(0x571A000000000003ULL,
                                 0x1111222233334444ULL,
                                 &fp_c, NULL);
        SW_CHECK("wire: same-seed runs complete", a && b);
        SW_CHECK("wire: same-seed FNV fingerprint deterministic",
                 a && b && fp_a == fp_b);
        SW_CHECK("wire: different seed still completes", c);
        if (a && c) {
            printf("[wire determinism] fp same-seed=0x%016" PRIx64
                   " different-seed=0x%016" PRIx64 "%s\n",
                   fp_a, fp_c, fp_a == fp_c ? " (same)" : " (different)");
        }
    }

    failures += test_simnet_wire_peer_malformed_frame();
    failures += test_simnet_wire_peer_bad_handshake();
    failures += test_simnet_wire_peer_flood();
    failures += test_simnet_wire_peer_slowloris();
    failures += test_simnet_wire_mixed_scenario();
    failures += test_simnet_wire_peer_invalid_block();
    failures += test_simnet_wire_peer_invalid_header();

    printf("=== simnet_wire: %d failures ===\n", failures);
    return failures;
}
