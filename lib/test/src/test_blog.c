/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"
#include "controllers/blog_controller.h"
#include "models/onion_announcement.h"
#include <unistd.h>

/* safe_path is static in blog_controller.c, so we replicate
 * the exact logic here for testing the validation rules. */
static bool test_safe_path(const char *path)
{
    if (!path || path[0] == '\0') return false;
    if (strstr(path, "..")) return false;
    if (path[0] == '/' && path[1] == '/') return false;
    return true;
}

int test_blog(void)
{
    int failures = 0;

    printf("blog: safe_path rejects .. traversal... ");
    {
        bool ok = !test_safe_path("../etc/passwd");
        ok = ok && !test_safe_path("foo/../bar");
        ok = ok && !test_safe_path("..hidden");
        ok = ok && !test_safe_path("..");
        ok = ok && !test_safe_path("a/b/../../c");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("blog: safe_path rejects // prefix... ");
    {
        bool ok = !test_safe_path("//etc/passwd");
        ok = ok && !test_safe_path("//");
        ok = ok && !test_safe_path("///foo");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("blog: safe_path accepts valid paths... ");
    {
        bool ok = test_safe_path("index.html");
        ok = ok && test_safe_path("/about");
        ok = ok && test_safe_path("posts/hello.html");
        ok = ok && test_safe_path("a/b/c.css");
        ok = ok && test_safe_path("style.css");
        /* Empty and NULL must be rejected */
        ok = ok && !test_safe_path("");
        ok = ok && !test_safe_path(NULL);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("blog: discover_onion_peers with empty/missing data... ");
    {
        struct onion_peer peers[10];
        bool ok = true;
        /* NULL checks */
        int found = blog_discover_onion_peers(NULL, peers, 10);
        ok = ok && (found == 0);
        found = blog_discover_onion_peers(".", NULL, 10);
        ok = ok && (found == 0);
        found = blog_discover_onion_peers(".", peers, 0);
        ok = ok && (found == 0);
        /* Non-existent dir — opens SQLite which may return 0 */
        char tmpdir[] = ".zcl_blog_disc_XXXXXX";
        char *dir = mkdtemp(tmpdir);
        if (dir) {
            found = blog_discover_onion_peers(dir, peers, 10);
            ok = ok && (found == 0);
            test_cleanup_tmpdir(dir);
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("blog: serve returns 404 for missing files... ");
    {
        /* Use a temp directory with no blog/ subdirectory */
        char tmpdir[] = ".zcl_blog_test_XXXXXX";
        char *dir = mkdtemp(tmpdir);
        bool ok = (dir != NULL);
        if (ok) {
            char out[4096];
            memset(out, 0, sizeof(out));
            size_t len = blog_serve(dir, "/nonexistent_page", out, sizeof(out));
            if (len < sizeof(out)) out[len] = '\0';
            ok = ok && (len > 0);
            ok = ok && (strstr(out, "404") != NULL);
            ok = ok && (strstr(out, "Not Found") != NULL);

            /* Also test with a deeper path */
            memset(out, 0, sizeof(out));
            len = blog_serve(dir, "/deep/nested/path.html", out, sizeof(out));
            if (len < sizeof(out)) out[len] = '\0';
            ok = ok && (len > 0);
            ok = ok && (strstr(out, "404") != NULL);

            test_cleanup_tmpdir(dir);
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("blog: serve returns 403 for path traversal... ");
    {
        char tmpdir[] = ".zcl_blog_403_XXXXXX";
        char *dir = mkdtemp(tmpdir);
        bool ok = (dir != NULL);
        if (ok) {
            char out[4096];
            memset(out, 0, sizeof(out));
            size_t len = blog_serve(dir, "/../../../etc/passwd",
                                    out, sizeof(out));
            if (len < sizeof(out)) out[len] = '\0';
            ok = ok && (len > 0);
            ok = ok && (strstr(out, "403") != NULL);
            test_cleanup_tmpdir(dir);
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("blog: serve handles NULL/empty inputs... ");
    {
        char out[4096];
        /* NULL path */
        size_t len = blog_serve(".", NULL, out, sizeof(out));
        bool ok = (len == 0);
        /* NULL output buffer */
        len = blog_serve(".", "/index.html", NULL, sizeof(out));
        ok = ok && (len == 0);
        /* Buffer too small */
        len = blog_serve(".", "/index.html", out, 100);
        ok = ok && (len == 0);
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("blog: node registry genesis builds valid output... ");
    {
        uint8_t buf[256];
        size_t len = blog_build_node_registry_genesis(buf, sizeof(buf));
        /* Should produce non-empty OP_RETURN script */
        bool ok = (len > 0 && len < sizeof(buf));
        if (ok) printf("OK (%zu bytes)\n", len);
        else { printf("FAIL (len=%zu)\n", len); failures++; }
    }

    printf("blog: auto_announce_onion rejects invalid input... ");
    {
        bool ok = !blog_auto_announce_onion(NULL, "test.onion");
        ok = ok && !blog_auto_announce_onion(".", NULL);
        ok = ok && !blog_auto_announce_onion(".", "");
        ok = ok && !blog_auto_announce_onion(".", "no_suffix");
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    printf("blog: auto_announce_onion announces new address... ");
    {
        char tmpdir[] = ".zcl_blog_ann_XXXXXX";
        char *dir = mkdtemp(tmpdir);
        bool ok = (dir != NULL);
        if (ok) {
            const char *addr1 =
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.onion";
            ok = ok && blog_auto_announce_onion(dir, addr1);

            /* Second call with same address returns false (no re-announce) */
            ok = ok && !blog_auto_announce_onion(dir, addr1);

            /* Different address announces successfully */
            const char *addr2 =
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.onion";
            ok = ok && blog_auto_announce_onion(dir, addr2);

            char db_path[1024];
            snprintf(db_path, sizeof(db_path), "%s/node.db", dir);
            struct node_db ndb;
            memset(&ndb, 0, sizeof(ndb));
            ok = ok && node_db_open(&ndb, db_path);
            if (ok) {
                struct db_onion_announcement rows[4];
                memset(rows, 0, sizeof(rows));
                int count = db_onion_announcement_recent(&ndb, rows, 4);
                ok = ok && (count >= 2);
                ok = ok && db_onion_announcement_exists(&ndb, addr2);
                ok = ok && rows[0].script_hex[0] != '\0';
                node_db_close(&ndb);
            }

            /* Clean up: remove node.db and tmpdir */
            test_cleanup_tmpdir(dir);
        }
        if (ok) printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}
