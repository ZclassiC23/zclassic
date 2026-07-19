/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the binary A/B fallback module (app/services/src/binary_ab_fallback.c)
 * and the systemd launcher (deploy/zclassic23-launch.sh).
 *
 * Two halves, matching the two-piece design:
 *   1. C seams — binary_ab_reset_streak / _promote / _on_ready / the blocker —
 *      driven directly against a throwaway temp slots dir (no env, no exec).
 *   2. The launcher's slot-selection / streak / fallback logic — exercised by
 *      shelling out to tools/scripts/test_binary_ab_launcher.sh, which runs the
 *      real launcher in its ZCL_LAUNCH_TEST_ECHO seam against fake slots.
 */

#include "test/test_helpers.h"
#include "services/binary_ab_fallback.h"
#include "platform/os_proc.h"
#include "util/blocker.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AB_CHECK(name, expr) do { \
    printf("ab: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* Walk up from the running test binary's path to the repo root (dir holding
 * both Makefile and the launcher). Same shape as test_make_lint_gates.c's
 * repo_root(), but via platform/os_proc.h to satisfy check-proc-self-shim. */
static const char *ab_repo_root(void)
{
    static char root[PATH_MAX];
    static int cached = 0;
    if (cached) return root[0] ? root : NULL;

    char exe[PATH_MAX];
    if (!os_proc_exe_path(exe, sizeof(exe))) { cached = 1; root[0] = '\0'; return NULL; }

    for (int depth = 0; depth < 6; depth++) {
        char *slash = strrchr(exe, '/');
        if (!slash || slash == exe) break;
        *slash = '\0';
        char probe[PATH_MAX];
        struct stat st;
        if (snprintf(probe, sizeof(probe), "%s/Makefile", exe) >= (int)sizeof(probe)) break;
        if (stat(probe, &st) != 0) continue;
        if (snprintf(probe, sizeof(probe), "%s/deploy/zclassic23-launch.sh", exe)
                >= (int)sizeof(probe)) break;
        if (stat(probe, &st) != 0) continue;
        snprintf(root, sizeof(root), "%s", exe);
        cached = 1;
        return root;
    }
    cached = 1; root[0] = '\0'; return NULL;
}

static int ab_write_file(const char *path, const char *contents, mode_t mode)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t len = strlen(contents);
    size_t w = fwrite(contents, 1, len, fp);
    fclose(fp);
    if (w != len) return -1;
    return chmod(path, mode);
}

/* Reads up to sz-1 bytes of `path` into `out` (NUL-terminated). -1 on error. */
static int ab_read_file(const char *path, char *out, size_t sz)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    size_t r = fread(out, 1, sz - 1, fp);
    out[r] = '\0';
    fclose(fp);
    return (int)r;
}

int test_binary_ab_fallback(void)
{
    printf("\n=== binary_ab_fallback tests ===\n");
    int failures = 0;

    blocker_module_init();

    char tmpl[] = "/tmp/zcl_ab_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) {
        printf("ab: mkdtemp failed — cannot run seam tests\n");
        return 1;
    }

    char streak[PATH_MAX], cur[PATH_MAX], lastgood[PATH_MAX], buf[256];
    snprintf(streak, sizeof(streak), "%s/%s", dir, BINARY_AB_STREAK_BASENAME);
    snprintf(lastgood, sizeof(lastgood), "%s/%s", dir, BINARY_AB_LASTGOOD_BASENAME);
    snprintf(cur, sizeof(cur), "%s/current-bin", dir);

    /* ── 1. reset_streak overwrites any value with "0\n" ─────────────── */
    {
        AB_CHECK("seed streak file with 7", ab_write_file(streak, "7\n", 0644) == 0);
        AB_CHECK("reset_streak succeeds", binary_ab_reset_streak(streak));
        AB_CHECK("streak file now reads 0", ab_read_file(streak, buf, sizeof(buf)) >= 0
                 && strcmp(buf, "0\n") == 0);
        AB_CHECK("reset_streak on empty path fails", !binary_ab_reset_streak(""));
        AB_CHECK("reset_streak on NULL path fails", !binary_ab_reset_streak(NULL));
    }

    /* ── 2. promote copies current bytes into the last-good slot ─────── */
    {
        AB_CHECK("write a fake current binary", ab_write_file(cur, "CURRENT-V1-BYTES", 0755) == 0);
        AB_CHECK("promote succeeds", binary_ab_promote(dir, cur));
        AB_CHECK("last-good exists with the current bytes",
                 ab_read_file(lastgood, buf, sizeof(buf)) >= 0
                 && strcmp(buf, "CURRENT-V1-BYTES") == 0);
        struct stat st;
        AB_CHECK("last-good is executable",
                 stat(lastgood, &st) == 0 && (st.st_mode & S_IXUSR));
        AB_CHECK("promote with empty current_path fails", !binary_ab_promote(dir, ""));
        AB_CHECK("promote with empty slots_dir fails", !binary_ab_promote("", cur));
    }

    /* ── 3. on_ready (normal): resets streak AND promotes current ────── */
    {
        AB_CHECK("re-seed streak to 5", ab_write_file(streak, "5\n", 0644) == 0);
        AB_CHECK("rewrite current to V2", ab_write_file(cur, "CURRENT-V2-BYTES", 0755) == 0);
        AB_CHECK("on_ready(fallback=false) succeeds", binary_ab_on_ready(dir, cur, false));
        AB_CHECK("on_ready reset streak to 0",
                 ab_read_file(streak, buf, sizeof(buf)) >= 0 && strcmp(buf, "0\n") == 0);
        AB_CHECK("on_ready promoted V2 to last-good",
                 ab_read_file(lastgood, buf, sizeof(buf)) >= 0
                 && strcmp(buf, "CURRENT-V2-BYTES") == 0);
    }

    /* ── 4. on_ready (fallback): resets streak but does NOT overwrite the
     *      good slot with the bad current binary ─────────────────────── */
    {
        AB_CHECK("last-good currently holds V2 (the good slot)",
                 ab_read_file(lastgood, buf, sizeof(buf)) >= 0
                 && strcmp(buf, "CURRENT-V2-BYTES") == 0);
        AB_CHECK("current is now a BAD V3 build", ab_write_file(cur, "BAD-V3-BYTES", 0755) == 0);
        AB_CHECK("seed streak to 9", ab_write_file(streak, "9\n", 0644) == 0);
        AB_CHECK("on_ready(fallback=true) succeeds", binary_ab_on_ready(dir, cur, true));
        AB_CHECK("fallback on_ready reset streak to 0",
                 ab_read_file(streak, buf, sizeof(buf)) >= 0 && strcmp(buf, "0\n") == 0);
        AB_CHECK("fallback on_ready LEFT last-good = V2 (never promoted the bad binary)",
                 ab_read_file(lastgood, buf, sizeof(buf)) >= 0
                 && strcmp(buf, "CURRENT-V2-BYTES") == 0);
    }

    /* ── 5. on_ready with no slots dir is a no-op success ────────────── */
    {
        AB_CHECK("on_ready(NULL slots) is a no-op success", binary_ab_on_ready(NULL, cur, false));
        AB_CHECK("on_ready(empty slots) is a no-op success", binary_ab_on_ready("", cur, false));
    }

    /* ── 6. the fallback blocker ─────────────────────────────────────── */
    {
        blocker_clear(BINARY_FALLBACK_BLOCKER_ID);
        binary_ab_raise_fallback_blocker(false);
        AB_CHECK("raise(false) does not raise the blocker",
                 !blocker_exists(BINARY_FALLBACK_BLOCKER_ID));
        binary_ab_raise_fallback_blocker(true);
        AB_CHECK("raise(true) raises binary.fallback_active",
                 blocker_exists(BINARY_FALLBACK_BLOCKER_ID));
        AB_CHECK("blocker is PERMANENT (operator-cleared)",
                 blocker_class_for(BINARY_FALLBACK_BLOCKER_ID) == BLOCKER_PERMANENT);
        blocker_clear(BINARY_FALLBACK_BLOCKER_ID);
    }

    /* ── 7. cleanup temp dir ─────────────────────────────────────────── */
    unlink(streak); unlink(lastgood); unlink(cur);
    rmdir(dir);

    /* ── 8. launcher shell logic via the fixture harness ─────────────── */
    {
        const char *root = ab_repo_root();
        if (!root) {
            printf("ab: repo root not resolved — SKIPPING launcher harness\n");
        } else {
            char cmd[PATH_MAX + 64];
            snprintf(cmd, sizeof(cmd),
                     "sh %s/tools/scripts/test_binary_ab_launcher.sh 2>&1", root);
            FILE *p = popen(cmd, "r");
            AB_CHECK("launcher harness popen opens", p != NULL);
            bool saw_ok = false;
            if (p) {
                char line[512];
                while (fgets(line, sizeof(line), p))
                    if (strstr(line, "LAUNCHER-HARNESS OK")) saw_ok = true;
                int rc = pclose(p);
                AB_CHECK("launcher harness exits 0", rc == 0);
                AB_CHECK("launcher harness reports OK", saw_ok);
            }
        }
    }

    return failures;
}
