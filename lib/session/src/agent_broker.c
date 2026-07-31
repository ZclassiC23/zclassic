/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * The broker: the privileged side of the confined-agent boundary.
 *
 * The privileged side is three files: this one (peer identity, the request
 * pipeline, and the listening socket), agent_broker_spawn.c (the confined
 * fork/execve), and agent_broker_status.c (the operator-facing report).
 *
 * THE ORDERING THAT MAKES THE BOUNDARY REAL — read this before editing
 * agent_broker_spawn_confined() (agent_broker_spawn.c) or
 * agent_broker_mode_main() (agent_broker_modes.c):
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
 *   3. The grant is never an argument to the child. It is not in argv (mode,
 *      script, scratch dir, and at most a canary path), not in envp (empty),
 *      and not in any file the child's Landlock domain can open. The child
 *      cannot present, forward, or leak an authority it has no way to name.
 *
 * The peer is identified once per connection, before any verb is dispatched,
 * from the kernel's own attribution — so a client that asserts an identity in
 * its own bytes is not merely disbelieved, it is never asked.
 *
 * WHICH kernel attribution is not a detail. SO_PEERCRED names the process that
 * CREATED the socket: for an accept()ed connection that is the peer, but for a
 * socketpair(2) it is the BROKER, on both ends, so it cannot tell the broker
 * apart from the child it handed the other end to. The socketpair posture
 * therefore identifies the peer from SCM_CREDENTIALS on the message it sent,
 * which the kernel stamps per message and the sender cannot forge. See
 * agent_broker_identify_peer().
 */

#define _GNU_SOURCE  /* struct ucred, execvpe — must precede every include */

#include "session/agent_broker.h"

#include "session/agent_broker_vocab.h"

#include "base/hex.h"
#include "base/log_macros.h"
#include "crypto/sha3.h"
#include "platform/clock.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

bool agent_broker_sender_cred(int fd, struct agent_peer_cred *out)
{
    if (!out)
        LOG_FAIL(BROKER_TAG, "null out for fd=%d", fd);
    memset(out, 0, sizeof(*out));
    if (fd < 0)
        LOG_FAIL(BROKER_TAG, "bad fd=%d", fd);

    /* Enabling this on the RECEIVING socket is what makes the kernel attach
     * credentials to messages; a sender cannot opt out of being named. */
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) != 0)
        LOG_FAIL(BROKER_TAG, "SO_PASSCRED on fd=%d failed: %s", fd,
                 strerror(errno));

    uint8_t peek;
    struct iovec iov = { .iov_base = &peek, .iov_len = 1 };
    union {
        struct cmsghdr align;
        char           bytes[CMSG_SPACE(sizeof(struct ucred))];
    } control;
    memset(&control, 0, sizeof(control));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = control.bytes;
    msg.msg_controllen = sizeof(control.bytes);

    ssize_t r;
    do {
        r = recvmsg(fd, &msg, MSG_PEEK);
    } while (r < 0 && errno == EINTR);
    if (r <= 0)
        LOG_FAIL(BROKER_TAG, "nothing to attribute on fd=%d: %s", fd,
                 r == 0 ? "peer closed without sending" : strerror(errno));

    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_CREDENTIALS ||
            c->cmsg_len != CMSG_LEN(sizeof(struct ucred)))
            continue;
        struct ucred uc;
        memcpy(&uc, CMSG_DATA(c), sizeof(uc));
        out->pid   = uc.pid;
        out->uid   = uc.uid;
        out->gid   = uc.gid;
        out->valid = true;
        return true;
    }
    LOG_FAIL(BROKER_TAG,
             "the kernel attached no credentials to the message on fd=%d "
             "(SO_PASSCRED must be set before the peer sends)", fd);
}

bool agent_broker_identify_peer(int fd, struct agent_peer_cred *out)
{
    if (!out)
        LOG_FAIL(BROKER_TAG, "null out for fd=%d", fd);
    if (!agent_broker_peercred(fd, out))
        LOG_FAIL(BROKER_TAG, "no socket credentials on fd=%d", fd);

    /* SO_PEERCRED just named the socket's CREATOR. When that is us, this is a
     * socketpair we made and both ends carry our pid — an answer about the
     * broker, not about the peer. Ask who actually sent instead. */
    if (out->pid == getpid()) {
        struct agent_peer_cred sender;
        if (agent_broker_sender_cred(fd, &sender) && sender.valid)
            *out = sender;
    }
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

/* ── the real property surface (registered by the composition root) ─────── */

static const struct agent_broker_provider *g_provider;

void agent_broker_provider_install(const struct agent_broker_provider *p)
{
    g_provider = p;
}

const struct agent_broker_provider *agent_broker_provider_get(void)
{
    return g_provider;
}

/* ── the request pipeline ───────────────────────────────────────────────── */

static void resp_init(struct mvap_response *r, const struct mvap_request *req,
                      int32_t status)
{
    memset(r, 0, sizeof(*r));
    r->verb       = req ? req->verb : MVAP_VERB_NONE;
    r->request_id = req ? req->request_id : 0;
    /* Reply in the dialect the peer spoke, not the one this build prefers. */
    r->version    = req ? req->version : 0;
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

/* One confinement receipt. `action_receipt_id` is the canonical metaverse
 * action receipt this row COMMITS TO — passed in from the COMMIT outcome, all
 * zero when the operation minted none (a refusal, or a query, which never
 * reaches here at all). It is inside the row's digest, so a confinement row
 * cannot later be pointed at a different action. */
static void broker_receipt(struct agent_broker_session *s,
                           const struct mvap_request *req,
                           struct mvap_response *resp, const char *detail,
                           const uint8_t action_receipt_id[32])
{
    if (!s->audit || !s->audit->open)
        return;
    struct agent_receipt r = { 0 };
    r.verb       = req->verb;
    r.request_id = req->request_id;
    r.status     = resp->status;
    r.value_zats = req->value_zats;
    memcpy(r.property_id, req->property_id, MVAP_PROPERTY_ID_LEN);
    if (action_receipt_id)
        memcpy(r.action_receipt_id, action_receipt_id, 32);
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

    /* Queries and actions are two different pipelines from here on. A QUERY
     * resolves and answers; it can reach neither PLAN/COMMIT nor a receipt,
     * because those calls are not on its path at all. */
    const bool is_action = mvap_verb_is_action(req->verb);
    if (!is_action && !mvap_verb_is_query(req->verb)) {
        resp_init(out, req, MVAP_ERR_UNKNOWN_VERB);
        resp_body(out, "{\"denied\":\"UNKNOWN_VERB\"}");
        s->requests_denied++;
        idem_store(s, req->request_id, out);
        return;
    }

    /* 1. Authorization, on what the agent CLAIMED. The verdict is the
     *    metaverse's; agent_grant_authorize() translates and narrows. */
    int32_t verdict = agent_grant_authorize(&s->grant, req, now);
    if (verdict != MVAP_OK) {
        resp_init(out, req, verdict);
        resp_body(out, "{\"denied\":\"%s\",\"stage\":\"authorize\"}",
                  mvap_status_name(verdict));
        s->requests_denied++;
        broker_receipt(s, req, out, "denied at pre-plan authorize", NULL);
        idem_store(s, req->request_id, out);
        return;
    }

    /* 2. Resolve the property through the node seam — `query` for a query,
     *    `plan` for an action. */
    struct agent_plan plan;
    bool (*resolve)(void *, const struct mvap_request *, struct agent_plan *) =
        is_action ? s->ops.plan
                  : (s->ops.query ? s->ops.query : s->ops.plan);
    if (!resolve || !resolve(s->ops.ctx, req, &plan)) {
        resp_init(out, req, MVAP_ERR_PLAN_FAILED);
        resp_body(out, "{\"denied\":\"PLAN_FAILED\"}");
        s->requests_denied++;
        broker_receipt(s, req, out, "plan refused", NULL);
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
     *    request that understated its kind to slip past the mask dies here.
     *    Skipped only when the request named no property at all, where there
     *    is no catalog kind to disagree with. */
    if (!mvap_property_id_is_zero(req->property_id)) {
        struct mvap_request authoritative = *req;
        authoritative.kind = plan.kind;
        verdict = agent_grant_authorize(&s->grant, &authoritative, now);
        if (verdict != MVAP_OK) {
            resp_init(out, req, verdict);
            resp_body(out, "{\"denied\":\"%s\",\"stage\":\"recheck\","
                           "\"actual_kind\":\"%s\"}",
                      mvap_status_name(verdict), mvap_kind_name(plan.kind));
            s->requests_denied++;
            broker_receipt(s, req, out, "denied at post-plan kind recheck",
                           NULL);
            idem_store(s, req->request_id, out);
            return;
        }
    }

    /* 4. A query answers from what it resolved. It mutates nothing, debits
     *    nothing, and mints no receipt — on either side. */
    if (!is_action) {
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
    struct agent_commit_outcome outcome;
    memset(&outcome, 0, sizeof(outcome));
    if (!plan.owner_matches) {
        resp_init(out, req, MVAP_ERR_DENIED_PROPERTY);
        resp_body(out, "{\"denied\":\"DENIED_PROPERTY\",\"stage\":\"commit\","
                       "\"detail\":\"controller is not the grant principal\"}");
        s->requests_denied++;
        broker_receipt(s, req, out, "commit refused: owner mismatch", NULL);
        idem_store(s, req->request_id, out);
        return;
    }
    if (!s->ops.commit ||
        !s->ops.commit(s->ops.ctx, req, &plan, &outcome)) {
        resp_init(out, req, MVAP_ERR_COMMIT_FAILED);
        resp_body(out, "{\"denied\":\"COMMIT_FAILED\"}");
        s->requests_denied++;
        broker_receipt(s, req, out, "commit refused by the node seam", NULL);
        idem_store(s, req->request_id, out);
        return;
    }

    agent_grant_commit_debit(&s->grant, req, now);
    resp_init(out, req, MVAP_OK);
    resp_body(out, "%s", outcome.body);
    /* The confinement envelope commits to the canonical action receipt rather
     * than restating what the action was. */
    char detail[AGENT_RECEIPT_DETAIL_MAX + 1];
    char action_hex[65];
    zcl_hex_encode(outcome.action_receipt_id, 32, action_hex);
    bool have_action_receipt = false;
    for (size_t i = 0; i < 32; i++)
        have_action_receipt |= outcome.action_receipt_id[i] != 0;
    snprintf(detail, sizeof(detail), "%s action_receipt=%s", plan.detail,
             have_action_receipt ? action_hex : "none");
    broker_receipt(s, req, out, detail, outcome.action_receipt_id);
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
    if (!agent_broker_identify_peer(fd, &s->peer))
        LOG_ERR(BROKER_TAG, "no peer credentials on fd=%d", fd);

    char why[160];
    if (!agent_broker_peer_authorized(&s->peer, &s->expect, why, sizeof(why))) {
        struct mvap_request empty = { .verb = MVAP_VERB_INSPECT };
        struct mvap_response resp;
        resp_init(&resp, &empty, MVAP_ERR_DENIED_PEER_IDENTITY);
        resp_body(&resp, "{\"denied\":\"DENIED_PEER_IDENTITY\",\"why\":\"%s\"}",
                  why);
        s->requests_denied++;
        broker_receipt(s, &empty, &resp, why, NULL);
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

