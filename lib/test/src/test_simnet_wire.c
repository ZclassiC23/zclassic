/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * simnet_wire honest loopback tests. These exercise real P2P framing,
 * receive reassembly, msgprocessor dispatch, and send_head capture without
 * starting network threads.
 */

#include "test/test_helpers.h"

#include "sim/simnet_wire.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define SW_CHECK(name, expr) do {          \
    printf("%s... ", (name));              \
    if ((expr)) printf("OK\n");            \
    else { printf("FAIL\n"); failures++; } \
} while (0)

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

    printf("=== simnet_wire: %d failures ===\n", failures);
    return failures;
}
