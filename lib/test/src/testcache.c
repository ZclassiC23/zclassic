/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * testcache — content-addressed per-group test result cache (see testcache.h).
 *
 * The key for a group is SHA3-256 over: a domain tag; the compiled-in toolchain
 * fingerprint; the group name; and, for every file in the group's forward
 * (callee) input closure sorted by path, the file's path and its SHA3-256
 * content hash. A stored PASS record addressed by that key (in the .zvcs object
 * store) means the exact same inputs already passed. Only PASS is ever stored,
 * a truncated/unresolved closure or a denylisted external-input group is never
 * cacheable, and the cold-audit path re-verifies every hit against a fresh run.
 *
 * Fail-open: any internal error reports the group UNCACHEABLE (so it runs) or
 * silently skips a store — a cache miss only costs a re-run, never correctness. */

#include "test/testcache.h"

#include "codeindex/codeindex.h"
#include "vcs/vcs_object.h"
#include "crypto/sha3.h"
#include "platform/time_compat.h"
#include "util/safe_alloc.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The toolchain fingerprint is injected at compile time by the Makefile
 * (tools/dev/build-epoch-key.sh compiler-id). Without that -D we fall back to
 * the compiler's own version string so the module still binds SOMETHING to the
 * toolchain and always builds. */
#ifndef ZCL_TESTCACHE_TOOLKEY
#define ZCL_TESTCACHE_TOOLKEY __VERSION__
#endif

/* Largest input closure we will hold for one group. A closure larger than this
 * overflows codeindex_forward_closure's cap and comes back *truncated -> the
 * group is UNCACHEABLE, which is exactly what we want for a giant blast radius. */
#define TRC_MAX_CLOSURE 8192

/* ── on-disk verdict record (fixed 56 bytes, addressed BY the cache key) ── */
#define TRC_MAGIC "ZTCACHE1"      /* 8 bytes, no NUL */
#define TRC_STATUS_PASS 1u
struct trc_record {
    char    magic[8];
    uint8_t status;
    uint8_t rsvd[7];
    uint8_t key_echo[32];         /* self-check vs the lookup address */
    uint8_t generation_le[8];     /* store wall-clock stamp (observability) */
};

/* ── file-hash memo (path -> SHA3-256), open addressing, per-run ── */
struct trc_memo_ent {
    char   *path;         /* NULL == empty slot */
    uint8_t hash[32];
};
struct trc_memo {
    struct trc_memo_ent *slots;
    size_t cap;           /* power of two */
    size_t len;
};

struct testcache {
    struct codeindex *ci;
    char              root[4096];
    struct trc_memo   memo;
    char            (*closure)[256];   /* TRC_MAX_CLOSURE scratch rows */
};

static uint64_t trc_hash_str(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

static bool trc_memo_init(struct trc_memo *m, size_t cap)
{
    m->slots = zcl_calloc(cap, sizeof(*m->slots), "trc_memo");
    if (!m->slots)
        return false;
    m->cap = cap;
    m->len = 0;
    return true;
}

static void trc_memo_free(struct trc_memo *m)
{
    if (!m->slots)
        return;
    for (size_t i = 0; i < m->cap; i++)
        free(m->slots[i].path);
    free(m->slots);
    m->slots = NULL;
    m->cap = m->len = 0;
}

static bool trc_memo_grow(struct trc_memo *m)
{
    size_t ncap = m->cap * 2;
    struct trc_memo_ent *ns = zcl_calloc(ncap, sizeof(*ns), "trc_memo_grow");
    if (!ns)
        return false;
    for (size_t i = 0; i < m->cap; i++) {
        if (!m->slots[i].path)
            continue;
        size_t j = (size_t)trc_hash_str(m->slots[i].path) & (ncap - 1);
        while (ns[j].path)
            j = (j + 1) & (ncap - 1);
        ns[j] = m->slots[i];
    }
    free(m->slots);
    m->slots = ns;
    m->cap = ncap;
    return true;
}

/* SHA3-256 the bytes of <root>/<relpath> via a streaming read (no whole-file
 * buffer). Returns false (and logs) if the file cannot be opened/read. */
static bool trc_hash_file(const char *root, const char *relpath,
                          uint8_t out[32])
{
    char path[4200];
    int n = snprintf(path, sizeof(path), "%s/%s", root, relpath);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        ZCL_LOG_EMIT_AT(ZCL_LOG_WARN, "[testcache] path overflow: %s\n", relpath);
        return false;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ZCL_LOG_EMIT_AT(ZCL_LOG_WARN, "[testcache] open failed: %s\n", path);
        return false;
    }
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    unsigned char buf[65536];
    size_t got;
    bool ok = true;
    while ((got = fread(buf, 1, sizeof(buf), fp)) > 0)
        sha3_256_write(&ctx, buf, got);
    if (ferror(fp)) {
        ZCL_LOG_EMIT_AT(ZCL_LOG_WARN, "[testcache] read error: %s\n", path);
        ok = false;
    }
    fclose(fp);
    if (ok)
        sha3_256_finalize(&ctx, out);
    return ok;
}

/* Memoized content hash of one closure file. Returns false on read failure
 * (the caller then treats the whole group as UNCACHEABLE). */
static bool trc_file_hash(struct testcache *tc, const char *relpath,
                          uint8_t out[32])
{
    struct trc_memo *m = &tc->memo;
    if (m->len * 10 >= m->cap * 7 && !trc_memo_grow(m))
        return false;
    size_t j = (size_t)trc_hash_str(relpath) & (m->cap - 1);
    while (m->slots[j].path) {
        if (strcmp(m->slots[j].path, relpath) == 0) {
            memcpy(out, m->slots[j].hash, 32);
            return true;
        }
        j = (j + 1) & (m->cap - 1);
    }
    uint8_t h[32];
    if (!trc_hash_file(tc->root, relpath, h))
        return false;
    char *dup = zcl_strdup(relpath, "trc_memo_key");
    if (!dup)
        return false;
    m->slots[j].path = dup;
    memcpy(m->slots[j].hash, h, 32);
    m->len++;
    memcpy(out, h, 32);
    return true;
}

/* Groups whose verdict depends on inputs OUTSIDE their source closure — on-disk
 * fixtures, the live node DB, an external zclassicd, ~/.zcash-params, built
 * binary artifacts, or a legacy datadir. These are NEVER cached (always run).
 * The list is intentionally conservative; the cold-audit path is the net that
 * catches anything mis-classified. `name` may carry the test_/spec_ prefix. */
static bool group_reads_external_inputs(const char *name)
{
    if (strncmp(name, "test_", 5) == 0 || strncmp(name, "spec_", 5) == 0)
        name += 5;
    static const char *const ext[] = {
        "make_lint_gates",                /* plants fixtures + compiles the tree */
        "consensus_state_snapshot",       /* fd-dup + atomic bundle publish to a
                                           * datadir; load-flaky, so its PASS is
                                           * not reliably reproducible (surfaced
                                           * by --cold-audit) */
        "dev_platform",                   /* reads lib/test/fixtures source */
        "chaos_harness",                  /* reads tests/fixtures block files */
        "explorer",                       /* live node DB coupling */
        "soak",                           /* soak_harness/soak_attestation: datadir */
        "oracle",                         /* zclassicd_oracle/oracle_policy: ext node */
        "binary_staleness",               /* reads built binary artifacts */
        "binary_ab_fallback",
        "snark_kat",                      /* ~/.zcash-params */
        "sapling_prover_rng_determinism",
        "simnet_sapling_shielded_send",
        "simnet_zmsg_onchain",
        "no_hardcoded_home",              /* scans tree + env for home usage */
        "chainstate_legacy",              /* legacy datadir reader */
        "importblockindex",               /* legacy datadir import */
        "e2e_cold_start",                 /* datadir */
        "offline_datadir",                /* datadir */
        "load_verify_boot",               /* datadir */
        "coldimport_restart_fragility",
    };
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
        if (strstr(name, ext[i]))
            return true;
    return false;
}

static void trc_put_u32le(unsigned char b[4], uint32_t v)
{
    b[0] = (unsigned char)(v);
    b[1] = (unsigned char)(v >> 8);
    b[2] = (unsigned char)(v >> 16);
    b[3] = (unsigned char)(v >> 24);
}

/* Fold the closure into the SHA3 key. Files are already sorted by
 * codeindex_forward_closure. Returns false on a file-read failure. */
static bool trc_compute_key(struct testcache *tc, const char *group_name,
                            int n_closure, uint8_t out_key[32])
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);

    static const char DOMAIN[] = "zcl.testcache.key.v1";
    sha3_256_write(&ctx, (const unsigned char *)DOMAIN, sizeof(DOMAIN)); /* +NUL */

    const char *tk = ZCL_TESTCACHE_TOOLKEY;
    sha3_256_write(&ctx, (const unsigned char *)tk, strlen(tk) + 1);
    sha3_256_write(&ctx, (const unsigned char *)group_name,
                   strlen(group_name) + 1);

    unsigned char le[4];
    trc_put_u32le(le, (uint32_t)n_closure);
    sha3_256_write(&ctx, le, 4);

    for (int i = 0; i < n_closure; i++) {
        const char *p = tc->closure[i];
        uint8_t fh[32];
        if (!trc_file_hash(tc, p, fh))
            return false;
        sha3_256_write(&ctx, (const unsigned char *)p, strlen(p) + 1);
        sha3_256_write(&ctx, fh, 32);
    }

    /* Reserved fixture-count slot (0 today: fixture-reading groups are
     * denylisted UNCACHEABLE). Keeps the preimage layout stable for a future
     * per-group declared-fixture extension without a key-version bump. */
    trc_put_u32le(le, 0);
    sha3_256_write(&ctx, le, 4);

    sha3_256_finalize(&ctx, out_key);
    return true;
}

struct testcache *testcache_open(const char *repo_root)
{
    const char *root = repo_root;
    if (!root || !root[0]) {
        const char *env = getenv("ZCL_DEV_SOURCE_ROOT");
        root = (env && env[0]) ? env : ".";
    }

    struct testcache *tc = zcl_calloc(1, sizeof(*tc), "testcache");
    if (!tc)
        LOG_NULL("testcache", "alloc handle failed");

    int n = snprintf(tc->root, sizeof(tc->root), "%s", root);
    if (n < 0 || (size_t)n >= sizeof(tc->root)) {
        free(tc);
        LOG_NULL("testcache", "root path overflow");
    }

    if (!vcs_object_store_init(tc->root)) {
        free(tc);
        LOG_NULL("testcache", "vcs object store init failed under %s", root);
    }

    tc->closure = zcl_malloc(sizeof(*tc->closure) * TRC_MAX_CLOSURE,
                             "testcache_closure");
    if (!tc->closure) {
        free(tc);
        LOG_NULL("testcache", "closure scratch alloc failed");
    }

    if (!trc_memo_init(&tc->memo, 4096)) {
        free(tc->closure);
        free(tc);
        LOG_NULL("testcache", "memo init failed");
    }

    tc->ci = codeindex_open(tc->root);
    if (!tc->ci) {
        trc_memo_free(&tc->memo);
        free(tc->closure);
        free(tc);
        LOG_NULL("testcache", "codeindex_open failed under %s", root);
    }
    return tc;
}

void testcache_close(struct testcache *tc)
{
    if (!tc)
        return;
    if (tc->ci)
        codeindex_close(tc->ci);
    trc_memo_free(&tc->memo);
    free(tc->closure);
    free(tc);
}

const char *testcache_toolkey(void)
{
    return ZCL_TESTCACHE_TOOLKEY;
}

/* Populate *out for group_name. Fail-open: any failure => uncacheable. */
void testcache_probe_group(struct testcache *tc, const char *group_name,
                           struct testcache_probe *out)
{
    memset(out, 0, sizeof(*out));
    if (!tc || !tc->ci || !group_name || !group_name[0]) {
        snprintf(out->reason, sizeof(out->reason), "no cache handle");
        return;
    }

    if (group_reads_external_inputs(group_name)) {
        snprintf(out->reason, sizeof(out->reason), "external-input denylist");
        return;
    }

    bool truncated = false, root_found = false;
    int nc = codeindex_forward_closure(tc->ci, group_name, tc->closure,
                                       TRC_MAX_CLOSURE, &truncated, &root_found);
    if (nc < 0) {
        snprintf(out->reason, sizeof(out->reason), "closure query error");
        return;
    }
    if (!root_found) {
        snprintf(out->reason, sizeof(out->reason), "entry symbol unresolved");
        return;
    }
    if (truncated) {
        snprintf(out->reason, sizeof(out->reason),
                 "closure truncated (%d files, cap hit)", nc);
        return;
    }
    if (nc == 0) {
        snprintf(out->reason, sizeof(out->reason), "empty closure");
        return;
    }

    if (!trc_compute_key(tc, group_name, nc, out->key)) {
        snprintf(out->reason, sizeof(out->reason), "input file unreadable");
        return;
    }

    out->cacheable = true;
    out->n_closure = nc;
    snprintf(out->reason, sizeof(out->reason), "%d input files", nc);

    /* Is there a stored PASS at this exact key? Probe existence first (a quiet
     * access() — a MISS is the common, non-error case) before the verifying
     * load, so a cold cache never spams the log with "object not found". */
    if (vcs_object_has(tc->root, out->key)) {
        uint8_t *buf = NULL;
        size_t len = 0;
        if (vcs_object_load_raw(tc->root, out->key, &buf, &len) == 0 && buf) {
            if (len >= sizeof(struct trc_record)) {
                const struct trc_record *r = (const struct trc_record *)buf;
                if (memcmp(r->magic, TRC_MAGIC, 8) == 0 &&
                    r->status == TRC_STATUS_PASS &&
                    memcmp(r->key_echo, out->key, 32) == 0)
                    out->hit = true;
            }
            free(buf);
        }
    }
}

void testcache_store_pass(struct testcache *tc, const uint8_t key[32])
{
    if (!tc || !key)
        return;
    struct trc_record r;
    memset(&r, 0, sizeof(r));
    memcpy(r.magic, TRC_MAGIC, 8);
    r.status = TRC_STATUS_PASS;
    memcpy(r.key_echo, key, 32);
    /* Best-effort observability stamp; correctness never depends on it. */
    uint64_t gen = (uint64_t)platform_time_wall_time_t();
    for (int i = 0; i < 8; i++)
        r.generation_le[i] = (uint8_t)(gen >> (8 * i));
    if (!vcs_object_put_addressed(tc->root, key,
                                  (const uint8_t *)&r, sizeof(r)))
        ZCL_LOG_EMIT_AT(ZCL_LOG_WARN,
                        "[testcache] store_pass put_addressed failed\n");
}

void testcache_dump_group(struct testcache *tc, const char *group_name)
{
    if (!tc || !group_name) {
        printf("testcache: no handle/group\n");
        return;
    }
    struct testcache_probe p;
    testcache_probe_group(tc, group_name, &p);
    printf("testcache dump: group=%s\n", group_name);
    printf("  toolkey=%s\n", testcache_toolkey());
    printf("  cacheable=%s  hit=%s  n_closure=%d  reason=%s\n",
           p.cacheable ? "yes" : "no", p.hit ? "yes" : "no",
           p.n_closure, p.reason);
    if (p.cacheable) {
        char kh[65];
        for (int i = 0; i < 32; i++)
            snprintf(kh + i * 2, 3, "%02x", p.key[i]);
        printf("  key=%s\n", kh);
        /* Recompute the closure for the listing (probe consumed tc->closure). */
        bool truncated = false, root_found = false;
        int nc = codeindex_forward_closure(tc->ci, group_name, tc->closure,
                                           TRC_MAX_CLOSURE, &truncated,
                                           &root_found);
        printf("  closure (%d files):\n", nc);
        for (int i = 0; i < nc; i++)
            printf("    %s\n", tc->closure[i]);
    }
}
