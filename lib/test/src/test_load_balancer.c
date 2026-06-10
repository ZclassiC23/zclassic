/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for on-chain load balancer. */

#include "test/test_helpers.h"
#include "net/load_balancer.h"

int test_load_balancer(void)
{
    int failures = 0;

    printf("load_balancer: select from empty list... ");
    {
        int best = site_select_replica(NULL, 0);
        if (best == -1) printf("OK\n");
        else { printf("FAIL (got %d)\n", best); failures++; }
    }

    printf("load_balancer: select single replica... ");
    {
        struct site_replica r = {
            .onion = "zc23test.onion",
            .capacity = 100,
            .version = 1,
            .height = 3046000,
            .latency_us = 200000,
            .reachable = true
        };
        int best = site_select_replica(&r, 1);
        if (best == 0) printf("OK\n");
        else { printf("FAIL (got %d)\n", best); failures++; }
    }

    printf("load_balancer: prefer reachable over unreachable... ");
    {
        struct site_replica r[2] = {
            { .onion = "fast.onion", .capacity = 200, .height = 3046000,
              .latency_us = 100000, .reachable = false },
            { .onion = "slow.onion", .capacity = 50, .height = 3045000,
              .latency_us = 500000, .reachable = true }
        };
        int best = site_select_replica(r, 2);
        if (best == 1) printf("OK\n");
        else { printf("FAIL (got %d, expected 1)\n", best); failures++; }
    }

    printf("load_balancer: prefer low latency... ");
    {
        struct site_replica r[3] = {
            { .onion = "a.onion", .capacity = 100, .height = 3046000,
              .latency_us = 500000, .reachable = true },
            { .onion = "b.onion", .capacity = 100, .height = 3046000,
              .latency_us = 100000, .reachable = true },
            { .onion = "c.onion", .capacity = 100, .height = 3046000,
              .latency_us = 300000, .reachable = true }
        };
        int best = site_select_replica(r, 3);
        if (best == 1) printf("OK\n");
        else { printf("FAIL (got %d, expected 1)\n", best); failures++; }
    }

    printf("load_balancer: prefer high capacity at same latency... ");
    {
        struct site_replica r[2] = {
            { .onion = "small.onion", .capacity = 50, .height = 3046000,
              .latency_us = 200000, .reachable = true },
            { .onion = "big.onion", .capacity = 500, .height = 3046000,
              .latency_us = 200000, .reachable = true }
        };
        int best = site_select_replica(r, 2);
        if (best == 1) printf("OK\n");
        else { printf("FAIL (got %d, expected 1)\n", best); failures++; }
    }

    printf("load_balancer: prefer fresh over stale... ");
    {
        struct site_replica r[2] = {
            { .onion = "old.onion", .capacity = 100, .height = 3000000,
              .latency_us = 200000, .reachable = true },
            { .onion = "new.onion", .capacity = 100, .height = 3046000,
              .latency_us = 200000, .reachable = true }
        };
        int best = site_select_replica(r, 2);
        if (best == 1) printf("OK\n");
        else { printf("FAIL (got %d, expected 1)\n", best); failures++; }
    }

    printf("load_balancer: probe recent replica → reachable... ");
    {
        struct site_replica r = {
            .onion = "recent.onion",
            .height = 3046450,
            .reachable = false
        };
        site_probe_replica(&r);
        if (r.reachable && r.latency_us > 0)
            printf("OK (latency=%lldus)\n", (long long)r.latency_us);
        else { printf("FAIL\n"); failures++; }
    }

    printf("load_balancer: probe stale replica → unreachable... ");
    {
        struct site_replica r = {
            .onion = "ancient.onion",
            .height = 1000000,
            .reachable = true /* should become false */
        };
        site_probe_replica(&r);
        if (!r.reachable) printf("OK\n");
        else { printf("FAIL (still reachable)\n"); failures++; }
    }

    printf("load_balancer: announce builds script... ");
    {
        bool ok = site_announce_replica("/tmp",
            "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
            "zc23test1234.onion", 200, 1);
        /* Returns false — tx broadcast not yet implemented */
        if (!ok) printf("OK (not implemented)\n");
        else { printf("FAIL (should return false)\n"); failures++; }
    }

    printf("load_balancer: connect_best with no chain data... ");
    {
        char onion[68];
        bool ok = site_connect_best("/tmp/nonexistent",
            "0000000000000000000000000000000000000000000000000000000000000000",
            onion, sizeof(onion));
        if (!ok) printf("OK (no replicas found)\n");
        else { printf("FAIL (should return false)\n"); failures++; }
    }

    return failures;
}
