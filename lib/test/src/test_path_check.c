/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the defensive path-input checkers
 * (lib/util/src/path_check.c).
 *
 * Both helpers are pure: deterministic, no I/O, no global state. So
 * tests are simple table-driven assertions. */

#include "test/test_helpers.h"
#include "net/https_server.h"
#include "util/file_io.h"
#include "util/path_check.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PC_CHECK(name, expr) do { \
    printf("path_check: %s... ", (name)); \
    if ((expr)) printf("OK\n");           \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static bool write_small_file(const char *path, const char *body)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fputs(body, f) >= 0;
    fclose(f);
    return ok;
}

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

    /* ── ACME redirect passthrough containment ───────────────── */
    {
        char dir[256];
        char root[PATH_MAX];
        char token_path[PATH_MAX];
        char outside_path[PATH_MAX];
        char link_path[PATH_MAX];
        char out[PATH_MAX];
        char real_token[PATH_MAX];

        test_make_tmpdir(dir, sizeof(dir), "path_check", "acme");
        snprintf(root, sizeof(root), "%s/acme", dir);
        PC_CHECK("acme root mkdir", mkdir(root, 0700) == 0);

        snprintf(token_path, sizeof(token_path), "%s/token", root);
        PC_CHECK("acme token fixture",
                 write_small_file(token_path, "challenge"));
        PC_CHECK("acme resolver accepts token",
                 realpath(token_path, real_token) &&
                 https_server_acme_challenge_filepath_for_testing(
                     root, "/.well-known/acme-challenge/token",
                     out, sizeof(out)) &&
                 strcmp(out, real_token) == 0);

        PC_CHECK("acme resolver rejects traversal segment",
                 !https_server_acme_challenge_filepath_for_testing(
                     root, "/.well-known/acme-challenge/../token",
                     out, sizeof(out)));
        PC_CHECK("acme resolver rejects empty challenge token",
                 !https_server_acme_challenge_filepath_for_testing(
                     root, "/.well-known/acme-challenge/",
                     out, sizeof(out)));

        snprintf(outside_path, sizeof(outside_path), "%s/outside", dir);
        snprintf(link_path, sizeof(link_path), "%s/link", root);
        PC_CHECK("acme outside fixture",
                 write_small_file(outside_path, "outside") &&
                 symlink("../outside", link_path) == 0);
        PC_CHECK("acme resolver rejects symlink escape",
                 !https_server_acme_challenge_filepath_for_testing(
                     root, "/.well-known/acme-challenge/link",
                     out, sizeof(out)));

        test_cleanup_tmpdir(dir);
    }

    /* ── zcl_read_whole_file / zcl_read_whole_file_text ──────── */
    {
        char dir[256];
        char present[PATH_MAX];
        char empty[PATH_MAX];
        char missing[PATH_MAX];

        test_make_tmpdir(dir, sizeof(dir), "path_check", "file_io");
        snprintf(present, sizeof(present), "%s/present.bin", dir);
        snprintf(empty, sizeof(empty), "%s/empty.bin", dir);
        snprintf(missing, sizeof(missing), "%s/missing.bin", dir);

        PC_CHECK("file_io present fixture",
                 write_small_file(present, "hello world"));
        PC_CHECK("file_io empty fixture",
                 write_small_file(empty, ""));

        {
            uint8_t *buf = NULL;
            size_t len = 0;
            PC_CHECK("read_whole_file reads full contents",
                     zcl_read_whole_file(present, 0, &buf, &len, "test") &&
                     len == strlen("hello world") &&
                     memcmp(buf, "hello world", len) == 0);
            free(buf);
        }

        {
            uint8_t *buf = (uint8_t *)0x1;
            size_t len = 99;
            PC_CHECK("read_whole_file on empty file yields NULL/0",
                     zcl_read_whole_file(empty, 0, &buf, &len, "test") &&
                     buf == NULL && len == 0);
        }

        {
            uint8_t *buf = (uint8_t *)0x1;
            size_t len = 99;
            PC_CHECK("read_whole_file on missing path fails and zeroes out",
                     !zcl_read_whole_file(missing, 0, &buf, &len, "test") &&
                     buf == NULL && len == 0);
        }

        {
            uint8_t *buf = (uint8_t *)0x1;
            size_t len = 99;
            PC_CHECK("read_whole_file refuses a file over max_len",
                     !zcl_read_whole_file(present, 4, &buf, &len, "test") &&
                     buf == NULL && len == 0);
        }

        {
            uint8_t *buf = NULL;
            size_t len = 0;
            PC_CHECK("read_whole_file accepts a file exactly at max_len",
                     zcl_read_whole_file(present, strlen("hello world"),
                                         &buf, &len, "test") &&
                     len == strlen("hello world"));
            free(buf);
        }

        {
            char *text = NULL;
            size_t len = 0;
            PC_CHECK("read_whole_file_text NUL-terminates",
                     zcl_read_whole_file_text(present, 0, &text, &len, "test") &&
                     len == strlen("hello world") &&
                     strcmp(text, "hello world") == 0);
            free(text);
        }

        {
            char *text = NULL;
            size_t len = 99;
            PC_CHECK("read_whole_file_text on empty file yields empty string",
                     zcl_read_whole_file_text(empty, 0, &text, &len, "test") &&
                     text != NULL && text[0] == '\0' && len == 0);
            free(text);
        }

        test_cleanup_tmpdir(dir);
    }

    return failures;
}
