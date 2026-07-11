/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Self-test for the `make check-raw-sqlite` gate.
 *
 * Problem: the `check-raw-sqlite` lint is the only thing stopping new
 * raw `sqlite3_step` calls from reintroducing the 2026-04-10 UTXO-wipe
 * class of bug. If someone loosens the grep pattern ("oh, it's
 * annoying on this PR, let me add another exemption"), the gate
 * silently stops catching violations. This test prevents that.
 *
 * Approach:
 *   1. Copy the fixture (`lib/test/fixtures/raw_sqlite_step_fixture.c`)
 *      into `app/` under a unique temp name so the Makefile's grep
 *      scope actually sees it.
 *   2. Run `make check-raw-sqlite`.
 *   3. Assert exit code != 0 (the gate caught the fixture).
 *   4. Remove the temp file and rerun to confirm the gate passes again.
 *
 * Gated by `ZCL_TESTING` so the shell-out + make invocation only fires
 * when the suite is built by `make test`; standalone compilations of
 * test_zcl without the macro silently turn this into a no-op pass. */

#define _POSIX_C_SOURCE 200809L

#include "test/test_helpers.h"

#ifdef ZCL_TESTING

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define FIXTURE_SRC_REL "lib/test/fixtures/raw_sqlite_step_fixture.c"
#define FIXTURE_DST_REL "app/_lint_gate_fixture_tmp.c"
#define NODE_DB_EXEC_FIXTURE_SRC_REL "lib/test/fixtures/raw_sqlite_exec_node_db_fixture.c"
#define NODE_DB_EXEC_FIXTURE_DST_REL "app/_node_db_exec_lint_fixture_tmp.c"
#define COINS_FIXTURE_SRC_REL "lib/test/fixtures/coins_lookup_guard_fixture.c"
#define COINS_FIXTURE_DST_REL "app/controllers/src/_coins_lookup_guard_fixture_tmp.c"
#define OBS_FIXTURE_SRC_REL "lib/test/fixtures/observability_unpaired_stderr_fixture.c"
#define OBS_FIXTURE_DST_REL "app/_observability_lint_fixture_tmp.c"
#define OBS_OK_FIXTURE_SRC_REL "lib/test/fixtures/observability_paired_stderr_fixture.c"
#define OBS_OK_FIXTURE_DST_REL "app/_observability_ok_lint_fixture_tmp.c"
#define RAW_MALLOC_FIXTURE_DST_REL "app/_raw_malloc_lint_fixture_tmp.c"
#define RAW_MALLOC_OK_FIXTURE_DST_REL "app/_raw_malloc_ok_lint_fixture_tmp.c"
#define RAW_SQLITE_SCRIPT_REL "tools/scripts/check_raw_sqlite.sh"
#define RAW_MALLOC_SCRIPT_REL "tools/scripts/check_raw_malloc.sh"
#define HOTSWAP_SCOPE_SCRIPT_REL  "tools/lint/check_hotswap_eligible_scope.sh"
#define HOTSWAP_STATIC_SCRIPT_REL "tools/lint/check_hotswap_static_state.sh"
#define HOTSWAP_MANIFEST_REL "config/hotswap_eligible.def"
#define HOTSWAP_BAD_SCOPE_MANIFEST_REL \
    "lib/test/fixtures/hotswap_manifest_bad_scope.def"
#define HOTSWAP_BAD_STATIC_MANIFEST_REL \
    "lib/test/fixtures/hotswap_manifest_bad_static.def"
#define HOTSWAP_NO_MACRO_MANIFEST_REL \
    "lib/test/fixtures/hotswap_manifest_no_macro.def"
#define GIT_HOOKS_SCRIPT_REL "tools/scripts/check_git_hooks_installed.sh"
#define GIT_HOOKS_PRE_PUSH_REL "tools/githooks/pre-push"

static int run_gate_script(const char *script_rel, const char *mode);

static const char *repo_root(void)
{
    static char root[PATH_MAX];
    static int cached = 0;

    if (cached) return root[0] ? root : NULL;

    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0 || n >= (ssize_t)sizeof(exe) - 1) {
        cached = 1;
        root[0] = '\0';
        return NULL;
    }
    exe[n] = '\0';

    /* The binary lives at <root>/build/bin/<name>; walk UP from the exe
     * until a directory holding both the Makefile and the raw-sqlite
     * fixture appears. A single dirname() here once left root at
     * build/bin, the entry stat failed, and the WHOLE suite silently
     * no-op-SKIPped (PASS in 1s) — every source-text gate in this file
     * was dead. Bounded walk so a stray Makefile high in the tree can't
     * send the shell-outs somewhere surprising. */
    for (int depth = 0; depth < 6; depth++) {
        char *slash = strrchr(exe, '/');
        if (!slash || slash == exe) break;
        *slash = '\0';

        char probe[PATH_MAX];
        struct stat st;
        if (snprintf(probe, sizeof(probe), "%s/Makefile", exe)
                >= (int)sizeof(probe))
            break;
        if (stat(probe, &st) != 0) continue;
        if (snprintf(probe, sizeof(probe), "%s/%s", exe, FIXTURE_SRC_REL)
                >= (int)sizeof(probe))
            break;
        if (stat(probe, &st) != 0) continue;

        if (snprintf(root, sizeof(root), "%s", exe) >= (int)sizeof(root))
            break;
        cached = 1;
        return root;
    }

    cached = 1;
    root[0] = '\0';
    return NULL;
}

static int repo_path(char *out, size_t outsz, const char *rel)
{
    const char *root = repo_root();
    if (!root || !out || outsz == 0 || !rel) return -1;
    return snprintf(out, outsz, "%s/%s", root, rel) >= (int)outsz ? -1 : 0;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) {
        fprintf(stderr, "copy_file: fopen(%s) failed: %s\n",
                src, strerror(errno));
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fprintf(stderr, "copy_file: fopen(%s) failed: %s\n",
                dst, strerror(errno));
        fclose(in);
        return -1;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fprintf(stderr, "copy_file: fwrite failed: %s\n",
                    strerror(errno));
            fclose(in); fclose(out);
            return -1;
        }
    }
    fclose(in);
    fclose(out);
    return 0;
}

static bool has_c_suffix(const char *path)
{
    size_t len = strlen(path);
    return len >= 2 && strcmp(path + len - 2, ".c") == 0;
}

static bool has_ch_suffix(const char *path)
{
    size_t len = strlen(path);
    return len >= 2 &&
           (strcmp(path + len - 2, ".c") == 0 ||
            strcmp(path + len - 2, ".h") == 0);
}

static int read_entire_file(const char *path, char **out_buf)
{
    *out_buf = NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }

    long len = ftell(fp);
    if (len < 0) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    char *buf = calloc((size_t)len + 1, 1);
    if (!buf) {
        fclose(fp);
        return -1;
    }

    if (len > 0 && fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        free(buf);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    *out_buf = buf;
    return 0;
}

static size_t count_occurrences(const char *haystack, const char *needle)
{
    size_t step = strlen(needle);
    if (step == 0) return 0;
    size_t n = 0;
    for (const char *p = strstr(haystack, needle); p;
         p = strstr(p + step, needle))
        n++;
    return n;
}

static int check_coins_guard_file(const char *path)
{
    char *buf = NULL;
    if (read_entire_file(path, &buf) != 0) return -1;

    int rc = 0;
    if (strstr(buf, "coins_view_cache_get_coins(") &&
        !strstr(buf, "rpc_require_chainstate_lookup_ready(")) {
        rc = 1;
    }

    free(buf);
    return rc;
}

static bool line_has_obs_ok(const char *line)
{
    const char *tag = strstr(line, "// obs-ok:");
    return tag && tag[10] != '\0' && tag[10] != '\n' && tag[10] != ' ';
}

static bool line_has_event_emit(const char *line)
{
    return strstr(line, "event_emit(") || strstr(line, "event_emitf(");
}

static bool line_has_terminal_propagation(const char *line)
{
    return strstr(line, "return false;") ||
           strstr(line, "return -1;") ||
           strstr(line, "return 1;") ||
           strstr(line, "return NULL;") ||
           strstr(line, "exit(") ||
           strstr(line, "abort(");
}

static bool observability_line_allowed(char lines[][4096], size_t count,
                                       size_t idx)
{
    if (line_has_obs_ok(lines[idx])) return true;

    size_t start = idx > 3 ? idx - 3 : 0;
    size_t end = idx + 3 < count ? idx + 3 : count - 1;
    for (size_t i = start; i <= end; i++) {
        if (line_has_event_emit(lines[i])) return true;
        if (i >= idx && line_has_terminal_propagation(lines[i])) return true;
    }
    return false;
}

static int check_observability_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    char lines[512][4096];
    size_t count = 0;
    while (count < 512 && fgets(lines[count], sizeof(lines[count]), fp)) {
        count++;
    }
    int read_error = ferror(fp) ? -1 : 0;
    fclose(fp);
    if (read_error != 0) return read_error;

    for (size_t i = 0; i < count; i++) {
        if (strstr(lines[i], "fprintf(stderr") &&
            !observability_line_allowed(lines, count, i))
            return 1;
    }
    return 0;
}

static bool active_chain_set_tip_file_allowed(const char *path)
{
    return strstr(path, "/app/services/src/chain_tip.c") ||
           strstr(path, "/app/services/src/chain_state_service.c");
}

static int check_active_chain_set_tip_file(const char *path)
{
    if (active_chain_set_tip_file_allowed(path))
        return 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        char *hit = strstr(line, "active_chain_set_tip(");
        if (!hit)
            continue;
        char *block_comment = strstr(line, "/*");
        char *star_comment = strstr(line, "*");
        char *line_comment = strstr(line, "//");
        bool comment_only = (block_comment && block_comment < hit) ||
                            (star_comment && star_comment < hit) ||
                            (line_comment && line_comment < hit);
        if (!comment_only) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static int walk_c_files(const char *dirpath,
                        int (*check_file)(const char *path))
{
    DIR *dir = opendir(dirpath);
    if (!dir) return -1;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dirpath, ent->d_name) >=
            (int)sizeof(path)) {
            closedir(dir);
            return -1;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            closedir(dir);
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            int rc = walk_c_files(path, check_file);
            if (rc != 0) {
                closedir(dir);
                return rc;
            }
            continue;
        }

        if (!S_ISREG(st.st_mode) || !has_c_suffix(path))
            continue;

        int rc = check_file(path);
        if (rc != 0) {
            closedir(dir);
            return rc;
        }
    }

    closedir(dir);
    return 0;
}

static int walk_ch_files(const char *dirpath,
                         int (*check_file)(const char *path))
{
    DIR *dir = opendir(dirpath);
    if (!dir) return -1;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dirpath, ent->d_name) >=
            (int)sizeof(path)) {
            closedir(dir);
            return -1;
        }

        struct stat st;
        if (stat(path, &st) != 0) {
            closedir(dir);
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            int rc = walk_ch_files(path, check_file);
            if (rc != 0) {
                closedir(dir);
                return rc;
            }
            continue;
        }

        if (!S_ISREG(st.st_mode) || !has_ch_suffix(path))
            continue;

        int rc = check_file(path);
        if (rc != 0) {
            closedir(dir);
            return rc;
        }
    }

    closedir(dir);
    return 0;
}

static int check_deleted_engine_names_file(const char *path)
{
    if (strstr(path, "/lib/test/") || strstr(path, "/app/views/"))
        return 0;

    char *buf = NULL;
    if (read_entire_file(path, &buf) != 0)
        return -1;

    const char *stale[] = {
        "connect_tip",
        "disconnect_tip",
        "activate_best_chain",
        "process_new_block",
        "accept_block()",
    };
    for (size_t i = 0; i < sizeof(stale) / sizeof(stale[0]); i++) {
        if (strstr(buf, stale[i])) {
            fprintf(stderr,
                    "deleted engine name %s still present in %s\n",
                    stale[i], path);
            free(buf);
            return 1;
        }
    }

    free(buf);
    return 0;
}

static bool build_commit_macro_file_allowed(const char *path)
{
    return strstr(path, "/lib/util/src/clientversion.c") ||
           strstr(path, "/lib/util/include/util/clientversion.h") ||
           strstr(path, "/lib/test/src/test_make_lint_gates.c");
}

static int check_build_commit_macro_file(const char *path)
{
    if (build_commit_macro_file_allowed(path))
        return 0;

    char *buf = NULL;
    if (read_entire_file(path, &buf) != 0)
        return -1;

    int rc = 0;
    if (strstr(buf, "ZCL_BUILD_COMMIT")) {
        fprintf(stderr,
                "ZCL_BUILD_COMMIT used outside clientversion getter: %s\n",
                path);
        rc = 1;
    }

    free(buf);
    return rc;
}

static int run_check_build_commit_macro_contract(void)
{
    const char *roots[] = {
        "app", "lib", "config", "tools", "src",
        "domain", "application", "adapters", "ports",
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        char dir[PATH_MAX];
        if (repo_path(dir, sizeof(dir), roots[i]) != 0)
            return -1;
        int rc = walk_ch_files(dir, check_build_commit_macro_file);
        if (rc != 0)
            return rc;
    }
    return 0;
}

static int run_check_raw_sqlite(void)
{
    return run_gate_script(RAW_SQLITE_SCRIPT_REL, NULL);
}

static int run_check_coins_lookup_nullcheck(void)
{
    char controllers_dir[PATH_MAX];
    if (repo_path(controllers_dir, sizeof(controllers_dir),
                  "app/controllers/src") != 0)
        return -1;
    return walk_c_files(controllers_dir, check_coins_guard_file);
}

static int run_check_service_tip_mutation_gate(void)
{
    char services_dir[PATH_MAX];
    if (repo_path(services_dir, sizeof(services_dir), "app/services/src") != 0)
        return -1;
    return walk_c_files(services_dir, check_active_chain_set_tip_file);
}

static int run_check_deleted_engine_names(void)
{
    const char *roots[] = {"app", "lib", "config", "tools"};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        char dir[PATH_MAX];
        if (repo_path(dir, sizeof(dir), roots[i]) != 0)
            return -1;
        int rc = walk_ch_files(dir, check_deleted_engine_names_file);
        if (rc != 0)
            return rc;
    }
    return 0;
}

static int t_observability_fixture_trips_gate(void)
{
    int failures = 0;
    char fixture_src[PATH_MAX];
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_src, sizeof(fixture_src), OBS_FIXTURE_SRC_REL) != 0 ||
        repo_path(fixture_dst, sizeof(fixture_dst), OBS_FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve observability fixture paths\n");
        return 1;
    }
    (void)unlink(fixture_dst);
    if (copy_file(fixture_src, fixture_dst) != 0) {
        fprintf(stderr,
                "[lint-gate] could not plant observability fixture -- aborting\n");
        return 1;
    }
    int rc = check_observability_file(fixture_dst);
    (void)unlink(fixture_dst);
    TEST("[lint-gate] unpaired stderr fixture trips observability gate") {
        ASSERT(rc != 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_observability_positive_controls_pass(void)
{
    int failures = 0;
    char fixture_src[PATH_MAX];
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_src, sizeof(fixture_src), OBS_OK_FIXTURE_SRC_REL) != 0 ||
        repo_path(fixture_dst, sizeof(fixture_dst), OBS_OK_FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve observability-ok fixture paths\n");
        return 1;
    }
    (void)unlink(fixture_dst);
    if (copy_file(fixture_src, fixture_dst) != 0) {
        fprintf(stderr,
                "[lint-gate] could not plant observability-ok fixture -- aborting\n");
        return 1;
    }
    int rc = check_observability_file(fixture_dst);
    (void)unlink(fixture_dst);
    TEST("[lint-gate] observable stderr positive controls pass") {
        ASSERT(rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_baseline_passes(void)
{
    int failures = 0;
    TEST("[lint-gate] baseline passes (no fixture)") {
        ASSERT(run_check_raw_sqlite() == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_fixture_trips_gate(void)
{
    int failures = 0;
    char fixture_src[PATH_MAX];
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_src, sizeof(fixture_src), FIXTURE_SRC_REL) != 0 ||
        repo_path(fixture_dst, sizeof(fixture_dst), FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve raw sqlite fixture paths\n");
        return 1;
    }
    (void)unlink(fixture_dst);
    if (copy_file(fixture_src, fixture_dst) != 0) {
        fprintf(stderr, "[lint-gate] could not plant fixture — aborting\n");
        return 1;
    }
    int rc = run_check_raw_sqlite();
    (void)unlink(fixture_dst);
    TEST("[lint-gate] planted fixture trips the gate (exit != 0)") {
        ASSERT(rc != 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_node_db_exec_fixture_trips_gate(void)
{
    int failures = 0;
    char fixture_src[PATH_MAX];
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_src, sizeof(fixture_src),
                  NODE_DB_EXEC_FIXTURE_SRC_REL) != 0 ||
        repo_path(fixture_dst, sizeof(fixture_dst),
                  NODE_DB_EXEC_FIXTURE_DST_REL) != 0) {
        fprintf(stderr,
                "[lint-gate] could not resolve node_db exec fixture paths\n");
        return 1;
    }
    (void)unlink(fixture_dst);
    if (copy_file(fixture_src, fixture_dst) != 0) {
        fprintf(stderr,
                "[lint-gate] could not plant node_db exec fixture -- aborting\n");
        return 1;
    }
    int rc = run_check_raw_sqlite();
    (void)unlink(fixture_dst);
    TEST("[lint-gate] raw node.db sqlite3_exec DML fixture trips the gate") {
        ASSERT(rc != 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_gate_recovers_after_removal(void)
{
    int failures = 0;
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_dst, sizeof(fixture_dst), FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve raw sqlite fixture path\n");
        return 1;
    }
    (void)unlink(fixture_dst);
    TEST("[lint-gate] gate passes again after fixture removed") {
        ASSERT(run_check_raw_sqlite() == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_coins_guard_baseline_passes(void)
{
    int failures = 0;
    TEST("[lint-gate] baseline guarded coin-lookups pass") {
        ASSERT(run_check_coins_lookup_nullcheck() == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_coins_guard_fixture_trips_gate(void)
{
    int failures = 0;
    char fixture_src[PATH_MAX];
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_src, sizeof(fixture_src), COINS_FIXTURE_SRC_REL) != 0 ||
        repo_path(fixture_dst, sizeof(fixture_dst), COINS_FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve coins guard fixture paths\n");
        return 1;
    }
    (void)unlink(fixture_dst);
    if (copy_file(fixture_src, fixture_dst) != 0) {
        fprintf(stderr,
                "[lint-gate] could not plant coins guard fixture — aborting\n");
        return 1;
    }
    int rc = run_check_coins_lookup_nullcheck();
    (void)unlink(fixture_dst);
    TEST("[lint-gate] unguarded coin lookup fixture trips the gate (exit != 0)") {
        ASSERT(rc != 0);
        PASS();
    } _test_next:;
    return failures;
}

static int write_file(const char *path, const char *contents)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t n = strlen(contents);
    int ok = fwrite(contents, 1, n, fp) == n;
    fclose(fp);
    return ok ? 0 : -1;
}

/* Invokes tools/scripts/check_raw_malloc.sh and returns the script's
 * exit status (0 = clean, non-zero = violations). */
static int run_check_raw_malloc_script(void)
{
    char script[PATH_MAX];
    if (repo_path(script, sizeof(script), RAW_MALLOC_SCRIPT_REL) != 0)
        return -1;

    char out_path[PATH_MAX];
    if (repo_path(out_path, sizeof(out_path),
                  "test-tmp/zcl_raw_malloc_lint.out") != 0)
        return -1;

    struct sigaction old_chld;
    struct sigaction dfl_chld;
    int restore_chld = 0;
    memset(&old_chld, 0, sizeof(old_chld));
    memset(&dfl_chld, 0, sizeof(dfl_chld));
    dfl_chld.sa_handler = SIG_DFL;
    sigemptyset(&dfl_chld.sa_mask);
    if (sigaction(SIGCHLD, NULL, &old_chld) == 0 &&
        sigaction(SIGCHLD, &dfl_chld, NULL) == 0) {
        restore_chld = 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (restore_chld)
            (void)sigaction(SIGCHLD, &old_chld, NULL);
        return -1;
    }
    if (pid == 0) {
        int fd = open(out_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0) {
            (void)dup2(fd, STDOUT_FILENO);
            (void)dup2(fd, STDERR_FILENO);
            close(fd);
        }
        execl(script, script, (char *)NULL);
        _exit(127);
    }

    int rc = 0;
    while (waitpid(pid, &rc, 0) < 0) {
        if (errno == EINTR)
            continue;
        if (restore_chld)
            (void)sigaction(SIGCHLD, &old_chld, NULL);
        return -1;
    }
    if (restore_chld)
        (void)sigaction(SIGCHLD, &old_chld, NULL);
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return -1;
}

/* Generalized gate-script runner: fork/exec the script at repo-relative
 * path `script_rel`, optionally with ZCL_LINT_MODE set to `mode` (NULL to
 * leave unset). Returns the script's exit status (0 = clean, non-zero =
 * violations), or -1 on harness failure. Mirrors run_check_raw_malloc_script
 * but parameterized so the four E-series gates share one driver. */
static int run_gate_script(const char *script_rel, const char *mode)
{
    char script[PATH_MAX];
    if (repo_path(script, sizeof(script), script_rel) != 0)
        return -1;

    char out_path[PATH_MAX];
    if (repo_path(out_path, sizeof(out_path),
                  "test-tmp/zcl_gate_lint.out") != 0)
        return -1;

    struct sigaction old_chld;
    struct sigaction dfl_chld;
    int restore_chld = 0;
    memset(&old_chld, 0, sizeof(old_chld));
    memset(&dfl_chld, 0, sizeof(dfl_chld));
    dfl_chld.sa_handler = SIG_DFL;
    sigemptyset(&dfl_chld.sa_mask);
    if (sigaction(SIGCHLD, NULL, &old_chld) == 0 &&
        sigaction(SIGCHLD, &dfl_chld, NULL) == 0) {
        restore_chld = 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (restore_chld)
            (void)sigaction(SIGCHLD, &old_chld, NULL);
        return -1;
    }
    if (pid == 0) {
        int fd = open(out_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0) {
            (void)dup2(fd, STDOUT_FILENO);
            (void)dup2(fd, STDERR_FILENO);
            close(fd);
        }
        if (mode)
            (void)setenv("ZCL_LINT_MODE", mode, 1);
        execl(script, script, (char *)NULL);
        _exit(127);
    }

    int rc = 0;
    while (waitpid(pid, &rc, 0) < 0) {
        if (errno == EINTR)
            continue;
        if (restore_chld)
            (void)sigaction(SIGCHLD, &old_chld, NULL);
        return -1;
    }
    if (restore_chld)
        (void)sigaction(SIGCHLD, &old_chld, NULL);
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return -1;
}

/* Like run_gate_script but ALSO exports ZCL_SUPERVISOR_WORKER_FILES so the
 * widened Gate #21 background-worker scan reads a planted fixture file
 * instead of the live config/src/boot_background_workers.c. `worker_files`
 * is a space-separated repo-relative path list; it is resolved to absolute
 * paths before export so the gate (which runs from repo root) finds it
 * regardless of cwd. Mirrors run_gate_script's fork/exec/redirect plumbing. */
static int run_gate_script_with_worker_files(const char *script_rel,
                                             const char *mode,
                                             const char *worker_files_rel)
{
    char script[PATH_MAX];
    if (repo_path(script, sizeof(script), script_rel) != 0)
        return -1;

    char worker_abs[PATH_MAX];
    if (worker_files_rel &&
        repo_path(worker_abs, sizeof(worker_abs), worker_files_rel) != 0)
        return -1;

    char out_path[PATH_MAX];
    if (repo_path(out_path, sizeof(out_path),
                  "test-tmp/zcl_gate_lint.out") != 0)
        return -1;

    struct sigaction old_chld;
    struct sigaction dfl_chld;
    int restore_chld = 0;
    memset(&old_chld, 0, sizeof(old_chld));
    memset(&dfl_chld, 0, sizeof(dfl_chld));
    dfl_chld.sa_handler = SIG_DFL;
    sigemptyset(&dfl_chld.sa_mask);
    if (sigaction(SIGCHLD, NULL, &old_chld) == 0 &&
        sigaction(SIGCHLD, &dfl_chld, NULL) == 0) {
        restore_chld = 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (restore_chld)
            (void)sigaction(SIGCHLD, &old_chld, NULL);
        return -1;
    }
    if (pid == 0) {
        int fd = open(out_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0) {
            (void)dup2(fd, STDOUT_FILENO);
            (void)dup2(fd, STDERR_FILENO);
            close(fd);
        }
        if (mode)
            (void)setenv("ZCL_LINT_MODE", mode, 1);
        if (worker_files_rel)
            (void)setenv("ZCL_SUPERVISOR_WORKER_FILES", worker_abs, 1);
        execl(script, script, (char *)NULL);
        _exit(127);
    }

    int rc = 0;
    while (waitpid(pid, &rc, 0) < 0) {
        if (errno == EINTR)
            continue;
        if (restore_chld)
            (void)sigaction(SIGCHLD, &old_chld, NULL);
        return -1;
    }
    if (restore_chld)
        (void)sigaction(SIGCHLD, &old_chld, NULL);
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return -1;
}

/* Like run_gate_script but exports ONE arbitrary env var (name=value) into the
 * gate's environment. Used by the META-GATE that points each hardened gate at
 * an empty scan dir (via its ZCL_*_SCAN_* override) and asserts exit 2 — the
 * proof that a fail-silent gate is now fail-LOUD on an empty scan set.
 * Mirrors run_gate_script's fork/exec/redirect plumbing. */
static int run_gate_script_with_env(const char *script_rel,
                                    const char *env_name,
                                    const char *env_value)
{
    char script[PATH_MAX];
    if (repo_path(script, sizeof(script), script_rel) != 0)
        return -1;

    char out_path[PATH_MAX];
    if (repo_path(out_path, sizeof(out_path),
                  "test-tmp/zcl_gate_lint.out") != 0)
        return -1;

    struct sigaction old_chld;
    struct sigaction dfl_chld;
    int restore_chld = 0;
    memset(&old_chld, 0, sizeof(old_chld));
    memset(&dfl_chld, 0, sizeof(dfl_chld));
    dfl_chld.sa_handler = SIG_DFL;
    sigemptyset(&dfl_chld.sa_mask);
    if (sigaction(SIGCHLD, NULL, &old_chld) == 0 &&
        sigaction(SIGCHLD, &dfl_chld, NULL) == 0) {
        restore_chld = 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (restore_chld)
            (void)sigaction(SIGCHLD, &old_chld, NULL);
        return -1;
    }
    if (pid == 0) {
        int fd = open(out_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0) {
            (void)dup2(fd, STDOUT_FILENO);
            (void)dup2(fd, STDERR_FILENO);
            close(fd);
        }
        if (env_name && env_value)
            (void)setenv(env_name, env_value, 1);
        execl(script, script, (char *)NULL);
        _exit(127);
    }

    int rc = 0;
    while (waitpid(pid, &rc, 0) < 0) {
        if (errno == EINTR)
            continue;
        if (restore_chld)
            (void)sigaction(SIGCHLD, &old_chld, NULL);
        return -1;
    }
    if (restore_chld)
        (void)sigaction(SIGCHLD, &old_chld, NULL);
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return -1;
}

static int run_git_hooks_gate_with_path(const char *hooks_path)
{
    return run_gate_script_with_env(GIT_HOOKS_SCRIPT_REL,
                                    "ZCL_GIT_HOOKS_PATH_FOR_TEST",
                                    hooks_path);
}

static int t_git_hooks_gate_enforces_tracked_pre_push(void)
{
    int failures = 0;
    TEST("[lint-gate] local pre-push hook gate enforces tools/githooks") {
        ASSERT(run_git_hooks_gate_with_path(".git/hooks") != 0);
        ASSERT(run_git_hooks_gate_with_path("tools/githooks") == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_git_hooks_gate_rejects_noop_pre_push(void)
{
    int failures = 0;
    char hook_path[PATH_MAX];
    char *orig = NULL;
    int resolved = repo_path(hook_path, sizeof(hook_path),
                             GIT_HOOKS_PRE_PUSH_REL);
    int read_ok = (resolved == 0 && read_entire_file(hook_path, &orig) == 0);
    int wrote_noop = 0;
    int noop_rc = -1;
    int restore_ok = 0;
    int restored_rc = -1;

    if (read_ok) {
        wrote_noop = (write_file(hook_path,
                      "#!/usr/bin/env bash\n"
                      "# fixture: no local CI gate\n"
                      "exit 0\n") == 0 &&
                      chmod(hook_path, 0755) == 0);
        if (wrote_noop)
            noop_rc = run_git_hooks_gate_with_path("tools/githooks");
        restore_ok = (write_file(hook_path, orig) == 0 &&
                      chmod(hook_path, 0755) == 0);
        if (restore_ok)
            restored_rc = run_git_hooks_gate_with_path("tools/githooks");
    }

    TEST("[lint-gate] local pre-push hook gate rejects no-op hook body") {
        ASSERT(read_ok);
        ASSERT(wrote_noop);
        ASSERT(noop_rc != 0);
        ASSERT(restore_ok);
        ASSERT(restored_rc == 0);
        PASS();
    } _test_next:;

    free(orig);
    return failures;
}

#define E1_SCRIPT_REL    "tools/scripts/check_file_size_ceiling.sh"
#define E1_FIXTURE_DST   "app/controllers/src/_e1_size_ceiling_fixture_tmp.c"
#define E9_SCRIPT_REL    "tools/scripts/check_operator_needed_sink.sh"
#define SYSMEM_SCRIPT_REL "tools/scripts/check_systemd_memory_budget.sh"
#define E10_SHAPE_SCRIPT_REL "tools/lint/framework_shape_check.sh"
#define E10_SHAPE_FIXTURE_DST "app/_e10_shape_fixture_tmp.c"
#define E10_SQL_SCRIPT_REL "tools/lint/check_no_raw_sqlite_in_controllers.sh"
#define E10_SQL_FIXTURE_DST "app/controllers/src/_e10_rawsql_fixture_tmp.c"
#define E11_SCRIPT_REL   "tools/scripts/check_doc_accuracy.sh"
#define MODEL_AR_SCRIPT_REL "tools/scripts/check_model_ar_lifecycle.sh"
#define MODEL_AR_FIXTURE_DST "app/models/src/_model_ar_lifecycle_fixture_tmp.c"
#define E2_SCRIPT_REL    "tools/scripts/check_one_result_type.sh"
#define E2_FIXTURE_DST   "app/services/src/_e2_one_result_fixture_tmp.c"
#define E3_SCRIPT_REL    "tools/scripts/check_shape_includes_header.sh"
#define E3_FIXTURE_DST   "app/conditions/src/_e3_shape_include_fixture_tmp.c"
#define E4_SCRIPT_REL    "tools/scripts/check_projections_pure.sh"
#define E4_FIXTURE_DST   "lib/storage/src/_e4_pure_fixture_projection.c"
/* Gate #45 — domain/ source purity (HARD). The fixture is a domain/ src file
 * carrying a forbidden include; clean tree → exit 0, fixture → exit != 0. */
#define DOMAIN_PURITY_SCRIPT_REL  "tools/scripts/check_domain_purity.sh"
#define DOMAIN_PURITY_FIXTURE_DST "domain/wallet/src/_domain_purity_fixture_tmp.c"
#define E5_SCRIPT_REL    "tools/scripts/check_stage_advances_or_blocks.sh"
#define E5_FIXTURE_DST   "app/jobs/src/_e5_stage_fixture_tmp_stage.c"
#define E6_SCRIPT_REL    "tools/scripts/check_one_write_path.sh"
#define E6_FIXTURE_DST   "app/services/src/_e6_write_path_fixture_tmp.c"
#define E7_SCRIPT_REL    "tools/scripts/check_no_authoritative_ram_state.sh"
#define E7_FIXTURE_DST   "app/services/src/_e7_ram_state_fixture_tmp.c"
#define FSUF_SCRIPT_REL  "tools/lint/check_framework_filename_suffix.sh"
/* A foreign-shape suffix (*_controller) planted under app/services/src. */
#define FSUF_FIXTURE_DST "app/services/src/_fsuf_fixture_tmp_controller.c"
#define LOG_MACRO_RETURN_SCRIPT_REL \
    "tools/lint/check_log_macro_return_type.sh"
#define LOG_MACRO_RETURN_FIXTURE_DST \
    "app/services/src/_log_macro_return_type_fixture_tmp.c"
#define E12_SCRIPT_REL   "tools/lint/check_honest_witness.sh"
/* A condition .c with a PURE-INVERSE witness (the canonical Law-7 lie:
 * "return !detect_x()"), planted under app/conditions/src so the gate's
 * scan scope sees it. */
#define E12_FIXTURE_DST  "app/conditions/src/_e12_honest_witness_fixture_tmp.c"
/* Gate #21 background-worker lock-in: the widened check_supervisor_domain.sh
 * also scans the boot worker file (config/src/boot_background_workers.c) and
 * fails any spawn (pthread_create / thread_registry_spawn) not paired with a
 * supervisor_register_in_domain. The fixtures are planted under test-tmp/ and
 * fed to the gate via ZCL_SUPERVISOR_WORKER_FILES so the assertion does not
 * depend on the live worker file's state. */
#define SUPDOM_SCRIPT_REL     "tools/lint/check_supervisor_domain.sh"
/* A worker that spawns a thread but registers NO domain contract — the lie
 * the widened gate must catch (an unsupervised background worker). */
#define SUPDOM_BAD_WORKER_REL "test-tmp/_supdom_unsupervised_worker_fixture_tmp.c"
/* The same worker WITH a supervisor_register_in_domain pairing — passes. */
#define SUPDOM_OK_WORKER_REL  "test-tmp/_supdom_supervised_worker_fixture_tmp.c"

#define RRUNG_SCRIPT_REL  "tools/scripts/check_no_new_repair_rung.sh"
/* A new repair-rung-named file (basename contains "reconcile") planted under
 * app/services/src so the gate's `find app -name '*.c'` scan sees it. */
#define RRUNG_FIXTURE_DST "app/services/src/_repair_rung_fixture_tmp_reconcile.c"
#define BORROWED_SEED_SCRIPT_REL "tools/lint/check_no_new_borrowed_seed.sh"
/* A production-scope caller planted under app/services/src so the borrowed-seed
 * ratchet's app/config/lib/tools scan sees it. */
#define BORROWED_SEED_FIXTURE_DST \
    "app/services/src/_borrowed_seed_fixture_tmp.c"
#define COIN_BACKFILL_CALLER_SCRIPT_REL \
    "tools/lint/check_no_new_coin_backfill_caller.sh"
/* Production-scope callers planted under app/services/src and domain/ so the
 * coin-backfill entry-point ratchet proves both app-layer and non-app compiled
 * production roots are scanned. */
#define COIN_BACKFILL_CALLER_FIXTURE_DST \
    "app/services/src/_coin_backfill_caller_fixture_tmp.c"
#define COIN_BACKFILL_CALLER_DOMAIN_FIXTURE_DST \
    "domain/wallet/src/_coin_backfill_caller_fixture_tmp.c"

static int plant_oversized_file(const char *rel, int n_lines)
{
    char path[PATH_MAX];
    if (repo_path(path, sizeof(path), rel) != 0) return -1;
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    for (int i = 0; i < n_lines; i++)
        fputs("// fixture line\n", fp);
    fclose(fp);
    return 0;
}

static void unlink_rel(const char *rel)
{
    char path[PATH_MAX];
    if (repo_path(path, sizeof(path), rel) == 0)
        (void)unlink(path);
}

/* E1 — file-size ceiling is an ENFORCED RATCHET (hard FAIL, not advisory):
 * a NEW (non-baselined) app/.c file over the 800-line ceiling trips the
 * gate (nonzero exit) and prints the violation report; removing it returns
 * to a clean, zero-exit run. This complements (does not replace) the hard
 * correctness gate check_long_functions.sh (<=500 lines/function). */
static int t_e1_file_size_ceiling(void)
{
    int failures = 0;
    unlink_rel(E1_FIXTURE_DST);
    int baseline_rc = run_gate_script(E1_SCRIPT_REL, NULL);
    int planted = plant_oversized_file(E1_FIXTURE_DST, 900);
    int trip_rc = planted == 0 ? run_gate_script(E1_SCRIPT_REL, NULL) : -1;
    /* Capture the FAIL run's stdout so we can prove the report still PRINTS
     * the violation detail alongside the nonzero exit. */
    char *fail_out = NULL;
    char fail_path[PATH_MAX];
    int fail_read = (planted == 0 &&
                     repo_path(fail_path, sizeof(fail_path),
                               "test-tmp/zcl_gate_lint.out") == 0)
                        ? read_entire_file(fail_path, &fail_out)
                        : -1;
    unlink_rel(E1_FIXTURE_DST);
    int recover_rc = run_gate_script(E1_SCRIPT_REL, NULL);
    TEST("[lint-gate] E1 file-size ceiling: clean, FAILS (exit != 0) on oversized, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        /* FAIL, not WARN: oversized file now exits nonzero (enforced). */
        ASSERT(trip_rc != 0);
        /* ...and the violation report must be printed alongside the fail. */
        ASSERT(fail_read == 0);
        ASSERT(fail_out != NULL && strstr(fail_out, "FAIL") != NULL);
        ASSERT(fail_out != NULL &&
               strstr(fail_out, "NEW oversized file") != NULL);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    free(fail_out);
    return failures;
}

/* TENACITY I3 ratchet — a NEW repair/reconcile/backfill rung in app/ with no
 * baseline entry and no `// repair-rung-ok:` marker trips the gate; adding the
 * marker (citing a write-time-invariant test) exempts it; removing the file
 * restores green. */
static int t_no_new_repair_rung(void)
{
    int failures = 0;
    char path[PATH_MAX];
    unlink_rel(RRUNG_FIXTURE_DST);
    int baseline_rc = run_gate_script(RRUNG_SCRIPT_REL, NULL);
    /* Unjustified new rung — must trip. */
    int planted_bad = (repo_path(path, sizeof(path), RRUNG_FIXTURE_DST) == 0 &&
                       write_file(path, "void rung(void){}\n") == 0) ? 0 : -1;
    int trip_rc = planted_bad == 0 ? run_gate_script(RRUNG_SCRIPT_REL, NULL) : -1;
    /* Same file WITH a write-time-invariant-test marker — must pass. */
    int planted_ok = (repo_path(path, sizeof(path), RRUNG_FIXTURE_DST) == 0 &&
                      write_file(path,
                          "// repair-rung-ok:test_fixture_write_time_invariant\n"
                          "void rung(void){}\n") == 0) ? 0 : -1;
    int marker_rc = planted_ok == 0 ? run_gate_script(RRUNG_SCRIPT_REL, NULL) : -1;
    unlink_rel(RRUNG_FIXTURE_DST);
    int recover_rc = run_gate_script(RRUNG_SCRIPT_REL, NULL);
    TEST("[lint-gate] TENACITY-I3 no-new-repair-rung: clean, trips, marker exempts, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted_bad == 0);
        ASSERT(trip_rc != 0);
        ASSERT(planted_ok == 0);
        ASSERT(marker_rc == 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* Sovereign-cure ratchet — coins_kv_seed_from_node_db is the borrowed UTXO
 * seed path. New production callers must fail the gate, and removing the caller
 * must restore green so the baseline remains shrink-only. */
static int t_no_new_borrowed_seed_caller(void)
{
    int failures = 0;
    char path[PATH_MAX];
    unlink_rel(BORROWED_SEED_FIXTURE_DST);
    int baseline_rc = run_gate_script(BORROWED_SEED_SCRIPT_REL, NULL);
    int planted = (repo_path(path, sizeof(path),
                             BORROWED_SEED_FIXTURE_DST) == 0 &&
                   write_file(path,
                              "void borrowed_seed_fixture(void) {\n"
                              "    coins_kv_seed_from_node_db(0, 0);\n"
                              "}\n") == 0) ? 0 : -1;
    int trip_rc =
        planted == 0 ? run_gate_script(BORROWED_SEED_SCRIPT_REL, NULL) : -1;
    unlink_rel(BORROWED_SEED_FIXTURE_DST);
    int recover_rc = run_gate_script(BORROWED_SEED_SCRIPT_REL, NULL);
    TEST("[lint-gate] sovereign-cure no-new-borrowed-seed: clean, trips on new caller, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* Sovereign-cure ratchet — stage_repair_coin_backfill_try is the public
 * borrowed-seed-era coin-backfill repair entry. New production callers widen
 * the repair fabric; the only allowed caller is the reducer-frontier dispatcher,
 * and it must remain a single call site. */
static int t_no_new_coin_backfill_caller(void)
{
    int failures = 0;
    char path[PATH_MAX];
    char allowed_path[PATH_MAX];
    char *allowed_orig = NULL;
    unlink_rel(COIN_BACKFILL_CALLER_FIXTURE_DST);
    unlink_rel(COIN_BACKFILL_CALLER_DOMAIN_FIXTURE_DST);
    int baseline_rc = run_gate_script(COIN_BACKFILL_CALLER_SCRIPT_REL, NULL);
    int planted = (repo_path(path, sizeof(path),
                             COIN_BACKFILL_CALLER_FIXTURE_DST) == 0 &&
                   write_file(path,
                              "void coin_backfill_caller_fixture(void) {\n"
                              "    stage_repair_coin_backfill_try(0, 0, 0, 0, 0);\n"
                              "}\n") == 0) ? 0 : -1;
    int trip_rc = planted == 0
                      ? run_gate_script(COIN_BACKFILL_CALLER_SCRIPT_REL, NULL)
                      : -1;
    unlink_rel(COIN_BACKFILL_CALLER_FIXTURE_DST);
    int recover_after_new_rc =
        run_gate_script(COIN_BACKFILL_CALLER_SCRIPT_REL, NULL);
    int planted_domain =
        (repo_path(path, sizeof(path),
                   COIN_BACKFILL_CALLER_DOMAIN_FIXTURE_DST) == 0 &&
         write_file(path,
                    "void coin_backfill_domain_fixture(void) {\n"
                    "    stage_repair_coin_backfill_try(0, 0, 0, 0, 0);\n"
                    "}\n") == 0) ? 0 : -1;
    int trip_domain_rc =
        planted_domain == 0
            ? run_gate_script(COIN_BACKFILL_CALLER_SCRIPT_REL, NULL)
            : -1;
    unlink_rel(COIN_BACKFILL_CALLER_DOMAIN_FIXTURE_DST);
    int recover_after_domain_rc =
        run_gate_script(COIN_BACKFILL_CALLER_SCRIPT_REL, NULL);

    int read_allowed =
        repo_path(allowed_path, sizeof(allowed_path),
                  "app/jobs/src/stage_repair_reducer_frontier_coin.c") == 0
            ? read_entire_file(allowed_path, &allowed_orig)
            : -1;
    int append_dup = -1;
    int trip_dup_rc = -1;
    int restore_allowed = -1;
    int recover_after_dup_rc = -1;
    if (read_allowed == 0) {
        FILE *fp = fopen(allowed_path, "ab");
        if (fp) {
            append_dup =
                fputs("\nvoid coin_backfill_duplicate_fixture(void) {\n"
                      "    stage_repair_coin_backfill_try(0, 0, 0, 0, 0);\n"
                      "}\n", fp) >= 0 ? 0 : -1;
            fclose(fp);
        }
        trip_dup_rc = append_dup == 0
                          ? run_gate_script(COIN_BACKFILL_CALLER_SCRIPT_REL,
                                            NULL)
                          : -1;
        restore_allowed = write_file(allowed_path, allowed_orig);
        recover_after_dup_rc =
            restore_allowed == 0
                ? run_gate_script(COIN_BACKFILL_CALLER_SCRIPT_REL, NULL)
                : -1;
    }

    TEST("[lint-gate] sovereign-cure no-new-coin-backfill-caller: clean, trips on new callers and duplicate dispatcher call, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_after_new_rc == 0);
        ASSERT(planted_domain == 0);
        ASSERT(trip_domain_rc != 0);
        ASSERT(recover_after_domain_rc == 0);
        ASSERT(read_allowed == 0);
        ASSERT(append_dup == 0);
        ASSERT(trip_dup_rc != 0);
        ASSERT(restore_allowed == 0);
        ASSERT(recover_after_dup_rc == 0);
        PASS();
    } _test_next:;
    free(allowed_orig);
    return failures;
}

/* E9 — operator-needed sink: the live tree satisfies the pairing
 * (emit + alerts.c subscriber), so the gate passes. (HARD gate; the
 * negative control is covered by the sandbox check in the standalone
 * script and would require mutating lib/util/src/alerts.c, which we do
 * not do in-tree.) */
static int t_e9_operator_needed_sink(void)
{
    int failures = 0;
    TEST("[lint-gate] E9 operator-needed sink pairing present in tree") {
        ASSERT(run_gate_script(E9_SCRIPT_REL, NULL) == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* P1-3 — systemd memory budget: the live repo units must fit under the host
 * budget, and the script's parser self-test covers over-budget, infinity,
 * invalid-size, absent-cap, and drop-in override behavior. */
static int t_systemd_memory_budget(void)
{
    int failures = 0;
    int baseline_rc = run_gate_script(SYSMEM_SCRIPT_REL, NULL);
    int env_rc = setenv("ZCL_SYSTEMD_MEMORY_BUDGET_SELFTEST", "1", 1);
    int selftest_rc = env_rc == 0 ? run_gate_script(SYSMEM_SCRIPT_REL, NULL) : -1;
    (void)unsetenv("ZCL_SYSTEMD_MEMORY_BUDGET_SELFTEST");
    TEST("[lint-gate] P1-3 systemd memory budget: baseline and selftest pass") {
        ASSERT(baseline_rc == 0);
        ASSERT(env_rc == 0);
        ASSERT(selftest_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* E10a — framework shape RATCHET: an off-shape app/.c file (not in the
 * allowlist) trips the gate in RATCHET mode; removing it restores green. */
static int t_e10_framework_shape_ratchet(void)
{
    int failures = 0;
    unlink_rel(E10_SHAPE_FIXTURE_DST);
    int baseline_rc = run_gate_script(E10_SHAPE_SCRIPT_REL, "RATCHET");
    char path[PATH_MAX];
    int planted = (repo_path(path, sizeof(path), E10_SHAPE_FIXTURE_DST) == 0 &&
                   write_file(path, "int e10_shape_fixture;\n") == 0) ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(E10_SHAPE_SCRIPT_REL, "RATCHET") : -1;
    unlink_rel(E10_SHAPE_FIXTURE_DST);
    int recover_rc = run_gate_script(E10_SHAPE_SCRIPT_REL, "RATCHET");
    TEST("[lint-gate] E10 framework-shape RATCHET: clean, trips off-shape, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* E10b — no-raw-sqlite-in-controllers RATCHET: a NEW controller file
 * (not in the baseline) with a raw sqlite call trips the gate; removing
 * it restores green. */
static int t_e10_no_raw_sqlite_ratchet(void)
{
    int failures = 0;
    unlink_rel(E10_SQL_FIXTURE_DST);
    int baseline_rc = run_gate_script(E10_SQL_SCRIPT_REL, "RATCHET");
    char path[PATH_MAX];
    int planted = (repo_path(path, sizeof(path), E10_SQL_FIXTURE_DST) == 0 &&
                   write_file(path,
                       "void f(void){ sqlite3_prepare_v2(d, s, n, &st, 0); }\n") == 0)
                  ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(E10_SQL_SCRIPT_REL, "RATCHET") : -1;
    unlink_rel(E10_SQL_FIXTURE_DST);
    int recover_rc = run_gate_script(E10_SQL_SCRIPT_REL, "RATCHET");
    TEST("[lint-gate] E10 no-raw-sqlite RATCHET: clean, trips new file, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* Gate #22 — framework filename suffix (HARD): a file in a shape folder
 * whose name carries a FOREIGN shape suffix (here a *_controller.c planted
 * under app/services/src/) trips the gate; removing it restores green. */
static int t_gate22_framework_filename_suffix(void)
{
    int failures = 0;
    unlink_rel(FSUF_FIXTURE_DST);
    int baseline_rc = run_gate_script(FSUF_SCRIPT_REL, NULL);
    char path[PATH_MAX];
    int planted = (repo_path(path, sizeof(path), FSUF_FIXTURE_DST) == 0 &&
                   write_file(path, "int fsuf_fixture;\n") == 0) ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(FSUF_SCRIPT_REL, NULL) : -1;
    unlink_rel(FSUF_FIXTURE_DST);
    int recover_rc = run_gate_script(FSUF_SCRIPT_REL, NULL);
    TEST("[lint-gate] #22 framework-filename-suffix: clean, trips foreign suffix, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* Returning LOG_* macros must match the enclosing function's return type:
 * LOG_ERR in bool functions used to return -1, which converts to true. */
static int t_log_macro_return_type_gate(void)
{
    int failures = 0;
    unlink_rel(LOG_MACRO_RETURN_FIXTURE_DST);
    int baseline_rc = run_gate_script(LOG_MACRO_RETURN_SCRIPT_REL, NULL);
    char path[PATH_MAX];
    int planted =
        (repo_path(path, sizeof(path), LOG_MACRO_RETURN_FIXTURE_DST) == 0 &&
         write_file(path,
                    "#include <stdbool.h>\n"
                    "#include \"util/log_macros.h\"\n"
                    "bool bad_bool(void) { LOG_ERR(\"fixture\", \"bad\"); }\n"
                    "int bad_int(void) { LOG_FAIL(\"fixture\", \"bad\"); }\n") == 0)
            ? 0
            : -1;
    int trip_rc =
        planted == 0 ? run_gate_script(LOG_MACRO_RETURN_SCRIPT_REL, NULL) : -1;
    unlink_rel(LOG_MACRO_RETURN_FIXTURE_DST);
    int recover_rc = run_gate_script(LOG_MACRO_RETURN_SCRIPT_REL, NULL);

    TEST("[lint-gate] LOG_* return-type gate: clean, trips, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* E11 — doc accuracy: the in-tree DEFENSIVE_CODING.md gate block matches
 * the Makefile lint: target, so the gate passes. */
static int t_e11_doc_accuracy(void)
{
    int failures = 0;
    TEST("[lint-gate] E11 doc gate list matches Makefile lint: target") {
        ASSERT(run_gate_script(E11_SCRIPT_REL, NULL) == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* Model AR lifecycle gate — model sources must not hand-run save callback
 * internals, and real db_<model>_save() definitions must reach the AR save
 * macros so validation and hooks stay one mechanical lifecycle. */
static int t_model_ar_lifecycle_gate(void)
{
    int failures = 0;
    unlink_rel(MODEL_AR_FIXTURE_DST);
    int baseline_rc = run_gate_script(MODEL_AR_SCRIPT_REL, NULL);
    char path[PATH_MAX];
    int planted =
        (repo_path(path, sizeof(path), MODEL_AR_FIXTURE_DST) == 0 &&
         write_file(path,
                    "#include \"models/activerecord.h\"\n"
                    "void fixture(struct ar_callbacks *cbs, void *row) {\n"
                    "    ar_run_after_save(cbs, row);\n"
                    "}\n") == 0)
            ? 0
            : -1;
    int direct_trip_rc =
        planted == 0 ? run_gate_script(MODEL_AR_SCRIPT_REL, NULL) : -1;
    unlink_rel(MODEL_AR_FIXTURE_DST);
    int planted_bare_save =
        (repo_path(path, sizeof(path), MODEL_AR_FIXTURE_DST) == 0 &&
         write_file(path,
                    "#include <stdbool.h>\n"
                    "struct node_db;\n"
                    "bool db_fixture_save(struct node_db *ndb, const void *row) {\n"
                    "    (void)ndb;\n"
                    "    (void)row;\n"
                    "    return true;\n"
                    "}\n") == 0)
            ? 0
            : -1;
    int bare_save_trip_rc =
        planted_bare_save == 0 ? run_gate_script(MODEL_AR_SCRIPT_REL, NULL) : -1;
    unlink_rel(MODEL_AR_FIXTURE_DST);
    int recover_rc = run_gate_script(MODEL_AR_SCRIPT_REL, NULL);

    TEST("[lint-gate] model AR lifecycle gate: clean, trips bypasses, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(direct_trip_rc != 0);
        ASSERT(planted_bare_save == 0);
        ASSERT(bare_save_trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* E2 — one-result-type RATCHET: a NEW (non-baselined) service file that
 * returns bare bool (no struct zcl_result) trips the gate; removing it
 * restores green. */
static int t_e2_one_result_type(void)
{
    int failures = 0;
    unlink_rel(E2_FIXTURE_DST);
    int baseline_rc = run_gate_script(E2_SCRIPT_REL, NULL);
    char path[PATH_MAX];
    int planted = (repo_path(path, sizeof(path), E2_FIXTURE_DST) == 0 &&
                   write_file(path,
                       "#include <stdbool.h>\n"
                       "bool e2_fixture(void){ return false; }\n") == 0)
                  ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(E2_SCRIPT_REL, NULL) : -1;
    unlink_rel(E2_FIXTURE_DST);
    int recover_rc = run_gate_script(E2_SCRIPT_REL, NULL);
    TEST("[lint-gate] E2 one-result-type RATCHET: clean, trips bare-bool service, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* E3 — shape-includes-header HARD: a condition file that includes neither
 * framework/condition.h nor a conditions/ header trips the gate; removing
 * it restores green. */
static int t_e3_shape_includes_header(void)
{
    int failures = 0;
    unlink_rel(E3_FIXTURE_DST);
    int baseline_rc = run_gate_script(E3_SCRIPT_REL, NULL);
    char path[PATH_MAX];
    int planted = (repo_path(path, sizeof(path), E3_FIXTURE_DST) == 0 &&
                   write_file(path,
                       "/* mislabeled condition: no shape header */\n"
                       "int e3_fixture;\n") == 0)
                  ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(E3_SCRIPT_REL, NULL) : -1;
    unlink_rel(E3_FIXTURE_DST);
    int recover_rc = run_gate_script(E3_SCRIPT_REL, NULL);
    TEST("[lint-gate] E3 shape-includes-header HARD: clean, trips headerless condition, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* E4 — projections-pure HARD: a projection file that includes an app-layer
 * (services/) header trips the gate; removing it restores green. */
static int t_e4_projections_pure(void)
{
    int failures = 0;
    unlink_rel(E4_FIXTURE_DST);
    int baseline_rc = run_gate_script(E4_SCRIPT_REL, NULL);
    char path[PATH_MAX];
    int planted = (repo_path(path, sizeof(path), E4_FIXTURE_DST) == 0 &&
                   write_file(path,
                       "#include \"services/sync_monitor.h\"\n"
                       "int e4_fixture;\n") == 0)
                  ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(E4_SCRIPT_REL, NULL) : -1;
    unlink_rel(E4_FIXTURE_DST);
    int recover_rc = run_gate_script(E4_SCRIPT_REL, NULL);
    TEST("[lint-gate] E4 projections-pure HARD: clean, trips app-layer include, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* Gate #45 — domain/ source purity HARD: a domain/ file that includes an
 * app-layer (services/) header trips the gate (rule a); an unlisted lib/
 * subsystem prefix also trips it (rule b); a bare domain-local sibling include
 * stays clean. Removing the fixture restores green. */
static int t_domain_purity(void)
{
    int failures = 0;
    char path[PATH_MAX];

    unlink_rel(DOMAIN_PURITY_FIXTURE_DST);
    int baseline_rc = run_gate_script(DOMAIN_PURITY_SCRIPT_REL, NULL);

    /* Rule (a): an app-layer (services/) include must trip the gate. */
    int planted_app = (repo_path(path, sizeof(path),
                                 DOMAIN_PURITY_FIXTURE_DST) == 0 &&
                       write_file(path,
                           "#include \"services/foo.h\"\n"
                           "int domain_purity_fixture;\n") == 0)
                      ? 0 : -1;
    int trip_app_rc = planted_app == 0
                      ? run_gate_script(DOMAIN_PURITY_SCRIPT_REL, NULL) : -1;
    unlink_rel(DOMAIN_PURITY_FIXTURE_DST);

    /* Rule (b): an unlisted lib/ subsystem prefix must also trip the gate. */
    int planted_lib = (repo_path(path, sizeof(path),
                                 DOMAIN_PURITY_FIXTURE_DST) == 0 &&
                       write_file(path,
                           "#include \"storage/foo.h\"\n"
                           "int domain_purity_fixture;\n") == 0)
                      ? 0 : -1;
    int trip_lib_rc = planted_lib == 0
                      ? run_gate_script(DOMAIN_PURITY_SCRIPT_REL, NULL) : -1;
    unlink_rel(DOMAIN_PURITY_FIXTURE_DST);

    /* A bare domain-local sibling include (no slash) must NOT trip the gate. */
    int planted_sib = (repo_path(path, sizeof(path),
                                 DOMAIN_PURITY_FIXTURE_DST) == 0 &&
                       write_file(path,
                           "#include \"reject_out.h\"\n"
                           "int domain_purity_fixture;\n") == 0)
                      ? 0 : -1;
    int sibling_rc = planted_sib == 0
                     ? run_gate_script(DOMAIN_PURITY_SCRIPT_REL, NULL) : -1;
    unlink_rel(DOMAIN_PURITY_FIXTURE_DST);

    int recover_rc = run_gate_script(DOMAIN_PURITY_SCRIPT_REL, NULL);

    TEST("[lint-gate] #45 domain-purity HARD: clean, trips app+lib includes, allows sibling, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted_app == 0);
        ASSERT(trip_app_rc != 0);
        ASSERT(planted_lib == 0);
        ASSERT(trip_lib_rc != 0);
        ASSERT(planted_sib == 0);
        ASSERT(sibling_rc == 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* E5 — stage-advances-or-blocks HARD: a Job step file that only ever returns
 * JOB_ADVANCED and references no cursor trips the gate; removing it restores
 * green. */
static int t_e5_stage_advances_or_blocks(void)
{
    int failures = 0;
    unlink_rel(E5_FIXTURE_DST);
    int baseline_rc = run_gate_script(E5_SCRIPT_REL, NULL);
    char path[PATH_MAX];
    int planted = (repo_path(path, sizeof(path), E5_FIXTURE_DST) == 0 &&
                   write_file(path,
                       "/* mislabeled Job stage: advances only, no cursor */\n"
                       "typedef int job_result_t;\n"
                       "#define JOB_ADVANCED 0\n"
                       "job_result_t fixture_stage_step_once(void){ return JOB_ADVANCED; }\n") == 0)
                  ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(E5_SCRIPT_REL, NULL) : -1;
    unlink_rel(E5_FIXTURE_DST);
    int recover_rc = run_gate_script(E5_SCRIPT_REL, NULL);
    TEST("[lint-gate] E5 stage-advances-or-blocks HARD: clean, trips advance-only stage, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* E6 — one-write-path RATCHET: a new production write surface outside the
 * baseline trips the gate; removing it restores green. */
static int t_e6_one_write_path(void)
{
    int failures = 0;
    unlink_rel(E6_FIXTURE_DST);
    int baseline_rc = run_gate_script(E6_SCRIPT_REL, NULL);
    char path[PATH_MAX];
    int planted = (repo_path(path, sizeof(path), E6_FIXTURE_DST) == 0 &&
                   write_file(path,
                       "struct active_chain; struct block_index;\n"
                       "int active_chain_set_tip(struct active_chain *, struct block_index *);\n"
                       "int e6_fixture(struct active_chain *c, struct block_index *b){ return active_chain_set_tip(c, b); }\n") == 0)
                  ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(E6_SCRIPT_REL, NULL) : -1;
    unlink_rel(E6_FIXTURE_DST);
    int recover_rc = run_gate_script(E6_SCRIPT_REL, NULL);
    TEST("[lint-gate] E6 one-write-path RATCHET: clean, trips new writer, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* E7 — no-authoritative-RAM-state RATCHET: a new direct active_chain
 * internals access trips the gate; removing it restores green. */
static int t_e7_no_authoritative_ram_state(void)
{
    int failures = 0;
    unlink_rel(E7_FIXTURE_DST);
    int baseline_rc = run_gate_script(E7_SCRIPT_REL, NULL);
    char path[PATH_MAX];
    int planted = (repo_path(path, sizeof(path), E7_FIXTURE_DST) == 0 &&
                   write_file(path,
                       "struct main_state { struct { int height; } chain_active; };\n"
                       "int e7_fixture(struct main_state *s){ return s->chain_active.height; }\n") == 0)
                  ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(E7_SCRIPT_REL, NULL) : -1;
    unlink_rel(E7_FIXTURE_DST);
    int recover_rc = run_gate_script(E7_SCRIPT_REL, NULL);
    TEST("[lint-gate] E7 no-authoritative-RAM-state RATCHET: clean, trips direct RAM state, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* E12 — honest witness (Law 7). The live tree is clean (FAIL mode passes:
 * every witness reads observable progress or carries a reviewed
 * // honest-witness-ok hatch). Plant a condition .c whose witness is a
 * PURE-INVERSE of detect ("return !detect_x()") — the canonical Law-7 lie
 * a no-op/self-certifying remedy hides behind — and assert the gate trips;
 * removing it restores green. This proves the gate has teeth and the
 * baseline is honestly empty. */
static int t_e12_honest_witness(void)
{
    int failures = 0;
    unlink_rel(E12_FIXTURE_DST);
    int baseline_rc = run_gate_script(E12_SCRIPT_REL, "FAIL");
    char path[PATH_MAX];
    int planted_good = (repo_path(path, sizeof(path), E12_FIXTURE_DST) == 0 &&
                        write_file(path,
                            "#include <stdbool.h>\n"
                            "#include <stdint.h>\n"
                            "static bool reducer_frontier_compute_hstar(void *db, int32_t *h, int32_t *s){\n"
                            "    (void)db; *h = 42; *s = 42; return true;\n"
                            "}\n"
                            "static bool witness_e12_frontier(int64_t t){\n"
                            "    int32_t hstar = -1;\n"
                            "    int32_t served = -1;\n"
                            "    return reducer_frontier_compute_hstar(0, &hstar, &served) && hstar >= (int)t;\n"
                            "}\n") == 0)
                       ? 0 : -1;
    int good_rc = planted_good == 0 ? run_gate_script(E12_SCRIPT_REL, "FAIL") : -1;
    unlink_rel(E12_FIXTURE_DST);
    int planted = (repo_path(path, sizeof(path), E12_FIXTURE_DST) == 0 &&
                   write_file(path,
                       "#include <stdbool.h>\n"
                       "#include <stdint.h>\n"
                       "static bool detect_e12_fixture(void){ return true; }\n"
                       "static bool witness_e12_fixture(int64_t t){\n"
                       "    (void)t;\n"
                       "    return !detect_e12_fixture();\n"
                       "}\n") == 0)
                  ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(E12_SCRIPT_REL, "FAIL") : -1;
    unlink_rel(E12_FIXTURE_DST);
    int recover_rc = run_gate_script(E12_SCRIPT_REL, "FAIL");
    TEST("[lint-gate] E12 honest-witness FAIL: accepts reducer H*, trips pure-inverse witness, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted_good == 0);
        ASSERT(good_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* Gate #21 background-worker lock-in (Shape 5 — Supervisor). The widened
 * check_supervisor_domain.sh ALSO scans the boot worker file and fails any
 * thread spawn not paired with supervisor_register_in_domain. This self-test
 * proves the widened scan has teeth and the (intentionally empty) baseline is
 * honest: plant a worker that spawns a pthread with NO register and assert the
 * gate FLAGS it (exit != 0); then a worker WITH a register and assert it
 * PASSES (exit == 0). Fixtures are fed via ZCL_SUPERVISOR_WORKER_FILES so the
 * result does not depend on the live boot_background_workers.c state. */
static int t_gate21_supervisor_worker_lockin(void)
{
    int failures = 0;
    char bad_path[PATH_MAX];
    char ok_path[PATH_MAX];
    if (repo_path(bad_path, sizeof(bad_path), SUPDOM_BAD_WORKER_REL) != 0 ||
        repo_path(ok_path, sizeof(ok_path), SUPDOM_OK_WORKER_REL) != 0) {
        fprintf(stderr,
                "[lint-gate] could not resolve supervisor worker fixture paths\n");
        return 1;
    }

    /* Unsupervised worker: spawns a thread, never registers a contract. */
    const char *bad =
        "/* fixture: boot background worker without a liveness contract */\n"
        "void *worker(void *a){ return a; }\n"
        "int boot_start_fixture_service(void){\n"
        "    pthread_create(&t, 0, worker, 0);\n"
        "    return 0;\n"
        "}\n";
    /* Same worker, now paired with a domain registration. */
    const char *ok =
        "/* fixture: boot background worker with a liveness contract */\n"
        "void *worker(void *a){ return a; }\n"
        "int boot_start_fixture_service(void){\n"
        "    pthread_create(&t, 0, worker, 0);\n"
        "    supervisor_register_in_domain(g_op_sup, &g_fixture_contract);\n"
        "    return 0;\n"
        "}\n";

    (void)unlink(bad_path);
    (void)unlink(ok_path);
    int planted_bad = write_file(bad_path, bad);
    int trip_rc = planted_bad == 0
        ? run_gate_script_with_worker_files(SUPDOM_SCRIPT_REL, "FAIL",
                                            SUPDOM_BAD_WORKER_REL)
        : -1;
    (void)unlink(bad_path);

    int planted_ok = write_file(ok_path, ok);
    int pass_rc = planted_ok == 0
        ? run_gate_script_with_worker_files(SUPDOM_SCRIPT_REL, "FAIL",
                                            SUPDOM_OK_WORKER_REL)
        : -1;
    (void)unlink(ok_path);

    TEST("[lint-gate] #21 supervisor worker lock-in: unsupervised spawn trips, registered passes") {
        ASSERT(planted_bad == 0);
        ASSERT(trip_rc != 0);
        ASSERT(planted_ok == 0);
        ASSERT(pass_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_coins_guard_gate_fails_loud_on_no_lookup_surface(void)
{
    int failures = 0;
    char test_tmp[PATH_MAX];
    char scan_dir[PATH_MAX];
    char fixture[PATH_MAX];

    if (repo_path(test_tmp, sizeof(test_tmp), "test-tmp") != 0 ||
        repo_path(scan_dir, sizeof(scan_dir),
                  "test-tmp/_coins_lookup_nohit_scan_dir") != 0) {
        fprintf(stderr,
                "[lint-gate] could not resolve coins no-hit fixture dir\n");
        return 1;
    }
    (void)mkdir(test_tmp, 0700);
    (void)mkdir(scan_dir, 0700);
    if (snprintf(fixture, sizeof(fixture), "%s/nohit.c", scan_dir) >=
        (int)sizeof(fixture)) {
        fprintf(stderr, "[lint-gate] coins no-hit fixture path too long\n");
        return 1;
    }

    (void)unlink(fixture);
    int planted = write_file(fixture,
        "int coins_lookup_nohit_fixture(void){ return 23; }\n");
    int trip_rc = planted == 0
        ? run_gate_script_with_env(
              "tools/scripts/check_coins_lookup_nullcheck.sh",
              "ZCL_COINS_LOOKUP_SCAN_DIR",
              scan_dir)
        : -1;
    (void)unlink(fixture);
    (void)rmdir(scan_dir);

    int green_rc = run_gate_script(
        "tools/scripts/check_coins_lookup_nullcheck.sh", NULL);

    TEST("[lint-gate] coins lookup guard fails loud when lookup surface disappears") {
        ASSERT(planted == 0);
        ASSERT(trip_rc == 2);
        ASSERT(green_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* ── META-GATE: fail-silent gates are now fail-LOUD on an empty scan ──────────
 *
 * A hollow gate reports "clean" exit 0 while a real violation is present: its
 * scan set silently emptied (a renamed/moved dir) and the violation loop ran
 * zero times. The fix (docs/work/lint-gate-hollowness-audit.md) is a non-empty
 * scan-set preflight that aborts exit 2 when the scan set is below a known
 * floor. Each hardened gate exposes a ZCL_*_SCAN_* env override of its scan
 * root so this meta-gate can feed it a GUARANTEED-EMPTY dir and assert exit 2
 * — the direct proof that "scanned nothing" is no longer a quiet pass.
 *
 * For each gate: (1) point its scan override at an empty dir → assert exit 2;
 * (2) run it with NO override → assert exit 0 (the real tree still passes).
 * This is the "plant → assert trip → remove → assert green" pattern, with the
 * empty scan dir as the planted fixture. */
/* One gate's empty-scan check: feed the gate an empty scan dir via its
 * override env var → assert exit 2 (fail-LOUD); run with no override → assert
 * exit 0 (real tree still clean). One TEST block per call (the TEST macro
 * defines a function-scoped `_test_next` label, so it must not repeat in a
 * single function). Returns 0 on pass, nonzero on failure. */
static int meta_gate_empty_scan_trips(const char *script_rel,
                                      const char *env_name,
                                      const char *empty_value)
{
    int failures = 0;
    int trip_rc = run_gate_script_with_env(script_rel, env_name, empty_value);
    int green_rc = run_gate_script(script_rel, NULL);
    TEST("[lint-gate] META: empty/drifted scan trips gate exit 2, real tree passes") {
        /* Empty scan set MUST be exit 2 (fail-LOUD), never 0 (hollow) and
         * never 1 (a violation it could not actually have seen). */
        if (trip_rc != 2) {
            fprintf(stderr,
                    "[lint-gate] %s with empty %s: expected exit 2, got %d "
                    "(hollow gate?)\n", script_rel, env_name, trip_rc);
        }
        ASSERT(trip_rc == 2);
        if (green_rc != 0) {
            fprintf(stderr,
                    "[lint-gate] %s with no override: expected exit 0, got %d\n",
                    script_rel, green_rc);
        }
        ASSERT(green_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* META-GATE: every gate hardened this wave must FAIL LOUD (exit 2) on an empty
 * scan set instead of reporting "clean" exit 0 (a hollow pass). Each gate
 * exposes a ZCL_*_SCAN_* override of its scan root so we can point it at a
 * guaranteed-empty dir (it EXISTS — a bare -d check would pass — but holds zero
 * source files, the exact hollow vector). See
 * docs/work/lint-gate-hollowness-audit.md. */
/* Run a hot-swap manifest gate against a specific manifest fixture by exporting
 * ZCL_HOTSWAP_MANIFEST (resolved to an absolute path). */
static int run_hotswap_gate_with_manifest(const char *script_rel,
                                          const char *manifest_rel)
{
    char manifest_abs[PATH_MAX];
    if (repo_path(manifest_abs, sizeof(manifest_abs), manifest_rel) != 0)
        return -1;
    return run_gate_script_with_env(script_rel, "ZCL_HOTSWAP_MANIFEST",
                                    manifest_abs);
}

static int t_hotswap_eligible_scope_gate(void)
{
    int failures = 0;
    TEST("hotswap eligible-scope gate: real manifest passes, forbidden root trips") {
        /* The committed manifest is all app-layer surfaces → clean (exit 0). */
        ASSERT(run_hotswap_gate_with_manifest(HOTSWAP_SCOPE_SCRIPT_REL,
                                              HOTSWAP_MANIFEST_REL) == 0);
        /* A seeded manifest that lists a lib/consensus TU trips the gate
         * (exit 1) — proof it is not hollow. */
        ASSERT(run_hotswap_gate_with_manifest(HOTSWAP_SCOPE_SCRIPT_REL,
                                              HOTSWAP_BAD_SCOPE_MANIFEST_REL) == 1);
        /* An app-layer TU that does NOT invoke ZCL_HOTSWAP_EXPORT_ROUTES (so it
         * could never actually export a generation) also trips the gate —
         * proof the macro-presence check added in Wave 3.1 is not hollow. */
        ASSERT(run_hotswap_gate_with_manifest(HOTSWAP_SCOPE_SCRIPT_REL,
                                              HOTSWAP_NO_MACRO_MANIFEST_REL) == 1);
        /* Recovery: back on the real manifest the gate passes again. */
        ASSERT(run_hotswap_gate_with_manifest(HOTSWAP_SCOPE_SCRIPT_REL,
                                              HOTSWAP_MANIFEST_REL) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_hotswap_static_state_gate(void)
{
    int failures = 0;
    TEST("hotswap static-state gate: real manifest passes, mutable static trips") {
        /* Every committed eligible TU is free of mutable file-scope statics. */
        ASSERT(run_hotswap_gate_with_manifest(HOTSWAP_STATIC_SCRIPT_REL,
                                              HOTSWAP_MANIFEST_REL) == 0);
        /* A seeded manifest that points at a fixture TU carrying a mutable
         * file-scope static trips the gate (exit 1). */
        ASSERT(run_hotswap_gate_with_manifest(HOTSWAP_STATIC_SCRIPT_REL,
                                              HOTSWAP_BAD_STATIC_MANIFEST_REL) == 1);
        /* Recovery. */
        ASSERT(run_hotswap_gate_with_manifest(HOTSWAP_STATIC_SCRIPT_REL,
                                              HOTSWAP_MANIFEST_REL) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_lint_gates_fail_loud_on_empty_scan(void)
{
    int failures = 0;

    /* A guaranteed-empty scan dir under the repo's test-tmp. mkdir is
     * idempotent; we never write into it, so it stays empty. */
    char empty_dir[PATH_MAX];
    if (repo_path(empty_dir, sizeof(empty_dir),
                  "test-tmp/_lint_empty_scan_dir") != 0) {
        fprintf(stderr, "[lint-gate] could not resolve empty-scan dir path\n");
        return 1;
    }
    (void)mkdir(empty_dir, 0700);

    failures += meta_gate_empty_scan_trips(
        "tools/scripts/check_one_write_path.sh", "ZCL_OWP_SCAN_ROOTS", empty_dir);
    failures += meta_gate_empty_scan_trips(
        "tools/scripts/check_projections_pure.sh", "ZCL_PROJ_SCAN_DIR", empty_dir);
    failures += meta_gate_empty_scan_trips(
        "tools/scripts/check_stage_advances_or_blocks.sh", "ZCL_JOBS_DIR", empty_dir);
    failures += meta_gate_empty_scan_trips(
        "tools/scripts/check_supervisor_registration.sh", "ZCL_SERVICES_DIR", empty_dir);
    failures += meta_gate_empty_scan_trips(
        "tools/scripts/check_coins_lookup_nullcheck.sh", "ZCL_COINS_LOOKUP_SCAN_DIR", empty_dir);
    failures += meta_gate_empty_scan_trips(
        "tools/scripts/check_no_secret_printf.sh", "ZCL_SECRET_PRINTF_SCAN_DIRS", empty_dir);
    failures += meta_gate_empty_scan_trips(
        "tools/lint/check_supervisor_domain.sh", "ZCL_SUPDOM_SCAN_ROOTS", empty_dir);

    /* The reorg-ratchet gate's hollow vector is a DRIFTED file list (a tracked
     * stage-log store moved/renamed), not an empty scan dir. Point its file
     * override at a nonexistent path and assert the drifted-surface preflight
     * fires exit 2 (the old code exit 1'd / a swallowed grep would mask it). */
    failures += meta_gate_empty_scan_trips(
        "tools/scripts/gate_stage_log_reorg_unsafe_ratchet.sh",
        "ZCL_REORG_RATCHET_FILES", "/nonexistent/_lint_missing_store.c");

    (void)rmdir(empty_dir);
    return failures;
}

static void unlink_lint_fixtures(void)
{
    const char *fixtures[] = {
        FIXTURE_DST_REL,
        NODE_DB_EXEC_FIXTURE_DST_REL,
        COINS_FIXTURE_DST_REL,
        OBS_FIXTURE_DST_REL,
        OBS_OK_FIXTURE_DST_REL,
        RAW_MALLOC_FIXTURE_DST_REL,
        RAW_MALLOC_OK_FIXTURE_DST_REL,
        E1_FIXTURE_DST,
        E10_SHAPE_FIXTURE_DST,
        E10_SQL_FIXTURE_DST,
        MODEL_AR_FIXTURE_DST,
        E2_FIXTURE_DST,
        E3_FIXTURE_DST,
        E4_FIXTURE_DST,
        DOMAIN_PURITY_FIXTURE_DST,
        E5_FIXTURE_DST,
        E6_FIXTURE_DST,
        E7_FIXTURE_DST,
        E12_FIXTURE_DST,
        SUPDOM_BAD_WORKER_REL,
        SUPDOM_OK_WORKER_REL,
    };

    for (size_t i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
        char path[PATH_MAX];
        if (repo_path(path, sizeof(path), fixtures[i]) == 0)
            (void)unlink(path);
    }
}

static int t_raw_malloc_fixture_trips_gate(void)
{
    int failures = 0;
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_dst, sizeof(fixture_dst), RAW_MALLOC_FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve raw_malloc fixture path\n");
        return 1;
    }
    (void)unlink(fixture_dst);
    const char *bad = "/* fixture */\n#include <stdlib.h>\nvoid *f(void){return malloc(16);}\n";
    if (write_file(fixture_dst, bad) != 0) {
        fprintf(stderr, "[lint-gate] could not plant raw_malloc fixture — aborting\n");
        return 1;
    }
    int rc = run_check_raw_malloc_script();
    (void)unlink(fixture_dst);
    TEST("[lint-gate] raw malloc fixture trips the gate (exit != 0)") {
        ASSERT(rc != 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_raw_malloc_zcl_fixture_passes(void)
{
    int failures = 0;
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_dst, sizeof(fixture_dst), RAW_MALLOC_OK_FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve raw_malloc-ok fixture path\n");
        return 1;
    }
    unlink_lint_fixtures();
    const char *good =
        "/* fixture */\n"
        "#include \"util/safe_alloc.h\"\n"
        "void *f(void){return zcl_malloc(16, \"fixture\");}\n";
    if (write_file(fixture_dst, good) != 0) {
        fprintf(stderr, "[lint-gate] could not plant raw_malloc-ok fixture — aborting\n");
        return 1;
    }
    int rc = run_check_raw_malloc_script();
    unlink_lint_fixtures();
    TEST("[lint-gate] zcl_malloc-only fixture passes the gate (exit == 0)") {
        ASSERT(rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_raw_malloc_gate_recovers(void)
{
    int failures = 0;
    char fixture_dst1[PATH_MAX];
    char fixture_dst2[PATH_MAX];
    if (repo_path(fixture_dst1, sizeof(fixture_dst1), RAW_MALLOC_FIXTURE_DST_REL) != 0 ||
        repo_path(fixture_dst2, sizeof(fixture_dst2), RAW_MALLOC_OK_FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve raw_malloc fixture paths\n");
        return 1;
    }
    (void)fixture_dst1;
    (void)fixture_dst2;
    unlink_lint_fixtures();
    TEST("[lint-gate] raw_malloc gate passes after fixtures removed") {
        ASSERT(run_check_raw_malloc_script() == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_coins_guard_gate_recovers(void)
{
    int failures = 0;
    char fixture_dst[PATH_MAX];
    if (repo_path(fixture_dst, sizeof(fixture_dst), COINS_FIXTURE_DST_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve coins guard fixture path\n");
        return 1;
    }
    (void)unlink(fixture_dst);
    TEST("[lint-gate] guarded coin-lookups pass again after fixture removed") {
        ASSERT(run_check_coins_lookup_nullcheck() == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_tools_z_mirror_fallback_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("tools/z mirror fallback preserves local authority contract") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "tools/z") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf,
                      "\"consensus_authority\":\"local_consensus_validation\"")
               != NULL);
        ASSERT(strstr(buf, "\"candidate_source\":\"legacy_advisory\"")
               != NULL);
        ASSERT(strstr(buf, "\"candidate_trust\":\"bounded_advisory_fallback\"")
               != NULL);
        ASSERT(strstr(buf,
                      "\"legacy_advisory_gated_by_native_retries\":false")
               != NULL);
        ASSERT(strstr(buf, "mirror_authorization_enabled") == NULL);
        ASSERT(strstr(buf, "mirror_source_trust") == NULL);
        ASSERT(strstr(buf, "\"blockers_total\":0") != NULL);
        ASSERT(strstr(buf, "\"stalls_total\":0") != NULL);
        ASSERT(strstr(buf, "\"unsafe_overrides_total\":0") != NULL);
        ASSERT(strstr(buf, "\"last_override_safe\":false") != NULL);
        ASSERT(strstr(buf, "\"last_override_scope\":\"\"") != NULL);
        ASSERT(strstr(buf, "active_detail=\"\"") != NULL);
        ASSERT(strstr(buf,
                      "active_detail=\"$(json_field \"$json\" last_error)\"")
               != NULL);
        ASSERT(strstr(buf,
                      "active_error_detail \"$active_detail\"") != NULL);
        ASSERT(strstr(buf, "ZCL_TOOLS_Z_SELFTEST") != NULL);
        ASSERT(strstr(buf, "blocker_recovered_by_tip_agreement") != NULL);
        ASSERT(strstr(buf, "tip_hashes_agree") != NULL);
        ASSERT(strstr(buf, "stale same-hash active error not cleared")
               != NULL);
        ASSERT(run_gate_script_with_env("tools/z",
                                        "ZCL_TOOLS_Z_SELFTEST",
                                        "1") == 0);
        ASSERT(strstr(buf,
                      "LEGACY_RPCPORT:-${LEGACY_RPC_PORT:-$(legacy_conf_value rpcport)}")
               != NULL);
        ASSERT(strstr(buf, "systemd_exec_arg()") != NULL);
        ASSERT(strstr(buf, "systemd_exec_arg_service()") != NULL);
        ASSERT(strstr(buf, "systemd_show_value()") != NULL);
        ASSERT(strstr(buf, "systemctl --user show \"$service\" -p \"$prop\" --value")
               != NULL);
        ASSERT(strstr(buf, "systemd_exec_arg_service zclassicd port")
               != NULL);
        ASSERT(strstr(buf, "DATADIR=\"${ZCL_DATADIR:-$DEFAULT_DATADIR}\"")
               != NULL);
        ASSERT(strstr(buf, "SERVICE_DATADIR=\"$(systemd_exec_arg datadir || true)\"")
               != NULL);
        ASSERT(strstr(buf, "RPCPORT=\"${ZCL_RPCPORT:-18232}\"") != NULL);
        ASSERT(strstr(buf, "SERVICE_RPCPORT=\"$(systemd_exec_arg rpcport || true)\"")
               != NULL);
        ASSERT(strstr(buf, "CLI_OPTS=(\"-datadir=$DATADIR\" \"-rpcport=$RPCPORT\")")
               != NULL);
        ASSERT(strstr(buf, "\"$CLI\" \"${CLI_OPTS[@]}\"") != NULL);
        ASSERT(strstr(buf, "lag_display=unknown") != NULL);
        ASSERT(strstr(buf, "candidate_lag_display=unknown") != NULL);
        ASSERT(strstr(buf, "\"$lag_display\"") != NULL);
        ASSERT(strstr(buf, "\"$candidate_lag_display\"") != NULL);
        ASSERT(strstr(buf, "lag_valid=\"$(json_field \"$json\" lag_valid)\"")
               != NULL);
        ASSERT(strstr(buf,
                      "candidate_lag_valid=\"$(json_field \"$json\" candidate_lag_valid)\"")
               != NULL);
        ASSERT(strstr(buf, "lag_valid=false") != NULL);
        ASSERT(strstr(buf, "lag_valid=true") != NULL);
        ASSERT(strstr(buf, "json_put_bool \"$json\" lag_valid \"$lag_valid\"")
               != NULL);
        ASSERT(strstr(buf,
                      "json_put_bool \"$json\" candidate_lag_valid \"$candidate_lag_valid\"")
               != NULL);
        ASSERT(strstr(buf, "json_put_bool \"$json\" lag_valid \"$lag_known\"")
               == NULL);
        ASSERT(strstr(buf,
                      "json_put_bool \"$json\" candidate_lag_valid \"$lag_known\"")
               == NULL);
        ASSERT(strstr(buf,
                      "\"$reachable\" \"$reachable\" \"$lag_known\" \"$lag_valid\"")
               != NULL);
        ASSERT(strstr(buf,
                      "active_error_detail=\"$(json_field \"$JSON\" active_error_detail)\"")
               != NULL);
        ASSERT(strstr(buf, "active_error_detail=%s") != NULL);
        ASSERT(strstr(buf, "blockers=%s") != NULL);
        ASSERT(strstr(buf, "stalls=%s") != NULL);
        ASSERT(strstr(buf, "unsafe_overrides=%s") != NULL);
        ASSERT(strstr(buf, "last_override_safe=%s") != NULL);
        ASSERT(strstr(buf, "legacy_advisory_gated=%s") != NULL);
        ASSERT(strstr(buf, "mirror_gated=%s") == NULL);
        ASSERT(strstr(buf, "\"consensus_authority\":\"zclassicd\"") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_service_tip_mutation_gate(void)
{
    int failures = 0;
    TEST("[lint-gate] services do not bypass canonical tip publication") {
        ASSERT(run_check_service_tip_mutation_gate() == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_legacy_candidate_source_has_no_override_scope(void)
{
    int failures = 0;
    char *mirror = NULL;
    TEST("legacy mirror monitor has no tip-mutation path") {
        char mirror_path[PATH_MAX];
        ASSERT(repo_path(mirror_path, sizeof(mirror_path),
                         "app/services/src/legacy_mirror_sync_service.c") == 0);
        ASSERT(read_entire_file(mirror_path, &mirror) == 0);
        ASSERT(strstr(mirror, "CSR_ROLLBACK_SOURCE_MIRROR") == NULL);
        ASSERT(strstr(mirror, "chain_set_active_tip(") == NULL);
        ASSERT(strstr(mirror, "active_chain_set_tip(") == NULL);
        PASS();
    } _test_next:;
    free(mirror);
    return failures;
}

static int t_tools_z_operator_diagnostics_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("tools/z exposes canonical operator diagnostics") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "tools/z") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "network|net)") != NULL);
        ASSERT(strstr(buf, "rpc getnetworkinfo") != NULL);
        ASSERT(strstr(buf, "status|health)") != NULL);
        ASSERT(strstr(buf, "rpc healthcheck") != NULL);
        ASSERT(strstr(buf, "case \"${1:-agent}\" in") != NULL);
        ASSERT(strstr(buf, "agent|summary|operator|ok|doctor)") != NULL);
        ASSERT(strstr(buf, "/api/v1/agent") != NULL);
        ASSERT(strstr(buf, "Binary: zclassic23 agent") != NULL);
        ASSERT(strstr(buf, "MCP:  zcl_agent") != NULL);
        ASSERT(strstr(buf, "deprecated compatibility shim") != NULL);
        ASSERT(strstr(buf, "do not add new operator logic here") != NULL);
        ASSERT(strstr(buf, "topology|availability|uptime)") != NULL);
        ASSERT(strstr(buf, "topology_status_json") != NULL);
        ASSERT(strstr(buf, "zcl.operator_topology.v1") != NULL);
        ASSERT(strstr(buf, "legacy_compatible_peers") != NULL);
        ASSERT(strstr(buf, "zclassicd P2P port drift") != NULL);
        ASSERT(strstr(buf, "advance|chain-advance)") != NULL);
        ASSERT(strstr(buf, "rpc dumpstate chain_advance_coordinator") != NULL);
        ASSERT(strstr(buf, "peerlife|peer-lifecycle)") != NULL);
        ASSERT(strstr(buf, "rpc dumpstate peer_lifecycle") != NULL);
        ASSERT(strstr(buf, "state|dumpstate)") != NULL);
        ASSERT(strstr(buf, "rpc dumpstate \"$2\"") != NULL);
        ASSERT(strstr(buf, "log|nodelog|node-log)") != NULL);
        ASSERT(strstr(buf, "rpc getnodelog \"$2\"") != NULL);
        ASSERT(strstr(buf, "sql|dbquery)") != NULL);
        ASSERT(strstr(buf, "rpc dbquery \"$2\"") != NULL);
        ASSERT(strstr(buf, "P2P reachability and handshake summary") != NULL);
        ASSERT(strstr(buf,
                      "Service ports, peer counts, mirror lag, and drift checks")
               != NULL);
        ASSERT(strstr(buf,
                      "Chain advance source scoring and selection blockers")
               != NULL);
        ASSERT(strstr(buf,
                      "Peer lifecycle attempts, handshakes, failures by source")
               != NULL);
        ASSERT(strstr(buf, "Generic dumpstate diagnostics") != NULL);
        ASSERT(strstr(buf, "Server-side node.log regex tail") != NULL);
        ASSERT(strstr(buf, "SELECT-only node.db query") != NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "docs/RUNBOOK.md") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "sources[].selectable=false") != NULL);
        ASSERT(strstr(buf, "selection_blocker") != NULL);
        ASSERT(strstr(buf, "initialized=true") != NULL);
        ASSERT(strstr(buf, "has_connman=true") != NULL);
        ASSERT(strstr(buf, "has_main_state=true") != NULL);
        ASSERT(strstr(buf, "has_node_db=true") != NULL);
        ASSERT(strstr(buf, "blockers_total") != NULL);
        ASSERT(strstr(buf, "stalls_total") != NULL);
        ASSERT(strstr(buf, "unsafe_overrides_total") != NULL);
        ASSERT(strstr(buf, "last_override_safe") != NULL);
        ASSERT(strstr(buf, "last_override_scope") != NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "tools/deploy_verify.sh") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "canonical diagnostics ready") != NULL);
        ASSERT(strstr(buf, "chain_advance_coordinator") != NULL);
        ASSERT(strstr(buf, "local_consensus_validation") != NULL);
        ASSERT(strstr(buf, "ZCL_DATADIR=$RPC_DATADIR") != NULL);
        ASSERT(strstr(buf, "ZCL_RPCPORT=$RPCPORT") != NULL);
        ASSERT(strstr(buf, "ZCL_DEPLOY_NODE_LOG") != NULL);
        ASSERT(strstr(buf, "pre-RPC recovery: reindex-chainstate") != NULL);
        ASSERT(strstr(buf, "zclassic-cli|zcl-rpc") == NULL);
        ASSERT(strstr(buf, "json_rpc_result") != NULL);
        ASSERT(strstr(buf, "checks.get(\"log_head\")") != NULL);
        ASSERT(strstr(buf, "checks_ca.get(\"local_height\")") != NULL);
        ASSERT(strstr(buf, "handshaked_connections") != NULL);
        ASSERT(strstr(buf, "legacy_compatible_peers") != NULL);
        ASSERT(strstr(buf, "legacy_magicbean_peers") != NULL);
        ASSERT(strstr(buf, "peer_lifecycle") != NULL);
        ASSERT(strstr(buf, "legacy_mirror") != NULL);
        ASSERT(strstr(buf, "blockers_total") != NULL);
        ASSERT(strstr(buf, "stalls_total") != NULL);
        ASSERT(strstr(buf, "unsafe_overrides_total") != NULL);
        ASSERT(strstr(buf, "unsafe_overrides_total 0") != NULL);
        ASSERT(strstr(buf, "last_override_safe") != NULL);
        ASSERT(strstr(buf, "last_override_scope") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_dev_lane_deploy_contract(void)
{
    int failures = 0;
    char *script = NULL;
    char *guard = NULL;
    char *lane_health = NULL;
    char *lane_recover = NULL;
    char *agent_status = NULL;
    char *clear_script = NULL;
    char *agent_doctor = NULL;
    char *handoff = NULL;
    char *makefile = NULL;
    char *live_unit = NULL;
    char *soak_unit = NULL;
    char *dev_unit = NULL;
    char *boot_index = NULL;
    char *coldstart = NULL;
    char *coldstart_tip = NULL;
    TEST("dev lane deploy self-cleans stale reindex override") {
        char script_path[PATH_MAX];
        char guard_path[PATH_MAX];
        char lane_health_path[PATH_MAX];
        char lane_recover_path[PATH_MAX];
        char agent_status_path[PATH_MAX];
        char clear_script_path[PATH_MAX];
        char agent_doctor_path[PATH_MAX];
        char handoff_path[PATH_MAX];
        char makefile_path[PATH_MAX];
        char live_unit_path[PATH_MAX];
        char soak_unit_path[PATH_MAX];
        char dev_unit_path[PATH_MAX];
        char boot_index_path[PATH_MAX];
        char coldstart_path[PATH_MAX];
        char coldstart_tip_path[PATH_MAX];
        ASSERT(repo_path(script_path, sizeof(script_path),
                         "tools/dev/deploy-dev-lane.sh") == 0);
        ASSERT(repo_path(guard_path, sizeof(guard_path),
                         "tools/deploy_guard.sh") == 0);
        ASSERT(repo_path(lane_health_path, sizeof(lane_health_path),
                         "tools/scripts/lane_health.sh") == 0);
        ASSERT(repo_path(lane_recover_path, sizeof(lane_recover_path),
                         "tools/scripts/lane_recover.sh") == 0);
        ASSERT(repo_path(agent_status_path, sizeof(agent_status_path),
                         "tools/dev/agent-dev-status.sh") == 0);
        ASSERT(repo_path(clear_script_path, sizeof(clear_script_path),
                         "tools/dev/agent-clear-stale-reindex.sh") == 0);
        ASSERT(repo_path(agent_doctor_path, sizeof(agent_doctor_path),
                         "tools/dev/agent-doctor.sh") == 0);
        ASSERT(repo_path(handoff_path, sizeof(handoff_path),
                         "docs/HANDOFF.md") == 0);
        ASSERT(repo_path(makefile_path, sizeof(makefile_path),
                         "Makefile") == 0);
        ASSERT(repo_path(live_unit_path, sizeof(live_unit_path),
                         "deploy/zclassic23.service") == 0);
        ASSERT(repo_path(soak_unit_path, sizeof(soak_unit_path),
                         "deploy/examples/zclassic23-soak-node.service") == 0);
        ASSERT(repo_path(dev_unit_path, sizeof(dev_unit_path),
                         "deploy/zcl23-dev.service") == 0);
        ASSERT(repo_path(boot_index_path, sizeof(boot_index_path),
                         "config/src/boot_index.c") == 0);
        ASSERT(repo_path(coldstart_path, sizeof(coldstart_path),
                         "tools/scripts/cold_start_test.sh") == 0);
        ASSERT(repo_path(coldstart_tip_path, sizeof(coldstart_tip_path),
                         "tools/scripts/cold_start_to_tip_probe.sh") == 0);
        ASSERT(read_entire_file(script_path, &script) == 0);
        ASSERT(read_entire_file(guard_path, &guard) == 0);
        ASSERT(read_entire_file(lane_health_path, &lane_health) == 0);
        ASSERT(read_entire_file(lane_recover_path, &lane_recover) == 0);
        ASSERT(read_entire_file(agent_status_path, &agent_status) == 0);
        ASSERT(read_entire_file(clear_script_path, &clear_script) == 0);
        ASSERT(read_entire_file(agent_doctor_path, &agent_doctor) == 0);
        ASSERT(read_entire_file(handoff_path, &handoff) == 0);
        ASSERT(read_entire_file(makefile_path, &makefile) == 0);
        ASSERT(read_entire_file(live_unit_path, &live_unit) == 0);
        ASSERT(read_entire_file(soak_unit_path, &soak_unit) == 0);
        ASSERT(read_entire_file(dev_unit_path, &dev_unit) == 0);
        ASSERT(read_entire_file(boot_index_path, &boot_index) == 0);
        ASSERT(read_entire_file(coldstart_path, &coldstart) == 0);
        ASSERT(read_entire_file(coldstart_tip_path, &coldstart_tip) == 0);

        ASSERT(strstr(script, "STALE_REINDEX_DROPIN=") != NULL);
        ASSERT(strstr(script, "zcl23-dev.service.d/reindex.conf") != NULL);
        ASSERT(strstr(script, "ZCL_DEV_ALLOW_REINDEX_DROPIN") != NULL);
        ASSERT(strstr(script, "removing stale reindex drop-in") != NULL);
        ASSERT(strstr(script, "rm -f \"$STALE_REINDEX_DROPIN\"") != NULL);
        ASSERT(strstr(script, "STALE_OOM_BUDGET_DROPIN=") != NULL);
        ASSERT(strstr(script, "zcl23-dev.service.d/zz-oom-budget.conf") != NULL);
        ASSERT(strstr(script, "ZCL_DEV_ALLOW_OOM_BUDGET_DROPIN") != NULL);
        ASSERT(strstr(script, "removing stale memory-budget drop-in") != NULL);
        ASSERT(strstr(script, "deploy/zcl23-dev.service owns the dev lane memory budget") != NULL);
        ASSERT(strstr(script, "rm -f \"$STALE_OOM_BUDGET_DROPIN\"") != NULL);
        ASSERT(strstr(script, "AUTO_REINDEX_SENTINEL=") != NULL);
        ASSERT(strstr(script, "auto_reindex_request") != NULL);
        ASSERT(strstr(script, "auto_reindex_status") != NULL);
        ASSERT(strstr(script, "guard_pending_auto_reindex") != NULL);
        ASSERT(strstr(script, "ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY") != NULL);
        ASSERT(strstr(script, "unreadable auto-reindex marker") != NULL);
        ASSERT(strstr(script, "ignoring malformed auto-reindex marker") != NULL);
        ASSERT(strstr(script, "pending crash-only auto-reindex request")
               != NULL);
        ASSERT(strstr(script, "refusing to start or hot-swap the dev lane")
               != NULL);
        ASSERT(strstr(script, "pre_rpc_boot_diagnostic") != NULL);
        ASSERT(strstr(script, "pre-RPC recovery: reindex-chainstate")
               != NULL);
        ASSERT(strstr(script, "boot diagnostic: $diag") != NULL);
        ASSERT(strstr(script, "DEPLOY_STATE=") != NULL);
        ASSERT(strstr(script, "agent-deploy.json") != NULL);
        ASSERT(strstr(script, "write_deploy_state") != NULL);
        ASSERT(strstr(script, "zcl.agent_dev_deploy.v1") != NULL);
        ASSERT(strstr(script, "\"verify_status\"") != NULL);
        ASSERT(strstr(script, "\"auto_reindex_pending\"") != NULL);
        ASSERT(strstr(script, "deploy state: $DEPLOY_STATE") != NULL);
        ASSERT(strstr(script, "BUILD_ID_DROPIN=") != NULL);
        ASSERT(strstr(script, "zcl23-dev.service.d/90-build-identity.conf") != NULL);
        ASSERT(strstr(script, "ZCL_AGENT_EXPECT_BUILD_COMMIT") != NULL);
        ASSERT(strstr(script, "ZCL_AGENT_EXPECT_BUILD_SOURCE=deploy-dev") != NULL);
        ASSERT(strstr(script, "ZCL_DEV_DEPLOY_BUILD") != NULL);
        ASSERT(strstr(script, "make fast-rebuild") != NULL);
        ASSERT(strstr(script, "build/bin/zclassic23-dev") != NULL);
        ASSERT(strstr(script, "case \"$DEV_DEPLOY_BUILD\"") != NULL);
        ASSERT(strstr(script, "strict)") != NULL);
        ASSERT(strstr(makefile, "deploy-dev-fast agent-deploy-fast") != NULL);
        ASSERT(strstr(script, "probe_agent_contract") != NULL);
        ASSERT(strstr(script, "ZCL_DEV_AGENT_TIMEOUT") != NULL);
        ASSERT(strstr(script, "agent_work_ready") != NULL);
        ASSERT(strstr(script, "chain_serving_ready") != NULL);
        ASSERT(strstr(script, "AGENT READY") != NULL);
        ASSERT(strstr(script, "BLOCKED: agent status=") != NULL);
        ASSERT(strstr(script, "SYNC OK") != NULL);
        ASSERT(strstr(script, "HEALTHY:") == NULL);
        ASSERT(strstr(agent_status, "\"deploy_blocker\"") != NULL);
        ASSERT(strstr(agent_status, "\"worker_lane\"") != NULL);
        ASSERT(strstr(agent_status, "\"role\":\"worker\"") != NULL);
        ASSERT(strstr(agent_status, "noncanonical_dev_only") != NULL);
        ASSERT(strstr(agent_status, "never_touches_live_or_soak") != NULL);
        ASSERT(strstr(agent_status, "make agent-stage-dev") != NULL);
        ASSERT(strstr(agent_status, "make agent-dev-recover") != NULL);
        ASSERT(strstr(agent_status,
                      "pending_auto_reindex_requires_explicit_recovery_boot")
               != NULL);
        ASSERT(strstr(agent_status,
                      "auto_reindex_stale_candidate") != NULL);
        ASSERT(strstr(agent_status,
                      "make agent-clear-stale-dev-reindex") != NULL);
        ASSERT(strstr(agent_status,
                      "ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY=1") != NULL);
        ASSERT(strstr(clear_script, "zcl.agent_dev_reindex_clear.v1") != NULL);
        ASSERT(strstr(clear_script, "stale_marker_proven") != NULL);
        ASSERT(strstr(clear_script, "marker_anchor_above_served_height")
               != NULL);
        ASSERT(strstr(clear_script, "mv \"$MARKER\" \"$archive\"") != NULL);
        ASSERT(strstr(clear_script, "systemctl --user show") != NULL);
        ASSERT(strstr(clear_script, "getblockcount") != NULL);
        ASSERT(strstr(clear_script, "python") == NULL);
        ASSERT(strstr(agent_doctor, "deploy_blocker=") != NULL);
        ASSERT(strstr(makefile, "agent-clear-stale-dev-reindex:") != NULL);
        ASSERT(strstr(makefile,
                      "tools/dev/agent-clear-stale-reindex.sh") != NULL);
        ASSERT(strstr(script, "systemctl --user daemon-reload") != NULL);
        ASSERT(strstr(makefile,
                      "./tools/deploy_guard.sh canonical-deploy") != NULL);
        ASSERT(strstr(makefile,
                      "zclassic23.service.d/90-build-identity.conf") != NULL);
        ASSERT(strstr(makefile,
                      "ZCL_AGENT_EXPECT_BUILD_SOURCE=make-deploy") != NULL);
        ASSERT(strstr(makefile,
                      "bash tools/scripts/cold_start_test.sh; rc=$$?")
               != NULL);
        ASSERT(strstr(makefile,
                      "$(MAKE) --no-print-directory ci-coldstart; rc=$$?")
               == NULL);
        ASSERT(strstr(makefile,
                      "GNU make returns 2 for a failed recipe") != NULL);
        ASSERT(strstr(coldstart, "SRC_BUNDLE_SNAP_CANDIDATES") != NULL);
        ASSERT(strstr(coldstart, "-load-snapshot-at-own-height") != NULL);
        ASSERT(strstr(coldstart, "grep -m1 -F -- \"$BUNDLE_SUCCESS_PATTERN\"")
               != NULL);
        ASSERT(strstr(coldstart, "fast_rebuild_authority_ready") != NULL);
        ASSERT(strstr(coldstart, "consensus_snapshot.db above the compiled checkpoint")
               != NULL);
        ASSERT(strstr(coldstart, "python") == NULL);
        ASSERT(strstr(coldstart_tip, "BUNDLE_SNAP_CANDIDATES") != NULL);
        ASSERT(strstr(coldstart_tip, "127.0.0.1:8033") != NULL);
        ASSERT(strstr(coldstart_tip, "-load-snapshot-at-own-height") != NULL);
        ASSERT(strstr(coldstart_tip, "BUNDLE_SUCCESS_PATTERN") != NULL);
        ASSERT(strstr(coldstart_tip, "zcl.c3_probe_artifact.v2") != NULL);
        ASSERT(strstr(coldstart_tip, "proof.json") != NULL);
        ASSERT(strstr(coldstart_tip, "\"seed_authority_loaded\"") != NULL);
        ASSERT(strstr(coldstart_tip, "\"reached_at_tip\"") != NULL);
        ASSERT(strstr(coldstart_tip,
                      "write_artifact \"skip\" 2 \"$*\"") != NULL);
        ASSERT(strstr(coldstart_tip,
                      "seed authority loaded but forward-sync did not complete")
               != NULL);
        ASSERT(strstr(coldstart_tip, "python") == NULL);
        ASSERT(strstr(makefile, "mvp-coldstart-to-tip-local:") != NULL);
        ASSERT(strstr(makefile, "tools/scripts/cold_start_to_tip_probe.sh")
               != NULL);
        ASSERT(strstr(makefile, "lane-recover:") != NULL);
        ASSERT(strstr(makefile, "tools/scripts/lane_recover.sh") != NULL);
        ASSERT(strstr(guard, "ZCL_DEPLOY_ALLOW_CANONICAL") != NULL);
        ASSERT(strstr(guard, "zcl.agent_deploy_guard.v1") != NULL);
        ASSERT(strstr(guard, "agentdeployguard") != NULL);
        ASSERT(strstr(guard, "ZCL_DEPLOY_GUARD_NATIVE_JSON") != NULL);
        ASSERT(strstr(guard,
                      "ZCL_DEPLOY_GUARD_RPC_TOOL=/nonexistent-zclassic23")
               != NULL);
        ASSERT(strstr(guard, "python") == NULL);
        ASSERT(strstr(guard, "systemctl --user show") != NULL);
        ASSERT(strstr(guard, "-operator-lane") != NULL);
        ASSERT(strstr(guard, "native agentdeployguard blocks") != NULL);
        ASSERT(strstr(guard, "allowed") != NULL);
        ASSERT(strstr(guard, "deploy-dev") != NULL);
        ASSERT(strstr(guard, "restart-dev") != NULL);
        ASSERT(run_gate_script_with_env("tools/deploy_guard.sh",
                                        "ZCL_DEPLOY_GUARD_SELFTEST",
                                        "1") == 0);
        ASSERT(strstr(lane_recover, "zcl.lane_recovery_plan.v1") != NULL);
        ASSERT(strstr(lane_recover, "ZCL_LANE_RECOVERY_SELFTEST") != NULL);
        ASSERT(strstr(lane_recover, "canonical/live/main recovery is not supported")
               != NULL);
        ASSERT(strstr(lane_recover, "copy_seed_install_loader_restart") != NULL);
        ASSERT(strstr(lane_recover, "install_loader_dropin_restart") != NULL);
        ASSERT(strstr(lane_recover, "import_headers_install_loader_restart")
               != NULL);
        ASSERT(strstr(lane_recover, "ZCL_LANE_RECOVERY_IMPORT_HEADERS")
               != NULL);
        ASSERT(strstr(lane_recover,
                      "ZCL_LANE_RECOVERY_ALLOW_STALE_HEADER_IMPORT")
               != NULL);
        ASSERT(strstr(lane_recover,
                      "header_import_skipped_snapshot_not_newer")
               != NULL);
        ASSERT(strstr(lane_recover, "ZCL_LANE_RECOVERY_IMPORT_TIMEOUT")
               != NULL);
        ASSERT(strstr(lane_recover, "--importblockindex") != NULL);
        ASSERT(strstr(lane_recover, "\"$bin\" --importblockindex \"$legacy\" \"$target_db\"")
               != NULL);
        ASSERT(strstr(lane_recover, "ZCL_LANE_SNAPSHOT_LOADER_FLAG") != NULL);
        ASSERT(strstr(lane_recover, "systemctl --user restart \"$unit\"")
               != NULL);
        ASSERT(strstr(lane_recover, "ZCL_LANE_RECOVERY_SEED_SOURCE") != NULL);
        ASSERT(strstr(lane_recover, "ZCL_LANE_RECOVERY_LEGACY_SRC") != NULL);
        ASSERT(strstr(lane_recover, "python") == NULL);
        ASSERT(run_gate_script_with_env("tools/scripts/lane_recover.sh",
                                        "ZCL_LANE_RECOVERY_SELFTEST",
                                        "1") == 0);
        ASSERT(strstr(live_unit, "-operator-lane=canonical") != NULL);
        ASSERT(strstr(soak_unit, "-operator-lane=soak") != NULL);
        ASSERT(strstr(dev_unit, "-operator-lane=dev") != NULL);
        ASSERT(strstr(soak_unit, "$ZCL_LANE_SNAPSHOT_LOADER_FLAG") != NULL);
        ASSERT(strstr(dev_unit, "$ZCL_LANE_SNAPSHOT_LOADER_FLAG") != NULL);
        ASSERT(strstr(boot_index,
                      "boot_promote_tip_preserving_header_via_csr") != NULL);
        ASSERT(strstr(boot_index, "\"utxo_chain_mismatch\"") != NULL);
        ASSERT(strstr(handoff, "Public daily-driver node") != NULL);
        ASSERT(strstr(handoff, "Fresh-build lane for frequent development restarts")
               != NULL);
        ASSERT(strstr(handoff, "Long-uptime / weekly evidence lane") != NULL);
        ASSERT(strstr(handoff, "zcl.operator_lane.v1") != NULL);
        ASSERT(strstr(handoff, "ZCL_DEV_ALLOW_REINDEX_DROPIN=1") != NULL);
        ASSERT(strstr(lane_health, "report_lane live zclassic23") != NULL);
        ASSERT(strstr(lane_health, "report_lane soak zclassic23-soak") != NULL);
        ASSERT(strstr(lane_health, "report_lane dev zcl23-dev") != NULL);
        ASSERT(strstr(lane_health, "forced_reindex_flag_present") != NULL);
        ASSERT(strstr(lane_health, "dev_booting_rpc_down") != NULL);
        ASSERT(strstr(lane_health, "ZCL_LANE_LAG_WARN") != NULL);
        ASSERT(strstr(lane_health, "ZCL_LANE_AGENT_TIMEOUT") != NULL);
        ASSERT(strstr(lane_health, "rpc_call_timeout") != NULL);
        ASSERT(strstr(lane_health, "ZCL_SOAK_LAG_WARN") != NULL);
        ASSERT(strstr(lane_health, "tip_lag_to_live") != NULL);
        ASSERT(strstr(lane_health, "getblockchaininfo") != NULL);
        ASSERT(strstr(lane_health, "chain_headers") != NULL);
        ASSERT(strstr(lane_health, "initialblockdownload") != NULL);
        ASSERT(strstr(lane_health, "lag_to_live_") != NULL);
        ASSERT(strstr(lane_health, "dumpstate reducer_frontier") != NULL);
        ASSERT(strstr(lane_health, "dumpstate condition_engine") != NULL);
        ASSERT(strstr(lane_health,
                      "dumpstate chain_advance_coordinator") != NULL);
        ASSERT(strstr(lane_health, "ZCL_LANE_HEALTH_SELFTEST") != NULL);
        ASSERT(strstr(lane_health, "json_first_bool_field") != NULL);
        ASSERT(strstr(lane_health, "agent_build_commit") != NULL);
        ASSERT(strstr(lane_health, "agent_rpc_state") != NULL);
        ASSERT(strstr(lane_health, "agent_timeout") != NULL);
        ASSERT(strstr(lane_health, "inspect_agent_timeout") != NULL);
        ASSERT(strstr(lane_health, "agent_contract_trusted") != NULL);
        ASSERT(strstr(lane_health, "agent_operator_needed") != NULL);
        ASSERT(strstr(lane_health, "agent_primary_blocker") != NULL);
        ASSERT(strstr(lane_health, "agent_validation_pack_ok") != NULL);
        ASSERT(strstr(lane_health, "agent_blocked") != NULL);
        ASSERT(strstr(lane_health, "inspect_agent_primary_blocker") != NULL);
        ASSERT(strstr(lane_health, "condition_operator_needed") != NULL);
        ASSERT(strstr(lane_health, "inspect_condition_engine") != NULL);
        ASSERT(run_gate_script_with_env("tools/scripts/lane_health.sh",
                                        "ZCL_LANE_HEALTH_SELFTEST",
                                        "1") == 0);
        ASSERT(strstr(lane_health, "chain_advance_current_json") != NULL);
        ASSERT(strstr(lane_health, "\\\"last_decision\\\"") != NULL);
        ASSERT(strstr(lane_health, "reducer_hstar") != NULL);
        ASSERT(strstr(lane_health, "reducer_pending_stage") != NULL);
        ASSERT(strstr(lane_health, "reducer_pending_detail") != NULL);
        ASSERT(strstr(lane_health, "projection_height") != NULL);
        ASSERT(strstr(lane_health, "projection_lag") != NULL);
        ASSERT(strstr(lane_health, "projection_deferred") != NULL);
        ASSERT(strstr(lane_health, "inspect_chain_advance_coordinator")
               != NULL);
        ASSERT(strstr(lane_health, "condition_active_count") != NULL);
        ASSERT(strstr(lane_health, "condition_operator_needed_count") != NULL);
        ASSERT(strstr(lane_health, "no_peers") != NULL);
        ASSERT(strstr(lane_health, "memory_pressure") != NULL);
        ASSERT(strstr(lane_health, "bootstrapstatus") != NULL);
        ASSERT(strstr(lane_health, "snapshot_seed_height") != NULL);
        ASSERT(strstr(lane_health, "snapshot_loader_configured") != NULL);
        ASSERT(strstr(lane_health, "snapshot_loader_path") != NULL);
        ASSERT(strstr(lane_health, "recovery_hint") != NULL);
        ASSERT(strstr(lane_health, "restart_with_load_snapshot_at_own_height")
               != NULL);
        ASSERT(strstr(lane_health, "install_tip_seed_snapshot") != NULL);
        ASSERT(strstr(lane_health, "inspect_reducer_frontier") != NULL);
        ASSERT(strstr(lane_health, "role_ready") != NULL);
        ASSERT(strstr(lane_health, "role_reason") != NULL);
        ASSERT(strstr(lane_health, "canonical_ready") != NULL);
        ASSERT(strstr(lane_health, "soak_evidence_ready") != NULL);
        ASSERT(strstr(lane_health, "dev_lane_ready") != NULL);
        ASSERT(strstr(lane_health, "soak_eligible") != NULL);
        ASSERT(strstr(lane_health, "soak_reason") != NULL);
        ASSERT(strstr(lane_health, "live_reference_missing") != NULL);
        ASSERT(strstr(lane_health, "REDUNDANCY canonical=") != NULL);
        ASSERT(strstr(lane_health, "--strict") != NULL);
        ASSERT(strstr(makefile, "lane-health:") != NULL);
        ASSERT(strstr(makefile, "tools/scripts/lane_health.sh") != NULL);
        ASSERT(strstr(handoff, "make lane-health") != NULL);
        ASSERT(strstr(handoff, "make lane-recover") != NULL);
        ASSERT(strstr(handoff, "read-only three-lane status check") != NULL);
        ASSERT(strstr(handoff, "lag from the live lane") != NULL);
        ASSERT(strstr(handoff, "memory pressure") != NULL);
        ASSERT(strstr(handoff, "bootstrapstatus.snapshot_loader") != NULL);
        ASSERT(strstr(handoff, "snapshot seed height") != NULL);
        ASSERT(strstr(handoff, "recovery_hint") != NULL);
        ASSERT(strstr(handoff, "role readiness") != NULL);
        ASSERT(strstr(handoff, "soak-evidence") != NULL);
        ASSERT(strstr(handoff, "soak_eligible=false") != NULL);
        PASS();
    } _test_next:;
    free(script);
    free(guard);
    free(lane_health);
    free(lane_recover);
    free(agent_status);
    free(clear_script);
    free(agent_doctor);
    free(handoff);
    free(makefile);
    free(live_unit);
    free(soak_unit);
    free(dev_unit);
    free(boot_index);
    free(coldstart);
    free(coldstart_tip);
    return failures;
}

static int t_agent_fast_ci_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    char *rules = NULL;
    char *main_src = NULL;
    char *arch_doc = NULL;
    TEST("agent fast CI stays cache-aware and native-service first") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "Makefile") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "fast-ci agent-fast-ci dev-ci") != NULL);
        ASSERT(strstr(buf, "t-fast") != NULL);
        ASSERT(strstr(buf, "test_parallel_fast") != NULL);
        ASSERT(strstr(buf, "fast-compile dev-build-only") != NULL);
        ASSERT(strstr(buf, "fast-changed-compile") != NULL);
        ASSERT(strstr(buf, "dev-bin zclassic23-dev") != NULL);
        ASSERT(strstr(buf,
                      "fast-rebuild rebuild-fast dev-rebuild "
                      "hot-rebuild super-rebuild") != NULL);
        ASSERT(strstr(buf, "agent-loop agent-dev-loop") != NULL);
        ASSERT(strstr(buf, "ZCL_AGENT_LOOP_BIN") != NULL);
        ASSERT(strstr(buf, "ZCL_AGENT_LOOP_DEPLOY") != NULL);
        ASSERT(strstr(buf, "ZCL_AGENT_LOOP_DEPLOY=stage") != NULL);
        ASSERT(strstr(buf, "$(MAKE) agent-stage-dev") != NULL);
        ASSERT(strstr(buf, "$(MAKE) agent-deploy-fast") != NULL);
        ASSERT(strstr(buf, "tools/agent_fast_ci.sh rebuild-dev") != NULL);
        ASSERT(strstr(buf, "ZCLASSIC23_DEV_BIN") != NULL);
        ASSERT(strstr(buf, "DEV_OBJ_DIR") != NULL);
        ASSERT(strstr(buf, "DEV_CFLAGS") != NULL);
        ASSERT(strstr(buf, "DEV_HOT_CFLAGS") != NULL);
        ASSERT(strstr(buf, "DEV_LDFLAGS") != NULL);
        ASSERT(strstr(buf, "ZCL_DEV_LINKER") != NULL);
        ASSERT(strstr(buf, "command -v sccache") != NULL);
        ASSERT(strstr(buf, "DEV_OBJ_COMPLETE") != NULL);
        ASSERT(strstr(buf, ".complete") != NULL);
        ASSERT(strstr(buf, "direct depfile") != NULL);
        ASSERT(strstr(buf, "not for release/deploy") != NULL);
        ASSERT(strstr(buf, "tools/agent_fast_ci.sh") != NULL);
        ASSERT(strstr(buf, "tools/deploy_guard.sh") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_TESTS") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_LIVE=0") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CACHE=0") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CACHE_RESET=1") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CHANGED_FILES_FILE") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CHANGED_FILES") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CHANGED_FILES_ONLY") != NULL);
        ASSERT(strstr(buf, "pre-push-ci") != NULL);
        ASSERT(strstr(buf, "agent-plan") != NULL);
        ASSERT(strstr(buf, "tools/agent_fast_ci.sh plan-json") != NULL);
        ASSERT(strstr(buf,
                      "immutable-history-canaries historical-canaries")
               != NULL);
        ASSERT(strstr(buf, "domain_consensus_tx_structural") != NULL);
        ASSERT(strstr(buf, "consensus_parity") != NULL);
        ASSERT(strstr(buf, "replay-canary-anchor") != NULL);
        ASSERT(strstr(buf,
                      "ZCL_FAST_LIVE=0 ZCL_FAST_COMPILE=strict $(MAKE) fast-ci")
               != NULL);
        ASSERT(strstr(buf, "check-agent-cli: zclassic23") != NULL);
        ASSERT(strstr(buf,
                      "tools/scripts/check_agentdeployguard_cli_exit.sh")
               != NULL);
        ASSERT(strstr(buf, "install-quality-linger") != NULL);
        ASSERT(strstr(buf, "quality-linger-status") != NULL);
        ASSERT(strstr(buf, "tools/scripts/background_quality_lane.sh") != NULL);
        ASSERT(strstr(buf, "background-tests") != NULL);
        ASSERT(strstr(buf, "zclassic23-test-suite.timer") != NULL);
        ASSERT(strstr(buf, "agent-mcp-call") != NULL);
        ASSERT(strstr(buf, "agent-mcp-call-hot") != NULL);
        ASSERT(strstr(buf, "agent-mcp-call-dev") != NULL);
        ASSERT(strstr(buf, "agent-dev-status") != NULL);
        ASSERT(strstr(buf, "agent-doctor") != NULL);
        ASSERT(strstr(buf, "tools/dev/agent-dev-status.sh") != NULL);
        ASSERT(strstr(buf, "tools/dev/agent-doctor.sh") != NULL);
        ASSERT(strstr(buf, "stage-dev-bin agent-stage-dev") != NULL);
        ASSERT(strstr(buf, "ZCL_DEV_USE_PREBUILT=1") != NULL);
        ASSERT(strstr(buf, "deploy-dev-lane.sh --stage") != NULL);
        ASSERT(strstr(buf, "mktemp \"$(ZCL_AGENT_DEV_BIN).next.XXXXXX\"")
               == NULL);
        ASSERT(strstr(buf, "mcpcall") != NULL);
        ASSERT(strstr(buf, "ZCL_AGENT_BIN") != NULL);
        ASSERT(strstr(buf, "ZCL_AGENT_DEV_BIN") != NULL);
        ASSERT(strstr(buf, "ZCL_AGENT_MCP_BUILD") != NULL);
        ASSERT(strstr(buf, "ZCL_AGENT_MCP_ARGS") != NULL);
        ASSERT(strstr(buf, "MCP_CALL_BIN") == NULL);
        ASSERT(strstr(buf, "mcp_call.c") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), "tools/dev/agent-doctor.sh") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "zcl.agent_doctor.v1") != NULL);
        ASSERT(strstr(buf, "latest_test_artifact_mtime") != NULL);
        ASSERT(strstr(buf, "build/bin/test_parallel_fast") != NULL);
        ASSERT(strstr(buf, "build/bin/test_parallel") != NULL);
        ASSERT(strstr(buf, "build/bin/test_zcl") != NULL);
        ASSERT(strstr(buf, "latest_failure_log") != NULL);
        ASSERT(strstr(buf, "cutoff_mtime") != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), "src/main.c") == 0);
        ASSERT(read_entire_file(path, &main_src) == 0);
        ASSERT(strstr(main_src, "cli_run_mcp_call") != NULL);
        ASSERT(strstr(main_src, "mcp_register_ops") != NULL);
        ASSERT(strstr(main_src, "mcp_middleware_dispatch") != NULL);
        ASSERT(strstr(main_src, "mw.default_timeout_ms = 0") != NULL);
        ASSERT(strstr(main_src, "ZCL_MCP_CALL_BEARER_TOKEN") != NULL);
        {
            const char *mcp_init = strstr(main_src,
                "mcp_rpc_client_init(datadir, cli_port);");
            const char *params_select = mcp_init
                ? strstr(mcp_init, "chain_params_select(CHAIN_MAIN);") : NULL;
            const char *route_register = params_select
                ? strstr(params_select, "cli_mcp_register_all();") : NULL;
            ASSERT(mcp_init != NULL);
            ASSERT(params_select != NULL);
            ASSERT(route_register != NULL);
        }

        ASSERT(repo_path(path, sizeof(path),
                         "docs/AGENT_ARCHITECTURE.md") == 0);
        ASSERT(read_entire_file(path, &arch_doc) == 0);
        ASSERT(strstr(arch_doc, "REST resource first") != NULL);
        ASSERT(strstr(arch_doc, "ActiveRecord lifecycle") != NULL);
        ASSERT(strstr(arch_doc, "validates_*") != NULL);
        ASSERT(strstr(arch_doc, "Make relationships explicit C APIs")
               != NULL);
        ASSERT(strstr(arch_doc, "database_schema.c") != NULL);
        ASSERT(strstr(arch_doc, "api_controller_routes.c") != NULL);
        ASSERT(strstr(arch_doc, "make agent-mcp-call TOOL=<tool>")
               != NULL);
        ASSERT(strstr(arch_doc, "make agent-mcp-call-hot TOOL=<tool>")
               != NULL);
        ASSERT(strstr(arch_doc, "make agent-mcp-call-dev TOOL=<tool>")
               != NULL);
        ASSERT(strstr(arch_doc, "make agent-dev-status") != NULL);
        ASSERT(strstr(arch_doc, "zclassic23 mcpcall <tool> [json]")
               != NULL);

        ASSERT(repo_path(path, sizeof(path), "tools/agent_fast_ci.sh") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "zcl.agent_fast_ci.v1") != NULL);
        ASSERT(strstr(buf, "zcl.agent_fast_plan.v1") != NULL);
        ASSERT(strstr(buf, "zcl.agent_changed_compile_plan.v1") != NULL);
        ASSERT(strstr(buf, "zcl.agent_fast_ci.cache.v1") != NULL);
        ASSERT(strstr(buf, "emit_plan_json") != NULL);
        ASSERT(strstr(buf, "recommended_command") != NULL);
        ASSERT(strstr(buf, "mcp_shortcuts") != NULL);
        ASSERT(strstr(buf, "green_input_cache") != NULL);
        ASSERT(strstr(buf, "sccache cc") != NULL);
        ASSERT(strstr(buf, "ccache cc") != NULL);
        ASSERT(strstr(buf, "git diff --check") != NULL);
        ASSERT(strstr(buf, "git ls-files --others --exclude-standard")
               != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CHANGED_FILES_FILE") != NULL);
        ASSERT(strstr(buf, "fast_changed_files_file") != NULL);
        ASSERT(strstr(buf, "fast_changed_files_only") != NULL);
        ASSERT(strstr(buf, "fast_changed_files_only()") != NULL);
        ASSERT(strstr(buf, "validate_changed_files_only") != NULL);
        ASSERT(strstr(buf, "impact_rules_file") != NULL);
        ASSERT(strstr(buf, "bash -n \"$script\"") != NULL);
        ASSERT(strstr(buf, "tools/deploy_guard.sh") != NULL);
        ASSERT(strstr(buf, "tools/dev/deploy-dev-lane.sh") != NULL);
        ASSERT(strstr(buf, "tools/dev/agent-dev-status.sh") != NULL);
        ASSERT(strstr(buf,
                      "tools/scripts/check_agentdeployguard_cli_exit.sh")
               != NULL);
        ASSERT(strstr(buf, "make_fast lint-fast") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_COMPILE") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CHANGED_COMPILE_LIMIT") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CHANGED_FILES_ONLY") != NULL);
        ASSERT(strstr(buf, "FAST_COMPILE=\"${ZCL_FAST_COMPILE:-changed}\"")
               != NULL);
        ASSERT(strstr(buf, "compile_changed_gate") != NULL);
        ASSERT(strstr(buf, "compute_changed_compile_plan") != NULL);
        ASSERT(strstr(buf, "is_graph_wide_compile_change") != NULL);
        ASSERT(strstr(buf, "is_direct_dependency_compile_change") != NULL);
        ASSERT(strstr(buf, "dev_depfiles_available") != NULL);
        ASSERT(strstr(buf, "add_dependent_dev_objects") != NULL);
        ASSERT(strstr(buf, "build/dev-obj/.complete") != NULL);
        ASSERT(strstr(buf, "DIRECT_DEV_OBJECT_COUNT") != NULL);
        ASSERT(strstr(buf, "is_node_c_source") != NULL);
        ASSERT(strstr(buf, "DIRECT_DEV_OBJECTS") != NULL);
        ASSERT(strstr(buf, "fast-changed-compile: direct dev object compile")
               != NULL);
        ASSERT(strstr(buf, "fallback to fast-compile") != NULL);
        ASSERT(strstr(buf, "run_compile_gate") != NULL);
        ASSERT(strstr(buf, "changed|changed-dev|auto") != NULL);
        ASSERT(strstr(buf, "target=\"fast-compile\"") != NULL);
        ASSERT(strstr(buf, "target=\"build-only\"") != NULL);
        ASSERT(strstr(buf, "make_fast \"$target\"") != NULL);
        ASSERT(strstr(buf, "fast_compile") != NULL);
        ASSERT(strstr(buf, "UNMAPPED_CODE_CHANGES") != NULL);
        ASSERT(strstr(buf, "no focused test mapping for code changes")
               != NULL);
        ASSERT(strstr(buf, "IMPACT_RULES_FILE") != NULL);
        ASSERT(strstr(buf, "agent_impact_rules.def") != NULL);
        ASSERT(strstr(buf, "match_shared_impact_rules") != NULL);
        ASSERT(strstr(buf, "extend $IMPACT_RULES_FILE") != NULL);
        ASSERT(strstr(buf, "target=\"t-fast\"") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_STRICT_TESTS") != NULL);
        ASSERT(strstr(buf, "make_fast \"$target\" ONLY") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_JOBS") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_NODE_BIN") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_DEV_NODE_BIN") != NULL);
        ASSERT(strstr(buf, "build/bin/zclassic23") != NULL);
        ASSERT(strstr(buf, "build/bin/zclassic23-dev") != NULL);
        ASSERT(strstr(buf, "run_dev_rebuild") != NULL);
        ASSERT(strstr(buf, "dev-bin link target=$DEV_NODE_BIN") != NULL);
        ASSERT(strstr(buf,
                      "rebuild-dev|dev-rebuild|fast-rebuild|hot-rebuild")
               != NULL);
        ASSERT(strstr(buf, "zcl.public_status.v1") != NULL);
        ASSERT(strstr(buf, ".status == \"healthy\"") != NULL);
        ASSERT(strstr(buf, ".healthy == true") != NULL);
        ASSERT(strstr(buf, "((.gap // 0) <= 1)") == NULL);
        ASSERT(strstr(buf, "agent probe summary") != NULL);
        ASSERT(strstr(buf, "healthcheck") != NULL);
        ASSERT(strstr(buf, ".checks.has_peers == true") != NULL);
        ASSERT(strstr(buf, "health probe summary") != NULL);
        ASSERT(strstr(buf, "tools/z topology --json") == NULL);
        ASSERT(strstr(buf, "native service binary") != NULL);
        ASSERT(strstr(buf, "run make build-only or set ZCL_FAST_LIVE=0")
               != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_TESTS") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_COMPILE") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_LIVE") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CACHE") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CACHE_RESET") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CACHE_DIR") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CHANGED_FILES_ONLY") != NULL);
        ASSERT(strstr(buf, "fast result cache hit") != NULL);
        ASSERT(strstr(buf,
                      "skipping lint-fast/compile-gate/focused tests")
               != NULL);
        ASSERT(strstr(buf, "record_fast_cache_pass") != NULL);
        ASSERT(strstr(buf, "not full release CI") != NULL);
        ASSERT(strstr(buf, "make pre-push-ci") != NULL);
        ASSERT(strstr(buf, "make install-quality-linger") != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "app/controllers/include/controllers/agent_impact_rules.def") == 0);
        ASSERT(read_entire_file(path, &rules) == 0);
        ASSERT(strstr(rules, "AGENT_IMPACT_RULE") != NULL);
        ASSERT(strstr(rules, "node_health_service") != NULL);
        ASSERT(strstr(rules, "mcp_controllers") != NULL);
        ASSERT(strstr(rules, "src/main.c") != NULL);
        ASSERT(strstr(rules, "app/controllers/src/agent_controller.c") != NULL);
        ASSERT(strstr(rules, "app/controllers/src/agent_contract_registry.c")
               != NULL);
        ASSERT(strstr(rules,
                      "app/controllers/src/agent_contracts_controller.c")
               != NULL);
        ASSERT(strstr(rules, "app/controllers/src/agent_interface_controller.c")
               != NULL);
        ASSERT(strstr(rules, "app/controllers/src/agent_runtime_controller.c")
               != NULL);
        ASSERT(strstr(rules,
                      "app/controllers/src/event_timeline_controller.c")
               != NULL);
        ASSERT(strstr(rules, "app/controllers/src/diagnostics_*.c")
               != NULL);
        ASSERT(strstr(rules,
                      "app/controllers/include/controllers/diagnostics_*.h")
               != NULL);
        ASSERT(strstr(rules, "app/controllers/src/api_controller*.c") != NULL);
        ASSERT(strstr(rules, "app/controllers/src/api_controller_internal.h")
               != NULL);
        ASSERT(strstr(rules, "lib/event/src/event.c") != NULL);
        ASSERT(strstr(rules, "lib/event/include/event/event.h") != NULL);
        ASSERT(strstr(rules, "lib/test/src/test_event.c") != NULL);
        ASSERT(strstr(rules, "\"event make_lint_gates\"") != NULL);
        ASSERT(strstr(rules, "app/controllers/src/blockchain_controller*.c")
               != NULL);
        ASSERT(strstr(rules, "app/controllers/include/controllers/blockchain_controller.h")
               != NULL);
        ASSERT(strstr(rules, "lib/test/src/test_rpc_safety.c") != NULL);
        ASSERT(strstr(rules, "rpc_safety") != NULL);
        ASSERT(strstr(rules, "app/models/src/*.c") != NULL);
        ASSERT(strstr(rules, "app/models/include/models/*.h") != NULL);
        ASSERT(strstr(rules, "lib/test/src/test_models*.c") != NULL);
        ASSERT(strstr(rules, "\"models make_lint_gates\"") != NULL);
        ASSERT(strstr(rules, "core/consensus/*") != NULL);
        ASSERT(strstr(rules, "core/params/*") != NULL);
        ASSERT(strstr(rules,
                      "\"consensus_parity domain_consensus_tx_structural chain\"")
               != NULL);
        ASSERT(strstr(rules, "lib/net/src/connman.c") != NULL);
        ASSERT(strstr(rules, "docs/AGENT_API.md") != NULL);
        ASSERT(strstr(rules, "deploy/*.service") != NULL);
        ASSERT(strstr(rules, "lib/net/include/net/msg_internal.h") != NULL);
        ASSERT(strstr(rules, "lib/net/include/net/port_policy.h") != NULL);
        ASSERT(strstr(rules, "README.md") != NULL);
        ASSERT(strstr(rules, ".github/CONTRIBUTING.md") != NULL);
        ASSERT(strstr(rules, "docs/BUILD.md") != NULL);
        ASSERT(strstr(rules, "docs/GETTING_STARTED.md") != NULL);
        ASSERT(strstr(rules, "app/controllers/include/controllers/network_controller.h")
               != NULL);
        ASSERT(strstr(rules, "app/jobs/src/tip_finalize_stage*.c") != NULL);
        ASSERT(strstr(rules, "app/jobs/include/jobs/tip_finalize_stage.h")
               != NULL);
        ASSERT(strstr(rules, "app/jobs/include/jobs/reducer_frontier.h")
               != NULL);
        ASSERT(strstr(rules, "tip_finalize_stage") != NULL);
        ASSERT(strstr(rules, "reducer_frontier") != NULL);
        ASSERT(strstr(rules, "app/jobs/src/validate_headers_stage.c")
               != NULL);
        ASSERT(strstr(rules, "app/jobs/include/jobs/validate_headers_stage.h")
               != NULL);
        ASSERT(strstr(rules, "app/conditions/src/block_failed_mask_at_tip.c")
               != NULL);
        ASSERT(strstr(rules, "lib/test/src/test_utxo_activation_paused.c")
               != NULL);
        ASSERT(strstr(rules, "utxo_activation_paused") != NULL);
        ASSERT(strstr(rules, "condition_engine") != NULL);
        ASSERT(strstr(rules, "app/conditions/src/download_queue_starved.c")
               != NULL);
        ASSERT(strstr(rules, "app/conditions/src/local_header_refill_needed.c")
               != NULL);
        ASSERT(strstr(rules, "app/conditions/src/tip_wedged_resnapshot.c")
               != NULL);
        ASSERT(strstr(rules, "app/conditions/src/tip_stall_oracle_rebuild.c")
               != NULL);
        ASSERT(strstr(rules, "lib/test/src/test_sync_watchdog_conditions.c")
               != NULL);
        ASSERT(strstr(rules,
                      "lib/test/src/test_tip_stall_oracle_rebuild_condition.c")
               != NULL);
        ASSERT(strstr(rules, "sync_watchdog_conditions") != NULL);
        ASSERT(strstr(rules, "tip_stall_oracle_rebuild_condition") != NULL);
        ASSERT(strstr(rules, "app/conditions/src/stale_validate_headers_repair.c")
               != NULL);
        ASSERT(strstr(rules, "lib/test/src/test_stale_validate_headers_repair_condition.c")
               != NULL);
        ASSERT(strstr(rules, "validate_headers_stage") != NULL);
        ASSERT(strstr(rules, "stale_validate_headers_repair_condition")
               != NULL);
        ASSERT(strstr(rules, "app/conditions/src/chain_integrity_failed.c")
               != NULL);
        ASSERT(strstr(rules, "app/services/include/services/chain_restore_integrity.h")
               != NULL);
        ASSERT(strstr(rules, "lib/test/src/test_chain_integrity_failed_condition.c")
               != NULL);
        ASSERT(strstr(rules, "lib/test/src/test_service_state.c") != NULL);
        ASSERT(strstr(rules, "chain_integrity_failed_condition") != NULL);
        ASSERT(strstr(rules, "service_state") != NULL);
        ASSERT(strstr(rules, "config/src/boot_services.c") != NULL);
        ASSERT(strstr(rules, "config/src/boot.c") != NULL);
        ASSERT(strstr(rules, "config/src/boot_index.c") != NULL);
        ASSERT(strstr(rules, "lib/test/src/test_load_verify_boot.c") != NULL);
        ASSERT(strstr(rules, "config/include/config/boot_internal.h") != NULL);
        ASSERT(strstr(rules, "boot_refold_window_extend") != NULL);
        ASSERT(strstr(rules, "chain_state_repo") != NULL);
        ASSERT(strstr(rules, "load_verify_boot") != NULL);
        ASSERT(strstr(rules, "config/src/app_context.c") != NULL);
        ASSERT(strstr(rules, "config/include/config/boot.h") != NULL);
        ASSERT(strstr(rules, "app_context") != NULL);
        ASSERT(strstr(rules, "models") != NULL);
        ASSERT(strstr(rules, "lib/test/src/test_syncdiag_rpc.c") != NULL);
        free(rules);
        rules = NULL;

        ASSERT(repo_path(path, sizeof(path), "docs/work/fast-path.md") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "`make fast-ci`") != NULL);
        ASSERT(strstr(buf, "`make t-fast ONLY=<group>`") != NULL);
        ASSERT(strstr(buf, "`make fast-changed-compile`") != NULL);
        ASSERT(strstr(buf, "`make fast-compile`") != NULL);
        ASSERT(strstr(buf, "`make dev-bin`") != NULL);
        ASSERT(strstr(buf, "`make ci-reproducible`") != NULL);
        ASSERT(strstr(buf, "build/bin/test_parallel_fast") != NULL);
        ASSERT(strstr(buf, "build/bin/zclassic23-dev") != NULL);
        ASSERT(strstr(buf, "direct `.h`/`.def` depfile dependents")
               != NULL);
        ASSERT(strstr(buf, "build/dev-obj/.complete") != NULL);
        ASSERT(strstr(buf, "ZCL_DEV_OPT=-Og") != NULL);
        ASSERT(strstr(buf, "ZCL_DEV_HOT_OPT=-O2") != NULL);
        ASSERT(strstr(buf, "ZCL_DEV_LINKER") != NULL);
        ASSERT(strstr(buf, "not a deploy or release artifact") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CC") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_COMPILE=strict") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_COMPILE=changed") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CHANGED_COMPILE_LIMIT") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CHANGED_FILES_ONLY=1") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_TESTS") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_STRICT_TESTS=1") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_JOBS") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_LIVE=0") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CACHE=0") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CACHE_RESET=1") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CACHE_DIR") != NULL);
        ASSERT(strstr(buf, ".cache/zcl-agent-fast-ci") != NULL);
        ASSERT(strstr(buf, "fast result cache hit") != NULL);
        ASSERT(strstr(buf, "build/bin/zclassic23 agent") != NULL);
        ASSERT(strstr(buf, "There is no `tools/z` fallback") != NULL);
        ASSERT(strstr(buf, "zclassic23 agentbuild") != NULL);
        ASSERT(strstr(buf, "zcl_agent_build") != NULL);
        ASSERT(strstr(buf, "`make immutable-history-canaries`") != NULL);
        ASSERT(strstr(buf, "h=478544") != NULL);
        ASSERT(strstr(buf, "replay-canary-anchor") != NULL);
        ASSERT(strstr(buf, "MCP tools") != NULL);
        ASSERT(strstr(buf, "`tools/z`") != NULL);
        ASSERT(strstr(buf, "not an agent interface") != NULL);
        ASSERT(strstr(buf, "origin/main..HEAD") != NULL);
        ASSERT(strstr(buf, "cached focused fast-ci") != NULL);
        ASSERT(strstr(buf, "live node condition remains") != NULL);
        ASSERT(strstr(buf, "zclassic23-fuzz.timer") != NULL);
        ASSERT(strstr(buf, "zclassic23-coverage.timer") != NULL);
        ASSERT(strstr(buf, "zclassic23-test-suite.timer") != NULL);
        ASSERT(strstr(buf, "zcl.background_quality_status.v1") != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), "README.md") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "docs/GETTING_STARTED.md") != NULL);
        ASSERT(strstr(buf, "Public start here") != NULL);
        ASSERT(strstr(buf, "make dev-bin") != NULL);
        ASSERT(strstr(buf, "488 parallel groups") != NULL);
        ASSERT(strstr(buf, "build/bin/zclassic23 agentops") != NULL);
        ASSERT(strstr(buf, "| jq") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), "docs/GETTING_STARTED.md") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "public first-run path for a fresh machine")
               != NULL);
        ASSERT(strstr(buf, "make vendor") != NULL);
        ASSERT(strstr(buf, "make -j\"$(nproc)\"") != NULL);
        ASSERT(strstr(buf, "make dev-bin") != NULL);
        ASSERT(strstr(buf, "make t-fast ONLY=<group>") != NULL);
        ASSERT(strstr(buf, "build/bin/zclassic23 -datadir=\"$HOME/.zclassic-c23\" agent")
               != NULL);
        ASSERT(strstr(buf, "build/bin/zclassic23 -datadir=\"$HOME/.zclassic-c23\" agentops")
               != NULL);
        ASSERT(strstr(buf, "zcl_agent_ops") != NULL);
        ASSERT(strstr(buf, "docs/BOOTSTRAPPING.md") != NULL);
        ASSERT(strstr(buf, "operator_needed=false") != NULL);
        ASSERT(strstr(buf, "docs/HANDOFF.md") != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), ".github/CONTRIBUTING.md") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "make vendor") != NULL);
        ASSERT(strstr(buf, "make build-only") != NULL);
        ASSERT(strstr(buf, "make dev-bin") != NULL);
        ASSERT(strstr(buf, "make t-fast ONLY=<group>") != NULL);
        ASSERT(strstr(buf, "make fast-ci") != NULL);
        ASSERT(strstr(buf, "fresh clone will not link") == NULL);
        ASSERT(strstr(buf, "make vendor` automation is on the roadmap")
               == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), "tools/githooks/pre-push") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "Fast local edit-loop before commit:  make fast-ci")
               != NULL);
        ASSERT(strstr(buf, "files being pushed to origin/main") != NULL);
        ASSERT(strstr(buf, "refs/heads/main") != NULL);
        ASSERT(strstr(buf, "ZCL_FAST_CHANGED_FILES_FILE") != NULL);
        ASSERT(strstr(buf, "git diff --name-only \"$rsha\" \"$lsha\"")
               != NULL);
        ASSERT(strstr(buf, "make install-quality-linger") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    free(rules);
    free(main_src);
    free(arch_doc);
    return failures;
}

static int t_remote_node_update_contract(void)
{
    int failures = 0;
    char *script = NULL;
    char *makefile = NULL;
    char *agent = NULL;
    char *doc = NULL;
    char *fast_ci = NULL;
    char *service = NULL;
    char *timer = NULL;
    TEST("remote node update is main-only and guarded") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path),
                         "tools/scripts/remote_node_update.sh") == 0);
        ASSERT(read_entire_file(path, &script) == 0);
        ASSERT(strstr(script, "zcl.remote_node_update.v1") != NULL);
        ASSERT(strstr(script, "ZCL_REMOTE_DRY_RUN") != NULL);
        ASSERT(strstr(script, "ZCL_REMOTE_BRANCH") != NULL);
        ASSERT(strstr(script, "ZCL_REMOTE_MAIN_REF") != NULL);
        ASSERT(strstr(script, "only origin/main may be used") != NULL);
        ASSERT(strstr(script, "git fetch --prune origin main") != NULL);
        ASSERT(strstr(script, "git merge --ff-only origin/main") != NULL);
        ASSERT(strstr(script, "remote checkout must be $expect_branch")
               != NULL);
        ASSERT(strstr(script, "remote update branch must be main") != NULL);
        ASSERT(strstr(script, "ZCL_REMOTE_ALLOW_DIRTY") != NULL);
        ASSERT(strstr(script, "make fast-rebuild") != NULL);
        ASSERT(strstr(script, "make zclassic23") != NULL);
        ASSERT(strstr(script, "ZCL_REMOTE_INSTALL_BIN") != NULL);
        ASSERT(strstr(script, "ZCL_REMOTE_RESTART") != NULL);
        ASSERT(strstr(script, "ZCL_REMOTE_JSON") != NULL);
        ASSERT(strstr(script, "ZCL_DEPLOY_ALLOW_CANONICAL=1") != NULL);
        ASSERT(strstr(script,
                      "ZCL_DEPLOY_ALLOW_CANONICAL=$(shell_quote") != NULL);
        ASSERT(strstr(script,
                      "ZCL_DEPLOY_ALLOW_CANONICAL=\"${ZCL_DEPLOY_ALLOW_CANONICAL:-0}\"")
               != NULL);
        ASSERT(strstr(script, "--json") != NULL);
        ASSERT(strstr(script, "json_escape") != NULL);
        ASSERT(strstr(script, "emit_json_summary") != NULL);
        ASSERT(strstr(script, "missing_vendor_archives") != NULL);
        ASSERT(strstr(script, "preflight_build") != NULL);
        ASSERT(strstr(script, "preflight_failed") != NULL);
        ASSERT(strstr(script, "missing_build_tool:c++_or_cmake") != NULL);
        ASSERT(strstr(script, "missing_build_tool:c++_or_g++") != NULL);
        ASSERT(strstr(script, "require_cxx_for_preflight") != NULL);
        ASSERT(strstr(script, "\\\"error\\\"") != NULL);
        ASSERT(strstr(script, "git_fetch_failed") != NULL);
        ASSERT(strstr(script, "build_failed:$build") != NULL);
        ASSERT(strstr(script, "install_failed:$install_bin") != NULL);
        ASSERT(strstr(script, "restart_failed:$unit") != NULL);
        ASSERT(strstr(script, "\\\"plan\\\"") != NULL);
        ASSERT(strstr(script, "\\\"safe_next_action\\\"") != NULL);
        ASSERT(strstr(script, "printf '%s\\n' \"$json\"") != NULL);
        const char *dry_done = strstr(script, "log \"dry_run_complete=1\"");
        const char *dry_summary =
            strstr(script,
                   "emit_json_summary \"ok\" \"$plan\" 0 0 0 0 \"$head\"");
        ASSERT(dry_done != NULL);
        ASSERT(dry_summary != NULL);
        ASSERT(dry_done < dry_summary);
        const char *complete_done =
            strstr(script,
                   "log \"complete=1 final_head=$final_head active=$active\"");
        const char *complete_summary =
            strstr(script,
                   "emit_json_summary \"ok\" \"$plan\" \"$updated\"");
        ASSERT(complete_done != NULL);
        ASSERT(complete_summary != NULL);
        ASSERT(complete_done < complete_summary);
        ASSERT(strstr(script, "tools/deploy_guard.sh") != NULL);
        ASSERT(strstr(script, "zcl.agent_deploy_guard.v1") != NULL);
        ASSERT(strstr(script, "No Python or jq is required") != NULL);
        ASSERT(strstr(script, "python") == NULL);
        free(script);
        script = NULL;

        ASSERT(repo_path(path, sizeof(path), "Makefile") == 0);
        ASSERT(read_entire_file(path, &makefile) == 0);
        ASSERT(strstr(makefile, "remote-node-update:") != NULL);
        ASSERT(strstr(makefile, "remote-node-update-json:") != NULL);
        ASSERT(strstr(makefile, "ZCL_REMOTE_JSON=1") != NULL);
        ASSERT(strstr(makefile, "tools/scripts/remote_node_update.sh")
               != NULL);
        ASSERT(strstr(makefile, "CXX_STDLIB_LDFLAGS") != NULL);
        ASSERT(strstr(makefile, "$(CXX) -print-file-name=libstdc++.a")
               != NULL);
        ASSERT(strstr(makefile, "install-self-update-linger:") != NULL);
        ASSERT(strstr(makefile, "self-update-status:") != NULL);
        ASSERT(strstr(makefile, "install-remote-test-node-linger:") != NULL);
        ASSERT(strstr(makefile, "remote-test-node-status:") != NULL);
        ASSERT(strstr(makefile, "zclassic23-remote-test-node.service") != NULL);
        ASSERT(strstr(makefile, "zclassic23-remote-test.env.example") != NULL);
        ASSERT(strstr(makefile, "zclassic23-self-update.timer") != NULL);
        ASSERT(strstr(makefile,
                      "systemctl --user enable --now zclassic23-self-update.timer")
               != NULL);
        free(makefile);
        makefile = NULL;

        ASSERT(repo_path(path, sizeof(path), "tools/scripts/build_vendor.sh")
               == 0);
        ASSERT(read_entire_file(path, &script) == 0);
        ASSERT(strstr(script, "VENDOR_LOCK_DIR") != NULL);
        ASSERT(strstr(script, "acquire_vendor_lock") != NULL);
        ASSERT(strstr(script, "release_vendor_lock") != NULL);
        ASSERT(strstr(script, "timed out waiting for vendor build lock")
               != NULL);
        ASSERT(strstr(script, "build_leveldb_direct") != NULL);
        ASSERT(strstr(script, "leveldb_cxx_compiler") != NULL);
        ASSERT(strstr(script, "LEVELDB_PLATFORM_POSIX=1") != NULL);
        ASSERT(strstr(script, "#define HAVE_SNAPPY 0") != NULL);
        free(script);
        script = NULL;

        ASSERT(repo_path(path, sizeof(path), "tools/agent_fast_ci.sh") == 0);
        ASSERT(read_entire_file(path, &fast_ci) == 0);
        ASSERT(strstr(fast_ci, "tools/scripts/remote_node_update.sh")
               != NULL);
        ASSERT(strstr(fast_ci,
                      "deploy/examples/zclassic23-remote-test-node.service")
               != NULL);
        ASSERT(strstr(fast_ci,
                      "deploy/examples/zclassic23-remote-test.env.example")
               != NULL);
        ASSERT(strstr(fast_ci,
                      "deploy/examples/zclassic23-self-update.service")
               != NULL);
        ASSERT(strstr(fast_ci,
                      "deploy/examples/zclassic23-self-update.timer")
               != NULL);
        free(fast_ci);
        fast_ci = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "app/controllers/src/agent_controller.c") == 0);
        ASSERT(read_entire_file(path, &agent) == 0);
        ASSERT(strstr(agent, "remote_node_update") != NULL);
        ASSERT(strstr(agent, "zcl.remote_node_update.v1") != NULL);
        ASSERT(strstr(agent, "make remote-node-update") != NULL);
        ASSERT(strstr(agent, "json_dry_run_command") != NULL);
        ASSERT(strstr(agent, "json_summary") != NULL);
        ASSERT(strstr(agent, "fast_forward_only") != NULL);
        ASSERT(strstr(agent, "restart_default") != NULL);
        free(agent);
        agent = NULL;

        ASSERT(repo_path(path, sizeof(path), "docs/AGENT_API.md") == 0);
        ASSERT(read_entire_file(path, &doc) == 0);
        ASSERT(strstr(doc, "## Remote Node Updates") != NULL);
        ASSERT(strstr(doc, "zcl.remote_node_update.v1") != NULL);
        ASSERT(strstr(doc, "make remote-node-update") != NULL);
        ASSERT(strstr(doc, "make remote-node-update-json") != NULL);
        ASSERT(strstr(doc, "ZCL_REMOTE_JSON=1") != NULL);
        ASSERT(strstr(doc, "status:\"error\"") != NULL);
        ASSERT(strstr(doc, "git merge --ff-only origin/main") != NULL);
        ASSERT(strstr(doc, "ZCL_REMOTE_RESTART=0") != NULL);
        ASSERT(strstr(doc, "direct C++11 fallback") != NULL);
        ASSERT(strstr(doc, "make install-self-update-linger") != NULL);
        ASSERT(strstr(doc, "make self-update-status") != NULL);
        ASSERT(strstr(doc, "make install-remote-test-node-linger") != NULL);
        ASSERT(strstr(doc, "make remote-test-node-status") != NULL);
        ASSERT(strstr(doc, "MemoryHigh=24G") != NULL);
        ASSERT(strstr(doc, "MemoryMax=32G") != NULL);
        ASSERT(strstr(doc, "ZCL_LANE_SNAPSHOT_LOADER_FLAG=-load-snapshot-at-own-height")
               != NULL);
        ASSERT(strstr(doc, "systemd memory-budget lint") != NULL);
        ASSERT(strstr(doc,
                      "deploy/examples/zclassic23-self-update.timer")
               != NULL);
        free(doc);
        doc = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "deploy/examples/zclassic23-remote-test-node.service")
               == 0);
        ASSERT(read_entire_file(path, &service) == 0);
        ASSERT(strstr(service, "EnvironmentFile=-%h/.config/zclassic23/remote-test.env")
               != NULL);
        ASSERT(strstr(service, "-datadir=%h/.zclassic23-test") != NULL);
        ASSERT(strstr(service, "-port=18033") != NULL);
        ASSERT(strstr(service, "-rpcport=18233") != NULL);
        ASSERT(strstr(service, "MemoryHigh=24G") != NULL);
        ASSERT(strstr(service, "MemoryMax=32G") != NULL);
        ASSERT(strstr(service, "\nMemoryMax=32G\n") == NULL);
        ASSERT(strstr(service, "$ZCL_LANE_SNAPSHOT_LOADER_FLAG") != NULL);
        ASSERT(strstr(service, "CPUWeight=30") != NULL);
        ASSERT(strstr(service, "IOWeight=30") != NULL);
        ASSERT(strstr(service, "StandardOutput=append:%h/.zclassic23-test/node.log")
               != NULL);
        free(service);
        service = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "deploy/examples/zclassic23-remote-test.env.example")
               == 0);
        ASSERT(read_entire_file(path, &service) == 0);
        ASSERT(strstr(service, "ZCL_TEST_EXTERNALIP_FLAG=-externalip=")
               != NULL);
        ASSERT(strstr(service, "ZCL_TEST_ADDNODE_FLAGS=-addnode=")
               != NULL);
        ASSERT(strstr(service,
                      "ZCL_LANE_SNAPSHOT_LOADER_FLAG=-load-snapshot-at-own-height")
               != NULL);
        free(service);
        service = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "deploy/examples/zclassic23-self-update.service")
               == 0);
        ASSERT(read_entire_file(path, &service) == 0);
        ASSERT(strstr(service,
                      "Documentation=file:%h/github/zclassic23/docs/AGENT_API.md")
               != NULL);
        ASSERT(strstr(service, "remote_node_update.sh self") != NULL);
        ASSERT(strstr(service, "ZCL_REMOTE_BUILD=fast-rebuild") != NULL);
        ASSERT(strstr(service, "ZCL_REMOTE_RESTART=0") != NULL);
        ASSERT(strstr(service, "MemoryHigh=8G") != NULL);
        ASSERT(strstr(service, "IOSchedulingClass=idle") != NULL);
        free(service);
        service = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "deploy/examples/zclassic23-self-update.timer")
               == 0);
        ASSERT(read_entire_file(path, &timer) == 0);
        ASSERT(strstr(timer, "OnCalendar=daily") != NULL);
        ASSERT(strstr(timer, "RandomizedDelaySec=1h") != NULL);
        PASS();
    } _test_next:;
    free(script);
    free(makefile);
    free(agent);
    free(doc);
    free(fast_ci);
    free(service);
    free(timer);
    return failures;
}

static int t_native_agent_api_contract(void)
{
    int failures = 0;
    char *main_buf = NULL;
    char *event_buf = NULL;
    char *agent_summary_buf = NULL;
    char *agent_summary_json_buf = NULL;
    char *agent_operator_buf = NULL;
    char *agent_ctrl_buf = NULL;
    char *agent_capability_registry_buf = NULL;
    char *agent_registry_buf = NULL;
    char *agent_review_registry_buf = NULL;
    char *agent_schema_registry_buf = NULL;
    char *agent_contracts_buf = NULL;
    char *agent_contracts_def_buf = NULL;
    char *agent_bg_quality_buf = NULL;
    char *agent_first_call_buf = NULL;
    char *agent_lanes_buf = NULL;
    char *agent_lane_runtime_buf = NULL;
    char *agent_liveness_buf = NULL;
    char *agent_diagnose_buf = NULL;
    char *event_timeline_buf = NULL;
    char *agent_anchor_status_buf = NULL;
    char *agent_iface_buf = NULL;
    char *agent_ops_buf = NULL;
    char *agent_runtime_buf = NULL;
    char *agent_readiness_buf = NULL;
    char *diag_ctrl_buf = NULL;
    char *diag_reg_buf = NULL;
    char *diag_catalog_buf = NULL;
    char *api_buf = NULL;
    char *api_status_buf = NULL;
    char *agent_doc_buf = NULL;
    TEST("zclassic23 binary exposes native API and agent commands") {
        char main_path[PATH_MAX];
        char event_path[PATH_MAX];
        char agent_summary_path[PATH_MAX];
        char agent_summary_json_path[PATH_MAX];
        char agent_operator_path[PATH_MAX];
        char agent_ctrl_path[PATH_MAX];
        char agent_capability_registry_path[PATH_MAX];
        char agent_registry_path[PATH_MAX];
        char agent_review_registry_path[PATH_MAX];
        char agent_schema_registry_path[PATH_MAX];
        char agent_contracts_path[PATH_MAX];
        char agent_contracts_def_path[PATH_MAX];
        char agent_bg_quality_path[PATH_MAX];
        char agent_first_call_path[PATH_MAX];
        char agent_lanes_path[PATH_MAX];
        char agent_lane_runtime_path[PATH_MAX];
        char agent_liveness_path[PATH_MAX];
        char agent_diagnose_path[PATH_MAX];
        char event_timeline_path[PATH_MAX];
        char agent_anchor_status_path[PATH_MAX];
        char agent_iface_path[PATH_MAX];
        char agent_ops_path[PATH_MAX];
        char agent_runtime_path[PATH_MAX];
        char agent_readiness_path[PATH_MAX];
        char diag_ctrl_path[PATH_MAX];
        char diag_reg_path[PATH_MAX];
        char diag_catalog_path[PATH_MAX];
        char api_path[PATH_MAX];
        char api_status_path[PATH_MAX];
        char agent_doc_path[PATH_MAX];
        ASSERT(repo_path(main_path, sizeof(main_path), "src/main.c") == 0);
        ASSERT(repo_path(event_path, sizeof(event_path),
                         "app/controllers/src/event_controller.c") == 0);
        ASSERT(repo_path(agent_summary_path, sizeof(agent_summary_path),
                         "app/controllers/src/event_agent_summary.c") == 0);
        ASSERT(repo_path(agent_summary_json_path,
                         sizeof(agent_summary_json_path),
                         "app/controllers/src/event_agent_summary_json.c")
               == 0);
        ASSERT(repo_path(agent_operator_path, sizeof(agent_operator_path),
                         "app/controllers/src/agent_operator_contracts.c")
               == 0);
        ASSERT(repo_path(agent_ctrl_path, sizeof(agent_ctrl_path),
                         "app/controllers/src/agent_controller.c") == 0);
        ASSERT(repo_path(agent_capability_registry_path,
                         sizeof(agent_capability_registry_path),
                         "app/controllers/src/agent_contract_capability_registry.c")
               == 0);
        ASSERT(repo_path(agent_registry_path, sizeof(agent_registry_path),
                         "app/controllers/src/agent_contract_registry.c")
               == 0);
        ASSERT(repo_path(agent_review_registry_path,
                         sizeof(agent_review_registry_path),
                         "app/controllers/src/agent_contract_review_registry.c")
               == 0);
        ASSERT(repo_path(agent_schema_registry_path,
                         sizeof(agent_schema_registry_path),
                         "app/controllers/src/agent_contract_schema_registry.c")
               == 0);
        ASSERT(repo_path(agent_contracts_path, sizeof(agent_contracts_path),
                         "app/controllers/src/agent_contracts_controller.c")
               == 0);
        ASSERT(repo_path(agent_contracts_def_path,
                         sizeof(agent_contracts_def_path),
                         "app/controllers/include/controllers/agent_contracts.def")
               == 0);
        ASSERT(repo_path(agent_bg_quality_path,
                         sizeof(agent_bg_quality_path),
                         "app/controllers/src/agent_background_quality.c")
               == 0);
        ASSERT(repo_path(agent_first_call_path,
                         sizeof(agent_first_call_path),
                         "app/controllers/src/agent_first_call.c")
               == 0);
        ASSERT(repo_path(agent_lanes_path, sizeof(agent_lanes_path),
                         "app/controllers/src/agent_lanes_controller.c") == 0);
        ASSERT(repo_path(agent_lane_runtime_path,
                         sizeof(agent_lane_runtime_path),
                         "app/controllers/src/agent_lane_runtime.c") == 0);
        ASSERT(repo_path(agent_liveness_path, sizeof(agent_liveness_path),
                         "app/controllers/src/agent_liveness_controller.c")
               == 0);
        ASSERT(repo_path(agent_diagnose_path, sizeof(agent_diagnose_path),
                         "app/controllers/src/agent_diagnose_controller.c")
               == 0);
        ASSERT(repo_path(event_timeline_path, sizeof(event_timeline_path),
                         "app/controllers/src/event_timeline_controller.c")
               == 0);
        ASSERT(repo_path(agent_anchor_status_path,
                         sizeof(agent_anchor_status_path),
                         "app/controllers/src/agent_anchor_status_controller.c")
               == 0);
        ASSERT(repo_path(agent_iface_path, sizeof(agent_iface_path),
                         "app/controllers/src/agent_interface_controller.c")
               == 0);
        ASSERT(repo_path(agent_ops_path, sizeof(agent_ops_path),
                         "app/controllers/src/agent_ops_controller.c") == 0);
        ASSERT(repo_path(agent_runtime_path, sizeof(agent_runtime_path),
                         "app/controllers/src/agent_runtime_controller.c") == 0);
        ASSERT(repo_path(agent_readiness_path, sizeof(agent_readiness_path),
                         "app/controllers/src/event_agent_readiness.c") == 0);
        ASSERT(repo_path(diag_ctrl_path, sizeof(diag_ctrl_path),
                         "app/controllers/src/diagnostics_controller.c") == 0);
        ASSERT(repo_path(diag_reg_path, sizeof(diag_reg_path),
                         "app/controllers/src/diagnostics_registry.c") == 0);
        ASSERT(repo_path(diag_catalog_path, sizeof(diag_catalog_path),
                         "app/controllers/src/diagnostics_catalog_controller.c")
               == 0);
        ASSERT(repo_path(api_path, sizeof(api_path),
                         "app/controllers/src/api_controller_agent_index.c")
               == 0);
        ASSERT(repo_path(api_status_path, sizeof(api_status_path),
                         "app/controllers/src/api_controller_status.c") == 0);
        ASSERT(repo_path(agent_doc_path, sizeof(agent_doc_path),
                         "docs/AGENT_API.md") == 0);
        ASSERT(read_entire_file(main_path, &main_buf) == 0);
        ASSERT(read_entire_file(event_path, &event_buf) == 0);
        ASSERT(read_entire_file(agent_summary_path, &agent_summary_buf) == 0);
        ASSERT(read_entire_file(agent_summary_json_path,
                                &agent_summary_json_buf) == 0);
        ASSERT(read_entire_file(agent_operator_path, &agent_operator_buf)
               == 0);
        ASSERT(read_entire_file(agent_ctrl_path, &agent_ctrl_buf) == 0);
        ASSERT(read_entire_file(agent_capability_registry_path,
                                &agent_capability_registry_buf) == 0);
        ASSERT(read_entire_file(agent_registry_path, &agent_registry_buf) == 0);
        ASSERT(read_entire_file(agent_review_registry_path,
                                &agent_review_registry_buf) == 0);
        ASSERT(read_entire_file(agent_schema_registry_path,
                                &agent_schema_registry_buf) == 0);
        ASSERT(read_entire_file(agent_contracts_path,
                                &agent_contracts_buf) == 0);
        ASSERT(read_entire_file(agent_contracts_def_path,
                                &agent_contracts_def_buf) == 0);
        ASSERT(read_entire_file(agent_bg_quality_path,
                                &agent_bg_quality_buf) == 0);
        ASSERT(read_entire_file(agent_first_call_path,
                                &agent_first_call_buf) == 0);
        ASSERT(read_entire_file(agent_lanes_path, &agent_lanes_buf) == 0);
        ASSERT(read_entire_file(agent_lane_runtime_path,
                                &agent_lane_runtime_buf) == 0);
        ASSERT(read_entire_file(agent_liveness_path,
                                &agent_liveness_buf) == 0);
        ASSERT(read_entire_file(agent_diagnose_path,
                                &agent_diagnose_buf) == 0);
        ASSERT(read_entire_file(event_timeline_path,
                                &event_timeline_buf) == 0);
        ASSERT(read_entire_file(agent_anchor_status_path,
                                &agent_anchor_status_buf) == 0);
        ASSERT(read_entire_file(agent_iface_path, &agent_iface_buf) == 0);
        ASSERT(read_entire_file(agent_ops_path, &agent_ops_buf) == 0);
        ASSERT(read_entire_file(agent_runtime_path, &agent_runtime_buf) == 0);
        ASSERT(read_entire_file(agent_readiness_path,
                                &agent_readiness_buf) == 0);
        ASSERT(read_entire_file(diag_ctrl_path, &diag_ctrl_buf) == 0);
        ASSERT(read_entire_file(diag_reg_path, &diag_reg_buf) == 0);
        ASSERT(read_entire_file(diag_catalog_path, &diag_catalog_buf) == 0);
        ASSERT(read_entire_file(api_path, &api_buf) == 0);
        ASSERT(read_entire_file(api_status_path, &api_status_buf) == 0);
        ASSERT(read_entire_file(agent_doc_path, &agent_doc_buf) == 0);
        ASSERT(strstr(main_buf,
                      "Agent/operator API commands (from agent_contracts.def)")
               != NULL);
        ASSERT(strstr(main_buf, "agent_print_native_usage(stdout, prog)")
               != NULL);
        ASSERT(strstr(main_buf, "AI-coder code/docs/test map") == NULL);
        ASSERT(strstr(main_buf, "Compact no-jq AI/operator command center")
               == NULL);
        ASSERT(strstr(main_buf, "-operator-lane=<name>") != NULL);
        ASSERT(strstr(main_buf, "ZCL_OPERATOR_LANE") != NULL);
        ASSERT(strstr(main_buf, "strncmp(argv[i], \"-operator-lane=\", 15)")
               != NULL);
        ASSERT(strstr(main_buf, "app_operator_lane_parse") != NULL);
        ASSERT(strstr(main_buf, "app_operator_lane_name(operator_lane)")
               != NULL);
        ASSERT(strstr(main_buf, "rpc_agent_set_boot_context") != NULL);
        ASSERT(strstr(main_buf, "cli_probe_static_agent_target") != NULL);
        ASSERT(strstr(main_buf, "agent_runtime_probe_method_name") != NULL);
        ASSERT(strstr(main_buf, "RPC_METHOD_NOT_FOUND") != NULL);
        ASSERT(strstr(main_buf, "cli_agent_contract_method") != NULL);
        ASSERT(strstr(main_buf,
                      "cli_print_contract_method_skew_diagnostic") != NULL);
        ASSERT(strstr(main_buf, "zcl.cli_rpc_diagnostic.v1") != NULL);
        ASSERT(strstr(main_buf,
                      "target_runtime_method_not_found") != NULL);
        ASSERT(strstr(main_buf,
                      "target_runtime_version_skew_or_contract_not_deployed")
               != NULL);
        ASSERT(strstr(main_buf, "cli_service_exec_arg") != NULL);
        ASSERT(strstr(main_buf, "systemctl --user show zclassic23") != NULL);
        ASSERT(strstr(main_buf, "cli_p2p_port") != NULL);
        ASSERT(strstr(main_buf, "cli_service_exec_arg(\"port\"") != NULL);
        ASSERT(strstr(main_buf,
                      "datadir, cli_port, cli_p2p_port") != NULL);
        ASSERT(strstr(main_buf, "cli_cookie_exists") != NULL);
        ASSERT(strstr(main_buf, "cannot accidentally query") != NULL);
        ASSERT(strstr(main_buf, "params_storage") != NULL);
        ASSERT(strstr(main_buf, "strcmp(method, \"--agent\")") != NULL);
        ASSERT(strstr(main_buf, "strcmp(method, \"--status\")") != NULL);
        ASSERT(strstr(main_buf, "cli_runtime_rpc_method") != NULL);
        ASSERT(strstr(main_buf, "strcmp(method, \"status\")") != NULL);
        ASSERT(strstr(main_buf, "return \"agent\"") != NULL);
        ASSERT(strstr(main_buf, "strcmp(argv[i], \"--agent\")") != NULL);
        ASSERT(strstr(main_buf, "strcmp(argv[i], \"--status\")") != NULL);
        ASSERT(strstr(main_buf, "cli_static_agent_method") != NULL);
        ASSERT(strstr(main_buf, "struct cli_static_agent_route") != NULL);
        ASSERT(strstr(main_buf, "g_cli_static_agent_routes") != NULL);
        ASSERT(strstr(main_buf, "cli_static_agent_lookup") != NULL);
        ASSERT(strstr(main_buf, "agent_contract_lookup(route->method)")
               != NULL);
        ASSERT(strstr(main_buf, "cli_run_static_agent_method") != NULL);
        ASSERT(strstr(main_buf, "cli_static_agent_result_exit_code") != NULL);
        ASSERT(strstr(main_buf, "json_get(result, \"exit_code\")") != NULL);
        ASSERT(strstr(main_buf, "code < 0 || code > 125") != NULL);
        ASSERT(strstr(main_buf, "\"agentmap\", rpc_agent_map") != NULL);
        ASSERT(strstr(main_buf, "\"agentlanes\", rpc_agent_lanes") != NULL);
        ASSERT(strstr(main_buf, "\"agentliveness\", rpc_agent_liveness")
               != NULL);
        ASSERT(strstr(main_buf, "\"agentimpact\", rpc_agent_impact") != NULL);
        ASSERT(strstr(main_buf, "\"agentcontracts\", rpc_agent_contracts")
               != NULL);
        ASSERT(strstr(main_buf, "\"agentbuild\", rpc_agent_build") != NULL);
        ASSERT(strstr(main_buf, "\"agentdevstatus\", rpc_agent_dev_status")
               != NULL);
        ASSERT(strstr(main_buf, "\"anchorstatus\", rpc_agent_anchor_status")
               != NULL);
        ASSERT(strstr(main_buf, "\"appprotocols\", rpc_app_protocols")
               != NULL);
        ASSERT(strstr(main_buf, "\"servicecatalog\", rpc_service_catalog")
               != NULL);
        ASSERT(strstr(main_buf, "\"agentinterface\", rpc_agent_interface")
               != NULL);
        ASSERT(strstr(main_buf, "\"agentops\", rpc_agent_ops") != NULL);
        ASSERT(strstr(main_buf, "\"statecatalog\", diag_rpc_statecatalog")
               != NULL);
        ASSERT(strstr(main_buf, "strcmp(method, \"agentdeployguard\")")
               != NULL);
        ASSERT(strstr(main_buf,
                      "\"agentdeployguard\", rpc_agent_deploy_guard")
               != NULL);
        ASSERT(strstr(main_buf, "route->handler(&params, false, &result)")
               != NULL);
        char *static_agent_dispatch =
            strstr(main_buf, "if (cli_static_agent_method(method))");
        char *cookie_read = static_agent_dispatch
            ? strstr(static_agent_dispatch, "if (!cli_read_cookie(datadir))")
            : NULL;
        ASSERT(static_agent_dispatch != NULL);
        ASSERT(cookie_read != NULL);
        ASSERT(static_agent_dispatch < cookie_read);
        ASSERT(strstr(event_buf, "{ \"control\", \"api\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"apiindex\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"appprotocols\"")
               != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"protocols\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"servicecatalog\"")
               != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"service_catalog\"")
               != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"agent\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"status\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"agentops\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"agentdiagnose\"")
               != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"timeline\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"agentmap\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"agentlanes\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"agentliveness\"")
               != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"agentimpact\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"agentcontracts\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"agentbuild\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"anchorstatus\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"agentinterface\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"agentdeployguard\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"summary\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"milestone\"") != NULL);
        ASSERT(strstr(event_buf, "{ \"control\", \"refold\"") != NULL);
        ASSERT(strstr(diag_ctrl_buf, "{ \"control\", \"statecatalog\"")
               != NULL);
        ASSERT(strstr(diag_reg_buf, "diagnostics_dumper_count") != NULL);
        ASSERT(strstr(diag_reg_buf, "diagnostics_dumper_at") != NULL);
        ASSERT(strstr(diag_catalog_buf, "zcl.state_catalog.v1") != NULL);
        ASSERT(strstr(diag_catalog_buf, "diagnostics_catalog_push_entry")
               != NULL);
        ASSERT(strstr(diag_catalog_buf, "diagnostics_catalog_cost") != NULL);
        ASSERT(strstr(diag_catalog_buf, "diagnostics_catalog_owner_file")
               != NULL);
        ASSERT(strstr(diag_catalog_buf, "\"owner_file\"") != NULL);
        ASSERT(strstr(diag_catalog_buf, "\"safety_level\"") != NULL);
        ASSERT(strstr(diag_catalog_buf, "\"accepted_keys\"") != NULL);
        ASSERT(strstr(diag_catalog_buf, "\"tests\"") != NULL);
        ASSERT(strstr(diag_catalog_buf, "\"drilldowns\"") != NULL);
        ASSERT(strstr(agent_summary_buf, "api_version\", \"v1\"") != NULL);
        ASSERT(strstr(event_buf, "#include \"event_agent_summary.h\"") != NULL);
        ASSERT(strstr(event_buf, "rpc_agent_summary") != NULL);
        ASSERT(strstr(agent_summary_buf, "zcl.public_status.v1") != NULL);
        ASSERT(strstr(agent_summary_buf, "agent_push_first_call_simple_json")
               != NULL);
        ASSERT(strstr(agent_summary_buf,
                      "ZCL_AGENT_FIRST_CALL_BUDGET_AGENT_MS") != NULL);
        ASSERT(strstr(agent_first_call_buf,
                      "zcl.first_call_contract.v1") != NULL);
        ASSERT(strstr(agent_first_call_buf, "\"result_completeness\"")
               != NULL);
        ASSERT(strstr(agent_first_call_buf,
                      "platform_time_monotonic_us") != NULL);
        ASSERT(strstr(agent_first_call_buf, "\"budget_ms\"") != NULL);
        ASSERT(strstr(agent_first_call_buf, "\"elapsed_ms\"") != NULL);
        ASSERT(strstr(agent_first_call_buf, "\"budget_exceeded\"")
               != NULL);
        ASSERT(strstr(agent_summary_buf,
                      "agent_operator_latch_suppressed_by_mirror") != NULL);
        ASSERT(strstr(agent_summary_buf,
                      "agent_push_operator_latch_contract_json") != NULL);
        ASSERT(strstr(agent_summary_buf,
                      "agent_push_condition_summary_contract_json") != NULL);
        ASSERT(strstr(agent_operator_buf, "zcl.operator_latch.v1") != NULL);
        ASSERT(strstr(agent_operator_buf,
                      "zcl.condition_engine_summary.v1") != NULL);
        ASSERT(strstr(agent_summary_buf,
                      "legacy_mirror_sync_push_status_contract_json")
               != NULL);
        ASSERT(strstr(agent_operator_buf,
                      "suppressed_by_mirror_contract") != NULL);
        /* Conditions are captured in one registry pass (operator-snapshot
         * refactor) — the summary reads condition_engine_get_summary, not
         * the per-count getters. */
        ASSERT(strstr(agent_summary_buf,
                      "condition_engine_get_summary") != NULL);
        ASSERT(strstr(agent_summary_buf, "agent_fast_collect") != NULL);
        ASSERT(strstr(agent_summary_buf, "agent_summary_push_detail_json")
               != NULL);
        ASSERT(strstr(agent_summary_buf, "dl_get_stats") != NULL);
        ASSERT(strstr(agent_summary_buf, "dl_get_diagnostics") != NULL);
        ASSERT(strstr(agent_summary_buf, "dl_get_throughput") != NULL);
        ASSERT(strstr(agent_summary_buf, "assign_attempts") != NULL);
        ASSERT(strstr(agent_summary_buf, "message_send_calls") != NULL);
        ASSERT(strstr(agent_summary_buf, "connman_get_message_cycle_stats")
               != NULL);
        ASSERT(strstr(agent_summary_buf, "dl_assign_result_name") == NULL);
        ASSERT(strstr(agent_summary_json_buf, "dl_assign_result_name")
               != NULL);
        ASSERT(strstr(agent_summary_buf, "sync_monitor_tip_advance_age")
               != NULL);
        ASSERT(strstr(agent_summary_buf, "AGENT_CATCHUP_STALL_SECS")
               != NULL);
        ASSERT(strstr(agent_summary_buf, "AGENT_DISPATCH_IDLE_SECS")
               != NULL);
        ASSERT(strstr(agent_summary_buf, "catchup_stalled") != NULL);
        ASSERT(strstr(agent_summary_buf, "download_dispatch_idle")
               != NULL);
        ASSERT(strstr(agent_summary_buf,
                      "block_source_policy_get_cached_status") != NULL);
        ASSERT(strstr(agent_summary_buf, "block_source_policy_get_status")
               == NULL);
        ASSERT(strstr(agent_summary_buf, "api_served_tip_height()") == NULL);
        ASSERT(strstr(agent_summary_buf, "node_db_sync_get_job_status")
               != NULL);
        ASSERT(strstr(agent_summary_json_buf, "\"indexer\"") != NULL);
        ASSERT(strstr(agent_summary_buf, "agent_push_readiness_contract_json")
               != NULL);
        ASSERT(strstr(api_status_buf, "agent_push_readiness_contract_json")
               != NULL);
        ASSERT(strstr(agent_summary_buf, "agent_push_security_posture_json")
               != NULL);
        ASSERT(strstr(api_status_buf, "agent_push_security_posture_json")
               != NULL);
        ASSERT(strstr(agent_readiness_buf, "agent_push_readiness_json")
               != NULL);
        ASSERT(strstr(agent_readiness_buf, "agent_push_readiness_fields_json")
               != NULL);
        ASSERT(strstr(agent_summary_buf, "agent_push_height_contract_fields_json")
               != NULL);
        ASSERT(strstr(api_status_buf, "agent_push_height_contract_fields_json")
               != NULL);
        ASSERT(strstr(agent_readiness_buf, "zcl.agent_readiness.v1") != NULL);
        ASSERT(strstr(agent_schema_registry_buf, "zcl.security_posture.v1")
               != NULL);
        ASSERT(strstr(agent_readiness_buf, "chain_serving_ready") != NULL);
        ASSERT(strstr(agent_readiness_buf, "index_projection_ready") != NULL);
        ASSERT(strstr(agent_readiness_buf, "agent_work_ready") != NULL);
        ASSERT(strstr(agent_readiness_buf, "readiness_status") != NULL);
        ASSERT(strstr(agent_readiness_buf, "readiness_next_action") != NULL);
        ASSERT(strstr(agent_readiness_buf, "serving_projection_deferred")
               != NULL);
        ASSERT(strstr(agent_summary_buf, "projection_lag") != NULL);
        ASSERT(strstr(agent_summary_buf, "projection_deferred") != NULL);
        ASSERT(strstr(agent_summary_buf, "projection_catchup_active")
               != NULL);
        ASSERT(strstr(agent_summary_buf, "node_health_collect(") == NULL);
        ASSERT(strstr(agent_ctrl_buf, "zcl.agent_map.v1") != NULL);
        ASSERT(strstr(agent_lanes_buf, "zcl.agent_lanes.v1") != NULL);
        ASSERT(strstr(agent_liveness_buf, "zcl.agent_liveness.v1") != NULL);
        ASSERT(strstr(agent_liveness_buf, "agent_push_first_call_simple_json")
               != NULL);
        ASSERT(strstr(agent_liveness_buf,
                      "ZCL_AGENT_FIRST_CALL_BUDGET_LIVENESS_MS") != NULL);
        ASSERT(strstr(agent_liveness_buf,
                      "background_quality_skipped_due_to_first_call_budget")
               != NULL);
        ASSERT(strstr(agent_liveness_buf, "supervisor_dump_state_json")
               != NULL);
        ASSERT(strstr(agent_liveness_buf,
                      "agent_build_background_quality_status") != NULL);
        ASSERT(strstr(agent_liveness_buf, "effective_runtime_reachable")
               != NULL);
        ASSERT(strstr(agent_liveness_buf, "effective_runtime_scope")
               != NULL);
        ASSERT(strstr(agent_diagnose_buf, "zcl.agent_diagnose.v1")
               != NULL);
        ASSERT(strstr(agent_diagnose_buf,
                      "ZCL_AGENT_FIRST_CALL_BUDGET_DIAGNOSE_MS") != NULL);
        ASSERT(strstr(agent_diagnose_buf, "peer_lifecycle_incidents_json")
               != NULL);
        ASSERT(strstr(agent_diagnose_buf, "rpc_healthcheck") != NULL);
        ASSERT(strstr(agent_diagnose_buf, "rpc_timeline") != NULL);
        ASSERT(strstr(agent_diagnose_buf, "diag_rpc_getmirrorstatus")
               != NULL);
        ASSERT(strstr(agent_liveness_buf,
                      "agent_push_contract_mcp_tool_json(&arr, \"agent\")")
               != NULL);
        ASSERT(strstr(agent_liveness_buf,
                      "agent_push_contract_mcp_tool_json(&arr, \"agentlanes\")")
               != NULL);
        ASSERT(strstr(agent_liveness_buf,
                      "agent_push_contract_mcp_tool_json(&arr, \"agentbuild\")")
               != NULL);
        ASSERT(strstr(event_timeline_buf,
                      "#include \"controllers/agent_controller.h\"") != NULL);
        ASSERT(strstr(event_timeline_buf,
                      "agent_push_contract_mcp_tool_json(&arr, "
                      "\"agentinterface\")")
               != NULL);
        ASSERT(strstr(event_timeline_buf,
                      "agent_push_contract_mcp_tool_json(&arr, \"agentops\")")
               != NULL);
        ASSERT(strstr(event_timeline_buf,
                      "agent_push_contract_mcp_tool_json(&arr, "
                      "\"statecatalog\")")
               != NULL);
        ASSERT(strstr(agent_diagnose_buf,
                      "agent_push_contract_native_command_json(&commands, "
                      "\"agent\")")
               != NULL);
        ASSERT(strstr(agent_diagnose_buf,
                      "agent_push_contract_native_command_json(&commands, "
                      "\"agentliveness\")")
               != NULL);
        ASSERT(strstr(agent_diagnose_buf,
                      "agent_push_contract_native_command_json(&commands, "
                      "\"timeline\")")
               != NULL);
        ASSERT(strstr(agent_anchor_status_buf,
                      "zcl.anchor_mint_status.v1") != NULL);
        ASSERT(strstr(agent_anchor_status_buf, "progress.kv") != NULL);
        ASSERT(strstr(agent_anchor_status_buf,
                      "validated_backlog_blocks") != NULL);
        ASSERT(strstr(agent_anchor_status_buf,
                      "stale_header_rows_above_anchor") != NULL);
        ASSERT(strstr(agent_anchor_status_buf,
                      "agent_next_action") != NULL);
        ASSERT(strstr(agent_anchor_status_buf,
                      "inspect_utxo_apply_idle_reason_before_waiting_more")
               != NULL);
        ASSERT(strstr(agent_ctrl_buf, "zcl.agent_impact.v1") != NULL);
        ASSERT(strstr(agent_ctrl_buf,
                      "app/controllers/src/agent_anchor_status_controller.c")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "anchorstatus") != NULL);
        ASSERT(strstr(agent_contracts_buf, "zcl.agent_contracts.v1") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "AGENT_CONTRACT") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl.public_status.v1")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "runtime_status_alias")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zclassic23 status")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl.agent_interface.v1")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl.agent_ops.v1") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl.agent_diagnose.v1")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl_agent_diagnose")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl.agent_liveness.v1")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl_agent_liveness")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl.agent_dev_status.v1")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl_agent_dev_status")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl.timeline.v1") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl_timeline") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl.state_catalog.v1")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl_state_catalog")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl.agent_deploy_guard.v1")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf,
                      "zcl.anchor_mint_status.v1") != NULL);
        ASSERT(strstr(agent_contracts_def_buf,
                      "zcl.operator_proof_bundle.v1") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl_proof_bundle") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "zcl.agent_build.v1") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "dev_node_binary") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "make agent-loop") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "ZCL_AGENT_LOOP_BIN=1 make agent-loop")
               != NULL);
        ASSERT(strstr(agent_ctrl_buf,
                      "ZCL_AGENT_LOOP_DEPLOY=dev make agent-loop") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "make agent-deploy-fast") != NULL);
        ASSERT(strstr(agent_ctrl_buf,
                      "build/bin/zclassic23 mcpcall <tool> [json]")
               != NULL);
        ASSERT(strstr(agent_ctrl_buf, "make agent-mcp-call-hot") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "make agent-mcp-call-dev") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "ZCL_AGENT_MCP_BUILD") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "make agent-dev-status") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "make agent-doctor") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "zclassic23 agentdevstatus") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "zcl_agent_dev_status") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "make agent-stage-dev") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "make dev-bin") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "build/bin/zclassic23-dev") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "ZCL_DEV_HOT_OPT=-O2") != NULL);
        ASSERT(strstr(agent_schema_registry_buf,
                      "zcl.background_quality_runtime.v1") != NULL);
        ASSERT(strstr(agent_schema_registry_buf,
                      "zcl.agent_dev_status.v1") != NULL);
        ASSERT(strstr(agent_schema_registry_buf, "zcl.first_call_contract.v1")
               != NULL);
        ASSERT(strstr(agent_schema_registry_buf,
                      "zcl.agent_readiness.v1") != NULL);
        ASSERT(strstr(agent_schema_registry_buf, "zcl.height_contract.v1")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl.mirror_status.v1")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "ops_surface") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "ops_rank") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "ops_name") != NULL);
        ASSERT(strstr(agent_schema_registry_buf,
                      "zcl.operator_latch.v1") != NULL);
        ASSERT(strstr(agent_schema_registry_buf,
                      "zcl.condition_engine_summary.v1") != NULL);
        ASSERT(strstr(agent_contracts_buf,
                      "contracts_push_agent_registry_schemas") != NULL);
        ASSERT(strstr(agent_contracts_buf,
                      "agent_push_contract_schema_surface_json") != NULL);
        ASSERT(strstr(agent_schema_registry_buf, "g_agent_schema_surfaces")
               != NULL);
        ASSERT(strstr(agent_schema_registry_buf,
                      "agent_contract_schema_surface_count") != NULL);
        ASSERT(strstr(agent_contracts_buf,
                      "agent_push_contract_summary_json") != NULL);
        ASSERT(strstr(agent_contracts_buf, "agent_contract_count()")
               != NULL);
        ASSERT(strstr(agent_contracts_buf, "agent_contract_at(i)")
               != NULL);
        ASSERT(strstr(agent_schema_registry_buf,
                      "zcl.agent_runtime_availability.v1") != NULL);
        ASSERT(strstr(agent_registry_buf, "schema_surface_count") != NULL);
        ASSERT(strstr(agent_registry_buf, "schema_registry_source") != NULL);
        ASSERT(strstr(agent_ops_buf, "zcl.agent_ops.v1") != NULL);
        ASSERT(strstr(agent_ops_buf,
                      "agent_push_contract_field_surface_json(result, \"agentops.first_call\")")
               != NULL);
        ASSERT(strstr(agent_ops_buf,
                      "agent_push_contract_ops_surface_json(&api_rules, \"direct\")")
               != NULL);
        ASSERT(strstr(agent_ops_buf,
                      "agent_push_contract_ops_surface_json(&commands, \"drilldown\")")
               != NULL);
        ASSERT(strstr(agent_ops_buf,
                      "agent_contract_lookup(") == NULL);
        ASSERT(strstr(agent_ops_buf, "anchor_status_command") == NULL);
        ASSERT(strstr(agent_contracts_def_buf, "anchor_mint_status") != NULL);
        ASSERT(strstr(agent_ops_buf, "refold_plain_english") != NULL);
        ASSERT(strstr(agent_ops_buf, "diagnostics_drilldown_command")
               == NULL);
        ASSERT(strstr(agent_ops_buf, "no_jq_required") != NULL);
        ASSERT(strstr(agent_ops_buf, "diagnose_command") == NULL);
        ASSERT(strstr(agent_ops_buf, "deploy_guard_command") == NULL);
        ASSERT(strstr(agent_ops_buf, "agentdiagnose") == NULL);
        ASSERT(strstr(agent_ops_buf, "top_next_work") != NULL);
        ASSERT(strstr(agent_ops_buf, "api_gaps") != NULL);
        ASSERT(strstr(agent_ops_buf, "api_ux") != NULL);
        ASSERT(strstr(agent_ops_buf, "agentops.workflow") != NULL);
        ASSERT(strstr(agent_ops_buf,
                      "agent_push_contract_work_surface_json(&gaps")
               != NULL);
        ASSERT(strstr(agent_ops_buf,
                      "agent_push_contract_work_surface_json(&workflow")
               != NULL);
        ASSERT(strstr(agent_ops_buf,
                      "agent_push_contract_work_surface_json(&work")
               != NULL);
        ASSERT(strstr(agent_ops_buf,
                      "agent_push_contract_review_surface_json(&review")
               != NULL);
        ASSERT(strstr(agent_ops_buf, "main_dry_problem") == NULL);
        ASSERT(strstr(agent_review_registry_buf,
                      "agentops.architecture_review") != NULL);
        ASSERT(strstr(agent_review_registry_buf,
                      "g_agent_review_surfaces") != NULL);
        ASSERT(strstr(agent_review_registry_buf,
                      "agent_contract_review_surface_count") != NULL);
        ASSERT(strstr(agent_review_registry_buf,
                      "agent_push_contract_review_surface_json") != NULL);
        ASSERT(strstr(agent_review_registry_buf, "main_dry_problem")
               != NULL);
        ASSERT(strstr(agent_ops_buf, "agentops.api_gaps") != NULL);
        ASSERT(strstr(agent_ops_buf, "agentops.top_next_work") != NULL);
        ASSERT(strstr(agent_ops_buf,
                      "finish_self_verified_utxo_anchor_rebuild") == NULL);
        ASSERT(strstr(agent_schema_registry_buf,
                      "zcl.agent_runtime_identity.v1")
               != NULL);
        ASSERT(strstr(agent_contracts_buf, "No Python is required") != NULL);
        ASSERT(strstr(agent_iface_buf, "build_commit") != NULL);
        ASSERT(strstr(agent_iface_buf, "runtime_identity") != NULL);
        ASSERT(strstr(agent_iface_buf, "runtime_availability") != NULL);
        ASSERT(strstr(agent_iface_buf, "preferred_transport") != NULL);
        ASSERT(strstr(agent_iface_buf,
                      "agent_push_contract_capabilities_json(&capabilities)")
               != NULL);
        ASSERT(strstr(agent_iface_buf,
                      "agent_push_contract_capability_json(") != NULL);
        ASSERT(strstr(agent_capability_registry_buf,
                      "agent_contract_count()") != NULL);
        ASSERT(strstr(agent_capability_registry_buf,
                      "agent_contract_at(i)") != NULL);
        ASSERT(strstr(agent_capability_registry_buf,
                      "registry_alias") != NULL);
        ASSERT(strstr(agent_capability_registry_buf,
                      "canonical_capability") != NULL);
        ASSERT(strstr(agent_capability_registry_buf,
                      "contract_source") != NULL);
        ASSERT(strstr(agent_iface_buf,
                      "agent_push_contract_mcp_field_json(&loop")
               != NULL);
        ASSERT(strstr(agent_iface_buf, "must_live_in_c") != NULL);
        ASSERT(strstr(agent_iface_buf,
                      "app/controllers/src/agent_interface_controller.c")
               != NULL);
        ASSERT(strstr(agent_iface_buf,
                      "do not require Python to parse agent API JSON")
               != NULL);
        ASSERT(strstr(agent_schema_registry_buf, "zcl.operator_lane.v1")
               != NULL);
        ASSERT(strstr(agent_schema_registry_buf,
                      "zcl.operator_deployment_safety.v1") != NULL);
        ASSERT(strstr(agent_schema_registry_buf,
                      "zcl.agent_runtime_services.v1") != NULL);
        ASSERT(strstr(agent_schema_registry_buf,
                      "zcl.mvp_operator_proofs.v1") != NULL);
        ASSERT(strstr(agent_lanes_buf, "agent_push_lane_topology") != NULL);
        ASSERT(strstr(agent_lanes_buf,
                      "agent_push_runtime_services_json") != NULL);
        ASSERT(strstr(agent_lanes_buf, "default_deploy_target") != NULL);
        ASSERT(strstr(agent_lanes_buf,
                      "agent_operator_lane_topology_count") != NULL);
        ASSERT(strstr(agent_runtime_buf,
                      "zcl.agent_runtime_services.v1") != NULL);
        ASSERT(strstr(agent_runtime_buf,
                      "zcl.agent_runtime_availability.v1") != NULL);
        ASSERT(strstr(agent_runtime_buf, "controllers/agent_contracts.def")
               != NULL);
        ASSERT(strstr(agent_runtime_buf, "agent_contract_count()") != NULL);
        ASSERT(strstr(agent_runtime_buf, "agent_contract_at(i)") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "static const struct agent_contract g_agent_contracts")
               != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "g_agent_command_surfaces") != NULL);
        ASSERT(strstr(agent_registry_buf, "g_agent_field_surfaces")
               != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "agent_contract_field_surface_count") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "agent_push_contract_field_surface_json") != NULL);
        ASSERT(strstr(agent_registry_buf, "agentops.first_call") != NULL);
        ASSERT(strstr(agent_registry_buf, "agentops.workflow") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "drill_down_only_when_needed") != NULL);
        ASSERT(strstr(agent_registry_buf, "anchor_status_command") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "diagnostics_drilldown_command") != NULL);
        ASSERT(strstr(agent_registry_buf, "deploy_guard_command") != NULL);
        ASSERT(strstr(agent_registry_buf, "diagnose_command") != NULL);
        ASSERT(strstr(agent_registry_buf, "agentdiagnose") != NULL);
        ASSERT(strstr(agent_registry_buf, "DIRECT_COMMAND") != NULL);
        ASSERT(strstr(agent_registry_buf, "native_override") != NULL);
        ASSERT(strstr(agent_registry_buf, "mcp_override") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "agent_contract_command_surface_count") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "agent_push_contract_command_surface_json") != NULL);
        ASSERT(strstr(agent_registry_buf, "g_agent_work_surfaces") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "agent_contract_work_surface_count") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "agent_push_contract_work_surface_json") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "mcp_binding_contract") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "native_declared_count") != NULL);
        ASSERT(strstr(agent_registry_buf, "mcp_declared_count") != NULL);
        ASSERT(strstr(agent_registry_buf, "rest_declared_count") != NULL);
        ASSERT(strstr(agent_registry_buf, "field_surface_count") != NULL);
        ASSERT(strstr(agent_registry_buf, "review_surface_count") != NULL);
        ASSERT(strstr(agent_registry_buf, "review_registry_source") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "agent_contract_review_surface_total_count") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "agentmap.commands.core") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "agentmap.commands.drilldown") != NULL);
        ASSERT(strstr(agent_registry_buf, "agentmap.telemetry") != NULL);
        ASSERT(strstr(agent_registry_buf, "\"node_db\", \"dbquery\"")
               != NULL);
        ASSERT(strstr(agent_registry_buf, "\"events\", \"eventlog\"")
               != NULL);
        ASSERT(strstr(agent_registry_buf, "\"command_center\"") != NULL);
        ASSERT(strstr(agent_registry_buf, "\"full_status\"") != NULL);
        ASSERT(strstr(agent_registry_buf, "\"quality_lanes\"") != NULL);
        ASSERT(strstr(agent_registry_buf, "zcl_operator_summary") != NULL);
        ASSERT(strstr(agent_registry_buf, "make quality-linger-status")
               != NULL);
        ASSERT(strstr(agent_registry_buf, "agent_contract_probe_params_json")
               != NULL);
        ASSERT(strstr(agent_registry_buf, "sqlite_master") != NULL);
        ASSERT(strstr(agent_registry_buf, "strcmp(method, \"eventlog\")")
               != NULL);
        ASSERT(strstr(agent_registry_buf, "agentops.api_gaps") != NULL);
        ASSERT(strstr(agent_registry_buf, "agentops.top_next_work") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "finish_self_verified_utxo_anchor_rebuild") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "harden_peer_bootstrap_lifecycle") != NULL);
        ASSERT(strstr(agent_registry_buf, "promote_mvp_operator_proofs")
               != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "shrink_boot_refold_supervised_units") != NULL);
        ASSERT(strstr(agent_registry_buf, "dry_agent_contract_registry")
               == NULL);
        ASSERT(strstr(agent_registry_buf, "agent_contract_lookup") != NULL);
        ASSERT(strstr(agent_registry_buf, "agent_print_native_usage") != NULL);
        ASSERT(strstr(agent_registry_buf, "agent_native_usage_tail") != NULL);
        ASSERT(strstr(agent_registry_buf, "native_command[prefix_len]")
               != NULL);
        ASSERT(strstr(agent_registry_buf, "c->native_command") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "agent_push_contract_command_json") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "agent_push_contract_native_field_json") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "agent_push_contract_mcp_field_json") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "agent_push_contract_native_command_json") != NULL);
        ASSERT(strstr(agent_registry_buf,
                      "agent_push_contract_mcp_tool_json") != NULL);
        ASSERT(strstr(agent_runtime_buf,
                      "unsupported_method_not_found") != NULL);
        ASSERT(strstr(agent_runtime_buf,
                      "target_runtime_support") != NULL);
        ASSERT(strstr(agent_runtime_buf,
                      "agent_runtime_probe_method_count") != NULL);
        ASSERT(strstr(agent_runtime_buf, "rpc_running") != NULL);
        ASSERT(strstr(agent_runtime_buf, "https_bound_port") != NULL);
        ASSERT(strstr(agent_runtime_buf, "fs_bound_port") != NULL);
        ASSERT(strstr(agent_schema_registry_buf, "zcl.node_resources.v1")
               != NULL);
        ASSERT(strstr(agent_contracts_buf,
                      "Automation must read deployment_safety") != NULL);
        ASSERT(strstr(agent_lane_runtime_buf, "zcl.operator_lane.v1")
               != NULL);
        ASSERT(strstr(agent_lane_runtime_buf,
                      "zcl.operator_deployment_safety.v1") != NULL);
        ASSERT(strstr(agent_lane_runtime_buf,
                      "agent_fill_operator_lane_contract_json") != NULL);
        ASSERT(strstr(agent_lane_runtime_buf,
                      "agent_operator_lane_topology_lookup") != NULL);
        ASSERT(strstr(agent_lane_runtime_buf, "zcl23-dev") != NULL);
        ASSERT(strstr(agent_lanes_buf,
                      "agent_push_contract_command_json(&commands, "
                      "\"status\", \"agent\"") != NULL);
        ASSERT(strstr(agent_lanes_buf,
                      "agent_push_contract_command_json(&commands, "
                      "\"lane_topology\"") != NULL);
        ASSERT(strstr(agent_lanes_buf,
                      "\"agentlanes\"") != NULL);
        ASSERT(strstr(agent_lanes_buf,
                      "agent_push_contract_command_json(&commands, "
                      "\"deploy_guard\"") != NULL);
        ASSERT(strstr(agent_lanes_buf,
                      "\"agentdeployguard\"") != NULL);
        ASSERT(strstr(agent_lanes_buf, "zclassic23 agent\",") == NULL);
        ASSERT(strstr(agent_lanes_buf, "zcl_agent\",") == NULL);
        ASSERT(strstr(agent_lanes_buf, "agent_lanes_push_external_command")
               != NULL);
        ASSERT(strstr(agent_iface_buf, "~/.zclassic-c23-dev") == NULL);
        ASSERT(strstr(agent_iface_buf, "agent_deploy_action_target_lane")
               != NULL);
        ASSERT(strstr(agent_iface_buf,
                      "agent_fill_known_operator_lane_contract_json")
               != NULL);
        ASSERT(strstr(agent_runtime_buf,
                      "agent_push_operator_lane_fields_json") != NULL);
        ASSERT(strstr(agent_summary_buf,
                      "agent_push_operator_lane_fields_json") != NULL);
        ASSERT(strstr(agent_lane_runtime_buf, "restart_policy") != NULL);
        ASSERT(strstr(agent_lane_runtime_buf, "safety_contract") != NULL);
        ASSERT(strstr(agent_lane_runtime_buf, "automation_restart_ok")
               != NULL);
        ASSERT(strstr(agent_lane_runtime_buf, "automation_deploy_ok")
               != NULL);
        ASSERT(strstr(agent_lane_runtime_buf,
                      "requires_operator_confirmation") != NULL);
        ASSERT(strstr(agent_lane_runtime_buf,
                      "safe_default_action") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "make ci-reproducible") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "ZCL_FAST_CACHE") != NULL);
        ASSERT(strstr(agent_ctrl_buf, ".cache/zcl-agent-fast-ci") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "agent_impact_rules.def") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "shared_rule_hits") != NULL);
        ASSERT(strstr(agent_ctrl_buf,
                      "agent_push_contract_command_surface_json(&commands")
               != NULL);
        ASSERT(strstr(agent_ctrl_buf, "agentmap.commands.core") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "agentmap.commands.drilldown") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "agentmap.telemetry") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "agent_push_command(") == NULL);
        ASSERT(strstr(agent_ctrl_buf, "command_center") == NULL);
        ASSERT(strstr(agent_ctrl_buf, "full_status") == NULL);
        ASSERT(strstr(agent_ctrl_buf, "zcl_operator_summary") == NULL);
        ASSERT(strstr(agent_ctrl_buf, "telemetry_drilldowns") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "zcl_events") == NULL);
        ASSERT(strstr(agent_ctrl_buf, "zclassic23 healthcheck") == NULL);
        ASSERT(strstr(agent_ctrl_buf, "zclassic23 dbquery <select>") == NULL);
        ASSERT(strstr(agent_ctrl_buf, "zclassic23 eventlog <count>")
               == NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl.sql_result.v1") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl_sql") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zclassic23 dbquery <SELECT>")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl.event_log.v1") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl_events") != NULL);
        ASSERT(strstr(agent_contracts_def_buf,
                      "zclassic23 eventlog <count>") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "background_quality_lanes") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "background_quality_status") != NULL);
        ASSERT(strstr(agent_bg_quality_buf, "native_status_reader") != NULL);
        ASSERT(strstr(agent_bg_quality_buf, "requires_python") != NULL);
        ASSERT(strstr(agent_bg_quality_buf, "ZCL_QUALITY_STATE_DIR") != NULL);
        ASSERT(strstr(agent_bg_quality_buf,
                      "agent_quality_read_json_file") != NULL);
        ASSERT(strstr(agent_bg_quality_buf,
                      "commit_matches_expected") != NULL);
        ASSERT(strstr(agent_bg_quality_buf,
                      "background_quality_stale") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "make quality-linger-status") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "zclassic23-fuzz.timer") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "zclassic23-coverage.timer") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "zclassic23-test-suite.timer") != NULL);
        ASSERT(strstr(agent_ctrl_buf, "pre-push-ci skips live service probe")
               != NULL);
        ASSERT(strstr(agent_ctrl_buf, "app/controllers/src/agent_controller.c")
               != NULL);
        ASSERT(strstr(agent_ctrl_buf,
                      "app/controllers/src/agent_background_quality.c")
               != NULL);
        ASSERT(strstr(agent_ctrl_buf,
                      "app/controllers/src/agent_lanes_controller.c")
               != NULL);
        ASSERT(strstr(agent_ctrl_buf,
                      "app/controllers/src/agent_liveness_controller.c")
               != NULL);
        ASSERT(strstr(agent_ctrl_buf, "zcl_agent_liveness") != NULL);
        ASSERT(strstr(agent_contracts_def_buf,
                      "\"api_command\", \"api_tool\"") != NULL);
        ASSERT(strstr(agent_contracts_def_buf,
                      "\"app_protocols_command\", "
                      "\"app_protocols_tool\"") != NULL);
        ASSERT(strstr(agent_contracts_def_buf,
                      "zcl.application_protocols.index.v1") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl_app_protocols")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf,
                      "\"service_catalog_command\", "
                      "\"service_catalog_tool\"") != NULL);
        ASSERT(strstr(agent_contracts_def_buf,
                      "zcl.service_catalog.v1") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl_service_catalog")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf,
                      "\"first_command\", \"first_tool\"") != NULL);
        ASSERT(strstr(agent_contracts_def_buf,
                      "\"ops_command\", \"ops_tool\"") != NULL);
        ASSERT(strstr(agent_contracts_def_buf,
                      "\"mirror_command\", \"mirror_tool\"") != NULL);
        ASSERT(strstr(agent_registry_buf, "api_cli_field") != NULL);
        ASSERT(strstr(agent_registry_buf, "api_mcp_field") != NULL);
        ASSERT(strstr(api_buf,
                      "agent_push_contract_api_cli_fields_json(cli)")
               != NULL);
        ASSERT(strstr(api_buf,
                      "agent_push_contract_api_mcp_fields_json(mcp)")
               != NULL);
        ASSERT(strstr(api_buf,
                      "agent_push_contract_native_field_json(cli,")
               == NULL);
        ASSERT(strstr(api_buf,
                      "agent_push_contract_mcp_field_json(mcp,")
               == NULL);
        ASSERT(strstr(api_buf,
                      "json_push_kv_str(cli, \"milestone_command\"")
               == NULL);
        ASSERT(strstr(api_buf,
                      "json_push_kv_str(cli, \"refold_command\"")
               == NULL);
        ASSERT(strstr(api_buf,
                      "json_push_kv_str(mcp, \"milestone_tool\"")
               == NULL);
        ASSERT(strstr(api_buf,
                      "json_push_kv_str(mcp, \"refold_tool\"")
               == NULL);
        ASSERT(strstr(agent_contracts_def_buf,
                      "\"drilldown_command\", \"drilldown_tool\"") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl.healthcheck.v1") != NULL);
        ASSERT(strstr(agent_contracts_def_buf,
                      "zcl.milestone_status.v1") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl.refold_status.v1")
               != NULL);
        ASSERT(strstr(agent_contracts_def_buf,
                      "zcl.operator_proof_bundle.v1") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl_milestone") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl_refold_status") != NULL);
        ASSERT(strstr(agent_contracts_def_buf, "zcl_proof_bundle") != NULL);
        ASSERT(strstr(api_buf, "\"compat_command\"") == NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 agentbuild") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_agent_build") != NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 anchorstatus") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.anchor_mint_status.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 proofbundle") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_proof_bundle") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.operator_proof_bundle.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 appprotocols") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_app_protocols") != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "zcl.application_protocols.index.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 servicecatalog") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_service_catalog") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.service_catalog.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.service_contract.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 agentlanes") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_agent_lanes") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.agent_lanes.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 agentliveness") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_agent_liveness") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.agent_liveness.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "agent_contracts.def") != NULL);
        ASSERT(strstr(agent_doc_buf, "g_cli_static_agent_routes") != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "agent_push_contract_command_json()` for "
                      "registry-owned commands")
               != NULL);
        ASSERT(strstr(agent_doc_buf, "probe_params_json") != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "tools/scripts/lane_health.sh --json")
               != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "Do not add a second\nallowlist") != NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 getmirrorstatus") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_mirror_status") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.agent_runtime_services.v1")
               != NULL);
        ASSERT(strstr(agent_doc_buf, "configured boot intent") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.agent_readiness.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.height_contract.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.mirror_status.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.operator_latch.v1") != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "zcl.condition_engine_summary.v1") != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "suppressed_by_mirror_contract") != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "zcl_state subsystem=condition_engine") != NULL);
        ASSERT(strstr(agent_doc_buf, "chain_serving_ready") != NULL);
        ASSERT(strstr(agent_doc_buf, "index_projection_ready") != NULL);
        ASSERT(strstr(agent_doc_buf, "readiness_status") != NULL);
        ASSERT(strstr(agent_doc_buf, "readiness_next_action") != NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 agentinterface") != NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 status") != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "native compatibility alias for\n"
                      "`zclassic23 agent`") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_agent_interface") != NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 agentops") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_agent_ops") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.agent_ops.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 agentdiagnose") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_agent_diagnose") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.agent_diagnose.v1") != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "zcl.agent_runtime_availability.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "effective_runtime_reachable") != NULL);
        ASSERT(strstr(agent_doc_buf, "effective_runtime_scope") != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "`build/bin/zclassic-cli` as\n"
                      "a zclassic23 status oracle") != NULL);
        ASSERT(strstr(agent_doc_buf, "`build/bin/zcl-rpc getblockcount`")
               != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "`build/bin/zclassic-cli -rpcport=18232 getblockcount`")
               != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "false \"zclassic23 is behind\"\n"
                      "diagnosis") != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "unsupported_method_not_found") != NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 statecatalog") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_state_catalog") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.state_catalog.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 timeline") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_timeline") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.timeline.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "no_jq_required=true") != NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 agentdeployguard") != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "zclassic23 agentdeployguard deploy-dev")
               != NULL);
        ASSERT(strstr(agent_doc_buf, "process exit status") != NULL);
        ASSERT(strstr(agent_doc_buf, "JSON `exit_code`") != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "Scripts therefore do not need\n`jq`") != NULL);
        ASSERT(strstr(agent_doc_buf, "make check-agent-cli") != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "hermetic executable regression") != NULL);
        ASSERT(strstr(agent_doc_buf, "target_lane_name=\"dev\"") != NULL);
        ASSERT(strstr(agent_doc_buf, "target_lane_name=\"canonical\"")
               != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "zclassic23 agentdeployguard -operator-lane=dev deploy")
               != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_agent_deploy_guard") != NULL);
        ASSERT(strstr(agent_doc_buf, "No Python is required") != NULL);
        ASSERT(strstr(agent_doc_buf, "docs/AGENT_ARCHITECTURE.md") != NULL);
        ASSERT(strstr(agent_doc_buf, "make agent-mcp-call TOOL=zcl_status")
               != NULL);
        ASSERT(strstr(agent_doc_buf, "make agent-mcp-call-hot") != NULL);
        ASSERT(strstr(agent_doc_buf, "make agent-mcp-call-dev") != NULL);
        ASSERT(strstr(agent_doc_buf, "make agent-dev-status") != NULL);
        ASSERT(strstr(agent_doc_buf, "make agent-doctor") != NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 agentdevstatus") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_agent_dev_status") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.agent_dev_status.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "deploy_blocker") != NULL);
        ASSERT(strstr(agent_doc_buf, "auto_reindex_stale_candidate")
               != NULL);
        ASSERT(strstr(agent_doc_buf, "make agent-stage-dev") != NULL);
        ASSERT(strstr(agent_doc_buf, "zclassic23 mcpcall zcl_status")
               != NULL);
        ASSERT(strstr(agent_doc_buf, "make agent-loop") != NULL);
        ASSERT(strstr(agent_doc_buf, "ZCL_AGENT_LOOP_BIN=1") != NULL);
        ASSERT(strstr(agent_doc_buf, "ZCL_AGENT_LOOP_DEPLOY=dev") != NULL);
        ASSERT(strstr(agent_doc_buf, "make build-only") != NULL);
        ASSERT(strstr(agent_doc_buf, "make fast-rebuild") != NULL);
        ASSERT(strstr(agent_doc_buf, "make dev-bin") != NULL);
        ASSERT(strstr(agent_doc_buf, "build/bin/zclassic23-dev") != NULL);
        ASSERT(strstr(agent_doc_buf, "ZCL_DEV_HOT_OPT=-O2") != NULL);
        ASSERT(strstr(agent_doc_buf, "make t-fast ONLY=<group>") != NULL);
        ASSERT(strstr(agent_doc_buf, "ZCL_FAST_CACHE=0") != NULL);
        ASSERT(strstr(agent_doc_buf, "ZCL_FAST_CACHE_RESET=1") != NULL);
        ASSERT(strstr(agent_doc_buf, "make ci-reproducible") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_operator_summary") != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "zcl.operator_deployment_safety.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "automation_restart_ok") != NULL);
        ASSERT(strstr(agent_doc_buf, "automation_deploy_ok") != NULL);
        ASSERT(strstr(agent_doc_buf, "operator_lane_name") != NULL);
        ASSERT(strstr(agent_doc_buf, "preferred_deploy_target") != NULL);
        ASSERT(strstr(agent_doc_buf, "agent-deploy.json") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.agent_dev_deploy.v1") != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "requires_operator_confirmation") != NULL);
        ASSERT(strstr(agent_doc_buf, "safe_default_action") != NULL);
        ASSERT(strstr(agent_doc_buf, "tools/deploy_guard.sh canonical-deploy")
               != NULL);
        ASSERT(strstr(agent_doc_buf, "ZCL_DEPLOY_ALLOW_CANONICAL=1")
               != NULL);
        ASSERT(strstr(agent_doc_buf, "make deploy-dev") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_state") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_node_log") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl_sql") != NULL);
        ASSERT(strstr(agent_doc_buf, "make pre-push-ci") != NULL);
        ASSERT(strstr(agent_doc_buf, "ZCL_FAST_LIVE=0") != NULL);
        ASSERT(strstr(agent_doc_buf, "make install-quality-linger") != NULL);
        ASSERT(strstr(agent_doc_buf, "make quality-linger-status") != NULL);
        ASSERT(strstr(agent_doc_buf, "background_quality_status") != NULL);
        ASSERT(strstr(agent_doc_buf,
                      "zcl.background_quality_runtime.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "zcl.background_quality_lane.v1") != NULL);
        ASSERT(strstr(agent_doc_buf, "commit_freshness") != NULL);
        ASSERT(strstr(agent_doc_buf, "background_quality_stale") != NULL);
        ASSERT(strstr(agent_doc_buf, "Do not add new operator logic to `tools/z`")
               != NULL);
        PASS();
    } _test_next:;
    free(main_buf);
    free(event_buf);
    free(agent_summary_buf);
    free(agent_summary_json_buf);
    free(agent_operator_buf);
    free(agent_ctrl_buf);
    free(agent_capability_registry_buf);
    free(agent_registry_buf);
    free(agent_review_registry_buf);
    free(agent_schema_registry_buf);
    free(agent_contracts_buf);
    free(agent_contracts_def_buf);
    free(agent_bg_quality_buf);
    free(agent_first_call_buf);
    free(agent_lanes_buf);
    free(agent_lane_runtime_buf);
    free(agent_liveness_buf);
    free(agent_diagnose_buf);
    free(event_timeline_buf);
    free(agent_anchor_status_buf);
    free(agent_iface_buf);
    free(agent_ops_buf);
    free(agent_runtime_buf);
    free(agent_readiness_buf);
    free(diag_ctrl_buf);
    free(diag_reg_buf);
    free(diag_catalog_buf);
    free(api_buf);
    free(api_status_buf);
    free(agent_doc_buf);
    return failures;
}

static int t_mvp_reporters_resolve_live_service_rpc_contract(void)
{
    int failures = 0;
    char *scoreboard = NULL;
    char *gate = NULL;
    TEST("MVP reporters resolve the live service datadir and RPC port") {
        char scoreboard_path[PATH_MAX];
        char gate_path[PATH_MAX];
        ASSERT(repo_path(scoreboard_path, sizeof(scoreboard_path),
                         "tools/scripts/mvp_scoreboard.sh") == 0);
        ASSERT(repo_path(gate_path, sizeof(gate_path), "tools/mvp_gate.sh") == 0);
        ASSERT(read_entire_file(scoreboard_path, &scoreboard) == 0);
        ASSERT(read_entire_file(gate_path, &gate) == 0);

        ASSERT(strstr(scoreboard, "ZCL_NODE_UNIT=\"${ZCL_NODE_UNIT:-zclassic23}\"")
               != NULL);
        ASSERT(strstr(scoreboard, "systemd_exec_arg()") != NULL);
        ASSERT(strstr(scoreboard,
                      "systemctl --user show \"$ZCL_NODE_UNIT\" -p ExecStart --value")
               != NULL);
        ASSERT(strstr(scoreboard, "SERVICE_DATADIR=\"$(systemd_exec_arg datadir || true)\"")
               != NULL);
        ASSERT(strstr(scoreboard, "SERVICE_RPCPORT=\"$(systemd_exec_arg rpcport || true)\"")
               != NULL);
        ASSERT(strstr(scoreboard, "ZCL_DATADIR=$LIVE_DATADIR") != NULL);
        ASSERT(strstr(scoreboard, "ZCL_RPCPORT=$LIVE_RPCPORT") != NULL);
        ASSERT(strstr(scoreboard, "TIP_GAP_OK") != NULL);
        ASSERT(strstr(scoreboard, "LIVE_GAP=$(( LIVE_HEADERS - LIVE_HEIGHT ))")
               != NULL);
        ASSERT(strstr(scoreboard, "gap=${LIVE_GAP:-?}<=$TIP_GAP_OK")
               != NULL);
        ASSERT(strstr(scoreboard,
                      "NOT_MET) VERDICT[6]=\"BLOCKED\"")
               != NULL);
        ASSERT(strstr(scoreboard,
                      "clean 168h evidence is not established yet")
               != NULL);

        ASSERT(strstr(gate, "ZCL_NODE_UNIT=\"${ZCL_NODE_UNIT:-$ZCL_SOAK_UNIT}\"")
               != NULL);
        ASSERT(strstr(gate, "systemd_exec_arg()") != NULL);
        ASSERT(strstr(gate,
                      "systemctl --user show \"$ZCL_NODE_UNIT\" -p ExecStart --value")
               != NULL);
        ASSERT(strstr(gate, "SERVICE_DATADIR=\"$(systemd_exec_arg datadir || true)\"")
               != NULL);
        ASSERT(strstr(gate, "SERVICE_RPCPORT=\"$(systemd_exec_arg rpcport || true)\"")
               != NULL);
        ASSERT(strstr(gate, "ZCL_DATADIR=\"$LIVE_DATADIR\" ZCL_RPCPORT=\"$LIVE_RPCPORT\"")
               != NULL);
        ASSERT(strstr(gate, "ZD_DATADIR=\"${ZD_DATADIR:-$HOME/.zclassic}\"")
               != NULL);
        ASSERT(strstr(gate, "ZCL_DATADIR=\"$ZD_DATADIR\" ZCL_RPCPORT=\"$ZD_RPCPORT\"")
               != NULL);
        ASSERT(strstr(gate,
                      "absence of a listed z-addr is\n"
                      "# BLOCKED to the owner/test proof")
               != NULL);
        ASSERT(strstr(gate,
                      "z_gettotalbalance answers but no sapling z-addr is listed")
               != NULL);
        ASSERT(strstr(gate, "z_gettotalbalance did not answer") != NULL);
        ASSERT(strstr(gate, "\"datadir\":\"%s\"") != NULL);
        ASSERT(strstr(gate, "\"rpcport\":%s") != NULL);
        PASS();
    } _test_next:;
    free(scoreboard);
    free(gate);
    return failures;
}

static int t_soak_assert_requires_known_mirror_lag(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("soak assert treats unknown mirror lag as a deviation") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "tools/scripts/soak_assert.sh") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "mirror_lag_known") != NULL);
        ASSERT(strstr(buf, "lag_known=$(echo \"$sync_detail\"") != NULL);
        ASSERT(strstr(buf, "fail=\"mirror_lag_unknown\"") != NULL);
        ASSERT(strstr(buf, "lag_known=$lag_known") != NULL);
        ASSERT(strstr(buf, "elif [ \"$lag_known\" != \"true\" ]; then")
               != NULL);
        ASSERT(strstr(buf, "elif [ \"$lag\" -gt \"$LAG_BREACH_BLOCKS\" ]; then")
               != NULL);
        ASSERT(strstr(buf, "mirror_lag is known and <=") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_boot_chain_advance_diagnostics_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("boot wiring initializes chain advance before diagnostics") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *init = strstr(buf, "block_source_policy_init(");
        char *diag_state = strstr(buf, "diagnostics_controller_set_state(");
        char *diag_register = strstr(buf, "register_diagnostics_rpc_commands(");
        ASSERT(init != NULL);
        ASSERT(diag_state != NULL);
        ASSERT(diag_register != NULL);
        ASSERT(init < diag_state);
        ASSERT(init < diag_register);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_boot_core_liveness_precedes_frontend_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("boot starts reducer liveness before optional frontend services") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *helper = strstr(buf, "static void boot_register_core_liveness_and_reducer(");
        char *helper_end = helper
            ? strstr(helper, "bool app_init_services(struct app_context *ctx,")
            : NULL;
        char *helper_stage = helper
            ? strstr(helper, "staged_sync_supervisor_register(svc->state);")
            : NULL;
        char *call = strstr(buf, "boot_register_core_liveness_and_reducer(svc, params);");
        char *frontend = strstr(buf, "boot_register_frontend_services(svc)");
        char *runtime = strstr(buf, "boot_register_runtime_services(svc)");
        ASSERT(helper != NULL);
        ASSERT(helper_end != NULL);
        ASSERT(helper_stage != NULL);
        ASSERT(call != NULL);
        ASSERT(frontend != NULL);
        ASSERT(runtime != NULL);
        ASSERT(helper_stage < helper_end);
        ASSERT(helper_stage < frontend);
        ASSERT(helper_stage < runtime);
        ASSERT(call < frontend);
        ASSERT(call < runtime);
        ASSERT(count_occurrences(buf, "staged_sync_supervisor_register(svc->state);") == 1);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_boot_addrman_persistence_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("boot uses one sidecar-protected addrman persistence path") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "connman_load_addrman(") != NULL);
        ASSERT(strstr(buf, "addr_db_read(") == NULL);
        ASSERT(strstr(buf, "addr_db_write(") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_lib_runtime_gauges_are_callback_injected(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("lib runtime gauges and peer preference are callback injected") {
        char path[PATH_MAX];
        /* The external-gauge injection moved with app_start_metrics into
         * config/src/boot_node_utilities.c (boot composition-root unit). */
        ASSERT(repo_path(path, sizeof(path),
                         "config/src/boot_node_utilities.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "boot_metrics_external_gauges") != NULL);
        ASSERT(strstr(buf, "svc->metrics->external_gauges =") != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), "config/src/boot_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "connman_set_known_zcl23_peer_source") != NULL);
        ASSERT(strstr(buf, "db_peer_fast_zcl23") != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), "lib/metrics/src/metrics.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "ctx->external_gauges") != NULL);
        ASSERT(strstr(buf, "models/database.h") == NULL);
        ASSERT(strstr(buf, "services/sync_monitor.h") == NULL);
        ASSERT(strstr(buf, "services/legacy_mirror_sync_service.h") == NULL);
        ASSERT(strstr(buf, "services/node_health_service.h") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), "lib/net/src/connman.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "known_zcl23_peers") != NULL);
        ASSERT(strstr(buf, "models/peer.h") == NULL);
        ASSERT(strstr(buf, "config/runtime.h") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_boot_shutdown_persistence_order_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("shutdown writes clean marker before the block-index flat save") {
        /* Durability-first ordering: node.db is WAL-checkpointed + closed, then
         * the verified-clean marker is written, and ONLY THEN the best-effort
         * block-index flat save runs. A kill during the slow flat save must not
         * be able to strand the marker, so the marker write must precede the
         * flat-save call. (This replaces the older "fast < connman_join"
         * contract, which required the flat save before the checkpoint.) */
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *network_stop = strstr(buf, "zcl_service_kernel_stop_all(&svc->network_kernel);");
        char *wal_checkpoint = strstr(buf, "node_db_wal_checkpoint(svc->node_db)");
        char *marker = strstr(buf, "boot_shutdown_marker_write_clean(svc->datadir);");
        char *fast = strstr(buf, "shutdown_persist_fast_restart_state(svc);");
        ASSERT(network_stop != NULL);
        ASSERT(wal_checkpoint != NULL);
        ASSERT(marker != NULL);
        ASSERT(fast != NULL);
        /* checkpoint precedes the marker (marker binds a checkpointed DB) */
        ASSERT(wal_checkpoint < marker);
        /* marker precedes the slow flat save (durability before optimization) */
        ASSERT(marker < fast);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_hodl_history_uses_runtime_db_service(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("HODL history worker uses runtime DB service") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path),
                         "config/src/boot_background_workers.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "hodl_history_fill_pending_write") != NULL);
        ASSERT(strstr(buf, "db_service_run_write(\n"
                           "                        dbsvc, "
                           "hodl_history_fill_pending_write") != NULL);
        ASSERT(strstr(buf, "node_db_open(&hdb") == NULL);
        ASSERT(strstr(buf, "private node.db open") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_db_service_query_handle_is_canonical(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("DB service query handle aliases canonical node DB") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/db_service.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "svc->query_db = svc->node_db->db") != NULL);
        ASSERT(strstr(buf, "sqlite3_open_v2(db_path, &svc->query_db") == NULL);
        ASSERT(strstr(buf, "SQLITE_OPEN_READONLY") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_txindex_releases_node_db_between_batches(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("txindex releases node.db between batches") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path),
                         "app/controllers/src/snapshot_controller_txindex.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "#define TX_INDEX_BATCH_TXS 1000") != NULL);
        ASSERT(strstr(buf, "#define TX_INDEX_BATCH_YIELD_MS 100") != NULL);
        ASSERT(strstr(buf, "if (complete >= 3)") != NULL);
        ASSERT(strstr(buf, "existing > 100000") == NULL);
        ASSERT(strstr(buf, "platform_sleep_ms(TX_INDEX_BATCH_YIELD_MS)") != NULL);
        char *fail_begin = strstr(buf, "tx_index begin bulk load transaction");
        char *finalize = fail_begin ? strstr(fail_begin, "sqlite3_finalize(query)") : NULL;
        char *close_read = fail_begin ? strstr(fail_begin, "sqlite3_close(read_db)") : NULL;
        char *close_ndb = fail_begin ? strstr(fail_begin, "node_db_close(&ndb)") : NULL;
        ASSERT(fail_begin != NULL);
        ASSERT(finalize != NULL);
        ASSERT(close_read != NULL);
        ASSERT(close_ndb != NULL);
        ASSERT(finalize < close_read);
        ASSERT(close_read < close_ndb);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_peer_save_busy_reports_db_error(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("peer save lock exhaustion is reported as DB error") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "app/models/src/peer.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "event_emitf(EV_DB_ERROR") != NULL);
        ASSERT(strstr(buf, "sqlite3_errstr(rc)") != NULL);
        ASSERT(strstr(buf, "model=peer op=%s rc=%d attempts=%d msg=%s")
               != NULL);
        ASSERT(strstr(buf, "peer %s skipped") != NULL);
        ASSERT(strstr(buf, "event_emitf(EV_MODEL_VALIDATION_FAILED, 0,\n"
                           "                    \"model=peer op=save") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_handshake_peer_save_is_async(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("handshake peer persistence is advisory async write") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "msg_processor_set_peer_save") != NULL);
        ASSERT(strstr(buf, "EV_DB_ERROR") == NULL);
        free(buf);
        buf = NULL;
        /* The advisory peer-save callback body (its async-write impl detail)
         * lives in boot_msg_callbacks.c. */
        ASSERT(repo_path(path, sizeof(path),
                         "config/src/boot_msg_callbacks.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "db_service_enqueue_write(dbsvc") != NULL);
        ASSERT(strstr(buf, "db_peer_save_advisory") != NULL);
        ASSERT(strstr(buf, "boot.peer_save_ctx") != NULL);
        ASSERT(strstr(buf, "enqueue_queue_full") != NULL);
        ASSERT(strstr(buf, "peer_lifecycle_note_cache_skipped") != NULL);
        ASSERT(strstr(buf, "peer_lifecycle_note_cache_skipped_addr") != NULL);
        ASSERT(strstr(buf, "EV_DB_ERROR") == NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "lib/net/src/msg_version.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "mp->peer_save") != NULL);
        ASSERT(strstr(buf, "models/peer.h") == NULL);
        ASSERT(strstr(buf, "models/database.h") == NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "lib/net/src/peer_lifecycle.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "EV_PEER_CACHE_SKIPPED") != NULL);
        ASSERT(strstr(buf, "\"cache_skipped\"") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_p2p_app_persistence_is_callback_injected(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("p2p app persistence is injected into net") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "boot_save_zmsg") != NULL);
        ASSERT(strstr(buf, "msg_processor_set_zmsg_save") != NULL);
        ASSERT(strstr(buf, "boot_save_file_offer") != NULL);
        ASSERT(strstr(buf, "msg_processor_set_file_offer_save") != NULL);
        ASSERT(strstr(buf, "boot_save_file_service") != NULL);
        ASSERT(strstr(buf, "msg_processor_set_file_service_save") != NULL);
        free(buf);
        buf = NULL;
        /* Callback bodies (the DB-write impl detail) live in boot_msg_callbacks.c. */
        ASSERT(repo_path(path, sizeof(path),
                         "config/src/boot_msg_callbacks.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "db_zmsg_save") != NULL);
        ASSERT(strstr(buf, "db_file_offer_save") != NULL);
        ASSERT(strstr(buf, "db_file_service_save") != NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "lib/net/src/msgprocessor.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "mp->zmsg_save") != NULL);
        ASSERT(strstr(buf, "mp->file_offer_save") != NULL);
        ASSERT(strstr(buf, "mp->file_service_save") != NULL);
        ASSERT(strstr(buf, "db_zmsg_save") == NULL);
        ASSERT(strstr(buf, "db_file_offer_save") == NULL);
        ASSERT(strstr(buf, "db_file_service_save") == NULL);
        ASSERT(strstr(buf, "models/database.h") == NULL);
        ASSERT(strstr(buf, "models/file_service.h") == NULL);
        ASSERT(strstr(buf, "sync/sync_planner.h") != NULL);
        ASSERT(strstr(buf, "services/" "block_sync_" "service.h") == NULL);
        ASSERT(strstr(buf, "services/" "header_sync_" "service.h") == NULL);
        ASSERT(strstr(buf, "net/snapshot_sync_contract.h") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_tx_wallet_sync_is_callback_injected(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("tx wallet persistence and snapshot state are injected into net") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "boot_wallet_tx_accepted") != NULL);
        ASSERT(strstr(buf, "msg_processor_set_snapshot_active") != NULL);
        ASSERT(strstr(buf, "msg_processor_set_snapshot_anchor_accessors") != NULL);
        ASSERT(strstr(buf, "msg_processor_set_wallet_tx_accepted") != NULL);
        free(buf);
        buf = NULL;
        /* Callback bodies (the wallet-sync impl detail) live in boot_msg_callbacks.c. */
        ASSERT(repo_path(path, sizeof(path),
                         "config/src/boot_msg_callbacks.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "wallet_sync_transaction") != NULL);
        ASSERT(strstr(buf, "node_db_sync_wallet_tx") != NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "lib/net/src/msg_tx.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "mp->wallet_tx_accepted") != NULL);
        ASSERT(strstr(buf, "msg_processor_snapshot_active") != NULL);
        ASSERT(strstr(buf, "wallet_sync_transaction") == NULL);
        ASSERT(strstr(buf, "node_db_sync_wallet_tx") == NULL);
        ASSERT(strstr(buf, "controllers/sync_controller.h") == NULL);
        ASSERT(strstr(buf, "models/database.h") == NULL);
        ASSERT(strstr(buf, "services/" "header_sync_" "service.h") == NULL);
        ASSERT(strstr(buf, "net/snapshot_sync_contract.h") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_p2p_block_submit_is_callback_injected(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("p2p block submission is injected into net") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "boot_submit_p2p_block") != NULL);
        ASSERT(strstr(buf, "msg_processor_set_block_submit") != NULL);
        ASSERT(strstr(buf, "boot_block_connected_observer") != NULL);
        ASSERT(strstr(buf, "msg_processor_set_block_connected") != NULL);
        free(buf);
        buf = NULL;
        /* Callback bodies (block-submit + observer impl detail) live in
         * boot_msg_callbacks.c. */
        ASSERT(repo_path(path, sizeof(path),
                         "config/src/boot_msg_callbacks.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "REDUCER_SRC_P2P") != NULL);
        ASSERT(strstr(buf, "sync_monitor_on_block_connected") != NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "lib/net/src/msg_blocks.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "mp->block_submit") != NULL);
        ASSERT(strstr(buf, "msg_processor_snapshot_active") != NULL);
        ASSERT(strstr(buf, "msg_processor_note_block_connected") != NULL);
        ASSERT(strstr(buf, "msg_processor_request_invalid_block_headers") != NULL);
        ASSERT(strstr(buf, "msg_processor_plan_valid_block_acceptance") != NULL);
        ASSERT(strstr(buf, "reducer_ingest_block") == NULL);
        ASSERT(strstr(buf, "boot_activation_controller") == NULL);
        ASSERT(strstr(buf, "controllers/sync_controller.h") == NULL);
        ASSERT(strstr(buf, "models/database.h") == NULL);
        ASSERT(strstr(buf, "services/" "block_sync_" "service.h") == NULL);
        ASSERT(strstr(buf, "services/chain_activation_service.h") == NULL);
        ASSERT(strstr(buf, "services/" "header_sync_" "service.h") == NULL);
        ASSERT(strstr(buf, "net/snapshot_sync_contract.h") == NULL);
        ASSERT(strstr(buf, "services/sync_monitor.h") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_flyclient_proof_builder_is_callback_injected(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("FlyClient proof builder is injected into net snapshot handler") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "boot_build_flyclient_proof") != NULL);
        ASSERT(strstr(buf, "msg_processor_set_flyclient_proof_builder") != NULL);
        ASSERT(strstr(buf, "boot_load_block_hashes_range") != NULL);
        ASSERT(strstr(buf, "msg_processor_set_block_hashes_range") != NULL);
        ASSERT(strstr(buf, "boot_compute_utxo_sha3") != NULL);
        ASSERT(strstr(buf, "msg_processor_set_utxo_sha3_compute") != NULL);
        ASSERT(strstr(buf, "snapsync_build_fc_response") == NULL);
        ASSERT(strstr(buf, "db_block_hashes_in_range") == NULL);
        ASSERT(strstr(buf, "utxo_commitment_sha3_compute") == NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_flyclient.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "boot_build_flyclient_proof") != NULL);
        ASSERT(strstr(buf, "snapsync_build_fc_response") != NULL);
        ASSERT(strstr(buf, "boot_load_block_hashes_range") != NULL);
        ASSERT(strstr(buf, "db_block_hashes_in_range") != NULL);
        ASSERT(strstr(buf, "boot_compute_utxo_sha3") != NULL);
        ASSERT(strstr(buf, "utxo_commitment_sha3_compute") != NULL);
        ASSERT(strstr(buf, "boot_prepare_mmb_leaf_store") != NULL);
        ASSERT(strstr(buf, "mmb_leaf_store_rebuild") != NULL);
        ASSERT(strstr(buf, "legacy_chain_rpc_") == NULL);
        ASSERT(strstr(buf, "legacy_chain_oracle") == NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path),
                         "lib/net/src/msgprocessor_snapshot.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "mp->flyclient_proof") != NULL);
        ASSERT(strstr(buf, "net/snapshot_sync_contract.h") != NULL);
        ASSERT(strstr(buf, "msg_snapshot_sync(") != NULL);
        ASSERT(strstr(buf, "msg_snapshot_sync_ensure") != NULL);
        ASSERT(strstr(buf, "mp->block_hashes_range") != NULL);
        ASSERT(strstr(buf, "mp->utxo_sha3_compute") != NULL);
        ASSERT(strstr(buf, "db_block_hashes_in_range") == NULL);
        ASSERT(strstr(buf, "utxo_commitment_sha3_compute") == NULL);
        ASSERT(strstr(buf, "rpc_blockchain_get_mmb") == NULL);
        ASSERT(strstr(buf, "g_mmb_leaf_store") == NULL);
        ASSERT(strstr(buf, "controllers/blockchain_controller.h") == NULL);
        ASSERT(strstr(buf, "models/mmb_leaf_store.h") == NULL);
        ASSERT(strstr(buf, "models/block.h") == NULL);
        ASSERT(strstr(buf, "coins/utxo_commitment.h") == NULL);
        ASSERT(strstr(buf, "controllers/sync_controller.h") == NULL);
        ASSERT(strstr(buf, "models/database.h") == NULL);
        ASSERT(strstr(buf, "services/chain_state_service.h") == NULL);
        ASSERT(strstr(buf, "services/" "snapshot_sync_" "service.h") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_fast_sync_uses_lib_sqlite_helpers(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("fast sync avoids direct AR, DB, and UTXO model includes") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "lib/net/src/fast_sync.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "util/ar_step_readonly.h") != NULL);
        ASSERT(strstr(buf, "AR_STEP_WRITE") != NULL);
        ASSERT(strstr(buf, "models/activerecord.h") == NULL);
        ASSERT(strstr(buf, "models/database.h") == NULL);
        ASSERT(strstr(buf, "models/utxo.h") == NULL);
        ASSERT(strstr(buf, "db_utxo_serialize_snapshot") == NULL);
        ASSERT(strstr(buf, "AR_BIND_") == NULL);
        ASSERT(strstr(buf, "AR_STEP_DONE") == NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "lib/net/include/net/fast_sync.h") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "fast_sync_snapshot_serialize_fn") != NULL);
        ASSERT(strstr(buf, "struct node_db") == NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_flyclient.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "boot_serialize_utxo_snapshot") != NULL);
        ASSERT(strstr(buf, "db_utxo_serialize_snapshot") != NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_snapshot_offer.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "ZCL_PUBLISH_FASTSYNC_ON_BOOT") != NULL);
        ASSERT(strstr(buf, "Fast sync snapshot publish skipped on boot") != NULL);
        ASSERT(strstr(buf, "boot_serialize_utxo_snapshot") != NULL);
        ASSERT(strstr(buf, "fast_sync_prebuild_snapshot") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_framework_reexport_headers_stay_deleted(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("framework primitive re-export headers stay deleted") {
        char path[PATH_MAX];
        struct stat st;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/framework/include/framework/mailbox.h") == 0);
        errno = 0;
        ASSERT(stat(path, &st) != 0 && errno == ENOENT);

        ASSERT(repo_path(path, sizeof(path),
                         "lib/framework/include/framework/projection.h") == 0);
        errno = 0;
        ASSERT(stat(path, &st) != 0 && errno == ENOENT);

        ASSERT(repo_path(path, sizeof(path),
                         "app/services/include/services/header_admit_inbox.h") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "util/mailbox.h") != NULL);
        ASSERT(strstr(buf, "framework/mailbox.h") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "app/controllers/src/chain_projection.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "util/projection.h") != NULL);
        ASSERT(strstr(buf, "framework/projection.h") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), "lib/framework/README.md") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "only purpose is to re-export") != NULL);
        ASSERT(strstr(buf, "include/framework/mailbox.h") == NULL);
        ASSERT(strstr(buf, "include/framework/projection.h") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_utxo_reimport_flag_is_storage_owned(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("utxo reimport flag is storage owned") {
        char path[PATH_MAX];

        ASSERT(repo_path(path, sizeof(path),
                         "app/services/include/services/utxo_recovery_service.h")
               == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "storage/utxo_reimport_flag.h") == NULL);
        ASSERT(strstr(buf, "re-export") == NULL);
        ASSERT(strstr(buf, "re-exports") == NULL);
        ASSERT(strstr(buf, "utxo_reimport_flag_check_and_clear") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), "config/src/boot.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "storage/utxo_reimport_flag.h") != NULL);
        ASSERT(strstr(buf, "utxo_reimport_flag_check_and_clear") != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/validation/src/"
                         "process_block_self_heal_hot_loop.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "storage/utxo_reimport_flag.h") != NULL);
        ASSERT(strstr(buf, "utxo_reimport_flag_set") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_net_sync_planners_are_lib_owned(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("net sync planners use lib-owned contract") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "lib/net/src/msg_headers.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "sync/sync_planner.h") != NULL);
        ASSERT(strstr(buf, "msg_processor_snapshot_active") != NULL);
        ASSERT(strstr(buf, "msg_processor_snapshot_anchor") != NULL);
        ASSERT(strstr(buf, "msg_processor_set_snapshot_anchor") != NULL);
        ASSERT(strstr(buf, "msg_processor_request_activation") != NULL);
        ASSERT(strstr(buf, "msg_processor_clear_activation_anchor") != NULL);
        ASSERT(strstr(buf, "msg_processor_repair_post_activation_anchor") != NULL);
        ASSERT(strstr(buf, "msg_processor_scan_block_files") != NULL);
        ASSERT(strstr(buf, "msg_processor_block_index_heights_repaired") != NULL);
        ASSERT(strstr(buf, "msg_processor_commit_header_tip") != NULL);
        ASSERT(strstr(buf, "msg_processor_recommit_snapshot_anchor") != NULL);
        ASSERT(strstr(buf, "services/" "block_sync_" "service.h") == NULL);
        ASSERT(strstr(buf, "services/block_index_integrity.h") == NULL);
        ASSERT(strstr(buf, "services/chain_activation_service.h") == NULL);
        ASSERT(strstr(buf, "services/chain_state_service.h") == NULL);
        ASSERT(strstr(buf, "services/chain_tip.h") == NULL);
        ASSERT(strstr(buf, "services/" "header_sync_" "service.h") == NULL);
        ASSERT(strstr(buf, "net/snapshot_sync_contract.h") == NULL);
        ASSERT(strstr(buf, "config/boot_internal.h") == NULL);
        ASSERT(strstr(buf, "boot_activation_controller") == NULL);
        ASSERT(strstr(buf, "activation_request_connect") == NULL);
        ASSERT(strstr(buf, "activation_clear_anchor") == NULL);
        ASSERT(strstr(buf, "bii_repair_post_activation_anchor") == NULL);
        ASSERT(strstr(buf, "csr_commit_tip") == NULL);
        ASSERT(strstr(buf, "csr_commit_header_tip") == NULL);
        ASSERT(strstr(buf, "csr_instance") == NULL);
        ASSERT(strstr(buf, "chain_state_commit") == NULL);
        ASSERT(strstr(buf, "scan_block_files_mark_data") == NULL);
        ASSERT(strstr(buf, "block_index_heights_repaired()") == NULL);
        ASSERT(strstr(buf, "snapsync_is_active") == NULL);
        ASSERT(strstr(buf, "snapsync_get_anchor") == NULL);
        ASSERT(strstr(buf, "snapsync_set_anchor") == NULL);
        ASSERT(strstr(buf, "TIP_FROM_P2P_REPAIR") == NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "boot_request_header_activation") != NULL);
        ASSERT(strstr(buf, "boot_clear_header_activation_anchor") != NULL);
        ASSERT(strstr(buf, "boot_repair_header_post_activation_anchor") != NULL);
        ASSERT(strstr(buf, "msg_processor_set_activation_hooks") != NULL);
        ASSERT(strstr(buf, "boot_scan_header_block_files") != NULL);
        ASSERT(strstr(buf, "scan_block_files_mark_data") != NULL);
        ASSERT(strstr(buf, "boot_header_block_index_heights_repaired") != NULL);
        ASSERT(strstr(buf, "block_index_heights_repaired") != NULL);
        ASSERT(strstr(buf, "msg_processor_set_header_index_hooks") != NULL);
        ASSERT(strstr(buf, "boot_commit_header_tip") != NULL);
        ASSERT(strstr(buf, "boot_recommit_snapshot_anchor") != NULL);
        ASSERT(strstr(buf, "msg_processor_set_header_chainstate_hooks") != NULL);
        free(buf);
        buf = NULL;
        /* The background_utxo_replay worker drives the post-snapshot activation
         * connect + chainstate commit. It lives in its own boot unit
         * (boot_utxo_replay.c, split out of boot_background_workers.c for the
         * file-size ratchet), so its activation_request_connect /
         * csr_commit_tip call sites live there — still boot-owned
         * (config/src), never in lib/net. */
        ASSERT(repo_path(path, sizeof(path),
                         "config/src/boot_utxo_replay.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "activation_request_connect") != NULL);
        ASSERT(strstr(buf, "csr_commit_tip") != NULL);
        free(buf);
        buf = NULL;
        /* Callback bodies (the activation/index/chainstate impl detail) live in
         * boot_msg_callbacks.c. */
        ASSERT(repo_path(path, sizeof(path),
                         "config/src/boot_msg_callbacks.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "activation_clear_anchor") != NULL);
        ASSERT(strstr(buf, "bii_repair_post_activation_anchor") != NULL);
        /* Header-tip mutation routes through the chain-state repository's
         * validated promote (operator-snapshot refactor). */
        ASSERT(strstr(buf, "csr_promote_header_tip") != NULL);
        ASSERT(strstr(buf, "chain_set_active_tip") != NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path),
                         "lib/sync/include/sync/sync_planner.h") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "struct sync_header_processing_plan") != NULL);
        ASSERT(strstr(buf, "struct sync_stall_recovery") != NULL);
        ASSERT(strstr(buf, "syncsvc_plan_periodic_getheaders") != NULL);
        ASSERT(strstr(buf, "syncsvc_assign_peer_blocks") != NULL);
        ASSERT(strstr(buf, "services/") == NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "lib/event/include/event/event.h") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "EV_SYNC_STATE_CHANGE") != NULL);
        ASSERT(strstr(buf, "#include \"sync/sync_state.h\"") == NULL);
        ASSERT(strstr(buf, "enum sync_state") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_header_peer_votes_are_callback_injected(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("header peer votes are injected into net") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "boot_record_peer_header_vote") != NULL);
        ASSERT(strstr(buf, "msg_processor_set_peer_header_vote") != NULL);
        free(buf);
        buf = NULL;
        /* Callback body (the quorum-oracle impl detail) lives in
         * boot_msg_callbacks.c. */
        ASSERT(repo_path(path, sizeof(path),
                         "config/src/boot_msg_callbacks.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "quorum_oracle_record_peer_header_vote") != NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path), "lib/net/src/msg_headers.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "msg_processor_record_peer_header_vote") != NULL);
        ASSERT(strstr(buf, "quorum_oracle_record_peer_header_vote") == NULL);
        ASSERT(strstr(buf, "services/quorum_oracle_service.h") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_process_block_node_db_access_is_runtime_owned(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("process block node_db access is runtime owned") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path),
                         "lib/validation/src/process_block.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "app_runtime_node_db_handle_open") != NULL);
        ASSERT(strstr(buf, "models/database.h") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/validation/src/process_block_flush_policy.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "app_runtime_node_db_handle_open") != NULL);
        ASSERT(strstr(buf, "app_runtime_node_db_state_set") != NULL);
        /* sync_flush_if_needed + wal_checkpoint positive-assertions removed:
         * their only use site here (flush_coins_if_needed, the dead
         * forward-writer) was deleted in the dead-code removal — process_block
         * no longer flushes coins to the node.db mirror (the staged pipeline
         * owns coin writes; the mirror is rebuilt one-way by
         * utxo_mirror_sync_service). The runtime-owned invariant stays enforced
         * by the accessors present (handle_open, state_set) + the negative
         * models/database.h guard. */
        ASSERT(strstr(buf, "models/database.h") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/validation/src/process_block_self_heal.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "app_runtime_node_db_utxo_max_height") != NULL);
        ASSERT(strstr(buf, "app_runtime_node_db_tx_index_find") == NULL);
        ASSERT(strstr(buf, "db_tx_find_native_or_reversed") == NULL);
        ASSERT(strstr(buf, "sqlite3_prepare_v2") == NULL);
        ASSERT(strstr(buf, "models/database.h") == NULL);
        ASSERT(strstr(buf, "models/tx_index.h") == NULL);
        ASSERT(strstr(buf, "rpc/legacy_rpc_client.h") == NULL);
        ASSERT(strstr(buf, "process_block_json_string") == NULL);
        ASSERT(strstr(buf, "process_block_legacy_rpc_body") == NULL);
        ASSERT(strstr(buf,
                      "process_block_recover_missing_utxo_from_sqlite_tx_index(")
               == NULL);
        ASSERT(strstr(buf, "read_block_from_disk_index") == NULL);
        ASSERT(strstr(buf, "process_block_recover_missing_utxo_from_chain_scan(")
               == NULL);
        ASSERT(strstr(buf, "block_tree_db_write_tx_index") == NULL);
        ASSERT(strstr(buf, "process_block_self_heal_scan_depth_limit") == NULL);
        ASSERT(strstr(buf, "process_block_self_heal_stats_snapshot") == NULL);
        ASSERT(strstr(buf, "g_self_heal_scan_hits") == NULL);
        ASSERT(strstr(buf, "utxo_reimport_flag_set") == NULL);
        ASSERT(strstr(buf, "FATAL_HOT_LOOP") == NULL);
        ASSERT(strstr(buf, "last_reimport_attempted") == NULL);
        ASSERT(strstr(buf, "process_block_inject_missing_utxo(") == NULL);
        ASSERT(strstr(buf, "coins_from_transaction") == NULL);
        ASSERT(strstr(buf, "coins_view_cache_modify_new") == NULL);
        ASSERT(strstr(buf, "COINS_CACHE_DIRTY") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/validation/src/"
                         "process_block_self_heal_hot_loop.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "process_block_maybe_write_needs_reimport_flag")
               != NULL);
        ASSERT(strstr(buf, "process_block_maybe_trigger_hot_loop_exit")
               != NULL);
        ASSERT(strstr(buf, "process_block_get_utxo_activation_paused_height")
               != NULL);
        ASSERT(strstr(buf, "process_block_clear_utxo_activation_pause_range")
               != NULL);
        ASSERT(strstr(buf, "utxo_reimport_flag_set") != NULL);
        ASSERT(strstr(buf, "FATAL_HOT_LOOP") != NULL);
        ASSERT(strstr(buf, "last_reimport_attempted") != NULL);
        ASSERT(strstr(buf, "app_runtime_node_db_tx_index_find") == NULL);
        ASSERT(strstr(buf, "read_block_from_disk_index") == NULL);
        ASSERT(strstr(buf, "block_tree_db_write_tx_index") == NULL);
        ASSERT(strstr(buf, "rpc/legacy_rpc_client.h") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/validation/src/"
                         "process_block_self_heal_scan_state.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "process_block_self_heal_scan_depth_limit") != NULL);
        ASSERT(strstr(buf, "process_block_self_heal_scan_enabled") != NULL);
        ASSERT(strstr(buf, "process_block_self_heal_stats_snapshot") != NULL);
        ASSERT(strstr(buf, "g_self_heal_scan_hits") != NULL);
        ASSERT(strstr(buf, "ZCL_SELF_HEAL_SCAN_DEPTH") != NULL);
        ASSERT(strstr(buf, "ZCL_SELF_HEAL_SCAN_ENABLE") != NULL);
        ASSERT(strstr(buf, "app_runtime_node_db_tx_index_find") == NULL);
        ASSERT(strstr(buf, "utxo_reimport_flag_set") == NULL);
        ASSERT(strstr(buf, "read_block_from_disk_index") == NULL);
        ASSERT(strstr(buf, "block_tree_db_write_tx_index") == NULL);
        ASSERT(strstr(buf, "rpc/legacy_rpc_client.h") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/validation/src/process_block_core.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "controllers/blockchain_controller.h") == NULL);
        ASSERT(strstr(buf, "controllers/sync_controller.h") == NULL);
        ASSERT(strstr(buf, "models/database.h") == NULL);
        ASSERT(strstr(buf, "models/tx_index.h") == NULL);
        ASSERT(strstr(buf, "services/chain_activation_service.h") == NULL);
        ASSERT(strstr(buf, "services/chain_evidence_authority_service.h") == NULL);
        ASSERT(strstr(buf, "services/chain_state_service.h") == NULL);
        ASSERT(strstr(buf, "services/chain_tip.h") == NULL);
        ASSERT(strstr(buf, "services/gap_fill_service.h") == NULL);
        ASSERT(strstr(buf, "net/snapshot_sync_contract.h") == NULL);
        ASSERT(strstr(buf, "process_block_set_gap_fill_kick") == NULL);
        ASSERT(strstr(buf, "process_block_set_tip_publication_hooks") == NULL);
        ASSERT(strstr(buf, "process_block_propagate_failed_child(") == NULL);
        ASSERT(strstr(buf, "block_index_hydrate_from_disk(") == NULL);
        ASSERT(strstr(buf, "find_block_pos(") == NULL);
        ASSERT(strstr(buf, "block_index_refresh_header(") == NULL);
        ASSERT(strstr(buf, "process_block_commit_tip(") == NULL);
        ASSERT(strstr(buf, "process_block_publish_tip(") == NULL);
        ASSERT(strstr(buf, "process_block_clear_tip(") == NULL);
        ASSERT(strstr(buf, "process_block_tip_is_best_work(") == NULL);
        ASSERT(strstr(buf, "process_block_verify_active_tip_child_on_disk(")
               == NULL);
        ASSERT(strstr(buf, "find_best_active_tip_child(") == NULL);
        ASSERT(strstr(buf, "find_verified_unlinked_active_tip_child(") == NULL);
        ASSERT(strstr(buf, "process_block_should_skip_contextual_header(")
               == NULL);
        ASSERT(strstr(buf, "process_block_pow_window_complete(") == NULL);
        ASSERT(strstr(buf, "consensus/params.h") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/validation/src/process_block_contextual_header.c")
               == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "process_block_should_skip_contextual_header")
               != NULL);
        ASSERT(strstr(buf, "process_block_pow_window_complete") != NULL);
        ASSERT(strstr(buf, "find_most_work_chain") == NULL);
        ASSERT(strstr(buf, "process_block_kick_gap_fill") == NULL);
        ASSERT(strstr(buf, "services/gap_fill_service.h") == NULL);
        ASSERT(strstr(buf, "models/database.h") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/validation/src/process_block_tip_child.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "process_block_verify_active_tip_child_on_disk")
               != NULL);
        ASSERT(strstr(buf, "find_best_active_tip_child") != NULL);
        ASSERT(strstr(buf, "find_verified_unlinked_active_tip_child") != NULL);
        ASSERT(strstr(buf, "controllers/blockchain_controller.h") == NULL);
        ASSERT(strstr(buf, "controllers/sync_controller.h") == NULL);
        ASSERT(strstr(buf, "models/database.h") == NULL);
        ASSERT(strstr(buf, "services/chain_state_service.h") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/validation/src/process_block_tip_publish.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "process_block_commit_tip") != NULL);
        ASSERT(strstr(buf, "update_tip") != NULL);
        ASSERT(strstr(buf, "process_block_tip_is_best_work") != NULL);
        ASSERT(strstr(buf, "process_block_publish_tip") != NULL);
        ASSERT(strstr(buf, "controllers/blockchain_controller.h") == NULL);
        ASSERT(strstr(buf, "services/chain_state_service.h") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/validation/src/process_block_index.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "find_block_pos") != NULL);
        ASSERT(strstr(buf, "block_index_refresh_header") != NULL);
        ASSERT(strstr(buf, "block_index_hydrate_from_disk") != NULL);
        ASSERT(strstr(buf, "block_index_snapshot_for_persist") != NULL);
        ASSERT(strstr(buf, "disk_block_index_init") != NULL);
        ASSERT(strstr(buf, "controllers/blockchain_controller.h") == NULL);
        ASSERT(strstr(buf, "models/database.h") == NULL);
        ASSERT(strstr(buf, "services/chain_state_service.h") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/validation/src/process_block_invalidate.c")
               == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "block_index_snapshot_for_persist") != NULL);
        ASSERT(strstr(buf, "disk_block_index_init") == NULL);
        ASSERT(strstr(buf, "nCachedBranchId") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/validation/src/process_block_revalidate.c")
               == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "block_index_snapshot_for_persist") != NULL);
        ASSERT(strstr(buf, "disk_block_index_init") == NULL);
        ASSERT(strstr(buf, "nCachedBranchId") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/validation/src/process_block_runtime_hooks.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "process_block_set_gap_fill_kick") != NULL);
        ASSERT(strstr(buf, "process_block_kick_gap_fill") != NULL);
        ASSERT(strstr(buf, "process_block_set_tip_publication_hooks") != NULL);
        ASSERT(strstr(buf, "process_block_publish_tip") != NULL);
        ASSERT(strstr(buf, "process_block_clear_tip") != NULL);
        ASSERT(strstr(buf, "controllers/blockchain_controller.h") == NULL);
        ASSERT(strstr(buf, "services/chain_state_service.h") == NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/validation/src/process_block_failed_child.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "process_block_propagate_failed_child") != NULL);
        ASSERT(strstr(buf, "PROPAGATE_FAILED_CHILD_SKIP_RATE_LIMITED") != NULL);
        ASSERT(strstr(buf, "zcl_malloc") != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), "config/src/runtime.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "app_runtime_node_db_handle_open") != NULL);
        ASSERT(strstr(buf, "app_runtime_node_db_state_set") != NULL);
        ASSERT(strstr(buf, "app_runtime_node_db_sync_flush_if_needed") != NULL);
        ASSERT(strstr(buf, "app_runtime_node_db_wal_checkpoint") != NULL);
        ASSERT(strstr(buf, "app_runtime_node_db_utxo_max_height") != NULL);
        ASSERT(strstr(buf, "app_runtime_node_db_tx_index_find") != NULL);
        ASSERT(strstr(buf, "db_tx_find_native_or_reversed") != NULL);
        ASSERT(strstr(buf, "SELECT MAX(height) FROM utxos") != NULL);
        ASSERT(strstr(buf, "node_db_state_set") != NULL);
        ASSERT(strstr(buf, "node_db_sync_flush") != NULL);
        ASSERT(strstr(buf, "ndb->open") != NULL);
        free(buf);
        buf = NULL;

        /* The process-block hooks were extracted to boot_tip_hooks.c
         * (behavior-neutral, Wave D). boot_services.c wires them via the seam
         * call and retains the inline NULL teardown; the hook bodies + the
         * non-NULL registration live in boot_tip_hooks.c. node_db is still
         * reached via svc (runtime-owned) in both. */
        ASSERT(repo_path(path, sizeof(path), "config/src/boot_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "boot_register_process_block_hooks(svc)") != NULL);
        ASSERT(strstr(buf, "process_block_set_gap_fill_kick(NULL, NULL)") != NULL);
        ASSERT(strstr(buf, "process_block_set_tip_publication_hooks(NULL, NULL, NULL)") != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), "config/src/boot_tip_hooks.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "boot_gap_fill_kick") != NULL);
        ASSERT(strstr(buf, "gap_fill_kick") != NULL);
        ASSERT(strstr(buf, "boot_process_block_commit_tip") != NULL);
        ASSERT(strstr(buf, "boot_process_block_clear_tip") != NULL);
        ASSERT(strstr(buf, "process_block_set_gap_fill_kick(boot_gap_fill_kick, svc)") != NULL);
        ASSERT(strstr(buf, "process_block_set_tip_publication_hooks(boot_process_block_commit_tip") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_boot_repaired_index_persistence_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("boot persists repaired canonical block index") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *height_repair = strstr(buf, "block_index_repair_heights(&g_state)");
        char *pprev_repair = strstr(buf, "block_index_repair_pprev(&g_state");
        char *repaired_save = strstr(buf, "Block index repaired: saving canonical flat file");
        char *integrity = strstr(buf, "bii_verify(ctx->datadir");
        ASSERT(height_repair != NULL);
        ASSERT(pprev_repair != NULL);
        ASSERT(repaired_save != NULL);
        ASSERT(integrity != NULL);
        ASSERT(height_repair < repaired_save);
        ASSERT(pprev_repair < repaired_save);
        ASSERT(repaired_save < integrity);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_chain_evidence_reconstruct_uses_retry_persistence(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("chain evidence reconstruct uses retry persistence") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path),
                         "app/services/src/chain_evidence_reconstruct.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "chain_evidence_state_set_retry") != NULL);
        ASSERT(strstr(buf, "chain_evidence_state_set_int_retry") != NULL);
        ASSERT(strstr(buf, "node_db_state_set(") == NULL);
        ASSERT(strstr(buf, "node_db_state_set_int(") == NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_boot_genesis_init_preserves_restored_authority_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("boot genesis init preserves restored non-genesis authority tip") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *restore = strstr(buf, "utxo_recovery_restore_chain_tip");
        char *guard = strstr(buf, "boot_restored_authority_tip = true");
        char *skip = strstr(buf, "skipped genesis_init");
        char *genesis = strstr(buf, "\"genesis_init\"");
        char *skip_activate = strstr(buf, "skip_initial_activate");
        ASSERT(restore != NULL);
        ASSERT(guard != NULL);
        ASSERT(skip != NULL);
        ASSERT(genesis != NULL);
        ASSERT(skip_activate != NULL);
        ASSERT(restore < guard);
        ASSERT(guard < genesis);
        ASSERT(skip < genesis);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path),
                         "app/services/src/utxo_recovery_restore.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "coins_best_block is genesis but UTXOs reach")
               != NULL);
        ASSERT(strstr(buf, "utxo_recovery_find_disk_backed_utxo_tip")
               != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_refold_from_anchor_explicit_span_gate_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("explicit refold-from-anchor gates body span before reset") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "config/src/boot.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *do_from_anchor = strstr(buf, "if (do_from_anchor)");
        ASSERT(do_from_anchor != NULL);
        char *resume =
            strstr(do_from_anchor, "active_chain_height(&g_state.chain_active)");
        char *span = strstr(do_from_anchor, "boot_refold_body_span_contiguous");
        char *reset =
            strstr(do_from_anchor, "boot_refold_from_anchor_reset(&g_node_db)");
        char *mark =
            strstr(do_from_anchor, "refold_progress_mark_started_from_anchor");
        ASSERT(resume != NULL);
        ASSERT(span != NULL);
        ASSERT(reset != NULL);
        ASSERT(mark != NULL);
        ASSERT(do_from_anchor < resume);
        ASSERT(resume < span);
        ASSERT(span < reset);
        ASSERT(reset < mark);
        ASSERT(strstr(buf, "refold_from_anchor body_gap") != NULL);
        ASSERT(strstr(buf, "first_missing") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_sha3_window_tool_check_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("gen_sha3_windows supports single-window source proof checks") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "tools/gen_sha3_windows.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *flag = strstr(buf, "--check-window=");
        char *no_write = strstr(buf, "without writing output files");
        char *compare = strstr(buf, "expected=%s actual=%s");
        char *return_mismatch = strstr(buf, "return ok ? 0 : 1");
        ASSERT(flag != NULL);
        ASSERT(no_write != NULL);
        ASSERT(compare != NULL);
        ASSERT(return_mismatch != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), "Makefile") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "lib/chain/src/sha3_windows.c") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_make_ignores_ephemeral_lint_fixture_sources(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("Makefile source globs ignore ephemeral lint fixture sources") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "Makefile") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "ZCL_EPHEMERAL_SOURCE_PATTERNS") == NULL);
        ASSERT(strstr(buf, "%/_%fixture%.c") == NULL);
        ASSERT(strstr(buf, "zcl_ephemeral_sources") != NULL);
        ASSERT(strstr(buf, "$(findstring /_,$(s))") != NULL);
        ASSERT(strstr(buf, "zcl_filter_ephemeral_sources") != NULL);
        ASSERT(count_occurrences(buf,
                   "$(call zcl_filter_ephemeral_sources,") >= 8);
        ASSERT(strstr(buf,
               "APP_SRCS = $(call zcl_filter_ephemeral_sources") != NULL);
        ASSERT(strstr(buf,
               "CONFIG_SRCS = $(call zcl_filter_ephemeral_sources") != NULL);
        ASSERT(strstr(buf,
               "LIB_SRCS = $(call zcl_filter_ephemeral_sources") != NULL);
        ASSERT(strstr(buf,
               "DOMAIN_SRCS = $(call zcl_filter_ephemeral_sources") != NULL);
        ASSERT(strstr(buf,
               "APPLICATION_SRCS = $(call zcl_filter_ephemeral_sources")
               != NULL);
        ASSERT(strstr(buf,
               "ADAPTERS_SRCS = $(call zcl_filter_ephemeral_sources")
               != NULL);
        ASSERT(strstr(buf,
               "MCP_SRCS = $(call zcl_filter_ephemeral_sources") != NULL);
        ASSERT(strstr(buf,
               "TEST_SRCS = $(call zcl_filter_ephemeral_sources") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_block_index_flat_atomic_save_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    /* Task #32 strengthened the contract: the block index now persists
     * as a SINGLE file with the 48-byte integrity header embedded inside
     * block_index.bin, published with ONE atomic rename. The old pin
     * encoded the TWO-file shape (body rename, then a separate
     * bii_write_sidecar_raw sidecar rename) — that shape WAS the bug
     * (a crash between the two renames stranded a fresh body under a
     * stale sidecar; live 2026-06-12). The pin below enforces that:
     *   - the writer streams a SHA3 over the payload in bif_emit_payload,
     *   - it publishes via bii_write_embedded (the shared
     *     placeholder-header → payload → back-patch → one-rename helper),
     *   - it does NOT fall back to the legacy two-file sidecar writer
     *     (bii_write_sidecar_raw is absent from the writer path).
     * The single-rename + fsync + tmp-unlink atomicity itself lives in
     * ssio_write_embedded, asserted separately below. */
    TEST("block index flat save is a single atomic embedded-header file") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path),
                         "app/services/src/block_index_loader.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        /* Payload is streamed through SHA3 as it is written. */
        char *stream_hash = strstr(buf, "sha3_256_init(&hctx)");
        char *stream_fin  = strstr(buf, "sha3_256_finalize(&hctx, out_payload_sha3)");
        /* Publish via the embedded single-file helper. */
        char *embedded = strstr(buf, "bii_write_embedded(datadir, bif_emit_payload");
        /* The legacy two-file sidecar writer must NOT be on the save path. */
        char *legacy_sidecar = strstr(buf, "bii_write_sidecar_raw(datadir");
        ASSERT(stream_hash != NULL);
        ASSERT(stream_fin != NULL);
        ASSERT(embedded != NULL);
        ASSERT(legacy_sidecar == NULL);
        ASSERT(stream_hash < embedded);
        free(buf);
        buf = NULL;

        /* The atomic publish (tmp, fsync, single rename) lives in the
         * shared embedded writer. */
        ASSERT(repo_path(path, sizeof(path),
                         "lib/storage/src/sha3_sidecar_io.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        char *fn        = strstr(buf, "ssio_write_embedded(");
        ASSERT(fn != NULL);
        char *open_tmp  = strstr(fn, "fopen(tmp_path, \"wb\")");
        char *fsync_fd  = strstr(fn, "(void)fsync(fd)");
        char *rename_tmp = strstr(fn, "rename(tmp_path, body_path)");
        char *unlink_err = strstr(fn, "unlink(tmp_path)");
        ASSERT(open_tmp != NULL);
        ASSERT(fsync_fd != NULL);
        ASSERT(rename_tmp != NULL);
        ASSERT(unlink_err != NULL);
        ASSERT(open_tmp < fsync_fd);
        ASSERT(fsync_fd < rename_tmp);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_process_block_split_uses_reducer_language(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("process-block split comments name reducer, not deleted engines") {
        const char *files[] = {
            "lib/validation/include/validation/process_block.h",
            "lib/validation/include/validation/process_block_internals.h",
            "lib/validation/include/validation/process_block_invalidate.h",
            "lib/validation/src/process_block.c",
            "lib/validation/src/process_block_core.c",
            "lib/validation/src/process_block_crash_hooks.c",
            "lib/validation/src/process_block_failed_child.c",
            "lib/validation/src/process_block_internal.h",
            "lib/validation/src/process_block_revalidate.c",
            "lib/validation/src/process_block_tip_child.c",
            "lib/validation/src/process_block_tip_publish.c",
        };
        const char *stale[] = {
            "connect_tip",
            "disconnect_tip",
            "activate_best_chain",
            "process_new_block",
            "accept_block()",
        };

        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
            char path[PATH_MAX];
            ASSERT(repo_path(path, sizeof(path), files[i]) == 0);
            ASSERT(read_entire_file(path, &buf) == 0);
            for (size_t j = 0; j < sizeof(stale) / sizeof(stale[0]); j++)
                ASSERT(strstr(buf, stale[j]) == NULL);
            free(buf);
            buf = NULL;
        }
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_production_comments_do_not_carry_refactor_scaffold_labels(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("production comments name purpose, not refactor scaffold labels") {
        const char *files[] = {
            "config/src/boot_services.c",
            "config/src/boot_flyclient.c",
            "config/src/boot_background_workers.c",
            "config/src/boot_msg_callbacks.c",
            "config/src/boot.c",
            "lib/storage/include/storage/utxo_reimport_flag.h",
            "lib/validation/include/validation/process_block.h",
            "lib/validation/src/process_block_core.c",
            "lib/net/src/connman.c",
            "app/jobs/include/jobs/header_probe_poll.h",
            "app/services/include/services/block_source_policy.h",
            "app/services/src/block_source_policy_runtime.c",
            "app/services/src/block_source_policy_status.c",
            "app/services/src/block_source_policy_persist.c",
            "app/services/src/block_source_policy_decisions.c",
            "app/services/src/chain_restore_executor.c",
            "app/services/src/chain_restore_repair.c",
            "lib/storage/src/utxo_reimport_flag.c",
            "app/services/src/utxo_recovery_restore.c",
            "lib/util/include/util/stage.h",
            "lib/storage/include/storage/progress_store.h",
            "app/supervisors/include/supervisors/staged_sync_supervisor.h",
            "app/jobs/include/jobs/utxo_apply_delta.h",
            "app/jobs/include/jobs/header_admit_stage.h",
            "app/jobs/include/jobs/validate_headers_stage.h",
            "app/jobs/include/jobs/body_fetch_stage.h",
            "app/jobs/src/utxo_apply_delta.c",
            "app/jobs/src/utxo_apply_delta_reorg.c",
            "app/jobs/src/utxo_apply_stage.c",
            "app/jobs/include/jobs/block_header_emit.h",
            "app/jobs/include/jobs/body_persist_stage.h",
            "app/jobs/include/jobs/script_validate_stage.h",
            "app/jobs/include/jobs/proof_validate_stage.h",
            "app/jobs/include/jobs/utxo_apply_stage.h",
            "app/jobs/include/jobs/tip_finalize_stage.h",
            "app/jobs/include/jobs/job.h",
            "app/jobs/include/jobs/stage_helpers.h",
            "app/jobs/README.md",
            "app/jobs/src/body_persist_stage.c",
            "app/jobs/src/script_validate_stage.c",
            "app/jobs/src/proof_validate_stage.c",
            "app/jobs/src/tip_finalize_stage.c",
            "app/jobs/src/header_admit_stage.c",
            "app/jobs/src/stage_repair_reducer_frontier_refill.c",
            "app/jobs/src/stage_repair_reducer_frontier_refill_scan.c",
            "app/jobs/src/tip_finalize_post_step.h",
            "app/jobs/src/validate_headers_internal.h",
            "app/jobs/src/validate_headers_report.c",
            "app/jobs/src/validate_headers_validator.c",
            "app/controllers/src/wallet_controller_history.c",
            "app/controllers/src/transaction_controller_sign.c",
            "app/controllers/src/wallet_controller_keys.c",
            "app/controllers/src/repair_controller_utxo.c",
            "app/controllers/src/wallet_controller_multisig.c",
            "app/controllers/src/store_controller_schema.c",
            "app/controllers/src/wallet_shielded_send.c",
            "app/controllers/src/wallet_shielded_keys.c",
            "app/controllers/src/wallet_shielded_send_shielded.c",
            "app/controllers/src/wallet_shielded_controller.c",
            "app/controllers/src/wallet_rescan_controller_coins.c",
            "app/controllers/src/wallet_rescan_controller_witness.c",
            "app/controllers/src/wallet_view_emit.c",
            "app/controllers/src/wallet_view_sync.c",
            "app/controllers/src/sync_controller_import.c",
            "app/controllers/src/sync_controller_catchup.c",
            "app/controllers/src/sync_controller_catchup_jobs.c",
            "app/controllers/include/controllers/diagnostics_controller.h",
            "app/controllers/src/api_controller_node.c",
            "app/controllers/src/blockchain_controller_chain.c",
            "app/controllers/src/explorer_controller_block.c",
            "app/controllers/src/explorer_controller_pages.c",
            "app/controllers/src/explorer_controller_dashboard.c",
            "app/controllers/src/explorer_controller_address.c",
            "app/controllers/src/wallet_view_helpers.c",
            "app/controllers/src/sync_controller_blocks.c",
            "app/controllers/src/sync_controller_writers.c",
            "app/views/include/views/explorer_stats_view.h",
            "app/views/include/views/explorer_block_view.h",
            "app/views/include/views/explorer_pages_view.h",
            "app/views/include/views/explorer_dashboard_view.h",
            "app/views/include/views/store_internal.h",
            "app/views/include/views/explorer_address_view.h",
            "app/views/include/views/explorer_pages_loading_view.h",
            "app/views/include/views/explorer_tx_view.h",
            "app/views/include/views/wallet_gui_internal.h",
            "app/views/src/store_view.c",
            "app/views/src/explorer_factoids_view.c",
            "app/views/src/explorer_stats_view.c",
            "app/views/src/explorer_factoids_history.c",
            "app/views/src/explorer_factoids_chaindata.c",
            "app/views/include/views/explorer_factoids_view.h",
            "app/views/src/explorer_pages_hodl.c",
            "app/views/src/explorer_block_view.c",
            "app/views/src/explorer_stats_gather.c",
            "app/views/src/explorer_stats_sections.c",
            "app/views/src/explorer_pages_loading_view.c",
            "app/views/src/explorer_address_view.c",
            "app/views/src/explorer_pages_view.c",
            "app/views/src/explorer_dashboard_view.c",
            "app/views/src/wallet_gui_bot.c",
            "app/views/src/wallet_gui.c",
            "app/views/include/views/explorer_main_view.h",
            "app/views/src/explorer_main_view.c",
            "app/views/src/explorer_tx_view.c",
            "app/views/include/views/wallet_view_coins_view.h",
            "app/views/src/wallet_view_coins_view.c",
            "app/views/include/views/wallet_view_dashboard_view.h",
            "app/views/src/wallet_view_dashboard_view.c",
            "app/views/include/views/wallet_view_history_view.h",
            "app/views/src/wallet_view_history_view.c",
            "app/views/include/views/wallet_view_node_view.h",
            "app/views/src/wallet_view_node_view.c",
            "app/views/include/views/wallet_view_shield_view.h",
            "app/views/src/wallet_view_shield_view.c",
            "app/services/src/block_source_policy_internal.h",
            "app/controllers/include/controllers/diagnostics_internal.h",
            "app/controllers/src/diagnostics_controller.c",
            "app/services/src/block_index_loader_rebuild.c",
            "app/services/src/utxo_recovery_service.c",
            "app/services/src/chain_evidence_reconstruct.c",
            "app/services/src/bg_validation_scripts.c",
            "app/services/include/services/block_index_loader.h",
            "app/services/include/services/utxo_recovery_service.h",
            "app/services/src/bg_validation_proofs.c",
            "app/services/src/bg_validation_internal.h",
            "app/services/src/block_index_loader.c",
            "app/services/src/chain_evidence_persistence_service.c",
            "app/services/src/consensus_reject_index.c",
            "app/services/include/services/chain_state_validator.h",
            "app/services/src/chain_state_validator.c",
            "app/services/src/utxo_recovery_backfill.c",
            "app/services/src/snapshot_sync_service.c",
            "app/services/src/snapshot_sync_internal.h",
            "app/services/src/snapshot_offer.c",
            "app/services/src/snapshot_fetch.c",
            "app/services/src/snapshot_verify.c",
            "app/services/src/snapshot_apply.c",
            "app/services/include/services/chain_activation_service.h",
            "app/services/src/chain_activation_service.c",
            "app/supervisors/include/supervisors/chain_supervisor.h",
            "app/models/include/models/database_internal.h",
            "app/models/include/models/wallet_tx_internal.h",
            "app/models/include/models/header_admit_log.h",
            "app/models/src/database_modes.c",
            "app/models/src/database_migrate.c",
            "app/models/src/sapling_note.c",
            "app/models/src/wallet_tx_reads.c",
            "app/controllers/src/hodl_controller.c",
            "app/controllers/src/mining_controller.c",
            "app/controllers/src/repair_controller_rebuild.c",
            "app/supervisors/src/net_supervisor.c",
            "app/supervisors/src/staged_sync_supervisor.c",
            "app/supervisors/src/chain_supervisor.c",
            "config/src/boot_snapshot_import.c",
            "config/src/boot_index.c",
            "tools/sim/chaos.c",
            "lib/crypto_registry/include/crypto_registry/crypto_registry.h",
            "lib/crypto_registry/src/crypto_registry.c",
            "lib/platform/include/platform/clock.h",
            "lib/platform/include/platform/rng.h",
            "lib/platform/include/platform/time_compat.h",
            "lib/platform/src/clock.c",
            "lib/platform/src/rng.c",
            "lib/storage/include/storage/projection_util.h",
            "lib/storage/include/storage/event_log.h",
            "lib/storage/include/storage/event_log_payloads.h",
            "lib/storage/include/storage/block_index_projection.h",
            "lib/storage/include/storage/utxo_projection.h",
            "lib/storage/include/storage/sha3_sidecar_io.h",
            "lib/storage/src/event_log.c",
            "lib/storage/src/mempool_projection.c",
            "lib/storage/src/peers_projection.c",
            "lib/storage/src/utxo_projection.c",
            "lib/storage/src/wallet_projection.c",
            "lib/storage/src/znam_projection.c",
            "app/controllers/src/diagnostics_registry.c",
            "lib/validation/include/validation/accept_block_header.h",
            "lib/validation/include/validation/process_block_invalidate.h",
            "lib/validation/include/validation/process_block_revalidate.h",
            "lib/validation/src/accept_block_header.c",
            "lib/validation/src/process_block.c",
            "lib/validation/src/process_block_crash_hooks.c",
            "lib/validation/src/process_block_failed_child.c",
            "lib/validation/src/process_block_flush_policy.c",
            "lib/validation/src/process_block_invalidate.c",
            "lib/validation/src/process_block_internal.h",
            "lib/validation/src/process_block_revalidate.c",
            "lib/validation/src/process_block_self_heal.c",
        };
        const char *stale[] = {
            "Phase 3 dissolve",
            "dissolve PR",
            "PR-",
            "B3:",
            "B3/",
            "B2:",
            "B5:",
            "B5 reorg",
            "B5 ordering",
            "C3 split",
            "D5",
            "F-1",
            "docs/dissolve",
            "dissolved chain_advance_coordinator",
            "Re-homed verbatim",
            "verbatim",
            "Behavior-preserving",
            "code motion",
            "Pure code-motion",
            "pure code move",
            "Pure code motion",
            "file-size ceiling E1",
            "until Phase 3",
            "Phase 3 unblocks",
            "Phase 3: release refs",
            "Split out of",
            "Split into",
            "split out of",
            "specific split",
            "split files",
            "behavior byte-identical",
            "byte-identical",
            "behavior unchanged",
            "byte-identically",
            "pre-split monolith",
            "extracted from",
            "checklist item",
            "checklist D5",
            "moved out of",
            "move, not a redesign",
            "not a redesign",
            "prior controller implementation",
            "prior inline",
            "No behavior change vs the original",
            "Behavior is byte-identical",
            "Extracted from",
            "extracted verbatim",
            "pure refactor",
            "pure code motion",
            "Pure code motion",
            "single-engine replacement",
            "single-engine",
            "Single-engine",
            "single engine",
            "copy-pasted",
            "Compatibility shim",
            "legacy controller includes",
            "lifted verbatim",
            "Moved verbatim",
            "byte-for-byte",
            "skeleton",
            "idle in this PR",
            "Later Phase",
            "Phase 5a",
            "Phase 6a",
            "Phase 6c",
            "Phase 4",
            "Phase 7a",
            "Wave F-5",
            "Wave T",
            "Wave M",
            "Wave-M",
            "Wave S",
            "Wave-S",
            "WS-6.4",
            "S-2",
            "S-3",
            "S-4",
            "S-5",
            "S-6",
            "S-7",
            "S-8",
            "S-9",
            "Back-compat",
            "Precedent:",
            "gate E1",
            "file-size ceiling",
            "Phase C",
            "boot decomposition Phase",
            "for file size",
        };

        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
            char path[PATH_MAX];
            ASSERT(repo_path(path, sizeof(path), files[i]) == 0);
            ASSERT(read_entire_file(path, &buf) == 0);
            for (size_t j = 0; j < sizeof(stale) / sizeof(stale[0]); j++) {
                if (strstr(buf, stale[j])) {
                    fprintf(stderr, "stale scaffold label %s still present in %s\n",
                            stale[j], files[i]);
                    ASSERT(strstr(buf, stale[j]) == NULL);
                }
            }
            free(buf);
            buf = NULL;
        }
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_deleted_engine_names_absent_from_production_sources(void)
{
    int failures = 0;
    TEST("deleted engine names are absent from production C/H sources") {
        ASSERT(run_check_deleted_engine_names() == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_build_commit_macro_stays_behind_getter(void)
{
    int failures = 0;
    TEST("build commit macro stays behind runtime getter") {
        ASSERT(run_check_build_commit_macro_contract() == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int t_projection_deferral_is_not_block_rejected_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("projection deferral is chain advance diagnostic, not block reject") {
        /* The one-engine deletion removed legacy connect_tip(); the reducer
         * consensus path (tip_finalize_post_step.c) is now the sole producer
         * of the projection-deferred DIAGNOSTIC. The contract anchor follows
         * the live consensus path: a deferred projection write is a
         * diagnostic counter on the new path, never a block reject. */
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path),
                         "app/jobs/src/tip_finalize_post_step.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "block_source_policy_note_projection_deferred") != NULL);
        ASSERT(strstr(buf, "\"consensus_path\"") != NULL);
        ASSERT(strstr(buf, "projection-deferred-consensus-path") == NULL);
        ASSERT(strstr(buf, "EV_CHAIN_ADVANCE_DECISION") == NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path),
                         "app/controllers/src/sync_controller_blocks.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "block_source_policy_note_projection_deferred") != NULL);
        ASSERT(strstr(buf, "\"no_db_service\"") != NULL);
        ASSERT(strstr(buf, "projection-deferred-no-db-service") == NULL);
        ASSERT(strstr(buf, "EV_CHAIN_ADVANCE_DECISION") == NULL);
        free(buf);
        buf = NULL;
        ASSERT(repo_path(path, sizeof(path),
                         "app/services/src/block_source_policy_runtime.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "op=projection_deferred reason=%s") != NULL);
        ASSERT(strstr(buf, "projection_deferred_total") != NULL);
        ASSERT(strstr(buf, "EV_CHAIN_ADVANCE_DECISION") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_trusted_peer_stall_guard_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("trusted-peer stall guards stay wired on msgprocessor rules A+B") {
        /* The trusted-peer stall exemption and the P2 frontier-parity
         * term exist only as condition terms on stall rules A and B —
         * deleting either guard is invisible to every unit test (the
         * rules still fire, just for the wrong peers). Pin the exact
         * source text so removal breaks this gate and forces a
         * conscious update here. Brittle by design. */
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "lib/net/src/msgprocessor.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        /* Trusted-peer predicate: loopback/whitelist exemption (P3). */
        ASSERT(strstr(buf, "stall_peer_trusted = net_addr_is_local") != NULL);
        /* Both consumers: rule A (stale-header disconnect) and rule B
         * (worst-outbound eviction) must each carry the exemption. */
        ASSERT(count_occurrences(buf, "!stall_peer_trusted &&") >= 2);
        /* Rule B's P2 frontier-parity term: no eviction when our header
         * frontier already reached the peer's claimed tip. */
        ASSERT(strstr(buf, "!header_frontier_at_peer_tip &&") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_gap_fill_wakes_connman_dispatch_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("gap-fill requeue wakes connman dispatch") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path),
                         "app/services/src/gap_fill_service.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "gap_fill_wake_dispatcher(\"timeout_sweep\")")
               != NULL);
        ASSERT(strstr(buf, "gap_fill_wake_dispatcher(\"queued_blocks\")")
               != NULL);
        ASSERT(strstr(buf, "gap_fill_wake_dispatch_if_idle(dm, \"queued_idle\")")
               != NULL);
        ASSERT(strstr(buf, "dispatch_wakes") != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "config/src/boot_runtime_sync_services.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "gap_fill_set_dispatch_wake(") != NULL);
        ASSERT(strstr(buf, "connman_wake_message_handler") != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/net/src/connman.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "connman_wake_message_handler") != NULL);
        ASSERT(strstr(buf, "pthread_cond_timedwait") != NULL);
        ASSERT(strstr(buf, "message_wakes") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

static int t_msg_process_yields_to_send_phase_contract(void)
{
    int failures = 0;
    char *buf = NULL;
    TEST("msg_process yields to send phase under recv backlog") {
        char path[PATH_MAX];
        ASSERT(repo_path(path, sizeof(path), "lib/net/src/msgprocessor.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "processed < ZCL_MSG_PROCESS_MAX_PER_CYCLE")
               != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), "lib/net/src/connman.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "Phase 2: send first") != NULL);
        ASSERT(strstr(buf, "message_send_calls") != NULL);
        ASSERT(strstr(buf, "message_process_calls") != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path),
                         "lib/net/include/net/msgprocessor.h") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "#define ZCL_MSG_PROCESS_MAX_PER_CYCLE 128")
               != NULL);
        ASSERT(strstr(buf, "send phase regularly") != NULL);
        free(buf);
        buf = NULL;

        ASSERT(repo_path(path, sizeof(path), "lib/net/src/connman.c") == 0);
        ASSERT(read_entire_file(path, &buf) == 0);
        ASSERT(strstr(buf, "connman_recv_cap_for_queue") != NULL);
        ASSERT(strstr(buf, "CONNMAN_RECV_LOW_WATER_SLOTS") != NULL);
        PASS();
    } _test_next:;
    free(buf);
    return failures;
}

int test_make_lint_gates(void)
{
    printf("\n=== make_lint_gates tests ===\n");

    /* Resolve against the built test binary location so later tests can
     * change cwd without breaking these shell-outs. */
    struct stat st;
    char fixture_src[PATH_MAX];
    char makefile[PATH_MAX];
    if (repo_path(fixture_src, sizeof(fixture_src), FIXTURE_SRC_REL) != 0 ||
        repo_path(makefile, sizeof(makefile), "Makefile") != 0 ||
        stat(fixture_src, &st) != 0 || stat(makefile, &st) != 0) {
        printf("[lint-gate] SKIP: repo root not discoverable from test_zcl path\n");
        return 0;
    }

    int failures = 0;
    failures += t_baseline_passes();
    failures += t_fixture_trips_gate();
    failures += t_node_db_exec_fixture_trips_gate();
    failures += t_gate_recovers_after_removal();
    failures += t_coins_guard_baseline_passes();
    failures += t_coins_guard_fixture_trips_gate();
    failures += t_coins_guard_gate_recovers();
    failures += t_coins_guard_gate_fails_loud_on_no_lookup_surface();
    failures += t_observability_fixture_trips_gate();
    failures += t_observability_positive_controls_pass();
    failures += t_raw_malloc_fixture_trips_gate();
    failures += t_raw_malloc_zcl_fixture_passes();
    failures += t_raw_malloc_gate_recovers();
    failures += t_service_tip_mutation_gate();
    failures += t_legacy_candidate_source_has_no_override_scope();
    failures += t_tools_z_mirror_fallback_contract();
    failures += t_tools_z_operator_diagnostics_contract();
    failures += t_dev_lane_deploy_contract();
    failures += t_agent_fast_ci_contract();
    failures += t_remote_node_update_contract();
    failures += t_native_agent_api_contract();
    failures += t_mvp_reporters_resolve_live_service_rpc_contract();
    failures += t_soak_assert_requires_known_mirror_lag();
    failures += t_boot_chain_advance_diagnostics_contract();
    failures += t_boot_core_liveness_precedes_frontend_contract();
    failures += t_boot_addrman_persistence_contract();
    failures += t_lib_runtime_gauges_are_callback_injected();
    failures += t_boot_shutdown_persistence_order_contract();
    failures += t_hodl_history_uses_runtime_db_service();
    failures += t_db_service_query_handle_is_canonical();
    failures += t_txindex_releases_node_db_between_batches();
    failures += t_peer_save_busy_reports_db_error();
    failures += t_handshake_peer_save_is_async();
    failures += t_p2p_app_persistence_is_callback_injected();
    failures += t_tx_wallet_sync_is_callback_injected();
    failures += t_p2p_block_submit_is_callback_injected();
    failures += t_flyclient_proof_builder_is_callback_injected();
    failures += t_fast_sync_uses_lib_sqlite_helpers();
    failures += t_framework_reexport_headers_stay_deleted();
    failures += t_utxo_reimport_flag_is_storage_owned();
    failures += t_net_sync_planners_are_lib_owned();
    failures += t_header_peer_votes_are_callback_injected();
    failures += t_process_block_node_db_access_is_runtime_owned();
    failures += t_process_block_split_uses_reducer_language();
    failures += t_production_comments_do_not_carry_refactor_scaffold_labels();
    failures += t_deleted_engine_names_absent_from_production_sources();
    failures += t_build_commit_macro_stays_behind_getter();
    failures += t_boot_repaired_index_persistence_contract();
    failures += t_chain_evidence_reconstruct_uses_retry_persistence();
    failures += t_boot_genesis_init_preserves_restored_authority_contract();
    failures += t_refold_from_anchor_explicit_span_gate_contract();
    failures += t_sha3_window_tool_check_contract();
    failures += t_make_ignores_ephemeral_lint_fixture_sources();
    failures += t_block_index_flat_atomic_save_contract();
    failures += t_projection_deferral_is_not_block_rejected_contract();
    failures += t_trusted_peer_stall_guard_contract();
    failures += t_gap_fill_wakes_connman_dispatch_contract();
    failures += t_msg_process_yields_to_send_phase_contract();
    failures += t_e1_file_size_ceiling();
    failures += t_no_new_repair_rung();
    failures += t_no_new_borrowed_seed_caller();
    failures += t_no_new_coin_backfill_caller();
    failures += t_e9_operator_needed_sink();
    failures += t_systemd_memory_budget();
    failures += t_git_hooks_gate_enforces_tracked_pre_push();
    failures += t_git_hooks_gate_rejects_noop_pre_push();
    failures += t_e10_framework_shape_ratchet();
    failures += t_e10_no_raw_sqlite_ratchet();
    failures += t_gate22_framework_filename_suffix();
    failures += t_log_macro_return_type_gate();
    failures += t_e11_doc_accuracy();
    failures += t_model_ar_lifecycle_gate();
    failures += t_e2_one_result_type();
    failures += t_e3_shape_includes_header();
    failures += t_e4_projections_pure();
    failures += t_domain_purity();
    failures += t_e5_stage_advances_or_blocks();
    failures += t_e6_one_write_path();
    failures += t_e7_no_authoritative_ram_state();
    failures += t_e12_honest_witness();
    failures += t_gate21_supervisor_worker_lockin();
    failures += t_hotswap_eligible_scope_gate();
    failures += t_hotswap_static_state_gate();
    failures += t_lint_gates_fail_loud_on_empty_scan();
    return failures;
}

#else  /* !ZCL_TESTING */

int test_make_lint_gates(void)
{
    /* No-op when the lint-gate integration test is disabled. */
    return 0;
}

#endif /* ZCL_TESTING */
