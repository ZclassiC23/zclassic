/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * What the operator reads afterwards: `<dir>/broker.json`, written by the
 * broker and rendered back by `metaverse agent status`.
 *
 * The document reports what the confinement ACHIEVED, never what was asked for
 * — the child's uid posture as it actually landed, whether a Landlock domain
 * was really built, which seccomp install path ran — because a status surface
 * that echoes the request is a surface that cannot tell you it failed. The
 * grant itself is never rendered; only its fingerprint, so an operator can see
 * WHICH authority is live without the document becoming a copy of it.
 *
 * The broker no longer HOLDS a grant either (session/agent_broker.h, struct
 * agent_authority_ref), so this file asks the provider for a labelled status
 * snapshot when it writes. The four fields that decide whether the document
 * can be believed are always present: `authority_source` (where the authority
 * came from), `authority_persistence` (always "ephemeral" — this store dies
 * with the process), `canonical_grant_id`, and `live_authority` (whether every
 * decision re-reads the store rather than a copy).
 */

#include "session/agent_broker.h"

#include "base/hex.h"
#include "base/log_macros.h"
#include "json/json.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BROKER_TAG "agent.broker"

/* ── status document ────────────────────────────────────────────────────── */

void agent_broker_write_status(const char *dir,
                               const struct agent_broker_session *s,
                               pid_t child_pid, const char *socket_path)
{
    if (!dir || !s)
        return;

    /* THE AUTHORITY SECTION IS PULLED, NOT REMEMBERED. The broker holds no
     * grant to render, so the four fields an operator needs — where the
     * authority came from, how durable it is, which canonical grant it is,
     * and whether decisions consult it live — are asked of the provider at
     * write time. A session that never bound reports ungranted, which is a
     * different document from a bound session with an empty grant. */
    const struct agent_authority_ref *a = s->authority;
    struct agent_authority_status auth;
    memset(&auth, 0, sizeof(auth));
    bool bound = a && a->bound && a->provider;
    bool have_status = false;
    if (bound && a->provider->status)
        have_status = a->provider->status(a->provider_ctx, a, &auth);
    if (bound && !have_status) {
        /* The provider named no status surface. Report what the binding
         * itself proves and nothing more. */
        snprintf(auth.authority_source, sizeof(auth.authority_source), "%s",
                 a->provider->name ? a->provider->name : "(unnamed provider)");
        snprintf(auth.canonical_grant_id, sizeof(auth.canonical_grant_id),
                 "%s", a->canonical_grant_id);
        snprintf(auth.principal, sizeof(auth.principal), "%s", a->principal);
        auth.live_authority = a->provider->authorize != NULL;
        auth.ephemeral = true;
    }
    char fphex[65];
    zcl_hex_encode(auth.fingerprint, 32, fphex);

    struct json_value doc;
    json_init(&doc);
    json_set_object(&doc);
    (void)json_push_kv_int(&doc, "broker_pid", (int64_t)getpid());
    (void)json_push_kv_int(&doc, "agent_pid", (int64_t)child_pid);
    (void)json_push_kv_str(&doc, "socket", socket_path ? socket_path
                                                       : "(socketpair)");
    (void)json_push_kv_bool(&doc, "granted", bound);
    (void)json_push_kv_str(&doc, "authority_source",
                           bound ? auth.authority_source : "none");
    /* Stated in every document, including an ungranted one: this store does
     * not survive a restart, so nothing here implies durable revocation or a
     * receipt anyone can verify after the process exits. */
    (void)json_push_kv_str(&doc, "authority_persistence", "ephemeral");
    (void)json_push_kv_str(&doc, "canonical_grant_id",
                           bound ? auth.canonical_grant_id : "");
    (void)json_push_kv_bool(&doc, "live_authority",
                            bound && auth.live_authority);
    (void)json_push_kv_str(&doc, "principal", bound ? auth.principal : "");
    (void)json_push_kv_str(&doc, "grant_fingerprint", bound ? fphex : "");
    (void)json_push_kv_bool(&doc, "grant_revoked", auth.revoked);
    (void)json_push_kv_int(&doc, "authority_generation",
                           (int64_t)auth.authority_generation);
    (void)json_push_kv_int(&doc, "budget_zats", (int64_t)auth.budget_zat);
    (void)json_push_kv_int(&doc, "spent_zats", (int64_t)auth.spent_zat);
    (void)json_push_kv_int(&doc, "grant_properties",
                           bound ? (int64_t)a->scope.n_properties : 0);
    (void)json_push_kv_int(&doc, "requests_served",
                           (int64_t)s->requests_served);
    (void)json_push_kv_int(&doc, "requests_denied",
                           (int64_t)s->requests_denied);
    (void)json_push_kv_int(&doc, "receipts_written",
                           (int64_t)s->receipts_written);
    (void)json_push_kv_int(&doc, "peer_pid", (int64_t)s->peer.pid);
    (void)json_push_kv_int(&doc, "peer_uid", (int64_t)s->peer.uid);
    (void)json_push_kv_int(&doc, "peer_gid", (int64_t)s->peer.gid);

    /* The ACHIEVED confinement, as the child reported it — never the
     * requested one. */
    const char *posture =
        s->child.uid_posture == AGENT_CONFINE_SEPARATE_UID ? "separate_uid"
        : s->child.uid_posture == AGENT_CONFINE_SAME_UID   ? "same_uid"
                                                           : "unknown";
    (void)json_push_kv_str(&doc, "agent_uid_posture", posture);
    (void)json_push_kv_int(&doc, "agent_uid", (int64_t)s->child.ran_as_uid);
    (void)json_push_kv_int(&doc, "agent_landlock_abi",
                           (int64_t)s->child.landlock_abi);
    (void)json_push_kv_bool(&doc, "agent_landlock_applied",
                            s->child.landlock_applied);
    (void)json_push_kv_bool(&doc, "agent_seccomp_applied",
                            s->child.seccomp_applied);
    (void)json_push_kv_bool(&doc, "agent_rlimits_applied",
                            s->child.rlimits_applied);
    (void)json_push_kv_int(&doc, "agent_fs_grants",
                           (int64_t)s->child.fs_grants);
    (void)json_push_kv_str(&doc, "agent_seccomp_method",
                           s->child.seccomp_method);

    char buf[4096];
    size_t n = json_write(&doc, buf, sizeof(buf));
    json_free(&doc);
    if (n == 0)
        return;

    char path[512];
    snprintf(path, sizeof(path), "%s/broker.json", dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        LOG_WARN(BROKER_TAG, "cannot write %s: %s", path, strerror(errno));
        return;
    }
    if (write(fd, buf, n) != (ssize_t)n)
        LOG_WARN(BROKER_TAG, "short write of %s", path);
    (void)close(fd);
}

size_t agent_broker_render_status_json(const char *dir, char *out,
                                       size_t out_cap)
{
    if (!dir || !out || out_cap == 0)
        return 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/broker.json", dir);

    struct json_value doc;
    json_init(&doc);
    json_set_object(&doc);
    (void)json_push_kv_str(&doc, "dir", dir);

    FILE *f = fopen(path, "re");
    if (!f) {
        (void)json_push_kv_bool(&doc, "broker_state_present", false);
        (void)json_push_kv_str(&doc, "reason",
                               "no broker.json in this directory — no confined "
                               "agent broker has run here");
        size_t n = json_write(&doc, out, out_cap);
        json_free(&doc);
        return n;
    }
    char buf[4096];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    (void)fclose(f);
    buf[got] = '\0';

    struct json_value state;
    json_init(&state);
    bool parsed = got > 0 && json_read(&state, buf, got);
    (void)json_push_kv_bool(&doc, "broker_state_present", parsed);
    if (parsed) {
        /* A recorded broker_pid that is no longer alive is reported as such
         * rather than implied by its presence. */
        int64_t bpid = json_get_int(json_get(&state, "broker_pid"));
        (void)json_push_kv_bool(&doc, "broker_running",
                                bpid > 0 && kill((pid_t)bpid, 0) == 0);
        (void)json_push_kv(&doc, "state", &state);
    } else {
        (void)json_push_kv_str(&doc, "reason", "broker.json is not valid JSON");
    }
    json_free(&state);

    size_t n = json_write(&doc, out, out_cap);
    json_free(&doc);
    return n;
}
