/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * The broker: the privileged side of the confined-agent boundary.
 *
 * THE ORDERING THAT MAKES THE BOUNDARY REAL — read this before editing
 * agent_broker_spawn_confined() or agent_broker_mode_main():
 *
 *   1. The child is confined by the PARENT, before execve. rlimits,
 *      no_new_privs, Landlock and the stage-1 seccomp filter are all applied in
 *      the forked child while it is still our code, and all four survive
 *      execve. A hostile agent therefore cannot decline to be sandboxed: by the
 *      time its own first instruction runs, the kernel is already refusing.
 *      The child narrows itself further (stage-2 seccomp drops execve) but that
 *      is defence in depth, not the boundary.
 *
 *   2. The grant is constructed AFTER the fork. A forked child shares a
 *      copy-on-write image of the parent's address space, so anything the
 *      broker is holding at fork time is readable in the child's memory. The
 *      grant is therefore not built until the child already exists — and
 *      execve then replaces that image entirely, along with the inherited
 *      environment, which is why /proc/<child>/environ is empty rather than a
 *      copy of the operator's session.
 *
 *   3. The grant is never an argument to the child. It is not in argv (four
 *      words: mode, script, scratch dir, terminator), not in envp (empty), and
 *      not in any file the child's Landlock domain can open. The child cannot
 *      present, forward, or leak an authority it has no way to name.
 *
 * SO_PEERCRED is consulted once per connection, before any verb is dispatched.
 * It is the kernel's answer about the peer process, so a client that asserts an
 * identity in its own bytes is not merely disbelieved — it is never asked.
 */

#define _GNU_SOURCE  /* struct ucred, execvpe — must precede every include */

#include "session/agent_broker.h"

#include "base/log_macros.h"
#include "base/result.h"
#include "crypto/sha3.h"
#include "json/json.h"
#include "platform/clock.h"
#include "platform/os_sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#define BROKER_TAG "agent.broker"

/* The child receives the connected socket as this descriptor. Fixed, so the
 * child needs no argument naming it — one less thing on a command line. */
#define AGENT_CHILD_SOCKET_FD 3

/* ── fd helpers (EINTR-safe, exact-length) ──────────────────────────────── */

static bool read_full(int fd, uint8_t *buf, size_t n, bool *peer_closed)
{
    size_t got = 0;
    if (peer_closed)
        *peer_closed = false;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r == 0) {
            if (peer_closed)
                *peer_closed = true;
            return false;
        }
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        got += (size_t)r;
    }
    return true;
}

static bool write_full(int fd, const uint8_t *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, buf + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        sent += (size_t)w;
    }
    return true;
}

/* ── SO_PEERCRED ────────────────────────────────────────────────────────── */

bool agent_broker_peercred(int fd, struct agent_peer_cred *out)
{
    if (!out)
        LOG_FAIL(BROKER_TAG, "null out for fd=%d", fd);
    memset(out, 0, sizeof(*out));
    if (fd < 0)
        LOG_FAIL(BROKER_TAG, "bad fd=%d", fd);

    struct ucred uc;
    socklen_t len = sizeof(uc);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &len) != 0 ||
        len != sizeof(uc))
        LOG_FAIL(BROKER_TAG, "SO_PEERCRED on fd=%d failed: %s", fd,
                 strerror(errno));

    out->pid   = uc.pid;
    out->uid   = uc.uid;
    out->gid   = uc.gid;
    out->valid = true;
    return true;
}

bool agent_broker_peer_authorized(const struct agent_peer_cred *c,
                                  const struct agent_peer_expectation *e,
                                  char *why, size_t why_cap)
{
    if (why && why_cap)
        why[0] = '\0';
    if (!c || !e) {
        if (why && why_cap)
            snprintf(why, why_cap, "internal: null credential or expectation");
        return false;
    }
    if (!c->valid) {
        if (why && why_cap)
            snprintf(why, why_cap,
                     "kernel did not supply peer credentials for this socket");
        return false;
    }
    /* An expectation that checks nothing would accept every local process.
     * That is a misconfiguration, and it fails closed. */
    if (!e->require_uid && !e->require_gid && !e->require_pid) {
        if (why && why_cap)
            snprintf(why, why_cap,
                     "expectation constrains nothing (no uid/gid/pid required)");
        return false;
    }
    if (e->require_uid && c->uid != e->uid) {
        if (why && why_cap)
            snprintf(why, why_cap, "uid mismatch: peer=%u expected=%u",
                     (unsigned)c->uid, (unsigned)e->uid);
        return false;
    }
    if (e->require_gid && c->gid != e->gid) {
        if (why && why_cap)
            snprintf(why, why_cap, "gid mismatch: peer=%u expected=%u",
                     (unsigned)c->gid, (unsigned)e->gid);
        return false;
    }
    if (e->require_pid && c->pid != e->pid) {
        if (why && why_cap)
            snprintf(why, why_cap, "pid mismatch: peer=%d expected=%d",
                     (int)c->pid, (int)e->pid);
        return false;
    }
    return true;
}

/* ── the fixture property catalog (Lane 1 reconciliation seam) ──────────── */

struct fixture_state {
    uint64_t revision[AGENT_BROKER_FIXTURE_PROPERTIES];
    bool     listed[AGENT_BROKER_FIXTURE_PROPERTIES];
    bool     hosted[AGENT_BROKER_FIXTURE_PROPERTIES];
};
static struct fixture_state g_fixture;

void agent_broker_fixture_property_id(size_t index,
                                      uint8_t out[MVAP_PROPERTY_ID_LEN])
{
    if (!out)
        return;
    struct sha3_256_ctx c;
    sha3_256_init(&c);
    sha3_256_write(&c, (const unsigned char *)"zcl.agent_fixture.property", 26);
    unsigned char i8 = (unsigned char)index;
    sha3_256_write(&c, &i8, 1);
    sha3_256_finalize(&c, out);
}

static int fixture_index_of(const uint8_t id[MVAP_PROPERTY_ID_LEN])
{
    for (size_t i = 0; i < AGENT_BROKER_FIXTURE_PROPERTIES; i++) {
        uint8_t want[MVAP_PROPERTY_ID_LEN];
        agent_broker_fixture_property_id(i, want);
        if (memcmp(want, id, MVAP_PROPERTY_ID_LEN) == 0)
            return (int)i;
    }
    return -1;
}

static bool fixture_plan(void *ctx, const struct mvap_request *req,
                         struct agent_plan *out)
{
    (void)ctx;
    if (!req || !out)
        LOG_FAIL(BROKER_TAG, "null argument in fixture plan");
    memset(out, 0, sizeof(*out));

    if (req->verb == MVAP_VERB_LIST) {
        out->found = true;
        out->kind  = req->kind;
        snprintf(out->detail, sizeof(out->detail), "catalog=fixture count=%d",
                 AGENT_BROKER_FIXTURE_PROPERTIES);
        return true;
    }
    int idx = fixture_index_of(req->property_id);
    if (idx < 0) {
        snprintf(out->detail, sizeof(out->detail),
                 "no property with that id in the fixture catalog");
        return true;                          /* resolved: not found */
    }
    out->found         = true;
    out->kind          = (uint16_t)(idx == 1 ? MVAP_KIND_ZCODE
                                             : MVAP_KIND_CONTENT);
    out->revision      = g_fixture.revision[idx];
    out->owner_matches = true;
    agent_broker_fixture_property_id((size_t)idx, out->content_root);
    snprintf(out->detail, sizeof(out->detail), "fixture index=%d revision=%llu",
             idx, (unsigned long long)out->revision);
    return true;
}

static bool fixture_commit(void *ctx, const struct mvap_request *req,
                           const struct agent_plan *plan, char *body,
                           size_t body_cap)
{
    (void)ctx;
    if (!req || !plan || !body)
        LOG_FAIL(BROKER_TAG, "null argument in fixture commit");
    int idx = fixture_index_of(req->property_id);
    if (idx < 0)
        LOG_FAIL(BROKER_TAG, "commit for a property absent from the fixture");

    /* The commit-time recheck the owner's spec requires: if the revision moved
     * between PLAN and COMMIT the caller planned against a stale view. */
    if (g_fixture.revision[idx] != plan->revision)
        LOG_FAIL(BROKER_TAG, "revision moved %llu -> %llu between plan and commit",
                 (unsigned long long)plan->revision,
                 (unsigned long long)g_fixture.revision[idx]);

    switch (req->verb) {
    case MVAP_VERB_HOST:             g_fixture.hosted[idx] = true;  break;
    case MVAP_VERB_LIST:                                            break;
    case MVAP_VERB_SELL:             g_fixture.listed[idx] = true;  break;
    case MVAP_VERB_PUBLISH_REVISION:
    case MVAP_VERB_UPDATE_POINTER:   g_fixture.revision[idx]++;     break;
    default:                                                        break;
    }

    char root[65];
    static const char hexd[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        root[2 * i]     = hexd[(plan->content_root[i] >> 4) & 0xF];
        root[2 * i + 1] = hexd[plan->content_root[i] & 0xF];
    }
    root[64] = '\0';

    int n = snprintf(body, body_cap,
        "{\"kind\":\"%s\",\"revision\":%llu,\"content_root\":\"%s\","
        "\"hosted\":%s,\"listed\":%s}",
        mvap_kind_name(plan->kind),
        (unsigned long long)g_fixture.revision[idx], root,
        g_fixture.hosted[idx] ? "true" : "false",
        g_fixture.listed[idx] ? "true" : "false");
    if (n < 0 || (size_t)n >= body_cap)
        LOG_FAIL(BROKER_TAG, "commit body does not fit %zu bytes", body_cap);
    return true;
}

struct agent_broker_node_ops agent_broker_fixture_ops(void)
{
    return (struct agent_broker_node_ops){
        .plan = fixture_plan, .commit = fixture_commit, .ctx = NULL };
}

/* ── the request pipeline ───────────────────────────────────────────────── */

static void resp_init(struct mvap_response *r, const struct mvap_request *req,
                      int32_t status)
{
    memset(r, 0, sizeof(*r));
    r->verb       = req ? req->verb : MVAP_VERB_NONE;
    r->request_id = req ? req->request_id : 0;
    r->status     = status;
}

static void resp_body(struct mvap_response *r, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void resp_body(struct mvap_response *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(r->body, sizeof(r->body), fmt, ap);
    va_end(ap);
    if (n < 0)
        r->body[0] = '\0';
}

/* Replay protection: a repeated request_id returns the FIRST outcome rather
 * than committing a second time. This is what "receipts are idempotent" buys —
 * a retry after a lost response cannot double-spend the grant's budget. */
static struct mvap_response *idem_find(struct agent_broker_session *s,
                                       uint32_t request_id)
{
    for (size_t i = 0; i < AGENT_IDEMPOTENCY_SLOTS; i++)
        if (s->idem[i].used && s->idem[i].request_id == request_id)
            return &s->idem[i].resp;
    return NULL;
}

static void idem_store(struct agent_broker_session *s, uint32_t request_id,
                       const struct mvap_response *resp)
{
    size_t slot = request_id % AGENT_IDEMPOTENCY_SLOTS;
    s->idem[slot].used       = true;
    s->idem[slot].request_id = request_id;
    s->idem[slot].resp       = *resp;
}

static void broker_receipt(struct agent_broker_session *s,
                           const struct mvap_request *req,
                           struct mvap_response *resp, const char *detail)
{
    if (!s->audit || !s->audit->open)
        return;
    struct agent_receipt r = { 0 };
    r.verb       = req->verb;
    r.request_id = req->request_id;
    r.status     = resp->status;
    r.value_zats = req->value_zats;
    memcpy(r.property_id, req->property_id, MVAP_PROPERTY_ID_LEN);
    snprintf(r.principal, sizeof(r.principal), "%s", s->grant.principal);
    snprintf(r.grant_id, sizeof(r.grant_id), "%s", s->grant.grant_id);
    r.peer = s->peer;
    snprintf(r.detail, sizeof(r.detail), "%s", detail ? detail : "");
    if (agent_audit_append(s->audit, &r)) {
        memcpy(resp->receipt_id, r.id, MVAP_RECEIPT_ID_LEN);
        s->receipts_written++;
    }
}

void agent_broker_handle(struct agent_broker_session *s,
                         const struct mvap_request *req,
                         struct mvap_response *out)
{
    if (!s || !req || !out)
        return;

    s->requests_served++;

    struct mvap_response *cached = idem_find(s, req->request_id);
    if (cached && cached->verb == req->verb) {
        *out = *cached;
        return;
    }

    int64_t now = clock_now_wall_ms();
    resp_init(out, req, MVAP_OK);

    /* 1. Grant scope, on what the agent CLAIMED. */
    int32_t verdict = agent_grant_authorize(&s->grant, req, now);
    if (verdict != MVAP_OK) {
        resp_init(out, req, verdict);
        resp_body(out, "{\"denied\":\"%s\",\"stage\":\"authorize\"}",
                  mvap_status_name(verdict));
        s->requests_denied++;
        broker_receipt(s, req, out, "denied at pre-plan authorize");
        idem_store(s, req->request_id, out);
        return;
    }

    /* 2. PLAN — resolve the property through the node seam. */
    struct agent_plan plan;
    if (!s->ops.plan || !s->ops.plan(s->ops.ctx, req, &plan)) {
        resp_init(out, req, MVAP_ERR_PLAN_FAILED);
        resp_body(out, "{\"denied\":\"PLAN_FAILED\"}");
        s->requests_denied++;
        broker_receipt(s, req, out, "plan refused");
        idem_store(s, req->request_id, out);
        return;
    }
    if (!plan.found) {
        resp_init(out, req, MVAP_ERR_NOT_FOUND);
        resp_body(out, "{\"denied\":\"NOT_FOUND\",\"detail\":\"%s\"}",
                  plan.detail);
        s->requests_denied++;
        idem_store(s, req->request_id, out);
        return;
    }

    /* 3. Re-authorize against the CATALOG's kind, not the agent's claim. A
     *    request that understated its kind to slip past the mask dies here. */
    if (req->verb != MVAP_VERB_LIST) {
        struct mvap_request authoritative = *req;
        authoritative.kind = plan.kind;
        verdict = agent_grant_authorize(&s->grant, &authoritative, now);
        if (verdict != MVAP_OK) {
            resp_init(out, req, verdict);
            resp_body(out, "{\"denied\":\"%s\",\"stage\":\"recheck\","
                           "\"actual_kind\":\"%s\"}",
                      mvap_status_name(verdict), mvap_kind_name(plan.kind));
            s->requests_denied++;
            broker_receipt(s, req, out, "denied at post-plan kind recheck");
            idem_store(s, req->request_id, out);
            return;
        }
    }

    /* 4. Reads answer from the plan; they mutate nothing and mint no receipt. */
    if (!mvap_verb_is_mutation(req->verb)) {
        resp_init(out, req, MVAP_OK);
        resp_body(out,
                  "{\"kind\":\"%s\",\"revision\":%llu,\"owner_matches\":%s,"
                  "\"detail\":\"%s\"}",
                  mvap_kind_name(plan.kind),
                  (unsigned long long)plan.revision,
                  plan.owner_matches ? "true" : "false", plan.detail);
        idem_store(s, req->request_id, out);
        return;
    }

    /* 5. COMMIT — rechecks ownership and revision inside the seam. */
    char body[MVAP_BODY_MAX + 1] = { 0 };
    if (!plan.owner_matches) {
        resp_init(out, req, MVAP_ERR_DENIED_PROPERTY);
        resp_body(out, "{\"denied\":\"DENIED_PROPERTY\",\"stage\":\"commit\","
                       "\"detail\":\"controller is not the grant principal\"}");
        s->requests_denied++;
        broker_receipt(s, req, out, "commit refused: owner mismatch");
        idem_store(s, req->request_id, out);
        return;
    }
    if (!s->ops.commit ||
        !s->ops.commit(s->ops.ctx, req, &plan, body, sizeof(body))) {
        resp_init(out, req, MVAP_ERR_COMMIT_FAILED);
        resp_body(out, "{\"denied\":\"COMMIT_FAILED\"}");
        s->requests_denied++;
        broker_receipt(s, req, out, "commit refused by the node seam");
        idem_store(s, req->request_id, out);
        return;
    }

    agent_grant_commit_debit(&s->grant, req, now);
    resp_init(out, req, MVAP_OK);
    resp_body(out, "%s", body);
    broker_receipt(s, req, out, plan.detail);
    idem_store(s, req->request_id, out);
}

/* ── serving ────────────────────────────────────────────────────────────── */

int agent_broker_serve_once(struct agent_broker_session *s, int fd)
{
    if (!s || fd < 0)
        LOG_ERR(BROKER_TAG, "bad session=%p fd=%d", (void *)s, fd);

    uint8_t prefix[MVAP_FRAME_PREFIX];
    bool closed = false;
    if (!read_full(fd, prefix, sizeof(prefix), &closed)) {
        if (closed)
            return 0;
        LOG_ERR(BROKER_TAG, "reading frame prefix failed: %s", strerror(errno));
    }
    uint32_t rec = mvap_frame_length(prefix, sizeof(prefix));
    if (rec == 0)
        LOG_ERR(BROKER_TAG, "peer declared an out-of-bounds frame length");

    uint8_t buf[MVAP_MAX_FRAME];
    if (!read_full(fd, buf, rec, &closed))
        LOG_ERR(BROKER_TAG, "short read of a %u-byte frame%s", rec,
                closed ? " (peer closed mid-frame)" : "");

    struct mvap_request req;
    struct mvap_response resp;
    if (!mvap_request_decode(buf, rec, &req)) {
        memset(&req, 0, sizeof(req));
        resp_init(&resp, &req, MVAP_ERR_BAD_REQUEST);
        resp_body(&resp, "{\"denied\":\"BAD_REQUEST\"}");
    } else {
        agent_broker_handle(s, &req, &resp);
    }

    uint8_t out[MVAP_FRAME_PREFIX + MVAP_MAX_FRAME];
    size_t n = mvap_response_encode(&resp, out, sizeof(out));
    if (n == 0)
        LOG_ERR(BROKER_TAG, "could not encode the response for request_id=%u",
                resp.request_id);
    if (!write_full(fd, out, n))
        LOG_ERR(BROKER_TAG, "writing the response failed: %s", strerror(errno));
    return 1;
}

int agent_broker_serve_fd(struct agent_broker_session *s, int fd,
                          uint64_t max_requests)
{
    if (!s || fd < 0)
        LOG_ERR(BROKER_TAG, "bad session=%p fd=%d", (void *)s, fd);

    /* The credential check happens ONCE, here, before a single verb is
     * dispatched. A peer that fails it never reaches agent_broker_handle. */
    if (!agent_broker_peercred(fd, &s->peer))
        LOG_ERR(BROKER_TAG, "no peer credentials on fd=%d", fd);

    char why[160];
    if (!agent_broker_peer_authorized(&s->peer, &s->expect, why, sizeof(why))) {
        struct mvap_request empty = { .verb = MVAP_VERB_INSPECT };
        struct mvap_response resp;
        resp_init(&resp, &empty, MVAP_ERR_DENIED_PEER_IDENTITY);
        resp_body(&resp, "{\"denied\":\"DENIED_PEER_IDENTITY\",\"why\":\"%s\"}",
                  why);
        s->requests_denied++;
        broker_receipt(s, &empty, &resp, why);
        uint8_t out[MVAP_FRAME_PREFIX + MVAP_MAX_FRAME];
        size_t n = mvap_response_encode(&resp, out, sizeof(out));
        if (n)
            (void)write_full(fd, out, n);
        LOG_ERR(BROKER_TAG, "peer refused: %s", why);
    }

    uint64_t served = 0;
    for (;;) {
        int r = agent_broker_serve_once(s, fd);
        if (r <= 0)
            return r == 0 ? (int)served : -1;
        served++;
        if (max_requests && served >= max_requests)
            return (int)served;
    }
}

int agent_broker_listen(const char *path)
{
    if (!path || !path[0])
        LOG_ERR(BROKER_TAG, "null socket path");
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strnlen(path, sizeof(sa.sun_path)) >= sizeof(sa.sun_path))
        LOG_ERR(BROKER_TAG, "socket path too long (%zu >= %zu): %s",
                strnlen(path, 4096), sizeof(sa.sun_path), path);
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        LOG_ERR(BROKER_TAG, "socket(AF_UNIX) failed: %s", strerror(errno));
    (void)unlink(path);

    /* 0700 before bind: the filesystem permission is a coarse first gate, and
     * SO_PEERCRED is the exact one. Neither replaces the other. */
    mode_t old = umask(0077);
    bool bound = bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0;
    (void)umask(old);
    if (!bound) {
        (void)close(fd);
        LOG_ERR(BROKER_TAG, "bind %s failed: %s", path, strerror(errno));
    }
    if (listen(fd, 4) != 0) {
        (void)close(fd);
        LOG_ERR(BROKER_TAG, "listen on %s failed: %s", path, strerror(errno));
    }
    return fd;
}

int agent_broker_accept_once(struct agent_broker_session *s, int listen_fd,
                             int timeout_ms)
{
    if (!s || listen_fd < 0)
        LOG_ERR(BROKER_TAG, "bad session=%p listen_fd=%d", (void *)s,
                listen_fd);
    struct pollfd p = { .fd = listen_fd, .events = POLLIN };
    int pr = poll(&p, 1, timeout_ms);
    if (pr == 0)
        return 0;
    if (pr < 0)
        LOG_ERR(BROKER_TAG, "poll on the listener failed: %s", strerror(errno));

    int cfd = accept(listen_fd, NULL, NULL);
    if (cfd < 0)
        LOG_ERR(BROKER_TAG, "accept failed: %s", strerror(errno));
    int served = agent_broker_serve_fd(s, cfd, 0);
    (void)close(cfd);
    return served < 0 ? -1 : 1;
}

/* ── spawning the confined child ────────────────────────────────────────── */

/* Stage-1 seccomp: the confined agent's allow-set PLUS exactly what a fresh
 * program start needs to reach main(). It is wider than stage 2 only in that
 * it permits the ONE execve this path performs and the loader/startup syscalls
 * that follow it — every escape class (sockets, clone, ptrace, mount,
 * namespaces, bpf, keyrings) is absent from both. */
static const int g_stage1_extra[] = {
    __NR_execve,
#ifdef __NR_execveat
    __NR_execveat,
#endif
    __NR_arch_prctl, __NR_set_tid_address, __NR_set_robust_list,
#ifdef __NR_rseq
    __NR_rseq,
#endif
    __NR_prctl,
};

/* Landlock stage 1, applied by the PARENT so it is irreversible for the child.
 * The grants are: the child's own scratch directory, its /proc/self, and the
 * read+execute set the dynamic loader needs to start the binary at all. The
 * datadir, the wallet, the RPC cookie, $HOME and /etc are all ABSENT, which is
 * what the adversarial test measures. */
static size_t spawn_build_grants(const struct agent_spawn_request *req,
                                 struct os_sandbox_path_rule *rules,
                                 size_t cap)
{
    size_t n = 0;
    if (n < cap && req->scratch_dir)
        rules[n++] = (struct os_sandbox_path_rule){
            .path = req->scratch_dir, .allow_read = true,
            .allow_write = true, .allow_create = true };
    if (n < cap)
        rules[n++] = (struct os_sandbox_path_rule){
            .path = OS_SANDBOX_PROC_SELF_PATH, .allow_read = true };
    if (n < cap && req->self_exe)
        rules[n++] = (struct os_sandbox_path_rule){
            .path = req->self_exe, .allow_read = true, .allow_execute = true };

    /* The loader set. Each is granted only if it exists, so this stays correct
     * on a host with a different lib layout. */
    static const char *const loader_dirs[] = { "/usr/lib", "/lib", "/lib64" };
    for (size_t i = 0; i < sizeof(loader_dirs) / sizeof(loader_dirs[0]); i++) {
        struct stat st;
        if (n < cap && stat(loader_dirs[i], &st) == 0)
            rules[n++] = (struct os_sandbox_path_rule){
                .path = loader_dirs[i], .allow_read = true,
                .allow_execute = true };
    }
    struct stat st;
    if (n < cap && stat("/etc/ld.so.cache", &st) == 0)
        rules[n++] = (struct os_sandbox_path_rule){
            .path = "/etc/ld.so.cache", .allow_read = true };
    return n;
}

/* Resource caps for the agent: one process (so it cannot fork a helper even if
 * clone were reachable), no core dump (a dump would spill inherited memory to
 * disk), a small address space, and a modest fd ceiling. */
static struct os_sandbox_rlimits spawn_rlimits(void)
{
    return (struct os_sandbox_rlimits){
        .as_bytes    = (uint64_t)512u * 1024u * 1024u,
        .cpu_seconds = 30,
        .nproc       = 1,
        .fsize_bytes = (uint64_t)16u * 1024u * 1024u,
        .nofile      = 32,
        .core_bytes  = 0,
    };
}

bool agent_broker_spawn_confined(const struct agent_spawn_request *req,
                                 struct agent_spawn_result *result)
{
    if (!req || !result || !req->self_exe || !req->scratch_dir || !req->script)
        LOG_FAIL(BROKER_TAG, "null argument to spawn");
    if (!mvap_param_is_safe(req->script))
        LOG_FAIL(BROKER_TAG, "script name '%s' is not a safe token",
                 req->script);
    memset(result, 0, sizeof(*result));

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        LOG_FAIL(BROKER_TAG, "socketpair failed: %s", strerror(errno));

    pid_t pid = fork();
    if (pid < 0) {
        (void)close(sv[0]);
        (void)close(sv[1]);
        LOG_FAIL(BROKER_TAG, "fork failed: %s", strerror(errno));
    }

    if (pid == 0) {
        /* ── the child, still our code, still trusted ─────────────────── */
        (void)close(sv[0]);
        if (sv[1] != AGENT_CHILD_SOCKET_FD) {
            if (dup2(sv[1], AGENT_CHILD_SOCKET_FD) < 0)
                _exit(90);
            (void)close(sv[1]);
        }
        /* The socket must survive execve; everything else must not. */
        (void)fcntl(AGENT_CHILD_SOCKET_FD, F_SETFD, 0);

        /* uid drop, when this process has the authority to do one. Groups go
         * first: dropping the uid before the groups would make setgroups
         * impossible and silently leave supplementary groups behind. */
        if (req->confined_uid != 0 && req->confined_uid != geteuid()) {
            if (setgroups(0, NULL) != 0 && errno != EPERM)
                _exit(91);
            if (setgid(req->confined_gid) != 0 && errno != EPERM)
                _exit(92);
            (void)setuid(req->confined_uid);   /* EPERM is the expected
                                                * unprivileged answer */
        }

        struct os_sandbox_rlimits lim = spawn_rlimits();
        if (!zcl_result_is_ok(os_sandbox_set_rlimits(&lim)))
            _exit(93);
        if (!os_sandbox_no_new_privs())
            _exit(94);

        struct os_sandbox_path_rule rules[8];
        size_t n = spawn_build_grants(req, rules, 8);
        struct zcl_result lr = os_sandbox_landlock_restrict(rules, n);
        /* A kernel without Landlock degrades loudly rather than pretending:
         * the child reports landlock_applied=false at handshake and the
         * operator sees it in `metaverse agent status`. */
        if (!zcl_result_is_ok(lr) && lr.code != OS_SANDBOX_ERR_LANDLOCK_UNAVAILABLE)
            _exit(95);

        size_t n_base = 0;
        const int *base = agent_confined_allowed_syscalls(&n_base);
        size_t n_extra = sizeof(g_stage1_extra) / sizeof(g_stage1_extra[0]);
        int stage1[320];
        if (n_base + n_extra > sizeof(stage1) / sizeof(stage1[0]))
            _exit(96);
        memcpy(stage1, base, n_base * sizeof(int));
        memcpy(stage1 + n_base, g_stage1_extra, n_extra * sizeof(int));
        struct zcl_result sr = os_sandbox_seccomp_allow(stage1,
                                                        n_base + n_extra);
        if (!zcl_result_is_ok(sr) && sr.code != OS_SANDBOX_ERR_SECCOMP_UNAVAILABLE)
            _exit(97);

        /* argv carries the mode, the script name, and the scratch dir. envp is
         * EMPTY — that is what makes /proc/<pid>/environ carry nothing of the
         * operator's session, and no grant material exists in either. */
        char *const argv[] = {
            (char *)req->self_exe,
            (char *)"--metaverse-agent-confined",
            (char *)req->script,
            (char *)req->scratch_dir,
            NULL,
        };
        char *const envp[] = { NULL };
        execve(req->self_exe, argv, envp);
        _exit(98);
    }

    (void)close(sv[1]);
    result->pid  = pid;
    result->sock = sv[0];
    return true;
}

/* ── status document ────────────────────────────────────────────────────── */

void agent_broker_write_status(const char *dir,
                               const struct agent_broker_session *s,
                               const struct agent_grant *g, pid_t child_pid,
                               const char *socket_path)
{
    if (!dir || !s || !g)
        return;
    uint8_t fp[32];
    agent_grant_fingerprint(g, fp);
    char fphex[65];
    static const char hexd[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        fphex[2 * i]     = hexd[(fp[i] >> 4) & 0xF];
        fphex[2 * i + 1] = hexd[fp[i] & 0xF];
    }
    fphex[64] = '\0';

    struct json_value doc;
    json_init(&doc);
    json_set_object(&doc);
    (void)json_push_kv_int(&doc, "broker_pid", (int64_t)getpid());
    (void)json_push_kv_int(&doc, "agent_pid", (int64_t)child_pid);
    (void)json_push_kv_str(&doc, "socket", socket_path ? socket_path
                                                       : "(socketpair)");
    (void)json_push_kv_str(&doc, "principal", g->principal);
    (void)json_push_kv_str(&doc, "grant_id", g->grant_id);
    (void)json_push_kv_str(&doc, "grant_fingerprint", fphex);
    (void)json_push_kv_bool(&doc, "grant_revoked", g->revoked);
    (void)json_push_kv_int(&doc, "grant_actions_mask", (int64_t)g->actions_mask);
    (void)json_push_kv_int(&doc, "grant_properties", (int64_t)g->n_properties);
    (void)json_push_kv_int(&doc, "budget_zats", (int64_t)g->budget_zats);
    (void)json_push_kv_int(&doc, "spent_zats", (int64_t)g->spent_zats);
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
