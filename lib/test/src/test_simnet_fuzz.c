/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_simnet_fuzz — deterministic seed-fuzzer over the REAL multi-node
 * simulator: `simnet_cluster` + `simnet_chain` drive the real
 * `connect_block()` / `disconnect_block()` paths and the real chainwork
 * fork-choice. This is the multi-node analogue of `test_reducer_stage_fuzz`
 * (which fuzzes the eight reducer stages in isolation): here we fuzz the
 * whole cluster and assert the network-level convergence invariants.
 *
 * The promise this test defends
 * -----------------------------
 * A set of honest nodes that all eventually see all blocks MUST converge:
 * one active tip hash, and a byte-identical UTXO set. Fork choice is a pure
 * function of the block set (chainwork, then a deterministic first-seen /
 * hash tie-break), so any random mint/relay/withhold schedule that ends with
 * every node holding every branch must land on the same answer everywhere.
 * If two honest nodes ever disagree on tip-or-coins after a full heal, the
 * fork-choice / disconnect-connect reorg machinery has a real bug.
 *
 * The scenario derived from each seed
 * -----------------------------------
 * Per seed we derive, deterministically:
 *   - node count (2..4);
 *   - a sequence of rounds; in each round every node mints a random-length
 *     branch (0..3 blocks) and a random subset of nodes "relays" (broadcasts
 *     its whole chain) while the rest stay silent — a send-side partition.
 *     A silent miner accumulates a private branch that competes on heal;
 *   - a FINAL HEAL round where every node broadcasts its complete chain and
 *     we deliver to quiescence, forcing every node to see every branch.
 * Then we assert the invariants:
 *   (1) every node shares node 0's tip hash;
 *   (2) `simnet_cluster_coins_digest` is identical across all nodes;
 *   (3) determinism — re-running the SAME seed yields the identical final
 *       tip, coins digest, and delivery fingerprint;
 *   (4) no crash / no leak (the whole battery runs under the harness/ASan).
 *
 * Adaptation to the public cluster API (honest, not hidden)
 * ---------------------------------------------------------
 * `simnet_cluster`'s public surface mints coinbase-only blocks and relays a
 * block to ALL peers (there is no per-link delivery filter and no
 * arbitrary-block inject). So:
 *   - "per-link partition" is modeled as SEND-side withholding: a node mines
 *     in isolation and withholds its branch until it relays — which is
 *     exactly what produces the competing-fork / reorg-on-heal dynamic the
 *     convergence invariant targets;
 *   - byzantine block rejection ("every rejected block was rejected with a
 *     NAMED reason") is exercised through the `simnet_byzantine` builders'
 *     own designed harness (`simnet_byzantine_run_connect_case` /
 *     `_run_header_case`) in the SAME seed loop, since the cluster has no
 *     surface to inject a would-be-rejected block into a node.
 *
 * Determinism / seed-replay
 * -------------------------
 * Every scheduling choice comes from a splitmix64 stream seeded from a single
 * 64-bit seed (independent of, but derived from, the same seed that drives
 * the cluster's internal seed-tape RNG). The base seed is printed once; on
 * ANY failure the failing per-seed value is printed AND a replay capsule is
 * written via the postmortem/seed-tape API so the exact case is recoverable.
 *
 * Budget / nightly depth
 * ----------------------
 * The fast pool runs a handful of seeds in well under 5 s. Set
 * `ZCL_SIMNET_FUZZ_SEEDS=<n>` for a deeper nightly sweep, and
 * `ZCL_SIMNET_FUZZ_SEED=<hex|dec>` to pin the base seed for replay.
 */

#include "test/test_helpers.h"

#include "coins/utxo_commitment.h"
#include "core/uint256.h"
#include "sim/postmortem.h"
#include "sim/seed_tape.h"
#include "sim/simnet_byzantine.h"
#include "sim/simnet_cluster.h"
#include "util/blocker.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── harness scaffolding ───────────────────────────────────────────── */

static int g_failures;
static uint64_t g_seed;  /* the top-level seed of the scenario in flight */

#define FZ_CHECK(name, expr) do {                                        \
    if ((expr)) {                                                        \
        /* quiet on success to keep the suite log readable */            \
    } else {                                                             \
        printf("simnet_fuzz: %s... FAIL  (replay seed=0x%016llx)\n",     \
               (name), (unsigned long long)g_seed);                      \
        g_failures++;                                                    \
    }                                                                    \
} while (0)

/* splitmix64 — tiny, deterministic, well-distributed. Self-contained so the
 * seed is the single source of truth for replay (same pattern as
 * test_reducer_stage_fuzz.c). */
static uint64_t sm_next(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static uint32_t sm_u32(uint64_t *s) { return (uint32_t)(sm_next(s) >> 32); }
static int sm_range(uint64_t *s, int lo, int hi)  /* inclusive */
{
    if (hi <= lo) return lo;
    return lo + (int)(sm_u32(s) % (uint32_t)(hi - lo + 1));
}

static int mkdir_p(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

/* ── replay capsule on failure ─────────────────────────────────────── */

/* Persist a seed-tape replay capsule for a failing seed. Best-effort: a
 * capsule-write failure must never mask the underlying test failure, so all
 * paths here just return quietly. No cluster/tape is installed at call time
 * (we call this after every cluster has been freed), so opening a fresh tape
 * from the seed does not collide with the platform rng/clock hooks. */
static void fz_save_capsule(uint64_t seed)
{
    char dir[256];
    snprintf(dir, sizeof(dir), "./test-tmp/simnet_fuzz_cap_%d", (int)getpid());
    if (mkdir_p("./test-tmp") != 0) return;
    if (mkdir_p(dir) != 0) return;

    seed_tape_t *tape = seed_tape_open(seed, 0);
    if (!tape) return;

    struct postmortem_capture_opts opts = {
        .dir = dir,
        .tape = tape,
        .crash_signal = 0,
        .crash_unix = 0,
        .reason = "simnet_fuzz-invariant",
        .log_path = NULL,
    };
    char path[512];
    int rc = postmortem_capture_write(&opts, path, sizeof(path));
    if (rc == 0)
        printf("simnet_fuzz: replay capsule for seed=0x%016llx -> %s\n",
               (unsigned long long)seed, path);

    seed_tape_close(tape);
}

/* ── the scenario runner ───────────────────────────────────────────── */

#define FZ_MAX_NODES  4
#define FZ_MAX_HASHES 64   /* rounds(<=5) * mints/round(<=3) = <=15 per node */

struct fz_node_chain {
    struct uint256 hashes[FZ_MAX_HASHES];
    size_t count;
};

static bool fz_relay_full_chain(struct simnet_cluster *cluster, size_t node_id,
                                const struct fz_node_chain *chain)
{
    /* Broadcast the node's entire chain in mint order so a parent is always
     * enqueued before its child; re-broadcasting an already-delivered block
     * is a harmless no-op at delivery time. This is what keeps the delivery
     * queue from wedging on an orphan (child with no queued parent). */
    for (size_t i = 0; i < chain->count; i++) {
        if (!simnet_cluster_broadcast(cluster, node_id, &chain->hashes[i]))
            return false;
    }
    return true;
}

/* Run one full multi-node scenario derived deterministically from `seed`.
 * Returns true iff the harness itself ran to completion (no mint/relay/deliver
 * error). On success:
 *   - *out_tip / *out_digest / *out_fp = node 0's final tip, coins digest,
 *     and the cluster delivery fingerprint;
 *   - *out_converged = whether EVERY node shares node 0's tip hash AND coins
 *     digest (the invariant under test). */
static bool fz_run_scenario(uint64_t seed,
                            struct uint256 *out_tip,
                            struct utxo_commitment *out_digest,
                            uint64_t *out_fp,
                            bool *out_converged)
{
    uint64_t rng = seed;                       /* schedule stream */
    size_t node_count = (size_t)sm_range(&rng, 2, FZ_MAX_NODES);

    struct simnet_cluster *cluster = simnet_cluster_init(node_count, seed);
    if (!cluster)
        return false;

    struct fz_node_chain chain[FZ_MAX_NODES];
    memset(chain, 0, sizeof(chain));

    bool ok = true;
    int rounds = sm_range(&rng, 2, 5);

    for (int r = 0; r < rounds && ok; r++) {
        /* Each node extends its own branch by a random amount this round. */
        for (size_t n = 0; n < node_count && ok; n++) {
            int mints = sm_range(&rng, 0, 3);
            for (int m = 0; m < mints; m++) {
                if (chain[n].count >= FZ_MAX_HASHES)
                    break;
                struct uint256 h;
                if (!simnet_cluster_mint_on(cluster, n, &h)) {
                    ok = false;
                    break;
                }
                chain[n].hashes[chain[n].count++] = h;
            }
        }
        /* Send-side partition: a random subset of nodes relays this round;
         * the rest withhold (private branch that competes on heal). */
        for (size_t n = 0; n < node_count && ok; n++) {
            bool relays = sm_range(&rng, 0, 3) != 0;  /* ~75% relay */
            if (relays && chain[n].count > 0) {
                if (!fz_relay_full_chain(cluster, n, &chain[n]))
                    ok = false;
            }
        }
        if (ok && !simnet_cluster_deliver_pending(cluster))
            ok = false;
    }

    /* FINAL HEAL: every node relays its complete chain, then deliver to
     * quiescence — after this every node has seen every branch. */
    for (size_t n = 0; n < node_count && ok; n++) {
        if (chain[n].count > 0 && !fz_relay_full_chain(cluster, n, &chain[n]))
            ok = false;
    }
    if (ok && !simnet_cluster_deliver_pending(cluster))
        ok = false;

    bool converged = false;
    if (ok) {
        ok = simnet_cluster_tip_hash(cluster, 0, out_tip) &&
             simnet_cluster_coins_digest(cluster, 0, out_digest);
        if (ok) {
            converged = true;
            for (size_t n = 1; n < node_count; n++) {
                struct uint256 tip_n;
                struct utxo_commitment dig_n;
                if (!simnet_cluster_tip_hash(cluster, n, &tip_n) ||
                    !simnet_cluster_coins_digest(cluster, n, &dig_n) ||
                    !uint256_eq(out_tip, &tip_n) ||
                    !utxo_commitment_equal(out_digest, &dig_n)) {
                    converged = false;
                    break;
                }
            }
        }
    }

    *out_fp = simnet_cluster_delivery_fingerprint(cluster);
    *out_converged = converged;
    simnet_cluster_free(cluster);
    return ok;
}

/* ── byzantine axis: every rejected block carries a NAMED reason ─────── */

/* Reuse the simnet_byzantine builders. For a random subset of the adversarial
 * classes, run each through its designed harness and assert the rejection is
 * NAMED: `observation_ok` requires rejected==true, a non-empty reject_reason,
 * a non-empty typed blocker_id, the expected blocker class, an unchanged tip,
 * and that the next honest block is still accepted. Runs after every cluster
 * is freed, so no seed-tape is installed over the platform rng/clock. */
static void fz_byzantine_named_reason(uint64_t *rng)
{
    for (int k = 0; k < SIMNET_BYZ_CLASS_COUNT; k++) {
        if (sm_range(rng, 0, 1) == 0)
            continue;                          /* random subset */

        enum simnet_byzantine_class kind = (enum simnet_byzantine_class)k;
        blocker_reset_for_testing();
        blocker_set_clock_for_testing(1000000 + (int64_t)k * 100000);

        struct simnet_byzantine_observation obs;
        bool ran = simnet_byzantine_class_tier(kind) ==
                       SIMNET_BYZ_TIER_CONNECT_BLOCK
                       ? simnet_byzantine_run_connect_case(kind, &obs)
                       : simnet_byzantine_run_header_case(kind, &obs);

        char lbl[96];
        snprintf(lbl, sizeof(lbl), "byzantine[%s] ran",
                 simnet_byzantine_class_name(kind));
        FZ_CHECK(lbl, ran);
        if (!ran)
            continue;

        snprintf(lbl, sizeof(lbl), "byzantine[%s] rejected with NAMED reason",
                 simnet_byzantine_class_name(kind));
        FZ_CHECK(lbl, obs.rejected && obs.reject_reason[0] != '\0' &&
                      obs.blocker_id[0] != '\0');

        snprintf(lbl, sizeof(lbl), "byzantine[%s] invariant holds",
                 simnet_byzantine_class_name(kind));
        FZ_CHECK(lbl, obs.invariant_ok);
    }
}

/* ── teeth: the convergence comparator must be able to FAIL ──────────── */

/* Build a genuine divergence — mint on node 0 and never relay it — and
 * confirm node 0 and node 1 do NOT share a tip. If this ever "passes" (tips
 * equal without relay) the tip comparator used above is hollow. */
static void fz_teeth(void)
{
    struct simnet_cluster *c = simnet_cluster_init(2, 0xFEEDFACEC0FFEEULL);
    FZ_CHECK("teeth: divergence cluster inits", c != NULL);
    if (!c)
        return;

    struct uint256 h;
    struct uint256 t0;
    struct uint256 t1;
    bool got = simnet_cluster_mint_on(c, 0, &h) &&   /* no broadcast/deliver */
               simnet_cluster_tip_hash(c, 0, &t0) &&
               simnet_cluster_tip_hash(c, 1, &t1);
    FZ_CHECK("teeth: un-relayed mint diverges the tips",
             got && !uint256_eq(&t0, &t1));

    simnet_cluster_free(c);
}

/* ── entry point ───────────────────────────────────────────────────── */

int test_simnet_fuzz(void)
{
    printf("\n=== simnet_fuzz — deterministic multi-node cluster fuzz ===\n");
    g_failures = 0;

    if (!blocker_module_init()) {
        printf("simnet_fuzz: blocker_module_init failed\n");
        return 1;
    }

    /* Base seed: env override for replay, else a fixed default so the fast
     * run is deterministic. */
    uint64_t base_seed = 0x51C0FFEE5EED0001ULL;
    const char *seed_env = getenv("ZCL_SIMNET_FUZZ_SEED");
    if (seed_env && *seed_env)
        base_seed = strtoull(seed_env, NULL, 0);

    /* Seed count: a handful in the fast pool (<5 s); deeper for nightly. */
    int seeds = 4;
    const char *n_env = getenv("ZCL_SIMNET_FUZZ_SEEDS");
    if (n_env && *n_env) {
        long v = strtol(n_env, NULL, 0);
        if (v > 0 && v <= 100000)
            seeds = (int)v;
    }

    printf("simnet_fuzz: base_seed=0x%016llx seeds=%d "
           "(set ZCL_SIMNET_FUZZ_SEED / ZCL_SIMNET_FUZZ_SEEDS to override)\n",
           (unsigned long long)base_seed, seeds);

    fz_teeth();

    for (int i = 0; i < seeds; i++) {
        uint64_t seed = base_seed ^ ((uint64_t)i * 0x100000001B3ULL);
        seed = sm_next(&seed);       /* spread consecutive base seeds apart */
        g_seed = seed;

        int fails_before = g_failures;

        /* Determinism: the SAME seed must yield the identical final tip,
         * coins digest, and delivery fingerprint. Run twice. */
        struct uint256 tip_a;
        struct uint256 tip_b;
        struct utxo_commitment dig_a;
        struct utxo_commitment dig_b;
        uint64_t fp_a = 0;
        uint64_t fp_b = 0;
        bool conv_a = false;
        bool conv_b = false;

        bool ran_a = fz_run_scenario(seed, &tip_a, &dig_a, &fp_a, &conv_a);
        bool ran_b = fz_run_scenario(seed, &tip_b, &dig_b, &fp_b, &conv_b);

        FZ_CHECK("scenario ran to completion", ran_a && ran_b);

        if (ran_a && ran_b) {
            /* (1)+(2) all nodes converge to one tip and one coins digest. */
            FZ_CHECK("all nodes converge (tip + coins digest)",
                     conv_a && conv_b);
            /* (3) deterministic replay. */
            FZ_CHECK("same seed => identical tip",
                     uint256_eq(&tip_a, &tip_b));
            FZ_CHECK("same seed => identical coins digest",
                     utxo_commitment_equal(&dig_a, &dig_b));
            FZ_CHECK("same seed => identical delivery fingerprint",
                     fp_a == fp_b);
        }

        /* Byzantine axis: every rejected block names its reason. */
        uint64_t byz_rng = seed ^ 0xB47A511E0000ULL;
        fz_byzantine_named_reason(&byz_rng);

        /* On any failure attributable to this seed, drop a replay capsule. */
        if (g_failures > fails_before)
            fz_save_capsule(seed);
    }

    printf("simnet_fuzz: %d failures (%d seeds)\n", g_failures, seeds);
    return g_failures;
}
