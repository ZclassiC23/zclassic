/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * testcache — the content-addressed per-group test result cache.
 *
 * Bazel-style: a test GROUP is SKIPPED when its exact transitive INPUT closure
 * is byte-identical to the last time that group PASSED. The input closure is
 * the forward (callee) closure of the group's entry symbol — every in-tree
 * source file whose bytes can change the group's verdict — plus the toolchain
 * fingerprint. The key is SHA3-256 over that closure; a stored PASS at that key
 * (in the .zvcs object store) is a proof the group would pass again.
 *
 * SOUNDNESS is the whole point. A cached SKIP must be provably equivalent to a
 * fresh PASS, so this module NEVER caches a group whose real inputs it cannot
 * bound:
 *   - the forward closure truncated (a cap/fan-out/depth limit) -> UNCACHEABLE
 *   - the entry symbol does not resolve in the code index      -> UNCACHEABLE
 *   - the group is on the external-input denylist (reads fixtures/live DB/
 *     network/params beyond its source closure)                 -> UNCACHEABLE
 *   - only PASS verdicts are ever stored (a fail is never cached)
 * An UNCACHEABLE group ALWAYS runs. The residual assumption (the call graph
 * captures a test's dependency edges by name; an indirect/function-pointer edge
 * is invisible to source scanning) is backed by the MANDATORY cold-audit path:
 * the canonical push gate runs COLD (cache disabled) and the --cold-audit mode
 * re-runs every group fresh and asserts every cache HIT would have matched the
 * fresh verdict. The cache accelerates the inner dev loop; it never gates a push.
 *
 * This is a TEST-BINARY-ONLY module (lib/test/), never linked into the node. */

#ifndef ZCL_TEST_TESTCACHE_H
#define ZCL_TEST_TESTCACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Opaque handle: open once per run in the parent, before any fork. */
struct testcache;

/* Open the cache for the source tree rooted at repo_root (NULL selects
 * ZCL_DEV_SOURCE_ROOT, else "."). Opens/rebuilds the code index and ensures the
 * .zvcs object store exists. Returns NULL on hard failure — the caller then
 * runs every group (fail-safe: no cache, never a wrong skip). */
struct testcache *testcache_open(const char *repo_root);
void testcache_close(struct testcache *tc);

/* The per-group cache decision. */
struct testcache_probe {
    bool    cacheable;    /* false => the group MUST run this time */
    bool    hit;          /* true  => a stored PASS exists at this exact key */
    uint8_t key[32];      /* the content-addressed key (valid iff cacheable) */
    int     n_closure;    /* number of input files hashed (diagnostic) */
    char    reason[96];   /* why uncacheable, or a short closure note */
};

/* Compute group_name's key + cacheability + whether a stored PASS exists.
 * group_name is the registry name and the entry symbol both ("test_<x>" /
 * "spec_<x>"). Never aborts: on ANY internal failure the group is reported
 * UNCACHEABLE (fail-safe). *out is always fully populated. */
void testcache_probe_group(struct testcache *tc, const char *group_name,
                           struct testcache_probe *out);

/* Store a PASS verdict at key[32] (best effort; a store failure is ignored —
 * it only costs a future re-run, never correctness). Pass the key from a
 * prior cacheable probe of a group that then ran and PASSED. */
void testcache_store_pass(struct testcache *tc, const uint8_t key[32]);

/* Diagnostic: print group_name's closure file list, key, and cacheability to
 * stdout. Drives the ZCL_TEST_CACHE_DUMP=<group> operator surface and the
 * soundness proofs. */
void testcache_dump_group(struct testcache *tc, const char *group_name);

/* The compiled-in toolchain fingerprint folded into every key (a compiler/
 * flags change busts the whole cache). Exposed for the dump surface. */
const char *testcache_toolkey(void);

#endif /* ZCL_TEST_TESTCACHE_H */
