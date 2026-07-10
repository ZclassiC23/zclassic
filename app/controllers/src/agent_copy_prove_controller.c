/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * agentcopyprove — native agent contract wrapping tools/repro_on_copy.sh
 * (the existing copy-prove H*-CLIMB harness, docs/CODEBASE_MAP.md /
 * CLAUDE.md "Copy-prove before live"). This controller NEVER reimplements
 * the harness — it validates+quotes agent-supplied parameters, launches
 * the script detached, and lets a caller poll progress through the
 * diagnostics registry (`zcl_state subsystem=agent_copy_prove`) instead
 * of holding an RPC/MCP worker thread for the run duration.
 *
 * Sync-vs-async: a real copy-prove run boots a full node and watches its
 * tip for `--deadline` seconds (default 180s, up to the 3600s cap this
 * contract enforces). The MCP dispatcher's default per-call budget is 5s,
 * and even the existing `k_long_running_tools` override table
 * (tools/mcp/middleware.c) caps out at 12s — nowhere near enough headroom
 * for a multi-minute proof. So this contract is asynchronous by design:
 * it returns as soon as the run is launched, and callers poll the
 * diagnostics-registry primitive (see CLAUDE.md "Adding state
 * introspection") for the result. `lib/util/src/long_op.c` was
 * considered and is NOT the right fit here — it is an in-process
 * heartbeat for the sync *reducer* watchdog, not a way to track a spawned
 * child process's completion from an RPC handler.
 *
 * Safety invariants (enforced in code, not comments):
 *   - this contract's parameter surface has NO "dest" field — the copy
 *     destination is always chosen by repro_on_copy.sh itself via its
 *     fixed <HOME>/.zclassic-c23-COPY-<timestamp>-<slug> formula, so a
 *     live-datadir alias is structurally unreachable through this API.
 *   - every poll of a result (agent_copy_prove_dump_state_json) re-checks
 *     that any copy_path it is about to report carries the throwaway
 *     "-COPY-" marker and does not equal a known live datadir, and
 *     refuses to affirm the result otherwise (see cp_path_safety_ok()).
 *   - slug/src/args are allowlist-validated before ever being embedded in
 *     a shell command line; no shell metacharacter can survive the
 *     allowlists, and every value is additionally single-quoted before
 *     being placed in the launched command as defense in depth.
 */

#include "controllers/agent_copy_prove_controller.h"
#include "controllers/agent_controller.h"
#include "controllers/strong_params.h"

#include "json/json.h"
#include "platform/time_compat.h"
#include "util/safe_alloc.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define COPY_PROVE_CONTRACT_SCHEMA "zcl.agent_copy_prove.v1"
#define COPY_PROVE_RESULT_SCHEMA   "zcl.copy_prove_result.v1"
#define COPY_PROVE_STATE_SCHEMA    "zcl.agent_copy_prove_state.v1"

/* ── input allowlists ──────────────────────────────────────────────
 *
 * These are the ONLY gate between agent-supplied strings and a shell
 * command line (see cp_launch_command()). Keep them conservative: no
 * quote, backtick, dollar, semicolon, pipe, ampersand, or whitespace
 * inside a single token.
 */

static bool cp_slug_valid(const char *s)
{
    if (!s || !s[0]) return false;
    size_t n = strlen(s);
    if (n > 64) return false;
    unsigned char first = (unsigned char)s[0];
    if (!(islower(first) || isdigit(first))) return false;
    if (s[n - 1] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(islower(c) || isdigit(c) || c == '-')) return false;
    }
    return true;
}

/* Empty is valid: it means "use the script's own default source". */
static bool cp_path_valid(const char *s)
{
    if (!s || !s[0]) return true;
    size_t n = strlen(s);
    if (n > 900 || s[0] != '/') return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '_' || c == '.' || c == '/' || c == '-'))
            return false;
    }
    return true;
}

/* Space-separated node flags, e.g. "-nobgvalidation -foo=bar". Every
 * token must start with '-' and contain only a conservative charset. */
static bool cp_args_valid(const char *s)
{
    if (!s || !s[0]) return true;
    if (strlen(s) > 2000) return false;
    const char *p = s;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p != '-') return false;
        while (*p && *p != ' ') {
            unsigned char c = (unsigned char)*p;
            if (!(isalnum(c) || c == '-' || c == '_' || c == '.' ||
                  c == ':' || c == '=' || c == ',' || c == '/'))
                return false;
            p++;
        }
    }
    return true;
}

/* ── copy-target safety invariant ──────────────────────────────────
 *
 * Never let this contract report success for (or, in principle, target)
 * a path that could be a live datadir. See file header. */

static bool cp_path_is_live_datadir(const char *path)
{
    if (!path || !path[0]) return false;
    const char *home = getenv("HOME");
    char buf[1200];
    if (home && home[0]) {
        snprintf(buf, sizeof(buf), "%s/.zclassic-c23", home);
        if (strcmp(path, buf) == 0) return true;
        snprintf(buf, sizeof(buf), "%s/.zclassic", home);
        if (strcmp(path, buf) == 0) return true;
    }
    const char *ctx = agent_runtime_context_datadir();
    if (ctx && ctx[0] && strcmp(path, ctx) == 0) return true;
    return false;
}

/* Every throwaway copy repro_on_copy.sh creates carries this marker in
 * its path: $HOME/.zclassic-c23-COPY-<timestamp>-<slug>. */
static bool cp_path_is_copy_marked(const char *path)
{
    return path && strstr(path, "/.zclassic-c23-COPY-") != NULL;
}

static bool cp_path_safety_ok(const char *path)
{
    return path && path[0] && cp_path_is_copy_marked(path) &&
           !cp_path_is_live_datadir(path);
}

/* ── status dir / file resolution ──────────────────────────────────
 *
 * ZCL_COPY_PROVE_STATUS_DIR lets tests point this at an isolated temp
 * dir; production default lives beside the *-COPY-* convention under
 * $HOME so it is obviously related and never collides with a real
 * datadir name. */

static void cp_status_dir(char *out, size_t out_sz)
{
    const char *override = getenv("ZCL_COPY_PROVE_STATUS_DIR");
    if (override && override[0]) {
        snprintf(out, out_sz, "%s", override);
        return;
    }
    const char *home = getenv("HOME");
    snprintf(out, out_sz, "%s/.zclassic-c23-copyprove-status",
             home && home[0] ? home : "/tmp");
}

static void cp_status_file(const char *slug, char *out, size_t out_sz)
{
    char dir[900];
    cp_status_dir(dir, sizeof(dir));
    snprintf(out, out_sz, "%s/%s.json", dir, slug);
}

static void cp_mkdir_p(const char *path)
{
    char tmp[900];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
}

/* ── shell quoting (defense in depth over the allowlists above) ───── */

static void cp_shell_squote(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    if (out_sz == 0) return;
    out[o++] = '\'';
    for (const char *p = in; p && *p && o + 5 < out_sz; p++) {
        if (*p == '\'') {
            out[o++] = '\'';
            out[o++] = '\\';
            out[o++] = '\'';
            out[o++] = '\'';
        } else {
            out[o++] = *p;
        }
    }
    if (o + 1 < out_sz) out[o++] = '\'';
    out[o] = '\0';
}

/* ── synchronous "queued" pre-write ─────────────────────────────────
 *
 * Closes the staleness window between "RPC accepted the request" and
 * "the launched script writes its first --status-file row": without
 * this, a poller landing in that gap would see a PREVIOUS run's
 * leftover "done" row and could mistake it for the current one. */

static bool cp_write_queued_status(const char *status_file, const char *slug,
                                   const char *src_resolved)
{
    struct json_value obj;
    json_init(&obj);
    json_set_object(&obj);
    json_push_kv_str(&obj, "schema", COPY_PROVE_RESULT_SCHEMA);
    json_push_kv_str(&obj, "state", "queued");
    json_push_kv_str(&obj, "slug", slug);
    json_push_kv_str(&obj, "src", src_resolved ? src_resolved : "");
    json_push_kv_int(&obj, "queued_at", platform_time_wall_unix());

    size_t need = json_write(&obj, NULL, 0) + 1;
    char *buf = zcl_malloc(need, "agent_copy_prove_queued_json");
    bool ok = false;
    if (buf) {
        json_write(&obj, buf, need);
        char tmp_path[1200];
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", status_file);
        FILE *f = fopen(tmp_path, "w");
        if (f) {
            fputs(buf, f);
            fputc('\n', f);
            fclose(f);
            ok = (rename(tmp_path, status_file) == 0);
            if (!ok)
                // obs-ok:agent-copy-prove-diagnostic-stderr (best-effort status telemetry / request refusal returns JSON error)
                fprintf(stderr, "[agent_copy_prove] %s:%d %s(): rename "
                        "%s -> %s failed: %s\n", __FILE__, __LINE__,
                        __func__, tmp_path, status_file, strerror(errno));
        } else {
            // obs-ok:agent-copy-prove-diagnostic-stderr (best-effort status telemetry / request refusal returns JSON error)
            fprintf(stderr, "[agent_copy_prove] %s:%d %s(): fopen %s "
                    "failed: %s\n", __FILE__, __LINE__, __func__,
                    tmp_path, strerror(errno));
        }
        free(buf);
    } else {
        // obs-ok:agent-copy-prove-diagnostic-stderr (best-effort status telemetry / request refusal returns JSON error)
        fprintf(stderr, "[agent_copy_prove] %s:%d %s(): zcl_malloc(%zu) "
                "failed for slug=%s\n", __FILE__, __LINE__, __func__,
                need, slug ? slug : "(null)");
    }
    json_free(&obj);
    return ok;
}

/* ── RPC: agentcopyprove ─────────────────────────────────────────── */

bool rpc_agent_copy_prove(const struct json_value *params, bool help,
                          struct json_value *result)
{
    RPC_HELP(help, result,
        "agentcopyprove ( slug src args expect_climb_past deadline_secs full no_run )\n"
        "\nKick off tools/repro_on_copy.sh in the background against a\n"
        "throwaway datadir COPY and return immediately without blocking\n"
        "for the run. There is no 'dest' parameter — the copy target is\n"
        "always <HOME>/.zclassic-c23-COPY-<timestamp>-<slug>, chosen by\n"
        "the script itself, never a live datadir.\n"
        "\nParams:\n"
        "  slug (string, required)   lowercase alnum + '-', <= 64 chars\n"
        "  src (string, optional)    source datadir to copy FROM; empty\n"
        "                            uses the script's own default\n"
        "  args (string, optional)   space-separated extra node flags\n"
        "                            passed through to the copy's node\n"
        "  expect_climb_past (int, optional)  H* CLIMB gate height\n"
        "  deadline_secs (int, optional, default 180, clamped 1..3600)\n"
        "  full (bool, optional, default false)     --full vs --light\n"
        "  no_run (bool, optional, default false)    --no-run\n"
        "\nPoll a run with: zclassic23 dumpstate agent_copy_prove <slug>\n"
        "or MCP zcl_state subsystem=agent_copy_prove key=<slug>. This\n"
        "call never blocks for the run duration — see cp_launch above.\n"
        "\nResult:\n"
        "  { \"schema\":\"zcl.agent_copy_prove.v1\", \"status\":\"started\", ... }\n");

    struct rpc_params p;
    rpc_params_init(&p, params);
    const char *slug = rpc_require_str(&p, 0, "slug");
    const char *src = rpc_permit_str(&p, 1, "src", "");
    const char *args = rpc_permit_str(&p, 2, "args", "");
    int64_t expect_climb_past = rpc_permit_int(&p, 3, "expect_climb_past", -1);
    int64_t deadline_secs = rpc_permit_int(&p, 4, "deadline_secs", 180);
    bool full = rpc_permit_bool(&p, 5, "full", false);
    bool no_run = rpc_permit_bool(&p, 6, "no_run", false);
    if (rpc_params_invalid(&p)) {
        rpc_params_error(&p, result);
        return false;
    }

    json_set_object(result);
    json_push_kv_str(result, "schema", COPY_PROVE_CONTRACT_SCHEMA);
    json_push_kv_str(result, "api_version", "v1");
    json_push_kv_str(result, "native_command", "zclassic23 agentcopyprove");
    json_push_kv_str(result, "mcp_tool", "zcl_agent_copy_prove");

    if (!cp_slug_valid(slug)) {
        json_push_kv_str(result, "status", "error");
        json_push_kv_str(result, "error", "invalid_slug");
        json_push_kv_str(result, "detail",
            "slug must match ^[a-z0-9][a-z0-9-]{0,63}$ and not end with '-'");
        // obs-ok:agent-copy-prove-diagnostic-stderr (best-effort status telemetry / request refusal returns JSON error)
        fprintf(stderr, "[agent_copy_prove] %s:%d %s(): refused invalid "
                "slug\n", __FILE__, __LINE__, __func__);
        return true;
    }
    if (!cp_path_valid(src)) {
        json_push_kv_str(result, "status", "error");
        json_push_kv_str(result, "error", "invalid_src");
        json_push_kv_str(result, "detail",
            "src must be an absolute path using only [A-Za-z0-9_./-]");
        // obs-ok:agent-copy-prove-diagnostic-stderr (best-effort status telemetry / request refusal returns JSON error)
        fprintf(stderr, "[agent_copy_prove] %s:%d %s(): refused invalid "
                "src for slug=%s\n", __FILE__, __LINE__, __func__, slug);
        return true;
    }
    if (!cp_args_valid(args)) {
        json_push_kv_str(result, "status", "error");
        json_push_kv_str(result, "error", "invalid_args");
        json_push_kv_str(result, "detail",
            "args must be space-separated flags starting with '-' using "
            "only [A-Za-z0-9_.:=,/-]");
        // obs-ok:agent-copy-prove-diagnostic-stderr (best-effort status telemetry / request refusal returns JSON error)
        fprintf(stderr, "[agent_copy_prove] %s:%d %s(): refused invalid "
                "args for slug=%s\n", __FILE__, __LINE__, __func__, slug);
        return true;
    }
    if (expect_climb_past < -1) {
        json_push_kv_str(result, "status", "error");
        json_push_kv_str(result, "error", "invalid_expect_climb_past");
        // obs-ok:agent-copy-prove-diagnostic-stderr (best-effort status telemetry / request refusal returns JSON error)
        fprintf(stderr, "[agent_copy_prove] %s:%d %s(): refused negative "
                "expect_climb_past for slug=%s\n", __FILE__, __LINE__,
                __func__, slug);
        return true;
    }
    if (deadline_secs < 1) deadline_secs = 1;
    /* Detached, so this cap is not an RPC-thread budget — it bounds how
     * long an unattended background run keeps a node process + disk
     * copy alive before the caller is expected to have polled it. */
    if (deadline_secs > 3600) deadline_secs = 3600;

    char status_dir[900];
    cp_status_dir(status_dir, sizeof(status_dir));
    cp_mkdir_p(status_dir);
    char status_file[1100];
    cp_status_file(slug, status_file, sizeof(status_file));

    if (!cp_write_queued_status(status_file, slug, src)) {
        json_push_kv_str(result, "status", "error");
        json_push_kv_str(result, "error", "status_file_write_failed");
        json_push_kv_str(result, "status_file", status_file);
        // obs-ok:agent-copy-prove-diagnostic-stderr (best-effort status telemetry / request refusal returns JSON error)
        fprintf(stderr, "[agent_copy_prove] %s:%d %s(): could not write "
                "queued status for slug=%s at %s\n", __FILE__, __LINE__,
                __func__, slug, status_file);
        return true;
    }

    const char *script = getenv("ZCL_AGENT_COPY_PROVE_SCRIPT");
    if (!script || !script[0]) script = "tools/repro_on_copy.sh";
    if (access(script, X_OK) != 0) {
        json_push_kv_str(result, "status", "error");
        json_push_kv_str(result, "error", "script_not_found");
        json_push_kv_str(result, "detail", script);
        // obs-ok:agent-copy-prove-diagnostic-stderr (best-effort status telemetry / request refusal returns JSON error)
        fprintf(stderr, "[agent_copy_prove] %s:%d %s(): script not "
                "executable: %s (%s)\n", __FILE__, __LINE__, __func__,
                script, strerror(errno));
        return true;
    }

    char q_slug[160], q_status[1200], q_src[1200], q_launch_log[1300];
    cp_shell_squote(slug, q_slug, sizeof(q_slug));
    cp_shell_squote(status_file, q_status, sizeof(q_status));
    char launch_log[1200];
    snprintf(launch_log, sizeof(launch_log), "%s.launch.log", status_file);
    cp_shell_squote(launch_log, q_launch_log, sizeof(q_launch_log));

    char cmd[6000];
    int n = snprintf(cmd, sizeof(cmd),
        "nohup %s %s --status-file=%s --deadline=%lld%s%s",
        script, q_slug, q_status, (long long)deadline_secs,
        full ? " --full" : "", no_run ? " --no-run" : "");
    if (n > 0 && src && src[0] && (size_t)n < sizeof(cmd)) {
        cp_shell_squote(src, q_src, sizeof(q_src));
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " --src=%s", q_src);
    }
    if (n > 0 && expect_climb_past >= 0 && (size_t)n < sizeof(cmd)) {
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n,
                      " --expect-climb-past=%lld",
                      (long long)expect_climb_past);
    }
    if (n > 0 && args && args[0] && (size_t)n < sizeof(cmd)) {
        /* args tokens are allowlist-validated above (alnum/-_.:=,/ only,
         * each starting with '-') so no shell metacharacter can be
         * present; they are safe to append unquoted as argv words. */
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n, " -- %s", args);
    }
    if (n > 0 && (size_t)n < sizeof(cmd)) {
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n,
                      " > %s 2>&1 < /dev/null &", q_launch_log);
    }
    if (n < 0 || (size_t)n >= sizeof(cmd)) {
        json_push_kv_str(result, "status", "error");
        json_push_kv_str(result, "error", "command_too_long");
        // obs-ok:agent-copy-prove-diagnostic-stderr (best-effort status telemetry / request refusal returns JSON error)
        fprintf(stderr, "[agent_copy_prove] %s:%d %s(): command exceeded "
                "%zu bytes for slug=%s\n", __FILE__, __LINE__, __func__,
                sizeof(cmd), slug);
        return true;
    }

    int rc = system(cmd);
    if (rc != 0) {
        json_push_kv_str(result, "status", "error");
        json_push_kv_str(result, "error", "spawn_failed");
        json_push_kv_int(result, "spawn_exit_code", rc);
        // obs-ok:agent-copy-prove-diagnostic-stderr (best-effort status telemetry / request refusal returns JSON error)
        fprintf(stderr, "[agent_copy_prove] %s:%d %s(): system() launch "
                "failed rc=%d for slug=%s cmd=%s\n", __FILE__, __LINE__,
                __func__, rc, slug, cmd);
        return true;
    }

    json_push_kv_str(result, "status", "started");
    json_push_kv_bool(result, "async", true);
    json_push_kv_str(result, "slug", slug);
    json_push_kv_str(result, "status_file", status_file);
    json_push_kv_str(result, "launch_log", launch_log);
    json_push_kv_str(result, "poll_native",
                     "zclassic23 dumpstate agent_copy_prove");
    json_push_kv_str(result, "poll_mcp",
                     "zcl_state subsystem=agent_copy_prove");
    json_push_kv_int(result, "deadline_secs", deadline_secs);
    json_push_kv_str(result, "budget_note",
        "detached background run; does not hold an RPC/MCP worker thread. "
        "Poll status_file via dumpstate/zcl_state instead of waiting on "
        "this call.");
    return true;
}

/* ── diagnostics registry: subsystem=agent_copy_prove ────────────────
 *
 * See CLAUDE.md "Adding state introspection". Reentrant-safe: reads a
 * file, allocates nothing the caller doesn't own, takes no lock. */

bool agent_copy_prove_dump_state_json(struct json_value *out, const char *key)
{
    if (!out) return false;
    json_set_object(out);
    json_push_kv_str(out, "schema", COPY_PROVE_STATE_SCHEMA);

    char status_dir[900];
    cp_status_dir(status_dir, sizeof(status_dir));

    if (!key || !key[0]) {
        json_push_kv_str(out, "status", "error");
        json_push_kv_str(out, "error", "missing_slug_key");
        json_push_kv_str(out, "detail",
            "pass the slug as the dumpstate key, e.g. "
            "dumpstate agent_copy_prove <slug>");
        json_push_kv_str(out, "status_dir", status_dir);
        return true;
    }
    if (!cp_slug_valid(key)) {
        json_push_kv_str(out, "status", "error");
        json_push_kv_str(out, "error", "invalid_slug_key");
        return true;
    }

    char status_file[1100];
    cp_status_file(key, status_file, sizeof(status_file));

    FILE *f = fopen(status_file, "r");
    if (!f) {
        json_push_kv_str(out, "status", "not_found");
        json_push_kv_str(out, "slug", key);
        json_push_kv_str(out, "status_file", status_file);
        return true;
    }
    char buf[16384];
    size_t used = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[used] = '\0';

    struct json_value parsed;
    json_init(&parsed);
    if (!json_read(&parsed, buf, used) || parsed.type != JSON_OBJ) {
        json_free(&parsed);
        json_push_kv_str(out, "status", "error");
        json_push_kv_str(out, "error", "status_file_invalid_json");
        json_push_kv_str(out, "slug", key);
        json_push_kv_str(out, "status_file", status_file);
        // obs-ok:agent-copy-prove-diagnostic-stderr (best-effort status telemetry / request refusal returns JSON error)
        fprintf(stderr, "[agent_copy_prove] %s:%d %s(): status file did "
                "not parse as a JSON object: %s\n", __FILE__, __LINE__,
                __func__, status_file);
        return true;
    }

    /* Re-check the copy-target safety invariant on every read (see file
     * header). A "queued" row has no copy_path yet — that is expected,
     * not a violation. */
    const char *copy_path = json_get_str(json_get(&parsed, "copy_path"));
    if (copy_path && copy_path[0] && !cp_path_safety_ok(copy_path)) {
        /* Log BEFORE freeing parsed — copy_path points into it. */
        // obs-ok:agent-copy-prove-diagnostic-stderr (best-effort status telemetry / request refusal returns JSON error)
        fprintf(stderr, "[agent_copy_prove] %s:%d %s(): SAFETY: refusing "
                "to report status for slug=%s, copy_path=%s aliases a "
                "live datadir or lacks the throwaway marker\n", __FILE__,
                __LINE__, __func__, key, copy_path);
        json_free(&parsed);
        json_push_kv_str(out, "status", "error");
        json_push_kv_str(out, "error", "safety_invariant_violated");
        json_push_kv_str(out, "detail",
            "status file copy_path is not a throwaway *-COPY-* path or "
            "aliases a known live datadir; refusing to report this result");
        json_push_kv_str(out, "slug", key);
        return true;
    }

    json_push_kv_str(out, "status", "ok");
    json_push_kv_str(out, "slug", key);
    json_push_kv_str(out, "status_file", status_file);
    json_push_kv(out, "result", &parsed);
    json_free(&parsed);
    return true;
}
