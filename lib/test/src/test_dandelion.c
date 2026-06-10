/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * BIP 156 (Dandelion) unit tests: per-edge routing, stempool byte
 * ownership, random embargo bounds, advertisement gate, pending
 * dandelion-request tracking, and the RNG-backed shuffle/coin hooks. */

#include "platform/time_compat.h"
#include "test/test_helpers.h"
#include "net/dandelion.h"
#include "core/uint256.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* test hooks — defined under ZCL_TESTING in lib/net/src/dandelion.c.
 * Exercise the RNG-driven shuffle, coin-flip, and embargo sampler
 * without needing a full net_manager fixture. */
bool dandelion_test_shuffle(node_id_t *inout, int n);
bool dandelion_test_should_stem_coin(bool *out_stem);
bool dandelion_test_embargo_offset(int64_t *out_secs);

static struct uint256 make_test_hash(uint8_t seed)
{
    struct uint256 h;
    memset(h.data, seed, 32);
    return h;
}

static const uint8_t k_tx_bytes[] = { 0x01, 0x02, 0x03, 0x04, 0x05 };

int test_dandelion(void)
{
    int failures = 0;

    /* ── init / free ───────────────────────────────────────────── */
    printf("dandelion_init / dandelion_free... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        if (ds.enabled && ds.num_stem_peers == 0 && ds.stempool_count == 0 &&
            ds.epoch_len_secs == DANDELION_EPOCH_MEAN_SECS)
            printf("OK\n");
        else {
            printf("FAIL\n"); failures++;
        }
        dandelion_free(&ds);
    }

    /* ── should_stem returns false when disabled (per-state) ───── */
    printf("dandelion_should_stem disabled... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        ds.enabled = false;
        ds.num_stem_peers = 2;
        ds.stem_peers[0] = 10;
        ds.stem_peers[1] = 20;
        bool stem = dandelion_should_stem(&ds, DANDELION_NODE_ID_NONE);
        if (!stem)
            printf("OK\n");
        else {
            printf("FAIL (expected false when disabled)\n"); failures++;
        }
        dandelion_free(&ds);
    }

    /* ── global -dandelion=0 switch gates routing ──────────────── */
    printf("dandelion global enable switch... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        ds.num_stem_peers = 2;
        ds.stem_peers[0] = 10;
        ds.stem_peers[1] = 20;

        dandelion_set_enabled(false);
        bool stem = dandelion_should_stem(&ds, DANDELION_NODE_ID_NONE);
        node_id_t p = dandelion_get_stem_peer(&ds, DANDELION_NODE_ID_NONE);
        bool off_ok = !stem && !dandelion_enabled() &&
                      p == DANDELION_NODE_ID_NONE;

        dandelion_set_enabled(true);
        node_id_t p2 = dandelion_get_stem_peer(&ds, DANDELION_NODE_ID_NONE);
        bool on_ok = dandelion_enabled() && p2 != DANDELION_NODE_ID_NONE;

        if (off_ok && on_ok)
            printf("OK\n");
        else {
            printf("FAIL (off_ok=%d on_ok=%d)\n", off_ok, on_ok); failures++;
        }
        dandelion_free(&ds);
    }

    /* ── should_stem returns false when no stem peers ──────────── */
    printf("dandelion_should_stem no peers... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        ds.enabled = true;
        ds.num_stem_peers = 0;
        bool stem = dandelion_should_stem(&ds, DANDELION_NODE_ID_NONE);
        if (!stem)
            printf("OK\n");
        else {
            printf("FAIL (expected false with no stem peers)\n"); failures++;
        }
        dandelion_free(&ds);
    }

    /* ── should_stem returns true ~90% when stem peers exist ───── */
    printf("dandelion_should_stem probability... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        ds.enabled = true;
        ds.num_stem_peers = 2;
        ds.stem_peers[0] = 100;
        ds.stem_peers[1] = 200;

        int stem_count = 0;
        int trials = 1000;
        for (int i = 0; i < trials; i++) {
            if (dandelion_should_stem(&ds, DANDELION_NODE_ID_NONE))
                stem_count++;
        }
        /* Should be ~90% stem (DANDELION_FLUFF_PROB = 10%) */
        double pct = (double)stem_count / trials * 100.0;
        if (pct >= 80.0 && pct <= 97.0)
            printf("OK (%.1f%% stem)\n", pct);
        else {
            printf("FAIL (%.1f%% stem, expected ~90%%)\n", pct); failures++;
        }
        dandelion_free(&ds);
    }

    /* ── per-edge mapping is stable within an epoch (BIP 156) ──── */
    printf("dandelion_get_stem_peer per-edge stability... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        ds.num_stem_peers = 2;
        ds.stem_peers[0] = 10;
        ds.stem_peers[1] = 20;
        ds.route_salt = 0xDEADBEEFCAFEF00DULL;

        bool stable = true;
        bool valid = true;
        for (node_id_t edge = 0; edge < 16 && stable && valid; edge++) {
            node_id_t first = dandelion_get_stem_peer(&ds, edge);
            if (first != 10 && first != 20)
                valid = false;
            for (int rep = 0; rep < 8; rep++) {
                if (dandelion_get_stem_peer(&ds, edge) != first) {
                    stable = false;
                    break;
                }
            }
        }
        /* Local (wallet) edge must also be stable. */
        node_id_t w1 = dandelion_get_stem_peer(&ds, DANDELION_NODE_ID_NONE);
        node_id_t w2 = dandelion_get_stem_peer(&ds, DANDELION_NODE_ID_NONE);

        if (stable && valid && w1 == w2 && (w1 == 10 || w1 == 20))
            printf("OK\n");
        else {
            printf("FAIL (stable=%d valid=%d w1=%d w2=%d)\n",
                   stable, valid, w1, w2);
            failures++;
        }
        dandelion_free(&ds);
    }

    /* ── per-edge mapping spreads across both destinations ─────── */
    printf("dandelion_get_stem_peer edge spread... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        ds.num_stem_peers = 2;
        ds.stem_peers[0] = 10;
        ds.stem_peers[1] = 20;
        ds.route_salt = 0x1234567890ABCDEFULL;

        int hits10 = 0, hits20 = 0;
        for (node_id_t edge = 0; edge < 64; edge++) {
            node_id_t p = dandelion_get_stem_peer(&ds, edge);
            if (p == 10) hits10++;
            else if (p == 20) hits20++;
        }
        /* The splitmix64-keyed mapping should land on both
         * destinations across 64 distinct edges (P(one-sided) ~
         * 2^-63). */
        if (hits10 > 0 && hits20 > 0 && hits10 + hits20 == 64)
            printf("OK (%d/%d)\n", hits10, hits20);
        else {
            printf("FAIL (hits10=%d hits20=%d)\n", hits10, hits20);
            failures++;
        }
        dandelion_free(&ds);
    }

    /* ── get_stem_peer never returns the sender ────────────────── */
    printf("dandelion_get_stem_peer skips sender... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        ds.num_stem_peers = 2;
        ds.stem_peers[0] = 10;
        ds.stem_peers[1] = 20;

        bool ok = true;
        for (uint64_t salt = 0; salt < 32 && ok; salt++) {
            ds.route_salt = salt * 0x9E3779B97F4A7C15ULL;
            if (dandelion_get_stem_peer(&ds, 10) != 20) ok = false;
            if (dandelion_get_stem_peer(&ds, 20) != 10) ok = false;
        }
        if (ok)
            printf("OK\n");
        else {
            printf("FAIL (mapping returned the sender)\n"); failures++;
        }
        dandelion_free(&ds);
    }

    /* ── get_stem_peer returns NONE when only peer is sender ───── */
    printf("dandelion_get_stem_peer all excluded... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        ds.num_stem_peers = 1;
        ds.stem_peers[0] = 10;

        node_id_t p = dandelion_get_stem_peer(&ds, 10);
        if (p == DANDELION_NODE_ID_NONE)
            printf("OK\n");
        else {
            printf("FAIL (got %d, expected NONE)\n", p); failures++;
        }
        dandelion_free(&ds);
    }

    /* ── stempool add / contains / copy / take / remove ────────── */
    printf("dandelion_stempool basic ops... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);

        struct uint256 h1 = make_test_hash(0xAA);
        struct uint256 h2 = make_test_hash(0xBB);

        bool a1 = dandelion_stempool_add(&ds, &h1, 1, 2,
                                         k_tx_bytes, sizeof k_tx_bytes);
        bool a2 = dandelion_stempool_add(&ds, &h2, 2, 3,
                                         k_tx_bytes, sizeof k_tx_bytes);

        bool c1 = dandelion_stempool_contains(&ds, &h1);
        bool c2 = dandelion_stempool_contains(&ds, &h2);
        struct uint256 h3 = make_test_hash(0xCC);
        bool c3 = dandelion_stempool_contains(&ds, &h3);

        if (a1 && a2 && c1 && c2 && !c3 && ds.stempool_count == 2)
            printf("OK\n");
        else {
            printf("FAIL (a1=%d a2=%d c1=%d c2=%d c3=%d count=%d)\n",
                   a1, a2, c1, c2, c3, ds.stempool_count);
            failures++;
        }

        /* copy keeps the entry; bytes round-trip */
        size_t sz = 0;
        uint8_t *copy = dandelion_stempool_copy(&ds, &h1, &sz);
        if (copy && sz == sizeof k_tx_bytes &&
            memcmp(copy, k_tx_bytes, sz) == 0 &&
            dandelion_stempool_contains(&ds, &h1))
            printf("  copy OK\n");
        else {
            printf("  copy FAIL\n"); failures++;
        }
        free(copy);

        /* take transfers ownership and removes the entry */
        struct dandelion_fluff_item it;
        bool took = dandelion_stempool_take(&ds, &h1, &it);
        if (took && it.tx_size == sizeof k_tx_bytes &&
            memcmp(it.tx_bytes, k_tx_bytes, it.tx_size) == 0 &&
            !dandelion_stempool_contains(&ds, &h1) &&
            ds.stempool_count == 1)
            printf("  take OK\n");
        else {
            printf("  take FAIL\n"); failures++;
        }
        if (took)
            free(it.tx_bytes);

        /* remove discards bytes internally */
        bool removed = dandelion_stempool_remove(&ds, &h2);
        if (removed && !dandelion_stempool_contains(&ds, &h2) &&
            ds.stempool_count == 0)
            printf("  remove OK\n");
        else {
            printf("  remove FAIL\n"); failures++;
        }

        dandelion_free(&ds);
    }

    /* ── stempool duplicate rejection ──────────────────────────── */
    printf("dandelion_stempool dedup... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);

        struct uint256 h = make_test_hash(0xDD);
        bool a1 = dandelion_stempool_add(&ds, &h, 1, 2,
                                         k_tx_bytes, sizeof k_tx_bytes);
        bool a2 = dandelion_stempool_add(&ds, &h, 2, 3,
                                         k_tx_bytes, sizeof k_tx_bytes);

        if (a1 && !a2 && ds.stempool_count == 1)
            printf("OK\n");
        else {
            printf("FAIL (a1=%d a2=%d count=%d, expected 1)\n",
                   a1, a2, ds.stempool_count);
            failures++;
        }
        dandelion_free(&ds);
    }

    /* ── random embargo lands in [MIN, MIN + 6*AVG_ADD] ────────── */
    printf("dandelion_stempool embargo bounds... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);

        struct uint256 h = make_test_hash(0xE1);
        int64_t before = (int64_t)platform_time_wall_time_t();
        dandelion_stempool_add(&ds, &h, 1, 2, k_tx_bytes, sizeof k_tx_bytes);
        int64_t after = (int64_t)platform_time_wall_time_t();

        int64_t embargo = 0;
        for (int i = 0; i < DANDELION_MAX_STEMPOOL; i++) {
            if (ds.stempool[i].active) {
                embargo = ds.stempool[i].embargo_time;
                break;
            }
        }
        int64_t lo = before + DANDELION_EMBARGO_MIN_SECS;
        int64_t hi = after + DANDELION_EMBARGO_MIN_SECS +
                     6 * DANDELION_EMBARGO_AVG_ADD_SECS;
        if (embargo >= lo && embargo <= hi)
            printf("OK (+%llds)\n", (long long)(embargo - before));
        else {
            printf("FAIL (embargo=%lld lo=%lld hi=%lld)\n",
                   (long long)embargo, (long long)lo, (long long)hi);
            failures++;
        }
        dandelion_free(&ds);
    }

    /* ── embargo offsets are random and within the cap ─────────── */
    printf("dandelion embargo offset sampler... ");
    {
        bool rng_ok = true;
        bool in_range = true;
        bool any_diff = false;
        int64_t first = -1;
        for (int i = 0; i < 64 && rng_ok && in_range; i++) {
            int64_t off = 0;
            rng_ok = dandelion_test_embargo_offset(&off);
            if (!rng_ok)
                break;
            if (off < 0 || off > 6 * DANDELION_EMBARGO_AVG_ADD_SECS)
                in_range = false;
            if (i == 0)
                first = off;
            else if (off != first)
                any_diff = true;
        }
        /* 64 exponential draws (mean 20s, 1s resolution) collapsing to
         * one value would imply a broken RNG. */
        if (rng_ok && in_range && any_diff)
            printf("OK\n");
        else {
            printf("FAIL (rng_ok=%d in_range=%d any_diff=%d)\n",
                   rng_ok, in_range, any_diff);
            failures++;
        }
    }

    /* ── take_expired returns the bytes and clears entries ─────── */
    printf("dandelion_stempool_take_expired... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);

        struct uint256 h = make_test_hash(0xEE);
        dandelion_stempool_add(&ds, &h, 1, 2, k_tx_bytes, sizeof k_tx_bytes);

        /* Force the embargo time into the past */
        for (int i = 0; i < DANDELION_MAX_STEMPOOL; i++) {
            if (ds.stempool[i].active) {
                ds.stempool[i].embargo_time =
                    (int64_t)platform_time_wall_time_t() - 1;
                break;
            }
        }

        struct dandelion_fluff_item out[8];
        int nexp = dandelion_stempool_take_expired(&ds, NULL, out, 8);
        bool ok = nexp == 1 && uint256_eq(&out[0].txhash, &h) &&
                  out[0].tx_size == sizeof k_tx_bytes &&
                  memcmp(out[0].tx_bytes, k_tx_bytes, out[0].tx_size) == 0 &&
                  ds.stempool_count == 0 &&
                  ds.stat_embargo_fluff == 1;
        for (int i = 0; i < nexp; i++)
            free(out[i].tx_bytes);

        /* Unexpired entries stay put */
        struct uint256 h2 = make_test_hash(0xEF);
        dandelion_stempool_add(&ds, &h2, 1, 2, k_tx_bytes, sizeof k_tx_bytes);
        int nexp2 = dandelion_stempool_take_expired(&ds, NULL, out, 8);

        if (ok && nexp2 == 0 && ds.stempool_count == 1)
            printf("OK\n");
        else {
            printf("FAIL (ok=%d nexp=%d nexp2=%d count=%d)\n",
                   ok, nexp, nexp2, ds.stempool_count);
            failures++;
        }
        dandelion_free(&ds);
    }

    /* ── stempool eviction when full ───────────────────────────── */
    printf("dandelion_stempool eviction... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);

        /* Fill the stempool — use unique hashes via 4-byte encoding */
        for (int i = 0; i < DANDELION_MAX_STEMPOOL; i++) {
            struct uint256 h;
            memset(h.data, 0, 32);
            memcpy(h.data, &i, sizeof(i)); /* unique via int bytes */
            dandelion_stempool_add(&ds, &h, 1, 2,
                                   k_tx_bytes, sizeof k_tx_bytes);
        }

        if (ds.stempool_count != DANDELION_MAX_STEMPOOL) {
            printf("FAIL (initial count=%d)\n", ds.stempool_count);
            failures++;
        } else {
            /* Add one more — should evict oldest */
            struct uint256 extra = make_test_hash(0xFF);
            dandelion_stempool_add(&ds, &extra, 1, 2,
                                   k_tx_bytes, sizeof k_tx_bytes);

            bool has_extra = dandelion_stempool_contains(&ds, &extra);
            if (has_extra && ds.stempool_count == DANDELION_MAX_STEMPOOL)
                printf("OK\n");
            else {
                printf("FAIL (has_extra=%d count=%d)\n",
                       has_extra, ds.stempool_count);
                failures++;
            }
        }
        dandelion_free(&ds);
    }

    /* ── advertisement gate (BIP 156 serve rule) ───────────────── */
    printf("dandelion advertisement gate... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);

        struct uint256 h = make_test_hash(0xA1);
        dandelion_mark_advertised(&ds, &h, 5);

        bool to5 = dandelion_was_advertised_to(&ds, &h, 5);
        bool to6 = dandelion_was_advertised_to(&ds, &h, 6);
        struct uint256 other = make_test_hash(0xA2);
        bool other5 = dandelion_was_advertised_to(&ds, &other, 5);

        if (to5 && !to6 && !other5)
            printf("OK\n");
        else {
            printf("FAIL (to5=%d to6=%d other5=%d)\n", to5, to6, other5);
            failures++;
        }
        dandelion_free(&ds);
    }

    /* ── pending dandelion getdata tracking ────────────────────── */
    printf("dandelion request table... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);

        struct uint256 h = make_test_hash(0xB1);
        dandelion_request_add(&ds, &h, 5);

        bool pend = dandelion_request_pending(&ds, &h);
        bool wrong_peer = dandelion_request_take(&ds, &h, 6);
        bool right_peer = dandelion_request_take(&ds, &h, 5);
        bool again = dandelion_request_take(&ds, &h, 5);
        bool pend_after = dandelion_request_pending(&ds, &h);

        if (pend && !wrong_peer && right_peer && !again && !pend_after)
            printf("OK\n");
        else {
            printf("FAIL (pend=%d wrong=%d right=%d again=%d after=%d)\n",
                   pend, wrong_peer, right_peer, again, pend_after);
            failures++;
        }
        dandelion_free(&ds);
    }

    /* ── NULL safety ───────────────────────────────────────────── */
    printf("dandelion NULL safety... ");
    {
        struct dandelion_fluff_item it;
        size_t sz;
        /* These should not crash */
        dandelion_should_stem(NULL, 0);
        dandelion_get_stem_peer(NULL, 0);
        dandelion_stempool_add(NULL, NULL, 0, 0, NULL, 0);
        dandelion_stempool_remove(NULL, NULL);
        dandelion_stempool_take(NULL, NULL, &it);
        dandelion_stempool_copy(NULL, NULL, &sz);
        dandelion_stempool_contains(NULL, NULL);
        dandelion_stempool_take_expired(NULL, NULL, NULL, 0);
        dandelion_mark_advertised(NULL, NULL, 0);
        dandelion_was_advertised_to(NULL, NULL, 0);
        dandelion_request_add(NULL, NULL, 0);
        dandelion_request_pending(NULL, NULL);
        dandelion_request_take(NULL, NULL, 0);
        dandelion_maybe_rotate_epoch(NULL, NULL);
        dandelion_dump_state_json(NULL, NULL);
        printf("OK\n");
    }

    /* ── stats zero-initialized ────────────────────────────────── */
    printf("dandelion stats... ");
    {
        struct dandelion_state ds;
        dandelion_init(&ds);
        if (ds.stat_stem_sent == 0 && ds.stat_stem_recv == 0 &&
            ds.stat_fluffed == 0 && ds.stat_embargo_fluff == 0 &&
            ds.stat_served == 0 && ds.stat_refused == 0)
            printf("OK\n");
        else {
            printf("FAIL (stats not zero-initialized)\n"); failures++;
        }
        dandelion_free(&ds);
    }

    /* ── stem shuffle is non-deterministic across calls ─────
     *
     * Pre-fix: xorshift64 seeded from platform_time_wall_time_t() ^ const → two boots
     * inside the same wall-clock second produced identical shuffles.
     * Post-fix: each call pulls fresh entropy from the cryptographic
     * RNG, so back-to-back shuffles of the same input differ with
     * probability 1 - 1/8! ≈ 99.9975%. We retry a few times to drive
     * the false-FAIL probability below 1e-20 (8! ^ -5 ≈ 9.5e-23). */
    printf("dandelion stem shuffle non-deterministic... ");
    {
        bool any_diff = false;
        bool rng_ok = true;
        for (int trial = 0; trial < 5 && !any_diff && rng_ok; trial++) {
            node_id_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
            node_id_t b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
            rng_ok = dandelion_test_shuffle(a, 8) &&
                     dandelion_test_shuffle(b, 8);
            if (rng_ok && memcmp(a, b, sizeof a) != 0)
                any_diff = true;
        }
        if (rng_ok && any_diff)
            printf("OK\n");
        else if (!rng_ok) {
            printf("FAIL (cryptographic RNG returned error)\n"); failures++;
        } else {
            printf("FAIL (5/5 shuffles identical — RNG appears deterministic)\n");
            failures++;
        }
    }

    /* ── per-tx fluff coin-flip is statistically uniform ────
     *
     * Asserts the RNG path doesn't bias the 90/10 stem/fluff decision.
     * With p_stem = 0.9, n = 10000 the expected stem count is 9000 and
     * σ = sqrt(n*p*(1-p)) = sqrt(900) = 30, so the ±3σ band is
     * [8910, 9090] INCLUSIVE. A healthy crypto RNG lands in-band 99.73%
     * of the time; a stuck, inverted, or low-entropy RNG lands far
     * outside it. The coin is crypto-seeded (not deterministically
     * seedable here), so to make this a zero-flake CI gate WITHOUT
     * losing power against a real defect we draw up to 3 independent
     * batches and pass if ANY is in-band: P(3 healthy batches all in
     * the 0.27% tail) is about 2e-8, while a biased coin fails every
     * batch. (Previously used strict comparisons that also wrongly
     * rejected the inclusive boundary values 8910 and 9090.) */
    printf("dandelion fluff coin-flip ±3σ uniformity (10k)... ");
    {
        const int trials = 10000;
        const int attempts = 3;
        int last_count = -1;
        bool rng_ok = true;
        bool in_band = false;
        for (int a = 0; a < attempts && !in_band; a++) {
            int stem_count = 0;
            rng_ok = true;
            for (int i = 0; i < trials; i++) {
                bool stem;
                if (!dandelion_test_should_stem_coin(&stem)) {
                    rng_ok = false;
                    break;
                }
                if (stem) stem_count++;
            }
            if (!rng_ok)
                break;
            last_count = stem_count;
            if (stem_count >= 8910 && stem_count <= 9090)
                in_band = true;
        }
        if (rng_ok && in_band)
            printf("OK (%d/%d stem)\n", last_count, trials);
        else if (!rng_ok) {
            printf("FAIL (cryptographic RNG returned error)\n"); failures++;
        } else {
            printf("FAIL (%d/%d stem — %d batches all outside ±3σ of 9000)\n",
                   last_count, trials, attempts);
            failures++;
        }
    }

    return failures;
}
