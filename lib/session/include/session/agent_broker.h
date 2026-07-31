/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * agent_broker — a REAL confinement boundary for an agent that acts on
 * metaverse properties on the operator's behalf.
 *
 * THE PROBLEM THIS EXISTS TO FIX: an "agent" that is merely a process in the
 * operator's own session is not confined at all. It inherits the environment
 * (every secret in it), can read every file the uid can read, and can exec
 * anything. A grant handed to such a process is a bearer token in a place that
 * leaks — /proc/<pid>/environ and a process listing both publish it.
 *
 * THE SHAPE OF THE FIX — two processes, asymmetric trust:
 *
 *   BROKER (privileged side, run by the operator/node)
 *     - HOLDS the grant. The grant is never written to the child's argv,
 *       environment, or any file the child can open. The child cannot name it,
 *       present it, forward it, or leak it, because it never has it.
 *     - Owns the socket. Validates SO_PEERCRED on EVERY connection: the kernel
 *       tells it the peer's real pid/uid/gid, so a client-asserted identity is
 *       never consulted.
 *     - Runs PLAN -> COMMIT against the node, rechecking the grant at commit.
 *     - Writes one tamper-evident audit receipt per action.
 *
 *   CONFINED AGENT (untrusted side)
 *     - Same binary, `--metaverse-agent-confined` mode.
 *     - Landlock: no datadir, no wallet, no RPC cookie, no $HOME. The only
 *       filesystem grant is its own scratch dir and /proc/self.
 *     - seccomp ALLOW-list (default KILL_PROCESS): no execve, no socket(), no
 *       connect(), no ptrace, no mount/namespace. It cannot open a NEW channel
 *       to anything — it has exactly one pre-opened socket fd from the broker
 *       and no syscall with which to make another.
 *     - rlimits: one process, no core dump, small address space.
 *     - Speaks only session/agent_broker_proto.h, whose wire cannot express a
 *       path, a shell word, or an RPC method.
 *
 * WHAT IS AND IS NOT A UID BOUNDARY (read this before trusting the model):
 * `agent_broker_spawn_confined()` will setgid/setuid to `confined_uid` when the
 * broker holds CAP_SETUID (the production posture: the node runs as a service
 * that can drop to a pre-provisioned unprivileged account). Where it cannot, it
 * records AGENT_CONFINE_SAME_UID in the report and keeps every other layer.
 * Landlock is not a weaker substitute for that: it denies access to the GRANTING
 * uid's own files, which a uid switch alone does not do. The honest summary is
 * that the filesystem boundary here is enforced by Landlock in both postures,
 * and the uid boundary is present only in the first.
 */

#ifndef ZCL_SESSION_AGENT_BROKER_H
#define ZCL_SESSION_AGENT_BROKER_H

#include "session/agent_broker_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ── the grant (broker-side only; never crosses to the child) ───────────── */

#define AGENT_GRANT_ID_MAX      32
#define AGENT_PRINCIPAL_MAX     64
#define AGENT_GRANT_MAX_PROPS   8
#define AGENT_ALLOWLIST_MAX     192

/* One capability grant.
 *
 * `actions_mask` is the CANONICAL metaverse_action_set — the same persisted
 * bit per action the metaverse grants and receipts carry. It used to be
 * `1u << verb` over the wire enum, a third incompatible bit layout that meant
 * a grant minted here and a grant read from the metaverse described different
 * rights with the same number. Set it only through
 * agent_grant_allow_action(), which translates the wire verb to its canonical
 * action.
 *
 * `queries_mask` is separate and keyed by WIRE value, because queries
 * (INSPECT, LIST) are not canonical actions at all and must never occupy a
 * canonical action bit.
 *
 * `kinds_mask` is a bitmask over enum mvap_kind. Scope is the INTERSECTION of
 * every field: the action must be granted AND the property must be in
 * `properties` (or, when n_properties == 0, its kind must be in kinds_mask)
 * AND the value must fit both ceilings AND the window must have room AND the
 * grant must be unexpired and unrevoked. */
struct agent_grant {
    char     grant_id[AGENT_GRANT_ID_MAX + 1];
    char     principal[AGENT_PRINCIPAL_MAX + 1];

    uint8_t  properties[AGENT_GRANT_MAX_PROPS][MVAP_PROPERTY_ID_LEN];
    size_t   n_properties;
    uint32_t kinds_mask;
    uint32_t actions_mask;        /* canonical metaverse_action_set          */
    uint32_t queries_mask;        /* bit N == wire query verb N              */

    uint64_t max_value_zats;      /* per-action ceiling; 0 == no value allowed */
    uint64_t budget_zats;         /* cumulative ceiling                        */
    uint64_t spent_zats;          /* debited at COMMIT                         */

    /* Counterparty allowlist: space-separated safe tokens; "" == any. */
    char     counterparty_allowlist[AGENT_ALLOWLIST_MAX + 1];

    int64_t  expires_unix_ms;     /* 0 == never                                */
    uint32_t rate_limit;          /* max committed actions per window; 0 == off */
    uint32_t window_seconds;
    uint32_t window_used;
    int64_t  window_start_ms;

    bool     may_delegate;
    uint32_t max_delegation_depth;

    uint64_t revocation_generation;
    bool     revoked;
};

/* Add a verb or kind to a grant's masks. `verb` is a WIRE value; the verb's
 * class decides which mask it lands in, so a caller never picks. An
 * unrecognized verb is ignored — it cannot be granted because it names
 * nothing. */
void agent_grant_allow_action(struct agent_grant *g, uint32_t verb);
void agent_grant_allow_kind(struct agent_grant *g, uint16_t kind);
bool agent_grant_add_property(struct agent_grant *g,
                              const uint8_t id[MVAP_PROPERTY_ID_LEN]);

/* SHA3-256 over the grant's canonical scope fields. Published in `metaverse
 * agent status` so an operator can see WHICH grant is live without the broker
 * ever rendering the grant itself. */
void agent_grant_fingerprint(const struct agent_grant *g, uint8_t out[32]);

/* Evaluate `req` against `g` at time `now_ms`. Returns MVAP_OK or the specific
 * refusal. Pure: debits nothing. `agent_grant_commit_debit()` is the mutating
 * half, called only after a successful COMMIT. */
int32_t agent_grant_authorize(const struct agent_grant *g,
                              const struct mvap_request *req, int64_t now_ms);
void agent_grant_commit_debit(struct agent_grant *g,
                              const struct mvap_request *req, int64_t now_ms);

/* ── peer identity (SO_PEERCRED) ────────────────────────────────────────── */

struct agent_peer_cred {
    pid_t pid;
    uid_t uid;
    gid_t gid;
    bool  valid;
};

/* What the broker will accept. Any `require_*` left false is not checked; at
 * least one must be set or agent_broker_peer_authorized() refuses outright —
 * an expectation that checks nothing is a misconfiguration, not a wildcard. */
struct agent_peer_expectation {
    bool  require_uid;  uid_t uid;
    bool  require_gid;  gid_t gid;
    bool  require_pid;  pid_t pid;
};

/* Read the peer's credentials from the kernel via getsockopt(SO_PEERCRED).
 * These are the values the KERNEL attributes to the peer process — nothing the
 * peer sends can influence them. Returns false (and leaves out->valid false)
 * when the socket is not AF_UNIX or the option is unavailable.
 *
 * THE TRAP THIS OPTION CARRIES: SO_PEERCRED reports the process that CREATED
 * the socket. On an accept()ed connection that is the connecting peer, which is
 * the answer we want. On a socketpair(2) it is the process that made the PAIR —
 * the broker itself, on BOTH ends — so it cannot distinguish the broker from
 * the child it handed the other end to. Use agent_broker_identify_peer() unless
 * you specifically want the socket-creation answer. */
bool agent_broker_peercred(int fd, struct agent_peer_cred *out);

/* Read the credentials of whoever SENT the next pending message, from the
 * kernel's SCM_CREDENTIALS on that message. The kernel fills these in itself
 * and refuses a pid/uid the sender does not hold, so they cannot be forged; and
 * because they are per-message they name the process on the other end of a
 * socketpair, which SO_PEERCRED cannot. Requires SO_PASSCRED, which this call
 * enables. The message is PEEKED, not consumed, so the caller still reads it
 * normally. Blocks until the peer sends or closes. */
bool agent_broker_sender_cred(int fd, struct agent_peer_cred *out);

/* The broker's canonical "who is actually on the other end of this fd" answer:
 * SO_PEERCRED when it names some OTHER process (an accept()ed connection), and
 * the per-message credentials when it names us (a socketpair, where the
 * socket-creation answer is about the broker and therefore says nothing about
 * the peer). Every credential check in the broker goes through this. */
bool agent_broker_identify_peer(int fd, struct agent_peer_cred *out);

/* True iff `c` satisfies `e`. On refusal, writes the exact mismatch into `why`
 * (e.g. "uid mismatch: peer=1000 expected=65534"). */
bool agent_broker_peer_authorized(const struct agent_peer_cred *c,
                                  const struct agent_peer_expectation *e,
                                  char *why, size_t why_cap);

/* ── tamper-evident audit receipts ──────────────────────────────────────── */

#define AGENT_RECEIPT_DETAIL_MAX 160

/* One CONFINEMENT receipt: `id` = SHA3-256 over the canonical preimage (which
 * includes `prev`, so the log is a hash chain); `sig` = Ed25519 over `id` under
 * the broker's audit key. A verifier needs only the file and the public key.
 *
 * WHAT THIS IS AND IS NOT. It is evidence about the BOUNDARY — which kernel-
 * attributed peer asked for what, under which grant, and what the broker did
 * about it. It is NOT a second account of what the action meant. The metaverse
 * mints the authoritative action receipt (metaverse/property_receipt.h: a
 * canonical, hash-chained, signed record), and this envelope COMMITS TO it:
 * `action_receipt_id` carries that receipt's chain hash and is inside this
 * row's digest. So a confinement row cannot be pointed at a different action
 * than the one the metaverse recorded, and the two logs are joined by a hash
 * instead of by two independent restatements of the same event.
 *
 * All-zero `action_receipt_id` means the operation minted no canonical
 * receipt — a query, or a refusal that never reached COMMIT. */
struct agent_receipt {
    uint64_t seq;                 /* 1-based; monotonic per log             */
    int64_t  unix_ms;
    uint8_t  prev[32];            /* previous receipt id; zeroes for seq 1  */
    uint8_t  id[32];
    uint8_t  sig[64];

    uint32_t verb;
    uint32_t request_id;
    int32_t  status;
    uint64_t value_zats;
    uint8_t  property_id[MVAP_PROPERTY_ID_LEN];
    uint8_t  action_receipt_id[32];  /* canonical metaverse receipt chain hash */

    char     principal[AGENT_PRINCIPAL_MAX + 1];
    char     grant_id[AGENT_GRANT_ID_MAX + 1];
    struct agent_peer_cred peer;
    char     detail[AGENT_RECEIPT_DETAIL_MAX + 1];
};

/* An open append-only audit log plus the broker's signing key. The key is
 * generated on first open and its PUBLIC half written next to the log; the
 * secret half never leaves this struct (and never reaches the confined child,
 * which has no Landlock grant on the broker's directory). */
struct agent_audit_log {
    char     dir[384];
    char     log_path[448];
    char     pub_path[448];
    uint8_t  sk[32];
    uint8_t  pk[32];
    uint64_t seq;
    uint8_t  head[32];
    bool     open;
};

/* Open (creating if absent) `<dir>/audit.log` + `<dir>/audit.pub`. When the log
 * already exists it is REPLAYED to recover seq and head, so an append after a
 * restart continues the same chain rather than starting a second one. */
bool agent_audit_open(struct agent_audit_log *log, const char *dir);

/* Fill r->{seq,unix_ms,prev,id,sig}, append one canonical JSON line, and fsync.
 * The caller fills the descriptive fields. Idempotency is the BROKER's job
 * (see agent_broker_session): this call always appends. */
bool agent_audit_append(struct agent_audit_log *log, struct agent_receipt *r);

struct agent_audit_verdict {
    uint64_t rows;
    uint64_t chain_breaks;   /* prev != previous row's id, or seq out of order */
    uint64_t bad_signatures;
    uint64_t malformed;
    uint8_t  head[32];
    bool     ok;             /* rows > 0 && every counter above == 0           */
};

/* Replay `<dir>/audit.log`, recompute every id from its preimage, check the
 * chain links, and verify every signature against `<dir>/audit.pub`. This is
 * the whole tamper-evidence claim: editing, reordering, or deleting any row
 * breaks either a recomputed id, a chain link, or a signature. */
bool agent_audit_verify_dir(const char *dir, struct agent_audit_verdict *out);

/* Render up to `max` most-recent receipts as JSON text into `out`. Used by
 * `metaverse agent audit`; read-only, creates nothing. */
size_t agent_audit_render_json(const char *dir, size_t max, char *out,
                               size_t out_cap);

/* ── the confinement report ─────────────────────────────────────────────── */

enum agent_confine_uid_posture {
    AGENT_CONFINE_UID_UNKNOWN = 0,
    AGENT_CONFINE_SEPARATE_UID,   /* setuid to a different uid SUCCEEDED     */
    AGENT_CONFINE_SAME_UID,       /* no CAP_SETUID here; other layers stand  */
};

/* What confinement the child ACTUALLY got. Reported to the broker over the
 * socket at handshake and republished by `metaverse agent status`, so the
 * operator reads the achieved posture rather than the requested one. */
struct agent_confine_report {
    enum agent_confine_uid_posture uid_posture;
    uid_t  ran_as_uid;
    gid_t  ran_as_gid;
    int    landlock_abi;
    bool   landlock_applied;
    bool   seccomp_applied;
    bool   rlimits_applied;
    size_t fs_grants;
    char   seccomp_method[16];
};

/* ── the node seam (PLAN -> COMMIT) ─────────────────────────────────────── */

/* What PLAN resolved about the target property. COMMIT rechecks `revision` and
 * `owner_matches`; a move between the two is MVAP_ERR_REVISION_MOVED. */
struct agent_plan {
    bool     found;
    uint16_t kind;
    uint64_t revision;
    bool     owner_matches;
    uint8_t  content_root[32];
    char     detail[AGENT_RECEIPT_DETAIL_MAX + 1];
};

/* What a COMMIT produced. `body` is the bounded JSON the agent gets back;
 * `action_receipt_id` is the canonical metaverse receipt's chain hash, which
 * the broker's confinement envelope then commits to. A seam that mints no
 * canonical receipt leaves it all-zero and says so rather than inventing
 * one. */
struct agent_commit_outcome {
    char    body[MVAP_BODY_MAX + 1];
    uint8_t action_receipt_id[32];
};

/* The seam onto the property catalog and the property grant service.
 *
 * QUERIES AND ACTIONS ARE DIFFERENT ENTRY POINTS, not one entry point with a
 * flag. `query` resolves a read; `plan` + `commit` are the two halves of an
 * action. The property that holds structurally is the one that matters: a
 * QUERY never reaches `commit`, because `commit` is not on its code path at
 * all — not because something remembered to check a flag first.
 *
 * `query` is OPTIONAL. A seam whose resolution is already side-effect-free may
 * leave it NULL and the broker resolves reads through `plan`, which is pure
 * resolution by contract; `commit` is the only call permitted to change
 * anything, and no query ever makes it. */
struct agent_broker_node_ops {
    bool (*query)(void *ctx, const struct mvap_request *req,
                  struct agent_plan *out);
    bool (*plan)(void *ctx, const struct mvap_request *req,
                 struct agent_plan *out);
    bool (*commit)(void *ctx, const struct mvap_request *req,
                   const struct agent_plan *plan,
                   struct agent_commit_outcome *out);
    void *ctx;
};

/* Where the broker gets its REAL authority and its REAL property surface.
 *
 * lib/ sits below app/services, so the property catalog and the property grant
 * service cannot be named from here; the composition root registers them. With
 * nothing registered `agent_broker_mode_main()` REFUSES to run. That is the
 * point: a broker that cannot reach the real catalog must not fall back to a
 * fixture, because a fixture grant that authorizes real-looking actions is
 * exactly the failure this seam exists to make impossible. */
struct agent_broker_provider {
    /* Fill `out` with the grant this broker session is to hold. Return false
     * to refuse the session (no grant, no broker). */
    bool (*grant)(void *ctx, struct agent_grant *out);
    /* The property surface the grant acts on. */
    struct agent_broker_node_ops (*ops)(void *ctx);
    void *ctx;
    const char *name;             /* named in the refusal and in status      */
};

void agent_broker_provider_install(const struct agent_broker_provider *p);
const struct agent_broker_provider *agent_broker_provider_get(void);

/* The fixture catalog and demo grant exist ONLY in a build compiled with
 * -DZCL_TESTING; in a production binary the symbols below are not declared and
 * not compiled, so no production path can reach them. Read
 * `agent_broker_fixtures_compiled_in` to say which build this is. */
extern const bool agent_broker_fixtures_compiled_in;

/* The deterministic ids of the fixture catalog's properties. Kept outside the
 * test-only guard because the confined-agent demo scripts name a target with
 * it: it derives an identifier and confers no authority and no behaviour. */
void agent_broker_fixture_property_id(size_t index,
                                      uint8_t out[MVAP_PROPERTY_ID_LEN]);
#define AGENT_BROKER_FIXTURE_PROPERTIES 2

#ifdef ZCL_TESTING
/* The fixture catalog: two properties (one content, one zcode). */
struct agent_broker_node_ops agent_broker_fixture_ops(void);

/* The demo grant + fixture ops as a provider, for the adversarial demo that
 * drives `--metaverse-broker --fixture`. */
void agent_broker_install_fixture_provider(void);
#endif

/* ── the broker session ─────────────────────────────────────────────────── */

#define AGENT_IDEMPOTENCY_SLOTS 32

/* One served connection. `grant` is BY VALUE here and this struct lives only in
 * the broker process — that is the mechanical reason the child cannot read it.
 * The idempotency ring makes a replayed request_id return the FIRST outcome
 * instead of committing twice. */
struct agent_broker_session {
    struct agent_grant            grant;
    struct agent_peer_expectation expect;
    struct agent_broker_node_ops  ops;
    struct agent_audit_log       *audit;

    struct agent_peer_cred        peer;
    struct agent_confine_report   child;

    uint64_t requests_served;
    uint64_t requests_denied;
    uint64_t receipts_written;

    struct {
        uint32_t request_id;
        bool     used;
        struct mvap_response resp;
    } idem[AGENT_IDEMPOTENCY_SLOTS];
};

/* Handle exactly ONE request frame already read off the wire. Returns the
 * response to send. This is the whole authorization pipeline in one function:
 *   peercred -> grant authorize -> PLAN -> grant recheck -> COMMIT -> receipt.
 * Never trusts a field of `req` beyond what mvap_request_decode() validated. */
void agent_broker_handle(struct agent_broker_session *s,
                         const struct mvap_request *req,
                         struct mvap_response *out);

/* Read one framed request from `fd`, handle it, write the framed response.
 * Returns 1 on a served request, 0 on a clean peer close, -1 on a protocol or
 * IO error (the caller closes the connection). */
int agent_broker_serve_once(struct agent_broker_session *s, int fd);

/* Serve `fd` until the peer closes or `max_requests` is reached (0 == until
 * close). Validates SO_PEERCRED ONCE up front and refuses the whole connection
 * when it does not match — a rejected peer never reaches the verb dispatch. */
int agent_broker_serve_fd(struct agent_broker_session *s, int fd,
                          uint64_t max_requests);

/* Bind + listen an AF_UNIX stream socket at `path` (unlinking a stale node),
 * mode 0700. Returns the listening fd or -1. This is the surface an arbitrary
 * local process can reach, which is exactly why every accept() is peercred
 * checked. */
int agent_broker_listen(const char *path);

/* Accept one connection, peercred-check it, and serve it. Returns 1 when a
 * connection was served, 0 on accept timeout, -1 on error. A peer that fails
 * the credential check is refused with MVAP_ERR_DENIED_PEER_IDENTITY, audited,
 * and disconnected without any verb being dispatched. */
int agent_broker_accept_once(struct agent_broker_session *s, int listen_fd,
                             int timeout_ms);

/* ── spawning the confined child ────────────────────────────────────────── */

struct agent_spawn_request {
    const char *self_exe;      /* path to this binary                        */
    const char *scratch_dir;   /* the ONLY writable Landlock grant the child gets */
    const char *script;        /* safe-token name of the child's built-in script */
    /* One path the BROKER wants the agent to TRY to open, so the refusal is
     * recorded as the kernel's errno in the child's report. It is an
     * instruction, never an authority: naming a path the child may not reach is
     * how the boundary is measured against a real asset instead of against
     * /etc/passwd. NULL or "" means "no canary". */
    const char *canary;
    uid_t       confined_uid;  /* target uid; 0 == "do not attempt a switch"  */
    gid_t       confined_gid;
};

struct agent_spawn_result {
    pid_t pid;
    int   sock;                /* broker end of the socketpair               */
};

/* socketpair() + fork() + exec self in confined-agent mode. The child end of
 * the pair becomes fd 3; argv carries ONLY the mode word, the script name, the
 * scratch dir and (when set) the canary path; the environment is EMPTY. No
 * grant material is placed in either, which is what makes the
 * /proc/<pid>/environ and cmdline assertions in the test hold.
 * Returns false and leaves result->pid <= 0 on failure. */
bool agent_broker_spawn_confined(const struct agent_spawn_request *req,
                                 struct agent_spawn_result *result);

/* ── the two argv modes (dispatched from src/main.c) ────────────────────── */

/* `--metaverse-agent-confined <script> <scratch-dir>` — the confined child.
 * Applies confinement to ITSELF, then speaks the protocol on fd 3. Never
 * returns to any node code path. */
int agent_confined_mode_main(int argc, char **argv);

/* `--metaverse-broker` — run a broker: spawn one confined agent, serve it,
 * write receipts, and exit. Accepts -datadir=, --broker-dir=,
 * --script=, --requests=, --listen (use a listening socket instead of the
 * inherited socketpair), and --expect-uid=.
 *
 * The grant and the property surface come from the registered
 * agent_broker_provider. With none registered this refuses rather than
 * serving; a ZCL_TESTING build additionally accepts --fixture to register the
 * demo provider, which is how the adversarial demo drives it. */
int agent_broker_mode_main(int argc, char **argv);

/* Apply the confined-agent profile to the CALLING process: rlimits ->
 * no_new_privs -> Landlock (scratch dir rw + /proc/self ro) -> seccomp
 * allow-list. ONE-WAY. `out` records what actually landed. Returns false only
 * when a step that MUST hold failed; a missing-Landlock kernel degrades and
 * says so in `out` rather than pretending. */
bool agent_confined_enter(const char *scratch_dir, uid_t want_uid,
                          gid_t want_gid, struct agent_confine_report *out);

/* The confined agent's seccomp ALLOW-list. Deliberately OMITS execve/execveat,
 * the whole socket family (socket/socketpair/connect/bind/accept/sendto/
 * recvfrom/sendmsg/recvmsg), clone/fork/vfork, ptrace/process_vm_*, mount/
 * setns/unshare/pivot_root, bpf/kexec/module ops, keyrings, and
 * open_by_handle_at. It DOES allow openat: the filesystem boundary is
 * Landlock's job, so a forbidden open must fail as EACCES (an operable,
 * attributable denial) rather than as an unattributable SIGSYS. */
const int *agent_confined_allowed_syscalls(size_t *count_out);

/* Write the achieved-confinement report / broker state to `<dir>/broker.json`
 * for `metaverse agent status` to read. Best-effort; a failure is logged and
 * does not stop the broker. */
void agent_broker_write_status(const char *dir,
                               const struct agent_broker_session *s,
                               const struct agent_grant *g, pid_t child_pid,
                               const char *socket_path);

/* Render `<dir>/broker.json` (or a not-running verdict) as JSON text. */
size_t agent_broker_render_status_json(const char *dir, char *out,
                                       size_t out_cap);

#endif /* ZCL_SESSION_AGENT_BROKER_H */
