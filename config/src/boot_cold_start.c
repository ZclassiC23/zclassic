/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_cold_start.c — the `-cold-start` staged, resumable driver. Contract +
 * rationale live in config/boot_cold_start.h.
 *
 * TWO layers, mirroring shutdown_stagewatch.c:
 *   (1) PURE helpers — stage naming, receipt path/write/read/match, and the
 *       resume decision. No child spawn, no global state; unit tested directly.
 *   (2) LIVE driver — fork/exec of each existing verb as a child, then exec of
 *       the plain serving boot. Composes the existing stages; never duplicates
 *       normal boot.
 */

#include "config/boot_cold_start.h"

#include "platform/os_proc.h"    /* os_proc_exe_path */
#include "util/file_tree_ops.h"  /* zcl_mkdir_p */
#include "util/log_macros.h"
#include "util/safe_alloc.h"     /* zcl_malloc */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define COLD_START_SUBSYS  "cold_start"
#define COLD_START_MAGIC   "ZCLCOLDSTART"
#define COLD_START_VERSION 1

/* ── (1) Pure helpers ─────────────────────────────────────────────────── */

const char *cold_start_stage_name(enum cold_start_stage stage)
{
    switch (stage) {
    case COLD_START_STAGE_HEADERS: return "headers";
    case COLD_START_STAGE_SEED:    return "seed";
    case COLD_START_STAGE_BUNDLE:  return "bundle";
    case COLD_START_STAGE_SERVE:   return "serve";
    }
    return "?";
}

const char *cold_start_stage_param(const struct cold_start_plan *plan,
                                   enum cold_start_stage stage)
{
    if (!plan)
        return NULL;
    const char *p = NULL;
    switch (stage) {
    case COLD_START_STAGE_HEADERS: p = plan->header_source;  break;
    case COLD_START_STAGE_SEED:    p = plan->seed_snapshot;  break;
    case COLD_START_STAGE_BUNDLE:  p = plan->install_bundle; break;
    case COLD_START_STAGE_SERVE:   p = NULL;                 break;
    }
    return (p && p[0]) ? p : NULL;
}

bool cold_start_stage_configured(const struct cold_start_plan *plan,
                                 enum cold_start_stage stage)
{
    if (stage == COLD_START_STAGE_SERVE)
        return true; /* the driver always ends by serving */
    return cold_start_stage_param(plan, stage) != NULL;
}

int cold_start_receipt_path(const char *datadir, enum cold_start_stage stage,
                            char *buf, size_t n)
{
    if (!datadir || !datadir[0] || !buf || n == 0)
        return -1;
    int w = snprintf(buf, n, "%s/coldstart/%s.receipt", datadir,
                     cold_start_stage_name(stage));
    if (w < 0 || (size_t)w >= n)
        return -1;
    return w;
}

/* fsync the directory that holds `path` so a rename into it is durable. */
static void cold_start_fsync_parent_dir(const char *path)
{
    char dir[PATH_MAX];
    int w = snprintf(dir, sizeof(dir), "%s", path);
    if (w < 0 || (size_t)w >= sizeof(dir))
        return;
    char *slash = strrchr(dir, '/');
    if (!slash)
        return;
    *slash = '\0';
    int fd = open(dir[0] ? dir : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return;
    (void)fsync(fd);
    (void)close(fd);
}

bool cold_start_receipt_write(const char *datadir, enum cold_start_stage stage,
                              const char *param)
{
    if (!datadir || !datadir[0])
        LOG_FAIL(COLD_START_SUBSYS, "receipt write: empty datadir");

    char dir[PATH_MAX];
    int dn = snprintf(dir, sizeof(dir), "%s/coldstart", datadir);
    if (dn < 0 || (size_t)dn >= sizeof(dir))
        LOG_FAIL(COLD_START_SUBSYS, "receipt write: coldstart dir path too long");
    struct zcl_result mk = zcl_mkdir_p(dir, 0755);
    if (!mk.ok)
        LOG_FAIL(COLD_START_SUBSYS, "receipt write: mkdir %s failed: %s",
                 dir, mk.message);

    char path[PATH_MAX];
    if (cold_start_receipt_path(datadir, stage, path, sizeof(path)) < 0)
        LOG_FAIL(COLD_START_SUBSYS, "receipt write: path build failed");
    char tmp[PATH_MAX];
    int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (tn < 0 || (size_t)tn >= sizeof(tmp))
        LOG_FAIL(COLD_START_SUBSYS, "receipt write: tmp path too long");

    char content[PATH_MAX + 128];
    int cn = snprintf(content, sizeof(content),
                      "magic=%s\nversion=%d\nstage=%s\nhas_param=%d\nparam=%s\n",
                      COLD_START_MAGIC, COLD_START_VERSION,
                      cold_start_stage_name(stage),
                      (param && param[0]) ? 1 : 0,
                      (param && param[0]) ? param : "");
    if (cn < 0 || (size_t)cn >= sizeof(content))
        LOG_FAIL(COLD_START_SUBSYS, "receipt write: content build failed");

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        LOG_FAIL(COLD_START_SUBSYS, "receipt write: open %s failed: %s",
                 tmp, strerror(errno));
    bool ok = (write(fd, content, (size_t)cn) == (ssize_t)cn) &&
              (fsync(fd) == 0);
    if (close(fd) != 0)
        ok = false;
    if (!ok) {
        (void)unlink(tmp);
        LOG_FAIL(COLD_START_SUBSYS, "receipt write: write/fsync %s failed", tmp);
    }
    if (rename(tmp, path) != 0) {
        (void)unlink(tmp);
        LOG_FAIL(COLD_START_SUBSYS, "receipt write: rename %s failed: %s",
                 path, strerror(errno));
    }
    cold_start_fsync_parent_dir(path);
    LOG_INFO(COLD_START_SUBSYS, "stage '%s' receipt written (%s)",
             cold_start_stage_name(stage), path);
    return true;
}

/* Extract the value of `key=` from a receipt buffer into out (bounded). Returns
 * true iff the key line is present. */
static bool cold_start_receipt_field(const char *buf, const char *key,
                                     char *out, size_t out_n)
{
    if (out_n)
        out[0] = '\0';
    size_t keylen = strlen(key);
    const char *p = buf;
    while (p && *p) {
        /* Match key= at the start of a line. */
        if (strncmp(p, key, keylen) == 0 && p[keylen] == '=') {
            const char *v = p + keylen + 1;
            const char *end = strchr(v, '\n');
            size_t len = end ? (size_t)(end - v) : strlen(v);
            if (len >= out_n)
                len = out_n ? out_n - 1 : 0;
            if (out_n) {
                memcpy(out, v, len);
                out[len] = '\0';
            }
            return true;
        }
        const char *nl = strchr(p, '\n');
        if (!nl)
            break;
        p = nl + 1;
    }
    return false;
}

bool cold_start_receipt_matches(const char *datadir, enum cold_start_stage stage,
                                const char *param)
{
    char path[PATH_MAX];
    if (cold_start_receipt_path(datadir, stage, path, sizeof(path)) < 0)
        return false;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    char buf[PATH_MAX + 256];
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    (void)close(fd);
    if (r <= 0)
        return false;
    buf[r] = '\0';

    char magic[32];
    if (!cold_start_receipt_field(buf, "magic", magic, sizeof(magic)) ||
        strcmp(magic, COLD_START_MAGIC) != 0)
        return false;

    char has_param[8];
    bool recorded_has = cold_start_receipt_field(buf, "has_param", has_param,
                                                 sizeof(has_param)) &&
                        strcmp(has_param, "1") == 0;
    bool want_has = (param && param[0]);
    if (recorded_has != want_has)
        return false;
    if (!want_has)
        return true; /* both parameter-less — a match */

    char recorded_param[PATH_MAX];
    if (!cold_start_receipt_field(buf, "param", recorded_param,
                                  sizeof(recorded_param)))
        return false;
    return strcmp(recorded_param, param) == 0;
}

enum cold_start_stage cold_start_plan_next(const struct cold_start_plan *plan)
{
    static const enum cold_start_stage order[COLD_START_PREP_STAGE_COUNT] = {
        COLD_START_STAGE_HEADERS,
        COLD_START_STAGE_SEED,
        COLD_START_STAGE_BUNDLE,
    };
    if (!plan || !plan->datadir || !plan->datadir[0])
        return COLD_START_STAGE_SERVE;
    for (int i = 0; i < COLD_START_PREP_STAGE_COUNT; i++) {
        enum cold_start_stage s = order[i];
        if (!cold_start_stage_configured(plan, s))
            continue;
        if (!cold_start_receipt_matches(plan->datadir, s,
                                        cold_start_stage_param(plan, s)))
            return s;
    }
    return COLD_START_STAGE_SERVE;
}

int cold_start_drive(const struct cold_start_plan *plan,
                     cold_start_stage_runner_fn runner, void *user,
                     enum cold_start_stage *out_reached)
{
    if (out_reached)
        *out_reached = COLD_START_STAGE_SERVE;
    if (!plan || !plan->datadir || !plan->datadir[0])
        LOG_ERR(COLD_START_SUBSYS, "drive: empty plan/datadir");
    if (!runner)
        LOG_ERR(COLD_START_SUBSYS, "drive: NULL stage runner");

    /* Bounded: each iteration either serves, fails, or converts exactly one
     * prep stage from "no receipt" to "receipt present", so the loop cannot run
     * more than the prep-stage count plus one. */
    for (int guard = 0; guard <= COLD_START_PREP_STAGE_COUNT; guard++) {
        enum cold_start_stage next = cold_start_plan_next(plan);
        if (out_reached)
            *out_reached = next;
        if (next == COLD_START_STAGE_SERVE)
            return 0;
        LOG_INFO(COLD_START_SUBSYS, "running stage '%s'",
                 cold_start_stage_name(next));
        int rc = runner(plan, next, user);
        if (rc != 0) {
            LOG_WARN(COLD_START_SUBSYS, "stage '%s' failed rc=%d — halting "
                     "(no receipt written; rerun -cold-start to resume here)",
                     cold_start_stage_name(next), rc);
            return rc;
        }
        if (!cold_start_receipt_write(plan->datadir, next,
                                      cold_start_stage_param(plan, next)))
            LOG_ERR(COLD_START_SUBSYS, "stage '%s' succeeded but its receipt "
                    "could not be persisted — halting to avoid an unrecordable "
                    "resume point", cold_start_stage_name(next));
    }
    LOG_ERR(COLD_START_SUBSYS, "drive: exceeded stage bound without reaching "
            "serve (a receipt is not persisting?)");
}

/* ── (2) Live driver ──────────────────────────────────────────────────── */

/* Resolve this process's executable path, stripping any kernel " (deleted)"
 * suffix a mid-deploy readlink can append. */
static bool cold_start_self_exe(char *buf, size_t n)
{
    if (!os_proc_exe_path(buf, n))
        return false;
    size_t len = strlen(buf);
    static const char del[] = " (deleted)";
    size_t dl = sizeof(del) - 1;
    if (len >= dl && strcmp(buf + len - dl, del) == 0)
        buf[len - dl] = '\0';
    return buf[0] != '\0';
}

/* Fork/exec a child with argv `child_argv` (NULL-terminated) and optionally set
 * `env_key=1` for it. Waits synchronously. Returns 0 iff the child exited 0. */
static int cold_start_spawn_wait(char *const child_argv[], const char *env_key)
{
    char exe[PATH_MAX];
    if (!cold_start_self_exe(exe, sizeof(exe)))
        LOG_ERR(COLD_START_SUBSYS, "spawn: cannot resolve own executable path");

    pid_t pid = fork();
    if (pid < 0)
        LOG_ERR(COLD_START_SUBSYS, "spawn: fork failed: %s", strerror(errno));
    if (pid == 0) {
        if (env_key)
            setenv(env_key, "1", 1);
        execv(exe, child_argv);
        /* Only reached on exec failure. */
        fprintf(stderr, "cold_start: execv %s failed: %s\n", exe,
                strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        LOG_ERR(COLD_START_SUBSYS, "spawn: waitpid failed: %s", strerror(errno));
    }
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code != 0)
            LOG_WARN(COLD_START_SUBSYS, "child exited with code %d", code);
        return code;
    }
    if (WIFSIGNALED(status))
        LOG_ERR(COLD_START_SUBSYS, "child killed by signal %d",
                WTERMSIG(status));
    LOG_ERR(COLD_START_SUBSYS, "child ended abnormally (status=0x%x)", status);
}

/* Live stage runner: fork/exec the existing verb for each prep stage. */
static int cold_start_run_stage_live(const struct cold_start_plan *plan,
                                     enum cold_start_stage stage, void *user)
{
    char exe[PATH_MAX];
    (void)user;
    if (!cold_start_self_exe(exe, sizeof(exe)))
        LOG_ERR(COLD_START_SUBSYS, "run stage: cannot resolve executable");

    char datadir_arg[PATH_MAX + 16];
    if (snprintf(datadir_arg, sizeof(datadir_arg), "-datadir=%s",
                 plan->datadir) >= (int)sizeof(datadir_arg))
        LOG_ERR(COLD_START_SUBSYS, "run stage: datadir too long");

    switch (stage) {
    case COLD_START_STAGE_HEADERS: {
        /* --importblockindex MUST be argv[1] or it silently no-ops; we build
         * argv so it always is. Target db = <datadir>/node.db. */
        char db_path[PATH_MAX + 16];
        if (snprintf(db_path, sizeof(db_path), "%s/node.db", plan->datadir) >=
            (int)sizeof(db_path))
            LOG_ERR(COLD_START_SUBSYS, "run stage: node.db path too long");
        char *argv[] = { exe, (char *)"--importblockindex",
                         (char *)plan->header_source, db_path, NULL };
        return cold_start_spawn_wait(argv, NULL);
    }
    case COLD_START_STAGE_SEED: {
        /* Seed as a clean one-shot: -coldstart-seed-oneshot makes app_init apply
         * the seed then exit before services (config/src/boot.c). Headers are
         * already imported (previous stage), so the snapshot gate binds. */
        char seed_arg[PATH_MAX + 40];
        if (snprintf(seed_arg, sizeof(seed_arg),
                     "-load-snapshot-at-own-height=%s", plan->seed_snapshot) >=
            (int)sizeof(seed_arg))
            LOG_ERR(COLD_START_SUBSYS, "run stage: seed path too long");
        char *argv[] = { exe, datadir_arg, seed_arg,
                         (char *)"-coldstart-seed-oneshot", NULL };
        return cold_start_spawn_wait(argv, NULL);
    }
    case COLD_START_STAGE_BUNDLE: {
        /* -install-consensus-bundle is terminal (installs then _exit()s). Runs
         * after the seed so the bundle installs onto the seeded datadir. */
        char bundle_arg[PATH_MAX + 40];
        if (snprintf(bundle_arg, sizeof(bundle_arg),
                     "-install-consensus-bundle=%s", plan->install_bundle) >=
            (int)sizeof(bundle_arg))
            LOG_ERR(COLD_START_SUBSYS, "run stage: bundle path too long");
        char *argv[] = { exe, datadir_arg, bundle_arg, NULL };
        return cold_start_spawn_wait(argv, NULL);
    }
    case COLD_START_STAGE_SERVE:
        LOG_ERR(COLD_START_SUBSYS, "run stage: SERVE is not a spawned prep "
                "stage");
    }
    LOG_ERR(COLD_START_SUBSYS, "run stage: unknown stage %d", (int)stage);
}

/* Exec a plain serving boot: the original argv minus the cold-start-only flags
 * (-cold-start, -cold-start-source=, -cold-start-seed=) and minus
 * -install-consensus-bundle= (already installed by the bundle stage). Does not
 * return on success. */
static int cold_start_exec_serve(int argc, char **argv)
{
    char exe[PATH_MAX];
    if (!cold_start_self_exe(exe, sizeof(exe)))
        LOG_ERR(COLD_START_SUBSYS, "serve: cannot resolve executable");

    char **serve_argv =
        zcl_malloc(((size_t)argc + 1) * sizeof(*serve_argv), "coldstart_serve_argv");
    if (!serve_argv)
        LOG_ERR(COLD_START_SUBSYS, "serve: argv alloc failed");
    int n = 0;
    serve_argv[n++] = exe;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-cold-start") == 0 ||
            strncmp(argv[i], "-cold-start-source=", 19) == 0 ||
            strncmp(argv[i], "-cold-start-seed=", 17) == 0 ||
            strncmp(argv[i], "-install-consensus-bundle=", 26) == 0)
            continue;
        serve_argv[n++] = argv[i];
    }
    serve_argv[n] = NULL;

    LOG_INFO(COLD_START_SUBSYS, "all prep stages complete — exec serving boot");
    execv(exe, serve_argv);
    int e = errno;
    free(serve_argv);
    LOG_ERR(COLD_START_SUBSYS, "serve: execv failed: %s", strerror(e));
}

int boot_cold_start_run(int argc, char **argv)
{
    struct cold_start_plan plan = {0};
    plan.datadir = NULL;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-datadir=", 9) == 0)
            plan.datadir = argv[i] + 9;
        else if (strncmp(argv[i], "-cold-start-source=", 19) == 0)
            plan.header_source = argv[i] + 19;
        else if (strncmp(argv[i], "-cold-start-seed=", 17) == 0)
            plan.seed_snapshot = argv[i] + 17;
        else if (strncmp(argv[i], "-install-consensus-bundle=", 26) == 0)
            plan.install_bundle = argv[i] + 26;
    }

    /* Default datadir mirrors the rest of the binary. */
    static char default_datadir[PATH_MAX];
    if (!plan.datadir || !plan.datadir[0]) {
        const char *home = getenv("HOME");
        if (snprintf(default_datadir, sizeof(default_datadir),
                     "%s/.zclassic-c23", home ? home : ".") >=
            (int)sizeof(default_datadir))
            LOG_ERR(COLD_START_SUBSYS, "default datadir path too long");
        plan.datadir = default_datadir;
    }

    /* Default header source: a co-located legacy zclassicd datadir with a block
     * index. Absent => skip the header stage and let the serving boot P2P-sync
     * headers. */
    static char default_source[PATH_MAX];
    if (!plan.header_source || !plan.header_source[0]) {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            char idx[PATH_MAX];
            if (snprintf(idx, sizeof(idx), "%s/.zclassic/blocks/index", home) <
                (int)sizeof(idx)) {
                struct stat st;
                if (stat(idx, &st) == 0 && S_ISDIR(st.st_mode)) {
                    if (snprintf(default_source, sizeof(default_source),
                                 "%s/.zclassic", home) <
                        (int)sizeof(default_source))
                        plan.header_source = default_source;
                }
            }
        }
    }

    LOG_INFO(COLD_START_SUBSYS,
             "cold-start: datadir=%s source=%s seed=%s bundle=%s",
             plan.datadir,
             plan.header_source ? plan.header_source : "(P2P)",
             plan.seed_snapshot ? plan.seed_snapshot : "(none)",
             plan.install_bundle ? plan.install_bundle : "(none)");
    printf("=== ZClassic cold-start driver ===\n"
           "  datadir : %s\n"
           "  headers : %s\n"
           "  seed    : %s\n"
           "  bundle  : %s\n\n",
           plan.datadir,
           plan.header_source ? plan.header_source : "(P2P header sync)",
           plan.seed_snapshot ? plan.seed_snapshot : "(none)",
           plan.install_bundle ? plan.install_bundle : "(none)");

    enum cold_start_stage reached = COLD_START_STAGE_SERVE;
    int rc = cold_start_drive(&plan, cold_start_run_stage_live, NULL, &reached);
    if (rc != 0) {
        fprintf(stderr,
                "cold-start: stopped at stage '%s' (rc=%d). Fix the cause and "
                "re-run the SAME -cold-start command; completed stages are "
                "skipped via their receipts under %s/coldstart/.\n",
                cold_start_stage_name(reached), rc, plan.datadir);
        return rc == 0 ? 1 : rc;
    }
    /* Every configured prep stage is receipted — become the serving node. */
    return cold_start_exec_serve(argc, argv);
}
