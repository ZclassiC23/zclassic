/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * soak_attestation_service — persistent evidence log for the 7-day soak
 * criterion (MVP criterion 7).
 *
 * Appends one JSON line per tick to <datadir>/soak_attestation.jsonl.
 * Fsyncs every SOAK_ATTESTATION_FSYNC_EVERY lines; rotates to .1 at 50 MB.
 *
 * Layout:
 *   1. Global state + mutex
 *   2. File management (open, rotate, fsync)
 *   3. soak_attestation_tick() — tick body
 *   4. soak_attestation_init() / soak_attestation_reset_for_test()
 *   5. soak_dump_state_json()
 *
 * LOCK-ORDER LAW: this service runs on the health-ring periodic tick only.
 * node_health_collect() is safe here (it takes csr->lock THEN coins_kv, the
 * established order on the health/RPC path). This service NEVER touches the
 * reducer drive — no coins_kv held on entry, no csr or chain-evidence init.
 *
 * The g_soak mutex is a leaf mutex (no further mutex is acquired while it is
 * held, aside from the libc write() / fsync() kernel calls). */

// one-result-type-ok:soak-attest-no-fallible-surface
/* E2 override. soak_attestation_tick() and soak_attestation_init() return
 * void: write failures are best-effort (logged once per streak, never a
 * node halt), so there is no error result to propagate. soak_dump_state_json
 * is a pure introspection query (bool = whether out was valid). */

#include "services/soak_attestation_service.h"
#include "controllers/agent_security_posture.h"
#include "services/node_health_service.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "util/clientversion.h"
#include "util/log_macros.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Global state ──────────────────────────────────────────────── */

static struct {
    pthread_mutex_t lock;           /* guards all fields below */
    bool   initialized;
    char   datadir[512];

    /* File handle; -1 = not open. */
    int    fd;
    /* Current file size in bytes (approximate; used for rotation). */
    int64_t file_bytes;
    /* Lines written since last fsync. */
    int64_t lines_since_fsync;

    /* Counters (protected by lock; read atomically for dump). */
    _Atomic int64_t lines_written;
    _Atomic int64_t last_ts;
    _Atomic bool    last_healthy;
    _Atomic bool    last_window_eligible;
    _Atomic bool    last_security_review_required;
    _Atomic int64_t rotations;
    _Atomic int64_t write_failures;

    /* Failure-streak latch: 0 = ok, >0 = in a failure streak.
     * We log exactly once when the streak starts (latch goes 0 → 1);
     * subsequent failures in the same streak are silent. */
    int failure_streak;
} g_soak = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .fd   = -1,
};

/* ── File management ────────────────────────────────────────────── */

/* Build the primary and rotation paths from the datadir. */
static void soak_path_primary(const char *datadir, char *out, size_t sz)
{
    snprintf(out, sz, "%s/soak_attestation.jsonl", datadir);
}
static void soak_path_rotated(const char *datadir, char *out, size_t sz)
{
    snprintf(out, sz, "%s/soak_attestation.jsonl.1", datadir);
}

/* Close + reopen the file in O_APPEND mode.  Returns the fd on success,
 * -1 on error (caller logs; we write the path for diagnosis). */
static int soak_open_file(const char *datadir)
{
    char path[560];
    soak_path_primary(datadir, path, sizeof(path));
    /* O_CREAT|O_WRONLY|O_APPEND: all writes land at the logical EOF even
     * with multiple writers or after a crash-reopen.  NFS O_APPEND is not
     * guaranteed atomic but we have exactly one writer on this path. */
    int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) {
        LOG_WARN("soak_attest", "open %s: %s", path, strerror(errno));
    }
    return fd;
}

/* Rotate: rename primary to .1 (overwrites any stale .1), close old fd,
 * open a fresh primary.  Called with g_soak.lock held. */
static void soak_rotate(void)
{
    const char *dd = g_soak.datadir;
    char primary[560], rotated[560];
    soak_path_primary(dd, primary, sizeof(primary));
    soak_path_rotated(dd, rotated, sizeof(rotated));

    /* fsync + close the current fd before rename so the kernel flushes the
     * just-written data to the rotation target. */
    if (g_soak.fd >= 0) {
        fsync(g_soak.fd);  // platform-ok:shutdown-fsync-best-effort
        close(g_soak.fd);
        g_soak.fd = -1;
    }
    /* Rename primary → rotated (overwrites).  Ignore ENOENT (no primary
     * yet) — that just means we have nothing to rotate. */
    if (rename(primary, rotated) < 0 && errno != ENOENT) {
        LOG_WARN("soak_attest", "rotate rename %s → %s: %s",
                 primary, rotated, strerror(errno));
    }
    atomic_fetch_add(&g_soak.rotations, 1);

    /* Open a fresh primary for subsequent writes. */
    g_soak.fd          = soak_open_file(dd);
    g_soak.file_bytes  = 0;
    g_soak.lines_since_fsync = 0;
}

/* ── Tick body ──────────────────────────────────────────────────── */

void soak_attestation_tick(void)
{
    pthread_mutex_lock(&g_soak.lock);
    if (!g_soak.initialized) {
        pthread_mutex_unlock(&g_soak.lock);
        return;
    }
    const char *dd = g_soak.datadir;

    /* Lazy open: first tick after init opens the file. */
    if (g_soak.fd < 0) {
        /* Seed file_bytes from the existing file size so we don't over-rotate. */
        char path[560];
        soak_path_primary(dd, path, sizeof(path));
        struct stat st;
        if (stat(path, &st) == 0)
            g_soak.file_bytes = (int64_t)st.st_size;
        g_soak.fd = soak_open_file(dd);
    }

    /* Rotation check: rotate BEFORE writing the new line so we don't
     * overshoot by a full line. */
    if (g_soak.file_bytes >= SOAK_ATTESTATION_ROTATE_BYTES)
        soak_rotate();

    pthread_mutex_unlock(&g_soak.lock);

    /* Health snapshot — safe to call outside the lock (node_health_collect is
     * reentrant; g_soak.lock must not be held here because the health path
     * itself may take csr->lock THEN coins_kv, and g_soak.lock is unrelated). */
    struct node_health_snapshot snap = {0};
    struct agent_security_posture security_posture;
    node_health_collect(&snap, NULL, NULL);
    agent_security_posture_collect(&security_posture, NULL);

    int64_t ts_now  = (int64_t)platform_time_wall_time_t();
    bool security_ok =
        agent_security_posture_allows_public_serving(&security_posture);
    bool healthy = snap.healthy && security_ok;
    bool window_eligible = healthy && security_ok;
    int     height  = snap.tip_height;
    int64_t uptime  = snap.uptime_seconds;

    /* Escape the degraded_reason into a JSON-safe buffer.
     * degrade_reason is already a C string (at most 128 chars); the only
     * JSON-special chars that appear are '"' and '\' which we replace with a
     * safe '?'.  (This is minimal — not full JSON string escaping — but
     * sufficient for the ASCII-only reason strings this code produces.) */
    char reason_esc[160] = {0};
    {
        const char *src = snap.degraded_reason;
        size_t out_pos = 0;
        for (; *src && out_pos < sizeof(reason_esc) - 1; src++) {
            unsigned char c = (unsigned char)*src;
            if (c == '"' || c == '\\' || c < 0x20) {
                reason_esc[out_pos++] = '?';
            } else {
                reason_esc[out_pos++] = (char)c;
            }
        }
        reason_esc[out_pos] = '\0';
    }

    const char *commit = zcl_build_commit();

    /* Format the JSON line.  Keep it compact (one line, no trailing space). */
    char line[512];
    int line_len = snprintf(line, sizeof(line),
        "{\"ts\":%lld,\"height\":%d,\"healthy\":%s,"
        "\"degraded_reason\":\"%s\","
        "\"security_review_required\":%s,"
        "\"security_posture_ok\":%s,\"window_eligible\":%s,"
        "\"build_commit\":\"%s\",\"uptime_s\":%lld}\n",
        (long long)ts_now,
        height,
        healthy ? "true" : "false",
        security_ok ? reason_esc : security_posture.status,
        security_posture.review_required ? "true" : "false",
        security_ok ? "true" : "false",
        window_eligible ? "true" : "false",
        commit ? commit : "unknown",
        (long long)uptime);

    if (line_len <= 0 || (size_t)line_len >= sizeof(line)) {
        /* Oversized or snprintf error — should never happen with fixed fields. */
        LOG_WARN("soak_attest", "line format failed len=%d", line_len);
        line_len = 0;
    }

    /* Write + failure tracking. */
    pthread_mutex_lock(&g_soak.lock);
    if (line_len > 0 && g_soak.fd >= 0) {
        ssize_t written = write(g_soak.fd, line, (size_t)line_len);
        if (written < 0 || written != line_len) {
            /* Log once when the streak starts; suppress repeats. errno is
             * only meaningful when write() itself failed — a short write
             * leaves it stale, so report the byte counts instead. */
            if (g_soak.failure_streak == 0) {
                if (written < 0)
                    LOG_WARN("soak_attest", "write to soak_attestation.jsonl failed: %s",
                             strerror(errno));
                else
                    LOG_WARN("soak_attest", "short write to soak_attestation.jsonl: %zd of %d bytes",
                             written, line_len);
            }
            g_soak.failure_streak++;
            atomic_fetch_add(&g_soak.write_failures, 1);
        } else {
            g_soak.failure_streak = 0;   /* streak cleared */
            g_soak.file_bytes += line_len;
            g_soak.lines_since_fsync++;
            int64_t total = atomic_fetch_add(&g_soak.lines_written, 1) + 1;
            atomic_store(&g_soak.last_ts, ts_now);
            atomic_store(&g_soak.last_healthy, healthy);
            atomic_store(&g_soak.last_window_eligible, window_eligible);
            atomic_store(&g_soak.last_security_review_required,
                         security_posture.review_required);

            /* fsync every N lines to bound data-loss window. */
            if (g_soak.lines_since_fsync >= SOAK_ATTESTATION_FSYNC_EVERY) {
                fsync(g_soak.fd);  // platform-ok:soak-attest-periodic-fsync
                g_soak.lines_since_fsync = 0;
            }
            (void)total; /* used above only for readable counter update */
        }
    } else if (line_len > 0 && g_soak.fd < 0) {
        /* fd open failed on this tick cycle (e.g. datadir not yet mounted). */
        if (g_soak.failure_streak == 0)
            LOG_WARN("soak_attest", "fd not open; skipping line write");
        g_soak.failure_streak++;
        atomic_fetch_add(&g_soak.write_failures, 1);
    }
    pthread_mutex_unlock(&g_soak.lock);
}

/* ── Init / reset ────────────────────────────────────────────────── */

void soak_attestation_init(const char *datadir)
{
    pthread_mutex_lock(&g_soak.lock);
    if (g_soak.fd >= 0) {
        close(g_soak.fd);
        g_soak.fd = -1;
    }
    if (datadir && datadir[0]) {
        snprintf(g_soak.datadir, sizeof(g_soak.datadir), "%s", datadir);
        g_soak.initialized = true;
    } else {
        g_soak.initialized = false;
    }
    g_soak.file_bytes        = 0;
    g_soak.lines_since_fsync = 0;
    g_soak.failure_streak    = 0;
    pthread_mutex_unlock(&g_soak.lock);
}

void soak_attestation_reset_for_test(void)
{
    pthread_mutex_lock(&g_soak.lock);
    if (g_soak.fd >= 0) { close(g_soak.fd); g_soak.fd = -1; }
    g_soak.initialized        = false;
    g_soak.datadir[0]         = '\0';
    g_soak.file_bytes         = 0;
    g_soak.lines_since_fsync  = 0;
    g_soak.failure_streak     = 0;
    pthread_mutex_unlock(&g_soak.lock);
    atomic_store(&g_soak.lines_written, 0);
    atomic_store(&g_soak.last_ts,       0);
    atomic_store(&g_soak.last_healthy,  false);
    atomic_store(&g_soak.last_window_eligible, false);
    atomic_store(&g_soak.last_security_review_required, false);
    atomic_store(&g_soak.rotations,     0);
    atomic_store(&g_soak.write_failures, 0);
}

/* ── State dump (see CLAUDE.md "Adding state introspection") ───── */

bool soak_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    pthread_mutex_lock(&g_soak.lock);
    bool   initialized = g_soak.initialized;
    char   datadir[sizeof(g_soak.datadir)];
    snprintf(datadir, sizeof(datadir), "%s", g_soak.datadir);
    int64_t file_bytes = g_soak.file_bytes;
    pthread_mutex_unlock(&g_soak.lock);

    json_push_kv_bool(out, "initialized",    initialized);
    json_push_kv_str (out, "datadir",        datadir[0] ? datadir : "");
    json_push_kv_int (out, "lines_written",
                      atomic_load(&g_soak.lines_written));
    json_push_kv_int (out, "last_ts",
                      atomic_load(&g_soak.last_ts));
    json_push_kv_bool(out, "last_healthy",
                      atomic_load(&g_soak.last_healthy));
    json_push_kv_bool(out, "last_window_eligible",
                      atomic_load(&g_soak.last_window_eligible));
    json_push_kv_bool(out, "last_security_review_required",
                      atomic_load(&g_soak.last_security_review_required));
    json_push_kv_int (out, "rotations",
                      atomic_load(&g_soak.rotations));
    json_push_kv_int (out, "write_failures",
                      atomic_load(&g_soak.write_failures));
    json_push_kv_int (out, "file_bytes",     file_bytes);
    return true;
}
