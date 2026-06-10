/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the defensive path-input checkers
 * (lib/util/src/path_check.c).
 *
 * Both helpers are pure: deterministic, no I/O, no global state. So
 * tests are simple table-driven assertions. */

#include "test/test_helpers.h"
#include "util/path_check.h"
#include <stdio.h>
#include <string.h>

#define PC_CHECK(name, expr) do { \
    printf("path_check: %s... ", (name)); \
    if ((expr)) printf("OK\n");           \
    else { printf("FAIL\n"); failures++; } \
} while (0)

int test_path_check(void)
{
    printf("\n=== path_check tests ===\n");
    int failures = 0;

    /* ── fs_arg: rejects NULL / empty / over-length ──────────── */
    PC_CHECK("fs_arg(NULL) rejected",
        !path_check_fs_arg(NULL, 16));
    PC_CHECK("fs_arg(\"\") rejected",
        !path_check_fs_arg("", 16));
    PC_CHECK("fs_arg too long rejected (cap=4, input=5)",
        !path_check_fs_arg("hello", 4));

    /* Boundary: exactly max_len bytes is accepted. */
    PC_CHECK("fs_arg exactly at cap accepted",
        path_check_fs_arg("hello", 5));

    /* ── fs_arg: rejects control characters ──────────────────── */
    {
        char with_nul[8] = "abc\0def";  /* strnlen will stop at \0 */
        /* strnlen('abc\0', max>3) returns 3; that's the full string the
         * checker sees, and it's printable — so this string is accepted.
         * The real NUL guard is: caller passing a string that *contains*
         * a NUL as data is impossible via JSON since JSON strings can't
         * have embedded NULs that survive the decoder. Verify the
         * non-NUL control-char rejection instead. */
        PC_CHECK("fs_arg with NUL-truncated string still accepted",
            path_check_fs_arg(with_nul, 8));
    }
    PC_CHECK("fs_arg with 0x01 (SOH) rejected",
        !path_check_fs_arg("a\x01" "b", 8));
    PC_CHECK("fs_arg with 0x1F (US) rejected",
        !path_check_fs_arg("a\x1f" "b", 8));
    PC_CHECK("fs_arg with 0x7F (DEL) rejected",
        !path_check_fs_arg("a\x7f" "b", 8));
    PC_CHECK("fs_arg with newline rejected",
        !path_check_fs_arg("a\nb", 8));
    PC_CHECK("fs_arg with tab rejected",
        !path_check_fs_arg("a\tb", 8));

    /* ── fs_arg: accepts legitimate paths ────────────────────── */
    PC_CHECK("fs_arg accepts \"foo.txt\"",
        path_check_fs_arg("foo.txt", 32));
    PC_CHECK("fs_arg accepts relative path",
        path_check_fs_arg("subdir/file", 32));
    PC_CHECK("fs_arg accepts absolute path (operator intent)",
        path_check_fs_arg("/tmp/zcl.dat", 64));
    PC_CHECK("fs_arg accepts \"..\" (operator intent, not filtered)",
        path_check_fs_arg("../foo", 16));

    /* ── zcl_node_db_path: canonical node.db path helper ─────── */
    {
        char db_path[64];
        PC_CHECK("node_db_path builds under datadir",
            strcmp(zcl_node_db_path(db_path, sizeof(db_path), "/tmp/zcl"),
                   "/tmp/zcl/node.db") == 0 &&
            strcmp(db_path, "/tmp/zcl/node.db") == 0);
        PC_CHECK("node_db_path rejects NULL buffer",
            strcmp(zcl_node_db_path(NULL, sizeof(db_path), "/tmp/zcl"), "") == 0);
        PC_CHECK("node_db_path rejects zero buffer",
            strcmp(zcl_node_db_path(db_path, 0, "/tmp/zcl"), "") == 0);
        PC_CHECK("node_db_path rejects NULL datadir",
            strcmp(zcl_node_db_path(db_path, sizeof(db_path), NULL), "") == 0);
    }

    /* ── url_arg: inherits fs_arg checks ─────────────────────── */
    PC_CHECK("url_arg(NULL) rejected",
        !path_check_url_arg(NULL, 16));
    PC_CHECK("url_arg(\"\") rejected",
        !path_check_url_arg("", 16));
    PC_CHECK("url_arg with control char rejected",
        !path_check_url_arg("/a\x01" "b", 16));

    /* ── url_arg: requires leading '/' ───────────────────────── */
    PC_CHECK("url_arg without leading / rejected",
        !path_check_url_arg("status", 16));
    PC_CHECK("url_arg \"/\" accepted",
        path_check_url_arg("/", 16));
    PC_CHECK("url_arg \"/status\" accepted",
        path_check_url_arg("/status", 16));

    /* ── url_arg: rejects ".." path segments ─────────────────── */
    PC_CHECK("url_arg \"/..\" rejected",
        !path_check_url_arg("/..", 16));
    PC_CHECK("url_arg \"/../foo\" rejected",
        !path_check_url_arg("/../foo", 16));
    PC_CHECK("url_arg \"/foo/..\" rejected",
        !path_check_url_arg("/foo/..", 16));
    PC_CHECK("url_arg \"/foo/../bar\" rejected",
        !path_check_url_arg("/foo/../bar", 16));

    /* 3 dots is NOT a `..` segment — accept. */
    PC_CHECK("url_arg \"/.../foo\" accepted (3 dots != ..)",
        path_check_url_arg("/.../foo", 16));

    /* A single dot segment is not rejected (it's a no-op, not an escape).
     * This is documented behavior: the helper filters only what is never
     * legitimate, and `/./foo` resolves to `/foo`. */
    PC_CHECK("url_arg \"/./foo\" accepted",
        path_check_url_arg("/./foo", 16));

    /* Nested directory paths accepted. */
    PC_CHECK("url_arg \"/api/health\" accepted",
        path_check_url_arg("/api/health", 32));
    PC_CHECK("url_arg \"/directory.json\" accepted",
        path_check_url_arg("/directory.json", 32));

    return failures;
}
