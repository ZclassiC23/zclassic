/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_dev_activation — hermetic tests for the native transactional dev-lane
 * activation engine (tools/dev/dev_activation.c + _stage/_verify). Every case
 * runs against a mkdtemp sandbox with HOME redirected so the confinement checks
 * resolve to sandbox paths, and drives the transaction through a fake in-memory
 * ops vtable (no systemctl, no /proc, no process exec). The real ops
 * (dev_activation_default_ops) live in a ZCL_DEV_BUILD-only TU absent from this
 * ZCL_TESTING harness, which is the point: the engine logic is exercised with
 * controllable service verdicts.
 */

#include "test/test_helpers.h"

#include "dev_activation.h"
#include "json/json.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ── fake ops ────────────────────────────────────────────────────────── */

struct fake_ctx {
    char gen_root[PATH_MAX];
    char fail_start_gen[80];  /* start returns non-zero for this generation */
    char fail_reload_gen[80]; /* daemon-reload fails for this generation */
    char fail_probe_gen[80];  /* activation probe fails for this generation */
    int preflight_result;     /* 0 => preflight passes */
    bool service_up;
    int stop_calls, start_calls, reload_calls, probe_calls, preflight_calls;
};

/* Read the generation id the `current` link points at into out. */
static bool fake_current_gen(struct fake_ctx *c, char *out, size_t out_sz)
{
    char link[PATH_MAX];
    snprintf(link, sizeof(link), "%s/current", c->gen_root);
    char tgt[PATH_MAX];
    ssize_t n = readlink(link, tgt, sizeof(tgt) - 1);
    if (n <= 0)
        return false;
    tgt[n] = 0;
    snprintf(out, out_sz, "%s", tgt);
    return true;
}

static int fk_stop(void *ctx)
{
    struct fake_ctx *c = ctx;
    c->stop_calls++;
    c->service_up = false;
    return 0;
}

static int fk_start(void *ctx)
{
    struct fake_ctx *c = ctx;
    c->start_calls++;
    char gen[80];
    if (c->fail_start_gen[0] && fake_current_gen(c, gen, sizeof(gen)) &&
        strcmp(gen, c->fail_start_gen) == 0)
        return 1;
    c->service_up = true;
    return 0;
}

static int fk_reload(void *ctx)
{
    struct fake_ctx *c = ctx;
    c->reload_calls++;
    char gen[80];
    if (c->fail_reload_gen[0] && fake_current_gen(c, gen, sizeof(gen)) &&
        strcmp(gen, c->fail_reload_gen) == 0)
        return 1;
    return 0;
}

static int fk_reset(void *ctx) { (void)ctx; return 0; }

static int fk_active(void *ctx)
{
    struct fake_ctx *c = ctx;
    return c->service_up ? 0 : 1;
}

static int fk_main_pid(void *ctx, long *pid_out)
{
    (void)ctx;
    *pid_out = 4242;
    return 0;
}

static int fk_running_exe(void *ctx, long pid, char *out, size_t out_sz)
{
    struct fake_ctx *c = ctx;
    (void)pid;
    char gen[80];
    if (!fake_current_gen(c, gen, sizeof(gen)))
        return -1;
    snprintf(out, out_sz, "%s/%s/zclassic23-dev", c->gen_root, gen);
    return 0;
}

static int fk_preflight(void *ctx, const char *cand_bin, const char *commit)
{
    struct fake_ctx *c = ctx;
    (void)cand_bin;
    (void)commit;
    c->preflight_calls++;
    return c->preflight_result;
}

static int fk_probe(void *ctx, const char *gen_id, const char *commit)
{
    struct fake_ctx *c = ctx;
    (void)commit;
    c->probe_calls++;
    if (c->fail_probe_gen[0] && strcmp(gen_id, c->fail_probe_gen) == 0)
        return 1;
    return 0;
}

static void fake_ops_init(struct dev_activation_ops *ops, struct fake_ctx *c)
{
    memset(ops, 0, sizeof(*ops));
    ops->service_stop = fk_stop;
    ops->service_start = fk_start;
    ops->service_daemon_reload = fk_reload;
    ops->service_reset_failed = fk_reset;
    ops->service_active = fk_active;
    ops->service_main_pid = fk_main_pid;
    ops->running_exe = fk_running_exe;
    ops->preflight = fk_preflight;
    ops->activation_probe = fk_probe;
    ops->ctx = c;
}

/* ── sandbox helpers ─────────────────────────────────────────────────── */

struct sandbox {
    char home[PATH_MAX];
    char datadir[PATH_MAX];
    char gen_root[PATH_MAX];
    char *saved_home;
};

static void sandbox_enter(struct sandbox *sb, const char *tag)
{
    char rel[PATH_MAX];
    test_make_tmpdir(rel, sizeof(rel), "dev_activation", tag);
    /* The harness runs from a relative CWD, so test_make_tmpdir hands back a
     * "./test-tmp/..." path — resolve it to an absolute HOME so the engine's
     * absolute-path confinement checks operate on real lane paths. */
    if (!realpath(rel, sb->home))
        snprintf(sb->home, sizeof(sb->home), "%s", rel);
    snprintf(sb->datadir, sizeof(sb->datadir), "%s/.zclassic-c23-dev", sb->home);
    snprintf(sb->gen_root, sizeof(sb->gen_root), "%s/lib/gens", sb->home);
    const char *h = getenv("HOME");
    sb->saved_home = h ? strdup(h) : NULL;
    setenv("HOME", sb->home, 1);
    setenv("ZCL_DEV_ACTIVATION_VERIFY_TIMEOUT_S", "0", 1);
}

static void sandbox_exit(struct sandbox *sb)
{
    if (sb->saved_home) {
        setenv("HOME", sb->saved_home, 1);
        free(sb->saved_home);
    } else {
        unsetenv("HOME");
    }
    unsetenv("ZCL_DEV_ACTIVATION_VERIFY_TIMEOUT_S");
    test_rm_rf_recursive(sb->home);
}

/* Write a distinct fake executable node binary. */
static bool write_fake_binary(const char *path, char fill)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    char buf[256];
    memset(buf, fill, sizeof(buf));
    fwrite(buf, 1, sizeof(buf), f);
    fclose(f);
    return chmod(path, 0755) == 0;
}

static void base_request(struct dev_activation_request *req, struct sandbox *sb,
                         const char *artifact)
{
    memset(req, 0, sizeof(*req));
    req->repo_root = sb->home;
    req->artifact_path = artifact;
    req->build_commit = "testcommit";
    req->build_type = "fast";
    req->gen_root = sb->gen_root;
    req->datadir = sb->datadir;
    req->unit = "zcl23-dev.service";
    req->rpcport = 18252;
    req->mode = DEV_ACTIVATION_MODE_ACTIVATE;
}

static bool read_link_str(const char *link, char *out, size_t out_sz)
{
    ssize_t n = readlink(link, out, out_sz - 1);
    if (n <= 0)
        return false;
    out[n] = 0;
    return true;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Read a whole file into buf; returns byte count or 0. */
static size_t read_file(const char *path, char *buf, size_t buf_sz)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, buf_sz - 1, f);
    fclose(f);
    buf[n] = 0;
    return n;
}

/* mkdir -p a path (best-effort, mirrors the lock-test inline helper). */
static void mkpath(const char *dir)
{
    char part[PATH_MAX];
    snprintf(part, sizeof(part), "%s", dir);
    for (char *p = part + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(part, 0755); *p = '/'; }
    mkdir(part, 0755);
}

/* Arm a crash-only auto-reindex sentinel "<anchor> <count>\n" in the dev
 * datadir (the on-disk format boot_auto_reindex.c writes). */
static bool write_auto_reindex_sentinel(struct sandbox *sb, int anchor, int count)
{
    mkpath(sb->datadir);
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/auto_reindex_request", sb->datadir);
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fprintf(f, "%d %d\n", anchor, count);
    fclose(f);
    return true;
}

/* Plant a stale activation.in_progress marker in the generation root, as if a
 * prior activation had crashed mid-flip. */
static bool write_in_progress_marker(struct sandbox *sb, const char *gen)
{
    mkpath(sb->gen_root);
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/activation.in_progress", sb->gen_root);
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fprintf(f, "{\"schema\":\"zcl.dev_activation_in_progress.v1\",\"pid\":999999,"
               "\"candidate_generation\":\"%s\"}\n", gen ? gen : "gen-dead");
    fclose(f);
    return true;
}

static bool in_progress_marker_exists(struct sandbox *sb)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/activation.in_progress", sb->gen_root);
    return file_exists(path);
}

static bool hex_to_bytes32(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64)
        return false;
    for (int i = 0; i < 32; i++) {
        char b[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        char *end = NULL;
        long v = strtol(b, &end, 16);
        if (end != b + 2)
            return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

/* ── tests ───────────────────────────────────────────────────────────── */

static int test_happy_activation(void)
{
    int failures = 0;
    TEST("dev_activation: happy activation flips current + writes deploy state") {
        struct sandbox sb;
        sandbox_enter(&sb, "happy");
        char artifact[PATH_MAX];
        snprintf(artifact, sizeof(artifact), "%s/cand", sb.home);
        ASSERT(write_fake_binary(artifact, 'A'));

        struct fake_ctx c = {0};
        snprintf(c.gen_root, sizeof(c.gen_root), "%s", sb.gen_root);
        struct dev_activation_ops ops;
        fake_ops_init(&ops, &c);
        struct dev_activation_request req;
        base_request(&req, &sb, artifact);
        struct dev_activation_result r;

        int rc = dev_activation_run(&req, &ops, &r);
        ASSERT_EQ(rc, DEV_ACTIVATION_OK);
        ASSERT_STR_EQ(r.activation_status, "active");
        ASSERT_STR_EQ(r.verify_status, "ready");
        ASSERT(strlen(r.candidate_sha256) == 64);

        char expect_gen[96];
        snprintf(expect_gen, sizeof(expect_gen), "gen-%s", r.candidate_sha256);
        ASSERT_STR_EQ(r.candidate_generation, expect_gen);
        ASSERT_STR_EQ(r.running_generation, expect_gen);

        /* current symlink resolves to the candidate generation */
        char cur[PATH_MAX], link[PATH_MAX];
        snprintf(link, sizeof(link), "%s/current", sb.gen_root);
        ASSERT(read_link_str(link, cur, sizeof(cur)));
        ASSERT_STR_EQ(cur, expect_gen);

        /* compat link points at the generation binary */
        char compat[PATH_MAX], ctgt[PATH_MAX], want[PATH_MAX];
        snprintf(compat, sizeof(compat), "%s/.local/bin/zclassic23-dev", sb.home);
        ASSERT(read_link_str(compat, ctgt, sizeof(ctgt)));
        snprintf(want, sizeof(want), "%s/current/zclassic23-dev", sb.gen_root);
        ASSERT_STR_EQ(ctgt, want);

        /* deploy-state contract */
        char state[PATH_MAX], js[8192];
        snprintf(state, sizeof(state), "%s/agent-deploy.json", sb.datadir);
        size_t jn = read_file(state, js, sizeof(js));
        ASSERT(jn > 0);
        struct json_value v = {0};
        ASSERT(json_read(&v, js, jn));
        ASSERT_STR_EQ(json_get_str(json_get(&v, "schema")),
                      "zcl.agent_dev_deploy.v1");
        ASSERT_STR_EQ(json_get_str(json_get(&v, "candidate_sha256")),
                      r.candidate_sha256);
        ASSERT_STR_EQ(json_get_str(json_get(&v, "running_generation")),
                      expect_gen);
        ASSERT_STR_EQ(json_get_str(json_get(&v, "current_generation")),
                      expect_gen);
        ASSERT_STR_EQ(json_get_str(json_get(&v, "activation_status")), "active");
        ASSERT_STR_EQ(json_get_str(json_get(&v, "build_commit")), "testcommit");
        ASSERT(json_get_bool(json_get(&v, "rollback_available")));
        json_free(&v);

        sandbox_exit(&sb);
        PASS();
    } _test_next:;
    return failures;
}

static int run_confinement_case(struct sandbox *sb, struct dev_activation_request *req)
{
    char artifact[PATH_MAX];
    snprintf(artifact, sizeof(artifact), "%s/cand", sb->home);
    write_fake_binary(artifact, 'A');
    struct fake_ctx c = {0};
    snprintf(c.gen_root, sizeof(c.gen_root), "%s", sb->gen_root);
    struct dev_activation_ops ops;
    fake_ops_init(&ops, &c);
    struct dev_activation_result r;
    return dev_activation_run(req, &ops, &r);
}

static int test_confinement_refusals(void)
{
    int failures = 0;
    TEST("dev_activation: confinement refuses non-dev lane (unit/datadir/port/root)") {
        struct sandbox sb;
        sandbox_enter(&sb, "confine");
        char artifact[PATH_MAX];
        snprintf(artifact, sizeof(artifact), "%s/cand", sb.home);

        struct dev_activation_request req;

        /* wrong unit */
        base_request(&req, &sb, artifact);
        req.unit = "zclassic23.service";
        ASSERT_EQ(run_confinement_case(&sb, &req), DEV_ACTIVATION_E_CONFINEMENT);

        /* wrong datadir */
        base_request(&req, &sb, artifact);
        char bad_dd[PATH_MAX];
        snprintf(bad_dd, sizeof(bad_dd), "%s/.zclassic-c23", sb.home);
        req.datadir = bad_dd;
        ASSERT_EQ(run_confinement_case(&sb, &req), DEV_ACTIVATION_E_CONFINEMENT);

        /* wrong rpcport */
        base_request(&req, &sb, artifact);
        req.rpcport = 18232;
        ASSERT_EQ(run_confinement_case(&sb, &req), DEV_ACTIVATION_E_CONFINEMENT);

        /* gen-root under the live datadir */
        base_request(&req, &sb, artifact);
        char bad_root[PATH_MAX];
        snprintf(bad_root, sizeof(bad_root), "%s/.zclassic-c23/gens", sb.home);
        req.gen_root = bad_root;
        ASSERT_EQ(run_confinement_case(&sb, &req), DEV_ACTIVATION_E_CONFINEMENT);

        sandbox_exit(&sb);
        PASS();
    } _test_next:;
    return failures;
}

static int test_lock_busy(void)
{
    int failures = 0;
    TEST("dev_activation: a held activation lock yields E_LOCK_BUSY") {
        struct sandbox sb;
        sandbox_enter(&sb, "lock");
        char artifact[PATH_MAX];
        snprintf(artifact, sizeof(artifact), "%s/cand", sb.home);
        ASSERT(write_fake_binary(artifact, 'A'));

        /* Take the lock ourselves first. */
        char lockp[PATH_MAX];
        snprintf(lockp, sizeof(lockp), "%s", sb.gen_root);
        char cmd_dir[PATH_MAX];
        snprintf(cmd_dir, sizeof(cmd_dir), "%s", sb.gen_root);
        /* mkdir -p gen_root */
        char part[PATH_MAX];
        snprintf(part, sizeof(part), "%s", sb.gen_root);
        for (char *p = part + 1; *p; p++)
            if (*p == '/') { *p = 0; mkdir(part, 0755); *p = '/'; }
        mkdir(part, 0755);
        snprintf(lockp, sizeof(lockp), "%s/activation.lock", sb.gen_root);
        int fd = open(lockp, O_RDWR | O_CREAT, 0644);
        ASSERT(fd >= 0);
        ASSERT(flock(fd, LOCK_EX | LOCK_NB) == 0);

        struct fake_ctx c = {0};
        snprintf(c.gen_root, sizeof(c.gen_root), "%s", sb.gen_root);
        struct dev_activation_ops ops;
        fake_ops_init(&ops, &c);
        struct dev_activation_request req;
        base_request(&req, &sb, artifact);
        struct dev_activation_result r;
        int rc = dev_activation_run(&req, &ops, &r);
        ASSERT_EQ(rc, DEV_ACTIVATION_E_LOCK_BUSY);
        ASSERT_EQ(c.stop_calls, 0);   /* never touched the service */

        flock(fd, LOCK_UN);
        close(fd);
        sandbox_exit(&sb);
        PASS();
    } _test_next:;
    return failures;
}

/* Activate gen A once; return its expected generation id in expect_gen. */
static int seed_generation(struct sandbox *sb, char fill, char *artifact,
                           size_t artifact_sz, char *expect_gen,
                           size_t expect_sz)
{
    snprintf(artifact, artifact_sz, "%s/cand_%c", sb->home, fill);
    if (!write_fake_binary(artifact, fill))
        return -1;
    struct fake_ctx c = {0};
    snprintf(c.gen_root, sizeof(c.gen_root), "%s", sb->gen_root);
    struct dev_activation_ops ops;
    fake_ops_init(&ops, &c);
    struct dev_activation_request req;
    base_request(&req, sb, artifact);
    struct dev_activation_result r;
    int rc = dev_activation_run(&req, &ops, &r);
    if (rc == DEV_ACTIVATION_OK)
        snprintf(expect_gen, expect_sz, "%s", r.running_generation);
    return rc;
}

static int test_preflight_fail_untouched(void)
{
    int failures = 0;
    TEST("dev_activation: preflight fail quarantines, leaves current untouched") {
        struct sandbox sb;
        sandbox_enter(&sb, "preflight");
        char artA[PATH_MAX], genA[96];
        ASSERT_EQ(seed_generation(&sb, 'A', artA, sizeof(artA), genA, sizeof(genA)),
                  DEV_ACTIVATION_OK);

        /* Second candidate B whose preflight fails. */
        char artB[PATH_MAX];
        snprintf(artB, sizeof(artB), "%s/cand_B", sb.home);
        ASSERT(write_fake_binary(artB, 'B'));
        struct fake_ctx c = {0};
        snprintf(c.gen_root, sizeof(c.gen_root), "%s", sb.gen_root);
        c.preflight_result = 1;
        struct dev_activation_ops ops;
        fake_ops_init(&ops, &c);
        struct dev_activation_request req;
        base_request(&req, &sb, artB);
        struct dev_activation_result r;
        int rc = dev_activation_run(&req, &ops, &r);
        ASSERT_EQ(rc, DEV_ACTIVATION_E_PREFLIGHT);
        ASSERT_EQ(c.stop_calls, 0);   /* never stopped the running service */

        /* current still points at A */
        char cur[PATH_MAX], link[PATH_MAX];
        snprintf(link, sizeof(link), "%s/current", sb.gen_root);
        ASSERT(read_link_str(link, cur, sizeof(cur)));
        ASSERT_STR_EQ(cur, genA);

        /* B quarantined */
        char reject[PATH_MAX];
        snprintf(reject, sizeof(reject), "%s/rejected/%s.json", sb.gen_root,
                 r.candidate_generation);
        ASSERT(file_exists(reject));

        sandbox_exit(&sb);
        PASS();
    } _test_next:;
    return failures;
}

static int test_start_fail_rolls_back(void)
{
    int failures = 0;
    TEST("dev_activation: candidate start failure rolls back to previous") {
        struct sandbox sb;
        sandbox_enter(&sb, "startfail");
        char artA[PATH_MAX], genA[96];
        ASSERT_EQ(seed_generation(&sb, 'A', artA, sizeof(artA), genA, sizeof(genA)),
                  DEV_ACTIVATION_OK);

        char artB[PATH_MAX];
        snprintf(artB, sizeof(artB), "%s/cand_B", sb.home);
        ASSERT(write_fake_binary(artB, 'B'));
        struct fake_ctx c = {0};
        snprintf(c.gen_root, sizeof(c.gen_root), "%s", sb.gen_root);
        struct dev_activation_ops ops;
        fake_ops_init(&ops, &c);
        struct dev_activation_request req;
        base_request(&req, &sb, artB);
        struct dev_activation_result r;

        /* Fail the start of candidate B (keyed by its generation id); the
         * rollback restart of A then succeeds. */
        {
            extern bool dev_activation_sha256_file(const char *, char[65]);
            char hex[65];
            ASSERT(dev_activation_sha256_file(artB, hex));
            snprintf(c.fail_start_gen, sizeof(c.fail_start_gen), "gen-%s", hex);
        }

        int rc = dev_activation_run(&req, &ops, &r);
        ASSERT_EQ(rc, DEV_ACTIVATION_E_ACTIVATE);
        ASSERT_STR_EQ(r.rollback_status, "verified");

        /* current restored to A, running == A */
        char cur[PATH_MAX], link[PATH_MAX];
        snprintf(link, sizeof(link), "%s/current", sb.gen_root);
        ASSERT(read_link_str(link, cur, sizeof(cur)));
        ASSERT_STR_EQ(cur, genA);
        ASSERT_STR_EQ(r.running_generation, genA);

        /* B quarantined */
        char reject[PATH_MAX];
        snprintf(reject, sizeof(reject), "%s/rejected/%s.json", sb.gen_root,
                 r.candidate_generation);
        ASSERT(file_exists(reject));

        sandbox_exit(&sb);
        PASS();
    } _test_next:;
    return failures;
}

static int test_verify_fail_quarantines(void)
{
    int failures = 0;
    TEST("dev_activation: verify failure quarantines and rolls back") {
        struct sandbox sb;
        sandbox_enter(&sb, "verifyfail");
        char artA[PATH_MAX], genA[96];
        ASSERT_EQ(seed_generation(&sb, 'A', artA, sizeof(artA), genA, sizeof(genA)),
                  DEV_ACTIVATION_OK);

        char artB[PATH_MAX];
        snprintf(artB, sizeof(artB), "%s/cand_B", sb.home);
        ASSERT(write_fake_binary(artB, 'B'));
        struct fake_ctx c = {0};
        snprintf(c.gen_root, sizeof(c.gen_root), "%s", sb.gen_root);
        struct dev_activation_ops ops;
        fake_ops_init(&ops, &c);
        struct dev_activation_request req;
        base_request(&req, &sb, artB);
        struct dev_activation_result r;

        {
            extern bool dev_activation_sha256_file(const char *, char[65]);
            char hex[65];
            ASSERT(dev_activation_sha256_file(artB, hex));
            /* probe fails only for B; the rollback probe of A passes */
            snprintf(c.fail_probe_gen, sizeof(c.fail_probe_gen), "gen-%s", hex);
        }

        int rc = dev_activation_run(&req, &ops, &r);
        ASSERT_EQ(rc, DEV_ACTIVATION_E_ACTIVATE);
        ASSERT_STR_EQ(r.rollback_status, "verified");

        char reject[PATH_MAX];
        snprintf(reject, sizeof(reject), "%s/rejected/%s.json", sb.gen_root,
                 r.candidate_generation);
        ASSERT(file_exists(reject));

        char cur[PATH_MAX], link[PATH_MAX];
        snprintf(link, sizeof(link), "%s/current", sb.gen_root);
        ASSERT(read_link_str(link, cur, sizeof(cur)));
        ASSERT_STR_EQ(cur, genA);

        sandbox_exit(&sb);
        PASS();
    } _test_next:;
    return failures;
}

static int test_mid_transaction_abort(void)
{
    int failures = 0;
    TEST("dev_activation: daemon-reload failure mid-flip restores previous") {
        struct sandbox sb;
        sandbox_enter(&sb, "abort");
        char artA[PATH_MAX], genA[96];
        ASSERT_EQ(seed_generation(&sb, 'A', artA, sizeof(artA), genA, sizeof(genA)),
                  DEV_ACTIVATION_OK);

        char artB[PATH_MAX];
        snprintf(artB, sizeof(artB), "%s/cand_B", sb.home);
        ASSERT(write_fake_binary(artB, 'B'));
        struct fake_ctx c = {0};
        snprintf(c.gen_root, sizeof(c.gen_root), "%s", sb.gen_root);
        struct dev_activation_ops ops;
        fake_ops_init(&ops, &c);
        struct dev_activation_request req;
        base_request(&req, &sb, artB);
        struct dev_activation_result r;

        {
            extern bool dev_activation_sha256_file(const char *, char[65]);
            char hex[65];
            ASSERT(dev_activation_sha256_file(artB, hex));
            /* daemon-reload fails after the flip to B; rollback reload of A ok */
            snprintf(c.fail_reload_gen, sizeof(c.fail_reload_gen), "gen-%s", hex);
        }

        int rc = dev_activation_run(&req, &ops, &r);
        ASSERT_EQ(rc, DEV_ACTIVATION_E_ACTIVATE);
        ASSERT_STR_EQ(r.rollback_status, "verified");
        ASSERT_STR_EQ(r.activation_status, "rolled_back");

        char cur[PATH_MAX], link[PATH_MAX];
        snprintf(link, sizeof(link), "%s/current", sb.gen_root);
        ASSERT(read_link_str(link, cur, sizeof(cur)));
        ASSERT_STR_EQ(cur, genA);

        sandbox_exit(&sb);
        PASS();
    } _test_next:;
    return failures;
}

static int test_activate_generation_by_sha(void)
{
    int failures = 0;
    TEST("dev_activation: activate_generation by sha (revert hook) + missing refusal") {
        struct sandbox sb;
        sandbox_enter(&sb, "bysha");
        char artA[PATH_MAX], genA[96];
        ASSERT_EQ(seed_generation(&sb, 'A', artA, sizeof(artA), genA, sizeof(genA)),
                  DEV_ACTIVATION_OK);

        /* decode A's sha (from the gen id "gen-<hex>") into 32 bytes */
        uint8_t shaA[32];
        ASSERT(hex_to_bytes32(genA + 4, shaA));

        struct fake_ctx c = {0};
        snprintf(c.gen_root, sizeof(c.gen_root), "%s", sb.gen_root);
        struct dev_activation_ops ops;
        fake_ops_init(&ops, &c);
        struct dev_activation_request req;
        base_request(&req, &sb, artA);
        struct dev_activation_result r;

        int rc = dev_activation_activate_generation(shaA, &req, &ops, &r);
        ASSERT_EQ(rc, DEV_ACTIVATION_OK);
        ASSERT_STR_EQ(r.running_generation, genA);
        ASSERT_STR_EQ(r.candidate_generation, genA);

        /* an unknown sha => no staged generation => refusal */
        uint8_t missing[32];
        memset(missing, 0xAB, sizeof(missing));
        struct fake_ctx c2 = {0};
        snprintf(c2.gen_root, sizeof(c2.gen_root), "%s", sb.gen_root);
        struct dev_activation_ops ops2;
        fake_ops_init(&ops2, &c2);
        struct dev_activation_result r2;
        rc = dev_activation_activate_generation(missing, &req, &ops2, &r2);
        ASSERT_EQ(rc, DEV_ACTIVATION_E_STAGE);
        ASSERT_EQ(c2.stop_calls, 0);   /* never touched the running service */

        sandbox_exit(&sb);
        PASS();
    } _test_next:;
    return failures;
}

static int test_auto_reindex_pending_blocks(void)
{
    int failures = 0;
    TEST("dev_activation: pending auto-reindex sentinel blocks before service_stop") {
        struct sandbox sb;
        sandbox_enter(&sb, "reindex_block");
        char artifact[PATH_MAX];
        snprintf(artifact, sizeof(artifact), "%s/cand", sb.home);
        ASSERT(write_fake_binary(artifact, 'A'));
        /* A pending crash-only reindex request (count > 0, not terminal). */
        ASSERT(write_auto_reindex_sentinel(&sb, 3173739, 1));

        struct fake_ctx c = {0};
        snprintf(c.gen_root, sizeof(c.gen_root), "%s", sb.gen_root);
        struct dev_activation_ops ops;
        fake_ops_init(&ops, &c);
        struct dev_activation_request req;
        base_request(&req, &sb, artifact);
        struct dev_activation_result r;

        /* Make sure the override is not leaking in from the environment. */
        unsetenv("ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY");
        int rc = dev_activation_run(&req, &ops, &r);
        ASSERT_EQ(rc, DEV_ACTIVATION_E_AUTO_REINDEX_PENDING);
        ASSERT_EQ(c.stop_calls, 0);       /* never touched the service */
        ASSERT_EQ(c.preflight_calls, 0);  /* refused before the lock/stage */
        ASSERT_STR_EQ(r.activation_status, "refused");
        ASSERT_STR_EQ(r.verify_status, "auto_reindex_pending");

        /* current link never created */
        char link[PATH_MAX];
        snprintf(link, sizeof(link), "%s/current", sb.gen_root);
        char cur[PATH_MAX];
        ASSERT(!read_link_str(link, cur, sizeof(cur)));

        sandbox_exit(&sb);
        PASS();
    } _test_next:;
    return failures;
}

static int test_auto_reindex_override_allows(void)
{
    int failures = 0;
    TEST("dev_activation: ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY=1 forces past the sentinel") {
        struct sandbox sb;
        sandbox_enter(&sb, "reindex_force");
        char artifact[PATH_MAX];
        snprintf(artifact, sizeof(artifact), "%s/cand", sb.home);
        ASSERT(write_fake_binary(artifact, 'A'));
        ASSERT(write_auto_reindex_sentinel(&sb, 3173739, 2));

        struct fake_ctx c = {0};
        snprintf(c.gen_root, sizeof(c.gen_root), "%s", sb.gen_root);
        struct dev_activation_ops ops;
        fake_ops_init(&ops, &c);
        struct dev_activation_request req;
        base_request(&req, &sb, artifact);
        struct dev_activation_result r;

        setenv("ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY", "1", 1);
        int rc = dev_activation_run(&req, &ops, &r);
        unsetenv("ZCL_DEV_ALLOW_AUTO_REINDEX_DEPLOY");
        ASSERT_EQ(rc, DEV_ACTIVATION_OK);
        ASSERT_STR_EQ(r.activation_status, "active");
        ASSERT(c.stop_calls > 0);

        /* deploy-state records the REAL sentinel presence (pending==true). */
        char state[PATH_MAX], js[8192];
        snprintf(state, sizeof(state), "%s/agent-deploy.json", sb.datadir);
        size_t jn = read_file(state, js, sizeof(js));
        ASSERT(jn > 0);
        struct json_value v = {0};
        ASSERT(json_read(&v, js, jn));
        ASSERT(json_get_bool(json_get(&v, "auto_reindex_pending")));
        ASSERT_STR_EQ(json_get_str(json_get(&v, "auto_reindex_anchor")),
                      "3173739");
        ASSERT_STR_EQ(json_get_str(json_get(&v, "auto_reindex_count")), "2");
        json_free(&v);

        sandbox_exit(&sb);
        PASS();
    } _test_next:;
    return failures;
}

static int test_stale_in_progress_refused(void)
{
    int failures = 0;
    TEST("dev_activation: a stale in-progress marker from a dead run is refused") {
        struct sandbox sb;
        sandbox_enter(&sb, "stale_inprog");
        char artA[PATH_MAX], genA[96];
        ASSERT_EQ(seed_generation(&sb, 'A', artA, sizeof(artA), genA, sizeof(genA)),
                  DEV_ACTIVATION_OK);
        /* A healthy activation clears its own marker. */
        ASSERT(!in_progress_marker_exists(&sb));

        /* Plant a marker as if a prior activation crashed mid-flip. */
        ASSERT(write_in_progress_marker(&sb, "gen-deadbeef"));

        char artB[PATH_MAX];
        snprintf(artB, sizeof(artB), "%s/cand_B", sb.home);
        ASSERT(write_fake_binary(artB, 'B'));
        struct fake_ctx c = {0};
        snprintf(c.gen_root, sizeof(c.gen_root), "%s", sb.gen_root);
        struct dev_activation_ops ops;
        fake_ops_init(&ops, &c);
        struct dev_activation_request req;
        base_request(&req, &sb, artB);
        struct dev_activation_result r;

        int rc = dev_activation_run(&req, &ops, &r);
        ASSERT_EQ(rc, DEV_ACTIVATION_E_STALE_IN_PROGRESS);
        ASSERT_EQ(c.stop_calls, 0);       /* refused before touching the lane */
        ASSERT_EQ(c.preflight_calls, 0);
        ASSERT_STR_EQ(r.activation_status, "refused");
        ASSERT_STR_EQ(r.verify_status, "activation_in_progress");
        /* it does NOT auto-roll-back: the marker is left for the operator */
        ASSERT(in_progress_marker_exists(&sb));

        /* current still points at A (untouched) */
        char cur[PATH_MAX], link[PATH_MAX];
        snprintf(link, sizeof(link), "%s/current", sb.gen_root);
        ASSERT(read_link_str(link, cur, sizeof(cur)));
        ASSERT_STR_EQ(cur, genA);

        sandbox_exit(&sb);
        PASS();
    } _test_next:;
    return failures;
}

static int test_in_progress_marker_cleared(void)
{
    int failures = 0;
    TEST("dev_activation: in-progress marker cleared on both success and rollback") {
        struct sandbox sb;
        sandbox_enter(&sb, "inprog_clear");

        /* success path: happy activation must leave no marker behind */
        char artA[PATH_MAX], genA[96];
        ASSERT_EQ(seed_generation(&sb, 'A', artA, sizeof(artA), genA, sizeof(genA)),
                  DEV_ACTIVATION_OK);
        ASSERT(!in_progress_marker_exists(&sb));

        /* rollback path: candidate B start fails, rollback to A verifies, and
         * the marker written at the flip must be cleared again. */
        char artB[PATH_MAX];
        snprintf(artB, sizeof(artB), "%s/cand_B", sb.home);
        ASSERT(write_fake_binary(artB, 'B'));
        struct fake_ctx c = {0};
        snprintf(c.gen_root, sizeof(c.gen_root), "%s", sb.gen_root);
        struct dev_activation_ops ops;
        fake_ops_init(&ops, &c);
        struct dev_activation_request req;
        base_request(&req, &sb, artB);
        struct dev_activation_result r;
        {
            extern bool dev_activation_sha256_file(const char *, char[65]);
            char hex[65];
            ASSERT(dev_activation_sha256_file(artB, hex));
            snprintf(c.fail_start_gen, sizeof(c.fail_start_gen), "gen-%s", hex);
        }
        int rc = dev_activation_run(&req, &ops, &r);
        ASSERT_EQ(rc, DEV_ACTIVATION_E_ACTIVATE);
        ASSERT_STR_EQ(r.rollback_status, "verified");
        ASSERT(!in_progress_marker_exists(&sb));  /* cleared on rollback */

        sandbox_exit(&sb);
        PASS();
    } _test_next:;
    return failures;
}

int test_dev_activation(void)
{
    int failures = 0;
    failures += test_happy_activation();
    failures += test_confinement_refusals();
    failures += test_lock_busy();
    failures += test_preflight_fail_untouched();
    failures += test_start_fail_rolls_back();
    failures += test_verify_fail_quarantines();
    failures += test_mid_transaction_abort();
    failures += test_activate_generation_by_sha();
    failures += test_auto_reindex_pending_blocks();
    failures += test_auto_reindex_override_allows();
    failures += test_stale_in_progress_refused();
    failures += test_in_progress_marker_cleared();
    printf("=== dev_activation: %d failures ===\n", failures);
    return failures;
}
