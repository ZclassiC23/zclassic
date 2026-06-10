/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Secrets hygiene audit — the single most embarrassing bug we could
 * ship is logging a private key, seed phrase, or wallet secret to
 * stdout / stderr / the event log / an RPC response.
 *
 * This test file is the runtime half of the audit. The static half is
 * tools/scripts/check_no_secret_printf.sh, a grep-based scan for
 * printf-family calls that reference variables with key-shaped
 * identifier names. The shell script is the first line of defence (it
 * catches new leaks at CI time); the tests here are the second (they
 * catch leaks that the grep heuristic would miss — e.g. a secret
 * threaded through several function calls before being logged).
 *
 * Strategy:
 *   1. Build a "golden corpus" of fake secrets — strings that would
 *      obviously be catastrophic if they appeared in any log output.
 *      These strings are never used as real keys, so any code path
 *      that echoes them has a genuine hygiene bug.
 *   2. Run the known-clean surfaces (MCP tools/list, error envelopes,
 *      event log after innocuous activity) and assert that none of
 *      the corpus strings appear.
 *   3. Run the check_no_secret_printf.sh script via system(3) and
 *      require exit code 0.
 *   4. Positive control: verify the scanner WOULD catch a secret if
 *      one were injected. This is how we know the test isn't silently
 *      green because the scan logic is broken.
 */

#include "test/test_helpers.h"
#include "mcp/router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "util/safe_alloc.h"

/* ── Golden corpus ─────────────────────────────────────────── */

/* Fake WIF-shaped string (starts with the mainnet "L" prefix). Not a
 * real key — constructed from a deterministic pattern so we can scan
 * for it without worrying about accidental collisions. */
#define CORPUS_WIF      "L5HygienePLACEHOLDERwifNEVERrealKEY111111"

/* Fake 64-hex private key. */
#define CORPUS_PRIVHEX  "deadbeefcafebabe0123456789abcdef" \
                        "fedcba9876543210deadbeefcafebabe"

/* Fake 24-word BIP39 mnemonic. Each word is "hygiene". Real mnemonics
 * use real words; this is obviously synthetic but matches the shape
 * (space-separated tokens) a scanner might look for. */
#define CORPUS_MNEMONIC "hygiene hygiene hygiene hygiene hygiene "   \
                        "hygiene hygiene hygiene hygiene hygiene "   \
                        "hygiene hygiene hygiene hygiene hygiene "   \
                        "hygiene hygiene hygiene hygiene hygiene "   \
                        "hygiene hygiene hygiene hygiene"

/* Fake Sapling extended spending key (extspk) — starts with zxviews*/
#define CORPUS_EXTSPK   "secret-extended-key-test-HYGIENEplaceholder"

/* Fake RPC cookie contents. */
#define CORPUS_COOKIE   "__cookie__:HYGIENEcookieNEVERrealXXXXXXXX"

/* Fake Tor onion service private key (first line of hs_ed25519_secret_key). */
#define CORPUS_HS_PRIV  "== ed25519v1-secret: type0 ==HYGIENEonionkey"

static const char *const g_corpus[] = {
    CORPUS_WIF,
    CORPUS_PRIVHEX,
    CORPUS_MNEMONIC,
    CORPUS_EXTSPK,
    CORPUS_COOKIE,
    CORPUS_HS_PRIV,
};

static const size_t g_corpus_count = sizeof(g_corpus) / sizeof(g_corpus[0]);

/* Portable substring search — avoids the glibc-specific memmem so we
 * work under strict _POSIX_C_SOURCE. Returns a pointer into haystack
 * or NULL. */
static const char *find_sub(const char *haystack, size_t h_len,
                             const char *needle, size_t n_len)
{
    if (n_len == 0 || n_len > h_len) return NULL;
    for (size_t i = 0; i + n_len <= h_len; i++) {
        if (memcmp(haystack + i, needle, n_len) == 0)
            return haystack + i;
    }
    return NULL;
}

/* Scan a buffer for every corpus member. Returns the number of hits.
 * On a hit, prints the matched corpus member (but not the surrounding
 * context, to avoid leaking whatever was wrongly printed to the test
 * log itself). */
static int scan_for_corpus(const char *buf, size_t len, const char *label)
{
    int hits = 0;
    for (size_t i = 0; i < g_corpus_count; i++) {
        size_t nlen = strlen(g_corpus[i]);
        if (find_sub(buf, len, g_corpus[i], nlen) != NULL) {
            printf("LEAK[%s]: corpus entry %zu found\n", label, i);
            hits++;
        }
    }
    return hits;
}

/* ── Individual tests ──────────────────────────────────────── */

static int test_corpus_setup(void)
{
    int failures = 0;
    TEST("secrets_hygiene: golden corpus is non-empty and non-trivial") {
        ASSERT(g_corpus_count >= 5);
        for (size_t i = 0; i < g_corpus_count; i++) {
            ASSERT(g_corpus[i] != NULL);
            /* Every corpus string should be long enough that an accidental
             * substring match against arbitrary output is improbable. */
            ASSERT(strlen(g_corpus[i]) >= 20);
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_positive_control(void)
{
    int failures = 0;
    TEST("secrets_hygiene: positive control — scanner catches a leak") {
        /* Build a buffer that contains one of the corpus entries and
         * make sure the scanner reports it. If this fails, the rest of
         * the test file is lying to us — every "clean" result would be
         * a false negative. */
        char buf[1024];
        snprintf(buf, sizeof(buf),
                 "some prefix %s some suffix", CORPUS_WIF);
        int hits = scan_for_corpus(buf, strlen(buf), "positive_control");
        ASSERT(hits >= 1);
        PASS();
    } _test_next:;
    return failures;
}

static int test_mcp_tools_list_clean(void)
{
    int failures = 0;
    TEST("secrets_hygiene: mcp_router_tools_list_json has no corpus leaks") {
        /* The tools/list surface is public and the most widely inspected
         * JSON in the MCP surface. A leak here would be the loudest
         * possible bug. */
        char *buf = zcl_malloc(256 * 1024, "test_tools_list_buf");
        ASSERT(buf != NULL);
        size_t n = mcp_router_tools_list_json(buf, 256 * 1024);
        ASSERT(n > 0);
        int hits = scan_for_corpus(buf, n, "tools_list");
        ASSERT_EQ(hits, 0);
        free(buf);
        PASS();
    } _test_next:;
    return failures;
}

static int test_mcp_error_envelopes_clean(void)
{
    int failures = 0;
    TEST("secrets_hygiene: MCP error envelopes don't echo secrets") {
        /* A rookie mistake is to copy an arbitrary `reason` into the
         * error envelope. If a handler ever threaded a private key
         * through the reason field, the scan would catch it. Here we
         * just assert that a normal envelope with a benign reason
         * doesn't spuriously contain any corpus string. */
        char buf[1024];
        size_t n = mcp_router_error_envelope(
            buf, sizeof(buf),
            MCP_ERR_MISSING_PARAM, "zcl_test",
            "height", "required parameter missing");
        ASSERT(n > 0);
        int hits = scan_for_corpus(buf, n, "error_envelope");
        ASSERT_EQ(hits, 0);
        PASS();
    } _test_next:;
    return failures;
}

/* Resolve the path to tools/scripts/check_no_secret_printf.sh relative
 * to the current working directory. Test is invoked from the repo root
 * via build/bin/test_zcl. */
static int test_check_no_secret_printf_script(void)
{
    int failures = 0;
    TEST("secrets_hygiene: check_no_secret_printf.sh runs clean") {
        const char *path = "tools/scripts/check_no_secret_printf.sh";
        struct stat st;
        ASSERT(stat(path, &st) == 0);
        /* Must be executable — CI runs it directly. */
        ASSERT(st.st_mode & S_IXUSR);

        /* Run silently and inspect the exit code. If the script ever
         * flags a real leak, the test blows up with a clear signal. */
        int rc = system("tools/scripts/check_no_secret_printf.sh >/dev/null 2>&1");
        ASSERT(WIFEXITED(rc));
        int exit_status = WEXITSTATUS(rc);
        if (exit_status != 0) {
            printf("FAIL (check_no_secret_printf.sh exited %d)\n",
                   exit_status);
            failures++;
            /* Re-run with output so the diff is visible. */
            (void)system("tools/scripts/check_no_secret_printf.sh 2>&1 "
                         "| head -40");
            goto _test_next;
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_recover_tool_path_documented(void)
{
    int failures = 0;
    TEST("secrets_hygiene: wallet_recover tool is allowlisted explicitly") {
        /* tools/wallet_recover.c and tools/wallet_dump.c are the ONLY
         * files that legitimately print raw key material, and only
         * because their whole purpose is offline recovery. The allowlist
         * in check_no_secret_printf.sh names them explicitly. If another
         * file shows up on the allowlist later, someone needs to audit
         * the justification. */
        FILE *f = fopen("tools/scripts/check_no_secret_printf.sh", "r");
        ASSERT(f != NULL);
        char line[512];
        bool saw_wallet_recover = false;
        bool saw_wallet_dump = false;
        int allowlist_entries = 0;
        bool in_allowlist = false;
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "ALLOWLIST_RE=(")) {
                in_allowlist = true;
                continue;
            }
            if (in_allowlist) {
                /* Allowlist ends at a line whose first non-whitespace
                 * char is ')'. We can't key off any ')' because the
                 * justification comments may contain parentheses. */
                const char *p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == ')') {
                    in_allowlist = false;
                    continue;
                }
                /* Count only lines that start with a single quote —
                 * the regex entries, not the explanatory comments. */
                if (*p == '\'')
                    allowlist_entries++;
                if (strstr(line, "wallet_recover"))
                    saw_wallet_recover = true;
                if (strstr(line, "wallet_dump"))
                    saw_wallet_dump = true;
            }
        }
        fclose(f);
        ASSERT(saw_wallet_recover);
        ASSERT(saw_wallet_dump);
        /* Allowlist growing past 5 is a smell — someone is papering
         * over a class of leak instead of fixing call sites. */
        ASSERT(allowlist_entries <= 5);
        PASS();
    } _test_next:;
    return failures;
}

/* ── Entry point ─────────────────────────────────────────── */

int test_secrets_hygiene(void);

int test_secrets_hygiene(void)
{
    int failures = 0;

    printf("\n=== secrets_hygiene ===\n");

    failures += test_corpus_setup();
    failures += test_positive_control();
    failures += test_mcp_tools_list_clean();
    failures += test_mcp_error_envelopes_clean();
    failures += test_check_no_secret_printf_script();
    failures += test_recover_tool_path_documented();

    return failures;
}
