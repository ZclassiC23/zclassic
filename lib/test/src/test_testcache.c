/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_testcache — the content-addressed per-group test cache (lib/test/src/
 * testcache.c). Drives the module against a tiny fixture code index + .zvcs
 * store under ./test-tmp/ (project no-/tmp convention), asserting the four
 * properties the whole design rests on:
 *
 *   1. A group with a bounded, resolvable forward closure is CACHEABLE; a fresh
 *      key has no stored PASS (miss); after store_pass the same key HITS.
 *   2. SOUNDNESS — editing any file IN the closure (a callee body OR an included
 *      header) changes the key, so the old stored PASS no longer hits.
 *   3. SELECTIVITY + persistence — editing a file OUTSIDE the closure leaves the
 *      key unchanged, so the stored PASS still hits across a reopen.
 *   4. UNCACHEABLE cases — an external-input denylisted group and an unresolved
 *      entry symbol are both reported uncacheable (=> they always run).
 *
 * The fixture models a callee chain test_demo_entry -> tc_mid -> tc_leaf plus an
 * unrelated tc_other, so the forward closure is exactly {top, mid, leaf, header}
 * and never tc_other. */

#include "test/test_helpers.h"
#include "test/testcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TC_FIX "test-tmp/tc_cache_fix"

#define TC_CHECK(name, expr) do {                                    \
    if (expr) { printf("  testcache: %s... OK\n", (name)); }         \
    else { printf("  testcache: %s... FAIL\n", (name)); failures++; }\
} while (0)

/* Write <base>/<rel>, creating parent dirs (same idiom as the sibling tests). */
static bool mk_write(const char *base, const char *rel, const char *content)
{
    char full[4096];
    snprintf(full, sizeof(full), "%s/%s", base, rel);
    /* create every parent component, including the fixture root itself */
    for (char *p = full + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(full, 0755); *p = '/'; }
    }
    FILE *f = fopen(full, "wb");
    if (!f) return false;
    size_t n = content ? strlen(content) : 0;
    if (n && fwrite(content, 1, n, f) != n) { fclose(f); return false; }
    fclose(f);
    return true;
}

static const char *TC_TOP =
    "/* lib/net/src/tc_top.c — the group entry point. */\n"
    "#include \"net/tc.h\"\n"
    "int test_demo_entry(void)\n"
    "{\n"
    "    return tc_mid() + 1;\n"
    "}\n";

static const char *TC_MID =
    "/* lib/net/src/tc_mid.c — middle of the callee chain. */\n"
    "#include \"net/tc.h\"\n"
    "int tc_mid(void)\n"
    "{\n"
    "    return tc_leaf() * 2;\n"
    "}\n";

static const char *TC_LEAF_A =
    "/* lib/net/src/tc_leaf.c — the leaf (pristine). */\n"
    "#include \"net/tc.h\"\n"
    "int tc_leaf(void)\n"
    "{\n"
    "    return 7;\n"
    "}\n";

static const char *TC_LEAF_B =
    "/* lib/net/src/tc_leaf.c — the leaf (edited body). */\n"
    "#include \"net/tc.h\"\n"
    "int tc_leaf(void)\n"
    "{\n"
    "    return 4242;\n"
    "}\n";

static const char *TC_OTHER_A =
    "/* lib/net/src/tc_other.c — NOT reachable from the entry (pristine). */\n"
    "#include \"net/tc.h\"\n"
    "int tc_other(void)\n"
    "{\n"
    "    return 99;\n"
    "}\n";

static const char *TC_OTHER_B =
    "/* lib/net/src/tc_other.c — NOT reachable from the entry (edited). */\n"
    "#include \"net/tc.h\"\n"
    "int tc_other(void)\n"
    "{\n"
    "    return 123456;\n"
    "}\n";

static const char *TC_H_A =
    "/* lib/net/include/net/tc.h — fixture header (pristine). */\n"
    "#ifndef NET_TC_H\n"
    "#define NET_TC_H\n"
    "int test_demo_entry(void);\n"
    "int tc_mid(void);\n"
    "int tc_leaf(void);\n"
    "int tc_other(void);\n"
    "#endif\n";

static const char *TC_H_B =
    "/* lib/net/include/net/tc.h — fixture header (edited comment). */\n"
    "#ifndef NET_TC_H\n"
    "#define NET_TC_H\n"
    "/* an added line that changes the header's content hash */\n"
    "int test_demo_entry(void);\n"
    "int tc_mid(void);\n"
    "int tc_leaf(void);\n"
    "int tc_other(void);\n"
    "#endif\n";

/* Write the fixture with the given leaf/other/header variants + depfiles so the
 * include closure resolves. */
static bool write_fixture(const char *leaf, const char *other, const char *hdr)
{
    return mk_write(TC_FIX, "lib/net/src/tc_top.c", TC_TOP) &&
           mk_write(TC_FIX, "lib/net/src/tc_mid.c", TC_MID) &&
           mk_write(TC_FIX, "lib/net/src/tc_leaf.c", leaf) &&
           mk_write(TC_FIX, "lib/net/src/tc_other.c", other) &&
           mk_write(TC_FIX, "lib/net/include/net/tc.h", hdr) &&
           mk_write(TC_FIX, "build/obj/tc_top.d",
                    "build/obj/tc_top.o: lib/net/src/tc_top.c "
                    "lib/net/include/net/tc.h\n") &&
           mk_write(TC_FIX, "build/obj/tc_mid.d",
                    "build/obj/tc_mid.o: lib/net/src/tc_mid.c "
                    "lib/net/include/net/tc.h\n") &&
           mk_write(TC_FIX, "build/obj/tc_leaf.d",
                    "build/obj/tc_leaf.o: lib/net/src/tc_leaf.c "
                    "lib/net/include/net/tc.h\n") &&
           mk_write(TC_FIX, "build/obj/tc_other.d",
                    "build/obj/tc_other.o: lib/net/src/tc_other.c "
                    "lib/net/include/net/tc.h\n");
}

int test_testcache(void)
{
    int failures = 0;
    system("rm -rf " TC_FIX);

    /* ── Phase A: cacheable, miss, store, hit ── */
    uint8_t keyA[32];
    bool have_keyA = false;
    TC_CHECK("fixture writes", write_fixture(TC_LEAF_A, TC_OTHER_A, TC_H_A));
    {
        struct testcache *tc = testcache_open(TC_FIX);
        TC_CHECK("testcache_open succeeds", tc != NULL);
        if (tc) {
            struct testcache_probe p;
            testcache_probe_group(tc, "test_demo_entry", &p);
            TC_CHECK("entry is cacheable", p.cacheable);
            TC_CHECK("closure is the 4 reachable files (top,mid,leaf,header)",
                     p.n_closure == 4);
            TC_CHECK("fresh key is a MISS", p.cacheable && !p.hit);
            if (p.cacheable) { memcpy(keyA, p.key, 32); have_keyA = true; }

            testcache_store_pass(tc, p.key);

            struct testcache_probe p2;
            testcache_probe_group(tc, "test_demo_entry", &p2);
            TC_CHECK("same key HITS after store_pass", p2.cacheable && p2.hit);
            TC_CHECK("key is stable across identical probes",
                     have_keyA && memcmp(keyA, p2.key, 32) == 0);
            testcache_close(tc);
        }
    }

    /* ── Phase B: SOUNDNESS — edit a CLOSURE-MEMBER body => key changes, miss ── */
    TC_CHECK("rewrite leaf body", write_fixture(TC_LEAF_B, TC_OTHER_A, TC_H_A));
    {
        struct testcache *tc = testcache_open(TC_FIX);
        if (tc) {
            struct testcache_probe p;
            testcache_probe_group(tc, "test_demo_entry", &p);
            TC_CHECK("still cacheable after leaf edit", p.cacheable);
            TC_CHECK("leaf-body edit changes the key",
                     p.cacheable && have_keyA && memcmp(keyA, p.key, 32) != 0);
            TC_CHECK("edited-closure key MISSES the old stored PASS",
                     p.cacheable && !p.hit);
            testcache_close(tc);
        }
    }

    /* ── Phase C: SELECTIVITY — restore leaf, edit an OUT-OF-closure file ──
     * key returns to keyA and the earlier stored PASS still hits (persisted). */
    TC_CHECK("restore leaf, edit unrelated tc_other",
             write_fixture(TC_LEAF_A, TC_OTHER_B, TC_H_A));
    {
        struct testcache *tc = testcache_open(TC_FIX);
        if (tc) {
            struct testcache_probe p;
            testcache_probe_group(tc, "test_demo_entry", &p);
            TC_CHECK("unrelated edit leaves the key unchanged",
                     p.cacheable && have_keyA && memcmp(keyA, p.key, 32) == 0);
            TC_CHECK("unchanged key still HITS across reopen (persisted PASS)",
                     p.cacheable && p.hit);
            testcache_close(tc);
        }
    }

    /* ── Phase D: UNCACHEABLE cases ── */
    {
        struct testcache *tc = testcache_open(TC_FIX);
        if (tc) {
            struct testcache_probe pd;
            testcache_probe_group(tc, "test_explorer_index", &pd);
            TC_CHECK("external-input denylisted group is uncacheable",
                     !pd.cacheable);

            struct testcache_probe pu;
            testcache_probe_group(tc, "test_no_such_symbol_zzz", &pu);
            TC_CHECK("unresolved entry symbol is uncacheable", !pu.cacheable);
            testcache_close(tc);
        }
    }

    /* ── Phase E: header edit (an included file) also changes the key ── */
    TC_CHECK("edit included header", write_fixture(TC_LEAF_A, TC_OTHER_A, TC_H_B));
    {
        struct testcache *tc = testcache_open(TC_FIX);
        if (tc) {
            struct testcache_probe p;
            testcache_probe_group(tc, "test_demo_entry", &p);
            TC_CHECK("included-header edit changes the key",
                     p.cacheable && have_keyA && memcmp(keyA, p.key, 32) != 0);
            testcache_close(tc);
        }
    }

    system("rm -rf " TC_FIX);
    printf("test_testcache: %d failure(s)\n", failures);
    return failures;
}
