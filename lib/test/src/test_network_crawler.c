/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_network_crawler — the whole-network OBSERVATORY.
 *
 *   1. the PURE census fold (network_census_compute) over synthetic probe
 *      results: version histogram + top-N ordering, onion/clearnet split,
 *      height distribution (modal/max/min/spread), and the whole-network
 *      eclipse signal (fires when our modal is a network minority, stays quiet
 *      when we agree with the network or the sample is too small),
 *   2. the census table's bounds through the injectable probe_fn seam: addr
 *      dedup, per-round cap, and prune-when-full (never exceeds the cap),
 *   3. the network_census dumper surfaces the eclipse signal + evidence.
 *
 * NO real sockets: the dialer is replaced by a synthetic probe_fn that encodes
 * each node's measured properties in its address octets.
 */

#include "test/test_helpers.h"

#include "services/network_crawler.h"
#include "net/netaddr.h"
#include "json/json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NC_CHECK(cond) do { \
    if (cond) { /* pass */ } \
    else { printf("  FAIL: %s (line %d)\n", #cond, __LINE__); failures++; } \
} while (0)

/* ── synthetic address + probe (properties encoded in IPv4 octets) ────── */

static struct net_address mk_addr(uint8_t hc, bool reachable, bool onion,
                                  uint8_t seq, uint16_t port)
{
    struct net_address na;
    net_address_init(&na);
    unsigned char flags = (uint8_t)((reachable ? 0x01 : 0) | (onion ? 0x02 : 0));
    unsigned char ip4[4] = { 10, hc, flags, seq };
    net_addr_set_ipv4(&na.svc.addr, ip4);
    na.svc.port = port;
    return na;
}

/* Decode the octets a mk_addr() encoded: height=3000000+hc, reachable/onion
 * from the flags octet. subver alternates by height parity. */
static bool synth_probe(const struct net_address *addr, int ct, int ht,
                        struct ncrawl_probe_result *out)
{
    (void)ct;
    (void)ht;
    if (!addr || !out)
        return false;
    memset(out, 0, sizeof(*out));
    net_service_to_string(&addr->svc, out->addr, sizeof(out->addr));
    if (!out->addr[0])
        return false;
    unsigned char hc = addr->svc.addr.ip[13];
    unsigned char fl = addr->svc.addr.ip[14];
    out->is_onion = (fl & 0x02) != 0;
    out->reachable = (fl & 0x01) != 0;
    out->version = 170011;
    out->services = 1;
    out->best_height = out->reachable ? (int64_t)(3000000 + hc) : -1;
    snprintf(out->subver, sizeof(out->subver), "%s",
             (hc & 1) ? "/zclassic23:1/" : "/MagicBean:2.1.2/");
    out->latency_us = 1234;
    out->last_probe_us = 1000 + (int64_t)addr->svc.addr.ip[15];
    return true;
}

/* Build a probe result directly (for the pure-fold tests). */
static struct ncrawl_probe_result mk_res(const char *addr, bool reachable,
                                         bool onion, int64_t height,
                                         const char *subver)
{
    struct ncrawl_probe_result r;
    memset(&r, 0, sizeof(r));
    snprintf(r.addr, sizeof(r.addr), "%s", addr);
    r.reachable = reachable;
    r.is_onion = onion;
    r.version = 170011;
    r.best_height = reachable ? height : -1;
    if (subver)
        snprintf(r.subver, sizeof(r.subver), "%s", subver);
    r.latency_us = 100;
    r.last_probe_us = 42;
    return r;
}

int test_network_crawler(void)
{
    int failures = 0;
    printf("network_crawler...\n");

    /* ── 1. pure fold: histograms, splits, height distribution ─────────── */
    printf("  census fold: histogram + split + height distribution... ");
    {
        struct ncrawl_probe_result r[6] = {
            mk_res("1.0.0.1:8033", true,  false, 100, "/A/"),
            mk_res("1.0.0.2:8033", true,  false, 100, "/A/"),
            mk_res("1.0.0.3:8033", true,  false, 100, "/B/"),
            mk_res("1.0.0.4:8033", true,  false, 200, "/A/"),
            mk_res("1.0.0.5:8033", true,  true,  200, "/A/"),
            mk_res("1.0.0.6:8033", false, false, 0,   "/A/"),
        };
        struct network_census_view v;
        network_census_compute(r, 6, /*own_modal*/ 100, /*now*/ 555, &v);
        NC_CHECK(v.ready);
        NC_CHECK(v.computed_at == 555);
        NC_CHECK(v.probed == 6);
        NC_CHECK(v.reachable_count == 5);
        NC_CHECK(v.onion_count == 1);
        NC_CHECK(v.clearnet_count == 4);
        NC_CHECK(v.heights_known == 5);
        NC_CHECK(v.max_height == 200);
        NC_CHECK(v.min_height == 100);
        NC_CHECK(v.height_spread == 100);
        NC_CHECK(v.modal_height == 100);
        NC_CHECK(v.modal_height_count == 3);
        NC_CHECK(v.num_versions == 2);
        /* histogram sorted descending: /A/ (4) before /B/ (1) */
        NC_CHECK(strcmp(v.versions[0].subver, "/A/") == 0);
        NC_CHECK(v.versions[0].count == 4);
        NC_CHECK(strcmp(v.versions[1].subver, "/B/") == 0);
        NC_CHECK(v.versions[1].count == 1);
        /* we agree with the network modal → no eclipse */
        NC_CHECK(v.eclipse_suspected == false);
        printf("done\n");
    }

    /* ── 2. eclipse FIRES: our modal is a network minority ─────────────── */
    printf("  eclipse fires when our modal is a network minority... ");
    {
        struct ncrawl_probe_result r[8];
        for (int i = 0; i < 6; i++)
            r[i] = mk_res("a", true, false, 200, "/A/"); /* addr unused by fold */
        for (int i = 6; i < 8; i++)
            r[i] = mk_res("b", true, false, 100, "/A/");
        struct network_census_view v;
        network_census_compute(r, 8, /*own_modal*/ 100, 1, &v);
        NC_CHECK(v.reachable_count == 8);
        NC_CHECK(v.network_modal_height == 200);
        NC_CHECK(v.modal_height_count == 6);
        NC_CHECK(v.own_modal_height == 100);
        NC_CHECK(v.network_count_at_own_modal == 2);
        NC_CHECK(v.eclipse_suspected == true);
        printf("done\n");
    }

    /* ── 3. eclipse QUIET: sample too small + agreement + boundary ─────── */
    printf("  eclipse quiet on small sample / agreement / 1/3 boundary... ");
    {
        /* too small: 3 reachable < NCRAWL_ECLIPSE_MIN even though minority */
        struct ncrawl_probe_result small[3];
        for (int i = 0; i < 3; i++)
            small[i] = mk_res("x", true, false, 200, "/A/");
        struct network_census_view v;
        network_census_compute(small, 3, 100, 1, &v);
        NC_CHECK(v.reachable_count == 3);
        NC_CHECK(v.eclipse_suspected == false);

        /* agreement: own_modal == network modal */
        struct ncrawl_probe_result agree[6];
        for (int i = 0; i < 6; i++)
            agree[i] = mk_res("y", true, false, 200, "/A/");
        network_census_compute(agree, 6, 200, 1, &v);
        NC_CHECK(v.eclipse_suspected == false);

        /* boundary: our height is exactly 1/3 (2 of 6) → not a minority */
        struct ncrawl_probe_result edge[6];
        for (int i = 0; i < 4; i++)
            edge[i] = mk_res("z", true, false, 200, "/A/");
        for (int i = 4; i < 6; i++)
            edge[i] = mk_res("w", true, false, 100, "/A/");
        network_census_compute(edge, 6, 100, 1, &v);
        NC_CHECK(v.network_count_at_own_modal == 2);
        NC_CHECK(v.eclipse_suspected == false);
        printf("done\n");
    }

    /* ── 4. fold bounds: oversized sample clamps to the cap ────────────── */
    printf("  census fold clamps oversized sample to NCRAWL_MAX_CENSUS... ");
    {
        int big = NCRAWL_MAX_CENSUS + 10;
        struct ncrawl_probe_result *r =
            calloc((size_t)big, sizeof(*r));
        NC_CHECK(r != NULL);
        if (r) {
            for (int i = 0; i < big; i++)
                r[i] = mk_res("c", true, false, 500, "/A/");
            struct network_census_view v;
            network_census_compute(r, big, -1, 1, &v);
            NC_CHECK(v.probed == NCRAWL_MAX_CENSUS);
            NC_CHECK(v.reachable_count == NCRAWL_MAX_CENSUS);
            free(r);
        }
        printf("done\n");
    }

    /* ── 5. census ingest via probe seam: dedup + per-round cap ────────── */
    printf("  census ingest: addr dedup + per-round cap (probe seam)... ");
    {
        network_crawler_test_reset();
        network_crawler_test_set_probe_fn(synth_probe);

        /* dedup: the same address twice in one round → one census row. */
        struct net_address dup[2] = {
            mk_addr(50, true, false, 7, 8033),
            mk_addr(50, true, false, 7, 8033),
        };
        int probed = network_crawler_test_probe_round(dup, 2);
        NC_CHECK(probed == 2);
        NC_CHECK(network_crawler_test_census_count() == 1);

        /* per-round cap: feed MORE than NCRAWL_MAX_PER_ROUND distinct addrs. */
        network_crawler_test_reset();
        network_crawler_test_set_probe_fn(synth_probe);
        int over = NCRAWL_MAX_PER_ROUND + 20;
        struct net_address *addrs = calloc((size_t)over, sizeof(*addrs));
        NC_CHECK(addrs != NULL);
        if (addrs) {
            for (int i = 0; i < over; i++)
                addrs[i] = mk_addr(60, true, false, (uint8_t)i,
                                   (uint16_t)(9000 + i));
            probed = network_crawler_test_probe_round(addrs, over);
            NC_CHECK(probed == NCRAWL_MAX_PER_ROUND);
            NC_CHECK(network_crawler_test_census_count() == NCRAWL_MAX_PER_ROUND);
            free(addrs);
        }
        printf("done\n");
    }

    /* ── 6. census prune: never exceeds NCRAWL_MAX_CENSUS ──────────────── */
    printf("  census prune: bounded at NCRAWL_MAX_CENSUS under overflow... ");
    {
        network_crawler_test_reset();
        network_crawler_test_set_probe_fn(synth_probe);
        int total = NCRAWL_MAX_CENSUS + 128;   /* overflow the table */
        int counter = 0;
        bool ever_exceeded = false;
        while (counter < total) {
            struct net_address round[NCRAWL_MAX_PER_ROUND];
            int n = 0;
            for (; n < NCRAWL_MAX_PER_ROUND && counter < total; n++, counter++) {
                round[n] = mk_addr(70, true, false,
                                   (uint8_t)(counter & 0xff),
                                   (uint16_t)(10000 + counter));
            }
            network_crawler_test_probe_round(round, n);
            if (network_crawler_test_census_count() > NCRAWL_MAX_CENSUS)
                ever_exceeded = true;
        }
        NC_CHECK(!ever_exceeded);
        NC_CHECK(network_crawler_test_census_count() == NCRAWL_MAX_CENSUS);
        printf("done\n");
    }

    /* ── 7. dumper surfaces the eclipse signal + evidence ──────────────── */
    printf("  network_census dumper surfaces eclipse + evidence... ");
    {
        network_crawler_test_reset();
        network_crawler_test_set_probe_fn(synth_probe);
        network_crawler_test_set_own_modal(3000100); /* our peers on hc=100 */

        struct net_address net[8];
        for (int i = 0; i < 6; i++)   /* network majority on hc=200 */
            net[i] = mk_addr(200, true, false, (uint8_t)i, (uint16_t)(11000 + i));
        for (int i = 6; i < 8; i++)   /* our minority height hc=100 */
            net[i] = mk_addr(100, true, false, (uint8_t)i, (uint16_t)(11000 + i));
        int probed = network_crawler_test_probe_round(net, 8);
        NC_CHECK(probed == 8);

        struct json_value out;
        json_init(&out);
        bool ok = network_crawler_dump_state_json(&out, NULL);
        NC_CHECK(ok);
        const struct json_value *ecl = json_get(&out, "eclipse_suspected");
        NC_CHECK(ecl != NULL && json_get_bool(ecl) == true);
        const struct json_value *reach = json_get(&out, "reachable_count");
        NC_CHECK(reach != NULL && json_get_int(reach) == 8);
        const struct json_value *hist = json_get(&out, "version_histogram");
        NC_CHECK(hist != NULL && hist->type == JSON_ARR);
        const struct json_value *evid = json_get(&out, "eclipse_evidence");
        NC_CHECK(evid != NULL && evid->type == JSON_OBJ);
        if (evid) {
            const struct json_value *own = json_get(evid, "own_modal_height");
            const struct json_value *nm = json_get(evid, "network_modal_height");
            const struct json_value *at = json_get(evid,
                                                   "network_count_at_own_modal");
            NC_CHECK(own != NULL && json_get_int(own) == 3000100);
            NC_CHECK(nm != NULL && json_get_int(nm) == 3000200);
            NC_CHECK(at != NULL && json_get_int(at) == 2);
        }
        json_free(&out);
        network_crawler_test_reset();
        printf("done\n");
    }

    if (failures == 0)
        printf("network_crawler: ALL PASS\n");
    else
        printf("network_crawler: %d FAILURE(S)\n", failures);
    return failures;
}
