/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * The `--metaverse-broker` argv mode: the operator-side entry point that stands
 * the whole boundary up in one process, so it can be driven by a test, a demo,
 * or a service unit without any of them re-implementing the choreography.
 *
 * THE ORDER OF THE FIRST THREE STEPS IS THE SECURITY PROPERTY, not a style
 * choice — see the ordering note atop agent_broker.c:
 *
 *   spawn the confined child   <- the child's address space is COW-copied HERE
 *   open the audit log         <- mints the signing key, AFTER the copy
 *   build the grant            <- the authority, AFTER the copy
 *
 * Moving either of the last two above the spawn would put a secret into the
 * child's inherited image. execve() then discards that image anyway, but the
 * ordering is what makes the property hold without depending on it.
 */

#define _GNU_SOURCE

#include "session/agent_broker.h"

#include "base/log_macros.h"
#include "json/json.h"
#include "platform/os_sandbox.h"
#include "platform/rng.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MODES_TAG "agent.broker.mode"

static bool ensure_dir(const char *path)
{
    if (mkdir(path, 0700) == 0 || errno == EEXIST)
        return true;
    LOG_FAIL(MODES_TAG, "mkdir %s failed: %s", path, strerror(errno));
}

static const char *arg_value(int argc, char **argv, const char *prefix)
{
    size_t n = strlen(prefix);
    for (int i = 1; i < argc; i++)
        if (strncmp(argv[i], prefix, n) == 0)
            return argv[i] + n;
    return NULL;
}

static bool arg_present(int argc, char **argv, const char *flag)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], flag) == 0)
            return true;
    return false;
}

/* The child writes its ACHIEVED posture into its own scratch directory; the
 * broker reads it back. A hostile child could of course lie here — which is
 * why the fields that matter to the operator (Landlock ABI, whether the domain
 * was built at all) are the BROKER's own observations, recorded below, and the
 * child's self-report is only used for what only it can see (its own uid, and
 * whether its stage-2 filter landed). */
static void read_child_report(const char *scratch_dir,
                              struct agent_confine_report *out)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/agent_report.json", scratch_dir);
    FILE *f = fopen(path, "re");
    if (!f)
        return;
    char buf[8192];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    (void)fclose(f);
    if (got == 0)
        return;
    buf[got] = '\0';

    struct json_value v;
    json_init(&v);
    if (json_read(&v, buf, got)) {
        out->ran_as_uid = (uid_t)json_get_int(json_get(&v, "uid"));
        out->ran_as_gid = (gid_t)json_get_int(json_get(&v, "gid"));
        out->landlock_applied = json_get_bool(json_get(&v, "landlock_applied"));
        out->seccomp_applied  = json_get_bool(json_get(&v, "seccomp_applied"));
        out->rlimits_applied  = json_get_bool(json_get(&v, "rlimits_applied"));
        const char *m = json_get_str(json_get(&v, "seccomp_method"));
        if (m)
            snprintf(out->seccomp_method, sizeof(out->seccomp_method), "%s", m);
        const char *p = json_get_str(json_get(&v, "uid_posture"));
        if (p && strcmp(p, "separate_uid") == 0)
            out->uid_posture = AGENT_CONFINE_SEPARATE_UID;
        else if (p && strcmp(p, "same_uid") == 0)
            out->uid_posture = AGENT_CONFINE_SAME_UID;
    }
    json_free(&v);
}

/* The demo grant: INSPECT/LIST/HOST/PUBLISH_REVISION/SELL over exactly ONE
 * fixture property. Deliberately narrow — the hostile script's attempts to
 * touch the OTHER property and to TRANSFER are refusals this grant produces by
 * construction, not by a special case in the broker. */
static void build_demo_grant(struct agent_grant *g)
{
    memset(g, 0, sizeof(*g));
    uint8_t r[8];
    if (!rng_fill(r, sizeof(r)))
        memset(r, 0, sizeof(r));
    static const char hexd[] = "0123456789abcdef";
    char id[17];
    for (size_t i = 0; i < 8; i++) {
        id[2 * i]     = hexd[(r[i] >> 4) & 0xF];
        id[2 * i + 1] = hexd[r[i] & 0xF];
    }
    id[16] = '\0';
    snprintf(g->grant_id, sizeof(g->grant_id), "%s", id);
    snprintf(g->principal, sizeof(g->principal), "confined-seller-agent");

    uint8_t prop[MVAP_PROPERTY_ID_LEN];
    agent_broker_fixture_property_id(0, prop);
    (void)agent_grant_add_property(g, prop);

    agent_grant_allow_action(g, MVAP_VERB_INSPECT);
    agent_grant_allow_action(g, MVAP_VERB_LIST);
    agent_grant_allow_action(g, MVAP_VERB_HOST);
    agent_grant_allow_action(g, MVAP_VERB_PUBLISH_REVISION);
    agent_grant_allow_action(g, MVAP_VERB_SELL);
    agent_grant_allow_kind(g, MVAP_KIND_CONTENT);
    agent_grant_allow_kind(g, MVAP_KIND_ANY);

    g->max_value_zats       = 100000000;
    g->budget_zats          = 500000000;
    g->rate_limit           = 64;
    g->window_seconds       = 60;
    g->may_delegate         = false;
    g->max_delegation_depth = 0;
}

int agent_broker_mode_main(int argc, char **argv)
{
    const char *dir     = arg_value(argc, argv, "--broker-dir=");
    const char *script  = arg_value(argc, argv, "--script=");
    const char *canary  = arg_value(argc, argv, "--canary=");
    const char *reqs    = arg_value(argc, argv, "--requests=");
    const char *euid_s  = arg_value(argc, argv, "--expect-uid=");
    const char *auid_s  = arg_value(argc, argv, "--agent-uid=");
    const char *revoke_s = arg_value(argc, argv, "--revoke-after=");
    bool listen_mode    = arg_present(argc, argv, "--listen");

    if (!dir || !dir[0]) {
        (void)fprintf(stderr,
            "usage: zclassic23 --metaverse-broker --broker-dir=DIR "
            "[--script=NAME] [--canary=PATH] [--requests=N] [--listen] "
            "[--expect-uid=N] [--agent-uid=N] [--revoke-after=N]\n");
        return 2;
    }
    if (!script || !script[0])
        script = "inspect";
    if (!canary)
        canary = "";

    char scratch[448];
    snprintf(scratch, sizeof(scratch), "%s/agent-scratch", dir);
    if (!ensure_dir(dir) || !ensure_dir(scratch))
        return 3;

    char self[PATH_MAX];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof(self) - 1);  // platform-ok:the broker must exec ITSELF; there is no other way to name this exact binary
    if (sl <= 0) {
        (void)fprintf(stderr, "broker: cannot resolve my own executable\n");
        return 3;
    }
    self[sl] = '\0';

    /* The Landlock ABI is recorded HERE, in the broker, because the confined
     * child cannot probe it without being killed by its own filter. */
    int landlock_abi = os_sandbox_landlock_abi();

    /* ── 1. spawn FIRST (nothing secret exists yet) ─────────────────────── */
    struct agent_spawn_request sreq = {
        .self_exe     = self,
        .scratch_dir  = scratch,
        .script       = script,
        .confined_uid = auid_s ? (uid_t)strtoul(auid_s, NULL, 10) : 0,
        .confined_gid = auid_s ? (gid_t)strtoul(auid_s, NULL, 10) : 0,
    };
    struct agent_spawn_result spawned;
    if (listen_mode) {
        memset(&spawned, 0, sizeof(spawned));
        spawned.sock = -1;
    } else if (!agent_broker_spawn_confined(&sreq, &spawned)) {
        (void)fprintf(stderr, "broker: could not spawn the confined agent\n");
        return 4;
    }

    /* ── 2. audit log, then 3. the grant — both AFTER the fork ──────────── */
    struct agent_audit_log audit;
    if (!agent_audit_open(&audit, dir)) {
        (void)fprintf(stderr, "broker: could not open the audit log in %s\n",
                      dir);
        return 5;
    }
    struct agent_grant grant;
    build_demo_grant(&grant);

    struct agent_broker_session s;
    memset(&s, 0, sizeof(s));
    s.grant = grant;
    s.ops   = agent_broker_fixture_ops();
    s.audit = &audit;
    s.child.landlock_abi = landlock_abi;

    int served = 0;
    char sockpath[512] = { 0 };

    if (listen_mode) {
        /* The listening surface: any local process can reach it, which is
         * exactly why the credential check is the first thing that runs. */
        snprintf(sockpath, sizeof(sockpath), "%s/agent.sock", dir);
        s.expect.require_uid = true;
        s.expect.uid = euid_s ? (uid_t)strtoul(euid_s, NULL, 10) : getuid();

        int lfd = agent_broker_listen(sockpath);
        if (lfd < 0) {
            (void)fprintf(stderr, "broker: cannot listen on %s\n", sockpath);
            return 6;
        }
        printf("broker: listening on %s expecting uid=%u\n", sockpath,
               (unsigned)s.expect.uid);
        (void)fflush(stdout);
        int r = agent_broker_accept_once(&s, lfd, 15000);
        served = r > 0 ? 1 : 0;
        (void)close(lfd);
        (void)unlink(sockpath);
    } else {
        /* The socketpair surface: the peer must be the EXACT process we
         * spawned. pid plus uid, not uid alone — on a host where no uid switch
         * was possible, uid alone would admit any process of the operator. */
        s.expect.require_pid = true;
        s.expect.pid = spawned.pid;
        s.expect.require_uid = true;
        s.expect.uid = auid_s ? (uid_t)strtoul(auid_s, NULL, 10) : getuid();

        uint64_t max = reqs ? strtoull(reqs, NULL, 10) : 0;
        uint64_t revoke_after = revoke_s ? strtoull(revoke_s, NULL, 10) : 0;

        if (!agent_broker_peercred(spawned.sock, &s.peer)) {
            (void)fprintf(stderr, "broker: no peer credentials on the pair\n");
            return 7;
        }
        char why[160];
        if (!agent_broker_peer_authorized(&s.peer, &s.expect, why,
                                          sizeof(why))) {
            (void)fprintf(stderr, "broker: refusing my own child: %s\n", why);
            return 8;
        }
        printf("broker: peer verified pid=%d uid=%u gid=%u\n", (int)s.peer.pid,
               (unsigned)s.peer.uid, (unsigned)s.peer.gid);
        (void)fflush(stdout);

        for (;;) {
            /* The revocation half of the vertical slice: after N served
             * requests the grant is revoked in place, and every later action
             * the agent attempts is refused with DENIED_REVOKED. */
            if (revoke_after && (uint64_t)served == revoke_after &&
                !s.grant.revoked) {
                s.grant.revoked = true;
                s.grant.revocation_generation++;
                printf("broker: grant %s REVOKED after %d request(s)\n",
                       s.grant.grant_id, served);
                (void)fflush(stdout);
            }
            int r = agent_broker_serve_once(&s, spawned.sock);
            if (r <= 0)
                break;
            served++;
            if (max && (uint64_t)served >= max)
                break;
        }
        (void)close(spawned.sock);
    }

    if (spawned.pid > 0) {
        read_child_report(scratch, &s.child);
        s.child.landlock_abi = landlock_abi;
        int st = 0;
        if (waitpid(spawned.pid, &st, 0) == spawned.pid) {
            if (WIFSIGNALED(st))
                printf("broker: agent pid=%d killed by signal %d\n",
                       (int)spawned.pid, WTERMSIG(st));
            else
                printf("broker: agent pid=%d exited %d\n", (int)spawned.pid,
                       WEXITSTATUS(st));
        }
    }

    agent_broker_write_status(dir, &s, &s.grant, spawned.pid,
                              sockpath[0] ? sockpath : NULL);

    struct agent_audit_verdict v;
    if (agent_audit_verify_dir(dir, &v))
        printf("broker: served=%d denied=%llu receipts=%llu "
               "audit_rows=%llu chain_breaks=%llu bad_sigs=%llu ok=%s\n",
               served, (unsigned long long)s.requests_denied,
               (unsigned long long)s.receipts_written,
               (unsigned long long)v.rows,
               (unsigned long long)v.chain_breaks,
               (unsigned long long)v.bad_signatures, v.ok ? "yes" : "no");
    else
        printf("broker: served=%d denied=%llu receipts=%llu (no audit rows)\n",
               served, (unsigned long long)s.requests_denied,
               (unsigned long long)s.receipts_written);
    (void)fflush(stdout);
    return 0;
}
