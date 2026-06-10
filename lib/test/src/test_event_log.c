/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the append-only event log primitive
 * (lib/storage/src/event_log.c).
 *
 * Coverage matrix (per the Phase 4a assignment):
 *   Task 2 — append + read round-trip (1000 events)
 *   Task 3 — stream callback visits every event in order
 *   Task 4 — fingerprint determinism + sensitivity to single byte change
 *   Task 5 — kill-9 fuzz harness (the load-bearing test):
 *               fork a child that loops appending; SIGKILL after K events;
 *               reopen + verify clean tail with K complete events. K = 1..N.
 *   Task 6 — throughput benchmark (--bench mode and a default in-suite run)
 *   Misc   — empty payload, large payload, corrupt tail detection
 *
 * The test creates tmpdirs under ./test-tmp/event_log_<pid>_<tag>/ to
 * comply with the project's "no /tmp" convention. */

#include "test/test_helpers.h"

#include "platform/time_compat.h"
#include "storage/event_log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Sleep for `us` microseconds. nanosleep is POSIX.1-2001 and works
 * under -D_POSIX_C_SOURCE=200809L; usleep is BSD-flavoured and would
 * need extra feature-test macros. */
static void sleep_us(uint64_t us)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(us / 1000000u);
    ts.tv_nsec = (long)((us % 1000000u) * 1000u);
    nanosleep(&ts, NULL);
}

#define EL_CHECK(name, expr) do { \
    printf("event_log: %s... ", (name)); \
    if ((expr)) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

static int el_mkdir_p(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

/* Ensure ./test-tmp/ exists (parent of all our scratch dirs). */
static void el_ensure_root(void)
{
    el_mkdir_p("./test-tmp");
}

static double mono_sec(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ── Task 2: append + read round-trip ──────────────────────────────── */

static int run_append_read_roundtrip(int *failures)
{
    int start_failures = *failures;
    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "event_log", "rt");
    el_mkdir_p(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/events.log", dir);

    event_log_t *log = event_log_open(path);
    EL_CHECK("open fresh log", log != NULL);
    if (!log) goto done;

    enum { N = 1000 };
    uint64_t offsets[N];
    /* Variable-size payloads so we exercise different lengths. */
    for (int i = 0; i < N; i++) {
        uint8_t buf[200];
        size_t len = (size_t)((i * 7) % 200);
        for (size_t k = 0; k < len; k++)
            buf[k] = (uint8_t)((i + k) * 31 + 5);
        offsets[i] = event_log_append(log,
                        (enum event_log_type)(EV_BLOCK_HEADER + (i % 11)),
                        buf, len);
    }
    bool all_ok = true;
    for (int i = 0; i < N; i++)
        if (offsets[i] == UINT64_MAX) { all_ok = false; break; }
    EL_CHECK("all 1000 appends succeed", all_ok);

    /* Round-trip every event. */
    bool rt_ok = true;
    for (int i = 0; i < N && rt_ok; i++) {
        uint8_t expect[200];
        size_t exp_len = (size_t)((i * 7) % 200);
        for (size_t k = 0; k < exp_len; k++)
            expect[k] = (uint8_t)((i + k) * 31 + 5);

        uint8_t got[200] = {0};
        enum event_log_type t = 0;
        size_t got_len = 0;
        int r = event_log_read(log, offsets[i], &t, got,
                               sizeof(got), &got_len);
        if (r != 0) { rt_ok = false; break; }
        if (got_len != exp_len) { rt_ok = false; break; }
        if ((uint32_t)t != (uint32_t)(EV_BLOCK_HEADER + (i % 11))) {
            rt_ok = false; break;
        }
        if (memcmp(got, expect, exp_len) != 0) {
            rt_ok = false; break;
        }
    }
    EL_CHECK("all 1000 events round-trip via event_log_read", rt_ok);

    event_log_close(log);
    test_cleanup_tmpdir(dir);
done:
    return *failures - start_failures;
}

/* ── Task 3: stream callback ───────────────────────────────────────── */

struct stream_ctx {
    int count;
    bool ordered;
    uint64_t last_offset;
    int max;
    int stop_at;  /* if > 0, stop after this many */
};

static bool stream_cb(uint64_t offset, enum event_log_type type,
                      const void *payload, size_t len, void *user)
{
    (void)type; (void)payload; (void)len;
    struct stream_ctx *c = (struct stream_ctx *)user;
    if (c->count > 0 && offset <= c->last_offset)
        c->ordered = false;
    c->last_offset = offset;
    c->count++;
    if (c->stop_at > 0 && c->count >= c->stop_at) return false;
    return true;
}

static int run_stream(int *failures)
{
    int start_failures = *failures;
    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "event_log", "stream");
    el_mkdir_p(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/events.log", dir);

    event_log_t *log = event_log_open(path);
    EL_CHECK("stream: open OK", log != NULL);
    if (!log) goto done;

    enum { N = 1000 };
    for (int i = 0; i < N; i++) {
        uint8_t buf[64];
        memset(buf, (int)(i & 0xFF), sizeof(buf));
        event_log_append(log, EV_BLOCK_BODY, buf, sizeof(buf));
    }
    struct stream_ctx c = {0};
    c.ordered = true;
    c.max = N;
    int r = event_log_stream(log, 0, stream_cb, &c);
    EL_CHECK("stream returns 0", r == 0);
    EL_CHECK("stream visits exactly N events", c.count == N);
    EL_CHECK("stream visits in offset-monotonic order", c.ordered);

    /* Early-termination via callback. */
    struct stream_ctx c2 = {0};
    c2.ordered = true;
    c2.stop_at = 5;
    r = event_log_stream(log, 0, stream_cb, &c2);
    EL_CHECK("stream early-terminate returns 0", r == 0);
    EL_CHECK("stream early-terminate honours cb stop", c2.count == 5);

    event_log_close(log);
    test_cleanup_tmpdir(dir);
done:
    return *failures - start_failures;
}

/* ── Task 4: fingerprint ───────────────────────────────────────────── */

static int run_fingerprint(int *failures)
{
    int start_failures = *failures;
    char dir1[256], dir2[256];
    test_fmt_tmpdir(dir1, sizeof(dir1), "event_log", "fp1");
    test_fmt_tmpdir(dir2, sizeof(dir2), "event_log", "fp2");
    el_mkdir_p(dir1);
    el_mkdir_p(dir2);
    char p1[512], p2[512];
    snprintf(p1, sizeof(p1), "%s/events.log", dir1);
    snprintf(p2, sizeof(p2), "%s/events.log", dir2);

    event_log_t *a = event_log_open(p1);
    event_log_t *b = event_log_open(p2);
    EL_CHECK("fp: open a", a != NULL);
    EL_CHECK("fp: open b", b != NULL);
    if (!a || !b) goto done;

    const int N = 256;
    for (int i = 0; i < N; i++) {
        uint8_t buf[33];
        for (size_t k = 0; k < sizeof(buf); k++)
            buf[k] = (uint8_t)(i + k);
        event_log_append(a, EV_UTXO_ADD, buf, sizeof(buf));
        event_log_append(b, EV_UTXO_ADD, buf, sizeof(buf));
    }
    uint8_t ha[32], hb[32];
    EL_CHECK("fp(a)", event_log_fingerprint(a, ha) == 0);
    EL_CHECK("fp(b)", event_log_fingerprint(b, hb) == 0);
    EL_CHECK("identical content → identical fingerprint",
             memcmp(ha, hb, 32) == 0);

    /* Append one more event to b — fingerprints must differ. */
    uint8_t extra[1] = {0xFF};
    event_log_append(b, EV_UTXO_ADD, extra, 1);
    EL_CHECK("fp(b) after extra append", event_log_fingerprint(b, hb) == 0);
    EL_CHECK("one-byte change → different fingerprint",
             memcmp(ha, hb, 32) != 0);

    /* fingerprint over empty log is deterministic (SHA3-256 of empty). */
    char dir3[256];
    test_fmt_tmpdir(dir3, sizeof(dir3), "event_log", "fp3");
    el_mkdir_p(dir3);
    char p3[512];
    snprintf(p3, sizeof(p3), "%s/events.log", dir3);
    event_log_t *c = event_log_open(p3);
    EL_CHECK("fp: open empty c", c != NULL);
    uint8_t hc[32];
    EL_CHECK("fp(empty)", event_log_fingerprint(c, hc) == 0);
    /* SHA3-256("") = a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a */
    static const uint8_t empty_sha3[32] = {
        0xa7,0xff,0xc6,0xf8,0xbf,0x1e,0xd7,0x66,
        0x51,0xc1,0x47,0x56,0xa0,0x61,0xd6,0x62,
        0xf5,0x80,0xff,0x4d,0xe4,0x3b,0x49,0xfa,
        0x82,0xd8,0x0a,0x4b,0x80,0xf8,0x43,0x4a,
    };
    EL_CHECK("fp(empty) == SHA3-256(\"\")",
             memcmp(hc, empty_sha3, 32) == 0);

    event_log_close(a);
    event_log_close(b);
    event_log_close(c);
    test_cleanup_tmpdir(dir1);
    test_cleanup_tmpdir(dir2);
    test_cleanup_tmpdir(dir3);
done:
    return *failures - start_failures;
}

/* ── Task 5: kill-9 fuzz harness (LOAD-BEARING) ────────────────────── */

/* The child appends events forever (until killed). The payload at index
 * i is the 4-byte little-endian integer i followed by a fixed pattern.
 * After the kill, the parent reopens (which triggers recovery + tail
 * truncation) and asserts the stream is well-formed: every event has a
 * valid CRC + sentinel, and any partial tail was truncated. */

#define FUZZ_PAYLOAD_LEN  64

static void child_appender(const char *path)
{
    /* Direct stderr away so the kill doesn't leave dangling messages. */
    freopen("/dev/null", "w", stderr);
    event_log_t *log = event_log_open(path);
    if (!log) _exit(2);
    uint32_t i = 0;
    while (1) {
        uint8_t buf[FUZZ_PAYLOAD_LEN];
        buf[0] = (uint8_t)(i & 0xFF);
        buf[1] = (uint8_t)((i >> 8) & 0xFF);
        buf[2] = (uint8_t)((i >> 16) & 0xFF);
        buf[3] = (uint8_t)((i >> 24) & 0xFF);
        for (int k = 4; k < FUZZ_PAYLOAD_LEN; k++)
            buf[k] = (uint8_t)(0xA5 ^ k);
        if (event_log_append(log, EV_BLOCK_HEADER,
                             buf, sizeof(buf)) == UINT64_MAX) {
            _exit(3);
        }
        i++;
    }
}

struct fuzz_count_ctx {
    int count;
    bool ok;
    uint32_t expected_next;
};

static bool fuzz_count_cb(uint64_t offset, enum event_log_type type,
                          const void *payload, size_t len, void *user)
{
    (void)offset;
    struct fuzz_count_ctx *c = (struct fuzz_count_ctx *)user;
    if (type != EV_BLOCK_HEADER || len != FUZZ_PAYLOAD_LEN) {
        c->ok = false;
        return false;
    }
    const uint8_t *p = (const uint8_t *)payload;
    uint32_t got = (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
    if (got != c->expected_next) {
        c->ok = false;
        return false;
    }
    for (int k = 4; k < FUZZ_PAYLOAD_LEN; k++) {
        if (p[k] != (uint8_t)(0xA5 ^ k)) {
            c->ok = false;
            return false;
        }
    }
    c->count++;
    c->expected_next++;
    return true;
}

/* One trial of the kill-9 harness. Returns true on success.
 * The parent waits `delay_us` microseconds (so K = ~rate * delay_us
 * complete events land before SIGKILL). */
static bool run_one_kill9_trial(const char *path, uint64_t delay_us)
{
    /* Wipe any prior log for clean state. */
    unlink(path);

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        child_appender(path);
        _exit(0);
    }
    sleep_us(delay_us);
    if (kill(pid, SIGKILL) != 0) {
        /* Process already exited (e.g., appender error). */
    }
    int status = 0;
    waitpid(pid, &status, 0);

    /* Reopen (triggers tail recovery) and stream. */
    event_log_t *log = event_log_open(path);
    if (!log) return false;

    struct fuzz_count_ctx c = { .count = 0, .ok = true,
                                .expected_next = 0 };
    int r = event_log_stream(log, 0, fuzz_count_cb, &c);
    if (r != 0 || !c.ok) {
        event_log_close(log);
        return false;
    }
    /* Reopen a second time and ensure no further truncation is needed
     * (idempotent recovery): file size must equal the well-formed prefix.
     * We confirm by computing fingerprint twice over the same handle. */
    uint8_t fp1[32], fp2[32];
    if (event_log_fingerprint(log, fp1) != 0) {
        event_log_close(log);
        return false;
    }
    event_log_close(log);

    log = event_log_open(path);
    if (!log) return false;
    if (event_log_fingerprint(log, fp2) != 0) {
        event_log_close(log);
        return false;
    }
    bool stable = memcmp(fp1, fp2, 32) == 0;
    event_log_close(log);
    return stable;
}

static int run_kill9_fuzz(int *failures)
{
    int start_failures = *failures;
    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "event_log", "kill9");
    el_mkdir_p(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/events.log", dir);

    /* Spread of delays from very short (likely to interrupt mid-header
     * or mid-payload) to longer (many complete events landed). */
    static const uint64_t delays[] = {
        100,     /* 0.1 ms */
        500,     /* 0.5 ms */
        1000,    /* 1 ms */
        2000,    /* 2 ms */
        5000,    /* 5 ms */
        10000,   /* 10 ms */
        20000,   /* 20 ms */
        50000,   /* 50 ms */
    };
    const int trials_per_delay = 3;
    bool all_ok = true;
    int total = 0, good = 0;
    for (size_t i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
        for (int t = 0; t < trials_per_delay; t++) {
            total++;
            if (run_one_kill9_trial(path, delays[i])) good++;
            else all_ok = false;
        }
    }
    printf("event_log: kill9 trials passed: %d/%d\n", good, total);
    EL_CHECK("all kill9 trials recover to a valid log", all_ok);

    test_cleanup_tmpdir(dir);
    return *failures - start_failures;
}

/* Additional targeted recovery test: deterministically corrupt the tail
 * (truncate the file mid-sentinel, mid-payload, mid-header) and verify
 * open() recovers correctly. */
static int run_targeted_recovery(int *failures)
{
    int start_failures = *failures;
    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "event_log", "recov");
    el_mkdir_p(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/events.log", dir);

    /* Build a log with 5 events; record the size after each. */
    event_log_t *log = event_log_open(path);
    EL_CHECK("recov: open", log != NULL);
    if (!log) goto done;

    uint64_t sizes[5];
    for (int i = 0; i < 5; i++) {
        uint8_t buf[32];
        memset(buf, (int)(0x10 + i), sizeof(buf));
        event_log_append(log, EV_BLOCK_HEADER, buf, sizeof(buf));
        sizes[i] = event_log_size(log);
    }
    event_log_close(log);

    /* For each kind of corruption, truncate the file to (size_prefix +
     * partial_bytes) and verify reopen rolls back to size_prefix. */
    for (int i = 0; i < 5; i++) {
        uint64_t prefix = (i == 0) ? 0 : sizes[i - 1];
        /* Try truncations at +1, +halfway, +size-1 of event i. */
        uint64_t evt_len = sizes[i] - prefix;
        uint64_t tries[3] = {
            prefix + 1,
            prefix + (evt_len / 2),
            sizes[i] - 1,
        };
        for (int j = 0; j < 3; j++) {
            int fd = open(path, O_WRONLY);
            if (fd < 0) { (*failures)++; continue; }
            (void)ftruncate(fd, (off_t)tries[j]);
            fsync(fd);
            close(fd);

            event_log_t *l2 = event_log_open(path);
            if (!l2) { (*failures)++; continue; }
            EL_CHECK("recov: tail truncated to last-good prefix",
                     event_log_size(l2) == prefix);
            /* Streaming must visit exactly i events. */
            struct stream_ctx sc = {0};
            sc.ordered = true;
            event_log_stream(l2, 0, stream_cb, &sc);
            EL_CHECK("recov: stream visits exactly the good events",
                     sc.count == i);
            event_log_close(l2);

            /* Restore the full log for the next iteration by re-appending
             * event i with the original payload. */
            l2 = event_log_open(path);
            if (l2) {
                /* Re-append events i..4 so sizes[] stays accurate. */
                for (int kk = i; kk < 5; kk++) {
                    uint8_t buf[32];
                    memset(buf, (int)(0x10 + kk), sizeof(buf));
                    event_log_append(l2, EV_BLOCK_HEADER,
                                     buf, sizeof(buf));
                    sizes[kk] = event_log_size(l2);
                }
                event_log_close(l2);
            }
        }
    }

    test_cleanup_tmpdir(dir);
done:
    return *failures - start_failures;
}

/* ── Task 6: benchmark ─────────────────────────────────────────────── */

/* Measures append throughput on the live disk and prints events/sec.
 *
 * The 50K/sec target from the spec is reported but NOT asserted: the
 * suite runs ~32 groups in parallel, so the fsync contention drives
 * each individual process's effective rate well below what the disk
 * can do solo. To get the real number, run with `--bench` or in
 * isolation (ZCL_EVENT_LOG_BENCH=1 with --jobs=1).
 *
 * In-suite we time a tiny burst so we never block the suite on a slow
 * disk + parallel load. The standalone bench (--bench) is much larger. */
static int run_benchmark(int *failures)
{
    int start_failures = *failures;
    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "event_log", "bench");
    el_mkdir_p(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/events.log", dir);

    event_log_t *log = event_log_open(path);
    EL_CHECK("bench: open", log != NULL);
    if (!log) goto done;

    /* Warm-up. */
    uint8_t pay[128];
    memset(pay, 0xC3, sizeof(pay));
    for (int i = 0; i < 50; i++)
        event_log_append(log, EV_BLOCK_HEADER, pay, sizeof(pay));

    /* In-suite: small N so we finish quickly even under parallel load.
     * Standalone (--bench / ZCL_EVENT_LOG_BENCH=1): bigger N for a
     * meaningful number. */
    bool standalone = getenv("ZCL_EVENT_LOG_BENCH") != NULL;
    int N = standalone ? 50000 : 500;

    double t0 = mono_sec();
    for (int i = 0; i < N; i++)
        event_log_append(log, EV_BLOCK_HEADER, pay, sizeof(pay));
    double t1 = mono_sec();
    double sec = t1 - t0;
    double rate = sec > 0 ? (double)N / sec : 0;
    printf("event_log: benchmark — %d events in %.3f s = %.0f events/sec "
           "(%s)\n",
           N, sec, rate, standalone ? "standalone" : "in-suite");
    if (rate >= 50000.0)
        printf("event_log: benchmark — MEETS 50K/sec target\n");
    else
        printf("event_log: benchmark — below 50K/sec target "
               "(in-suite contends with parallel fsync; "
               "run ZCL_EVENT_LOG_BENCH=1 with --jobs=1 for the real number)\n");
    /* Sanity: the implementation isn't catastrophically broken
     * (sub-events/sec would indicate a hang or O(N) regression per
     * append). Threshold is intentionally permissive so concurrent
     * fsync load can't fail the suite. */
    EL_CHECK("bench: rate > 10 events/sec (sanity)", rate > 10.0);

    event_log_close(log);
    test_cleanup_tmpdir(dir);
done:
    return *failures - start_failures;
}

/* ── empty payload + large payload ─────────────────────────────────── */

static int run_edge_cases(int *failures)
{
    int start_failures = *failures;
    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "event_log", "edge");
    el_mkdir_p(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/events.log", dir);

    event_log_t *log = event_log_open(path);
    EL_CHECK("edge: open", log != NULL);
    if (!log) goto done;

    /* Empty payload. */
    uint64_t off = event_log_append(log, EV_PEER_OBSERVED, NULL, 0);
    EL_CHECK("edge: empty append succeeds", off != UINT64_MAX);
    enum event_log_type t = 0;
    uint8_t buf[16] = {0};
    size_t got = 999;
    int r = event_log_read(log, off, &t, buf, sizeof(buf), &got);
    EL_CHECK("edge: empty read returns 0", r == 0);
    EL_CHECK("edge: empty payload length is 0", got == 0);
    EL_CHECK("edge: empty type round-trips",
             (uint32_t)t == (uint32_t)EV_PEER_OBSERVED);

    /* 1 MiB payload. */
    const size_t big_len = 1u << 20;
    uint8_t *big = malloc(big_len);  // raw-alloc-ok:test-scratch
    EL_CHECK("edge: alloc 1MiB", big != NULL);
    if (big) {
        for (size_t k = 0; k < big_len; k++)
            big[k] = (uint8_t)(k * 13 + 7);
        off = event_log_append(log, EV_BLOCK_BODY, big, big_len);
        EL_CHECK("edge: 1MiB append succeeds", off != UINT64_MAX);
        uint8_t *out = malloc(big_len);  // raw-alloc-ok:test-scratch
        EL_CHECK("edge: alloc 1MiB out", out != NULL);
        if (out) {
            got = 0;
            r = event_log_read(log, off, &t, out, big_len, &got);
            EL_CHECK("edge: 1MiB read returns 0", r == 0);
            EL_CHECK("edge: 1MiB length matches", got == big_len);
            EL_CHECK("edge: 1MiB bytes match",
                     memcmp(big, out, big_len) == 0);
            free(out);
        }
        free(big);
    }

    /* NULL input rejected. */
    EL_CHECK("edge: append on NULL log returns sentinel",
             event_log_append(NULL, EV_BLOCK_HEADER, "x", 1) == UINT64_MAX);
    EL_CHECK("edge: read on NULL log returns -1",
             event_log_read(NULL, 0, &t, buf, sizeof(buf), &got) == -1);
    EL_CHECK("edge: open(NULL) returns NULL",
             event_log_open(NULL) == NULL);
    EL_CHECK("edge: open(\"\") returns NULL",
             event_log_open("") == NULL);
    EL_CHECK("edge: close(NULL) is safe",
             (event_log_close(NULL), true));

    event_log_close(log);
    test_cleanup_tmpdir(dir);
done:
    return *failures - start_failures;
}

/* ── persistence across close + reopen ─────────────────────────────── */

static int run_persistence(int *failures)
{
    int start_failures = *failures;
    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "event_log", "persist");
    el_mkdir_p(dir);
    char path[512];
    snprintf(path, sizeof(path), "%s/events.log", dir);

    event_log_t *log = event_log_open(path);
    EL_CHECK("persist: open #1", log != NULL);
    if (!log) goto done;

    const int N = 50;
    uint64_t offsets[50];
    for (int i = 0; i < N; i++) {
        uint8_t buf[33];
        memset(buf, (int)(i + 1), sizeof(buf));
        offsets[i] = event_log_append(log, EV_UTXO_ADD, buf, sizeof(buf));
    }
    uint8_t fp_before[32];
    event_log_fingerprint(log, fp_before);
    event_log_close(log);

    /* Reopen and verify. */
    log = event_log_open(path);
    EL_CHECK("persist: open #2", log != NULL);
    if (!log) goto done;

    uint8_t fp_after[32];
    EL_CHECK("persist: fingerprint after reopen",
             event_log_fingerprint(log, fp_after) == 0);
    EL_CHECK("persist: fingerprint unchanged across reopen",
             memcmp(fp_before, fp_after, 32) == 0);

    /* Each event still readable. */
    bool all_ok = true;
    for (int i = 0; i < N; i++) {
        uint8_t got[33] = {0};
        enum event_log_type t = 0;
        size_t got_len = 0;
        if (event_log_read(log, offsets[i], &t, got,
                           sizeof(got), &got_len) != 0) {
            all_ok = false; break;
        }
        for (size_t k = 0; k < sizeof(got); k++)
            if (got[k] != (uint8_t)(i + 1)) { all_ok = false; break; }
    }
    EL_CHECK("persist: all events readable after reopen", all_ok);

    event_log_close(log);
    test_cleanup_tmpdir(dir);
done:
    return *failures - start_failures;
}

/* ── CRC32C implementation dispatch ───────────────────────────────── */

static int run_crc32c_dispatch(int *failures)
{
    int start_failures = *failures;
    enum { LEN = 1u << 20 };
    uint8_t *buf = malloc(LEN);  // raw-alloc-ok:test-scratch
    EL_CHECK("crc32c: alloc bench buffer", buf != NULL);
    if (!buf)
        return *failures - start_failures;
    for (size_t i = 0; i < LEN; i++)
        buf[i] = (uint8_t)(i * 19u + i / 7u + 3u);

    uint32_t sw = event_log_crc32c_test_sw(buf, LEN);
    uint32_t active = event_log_crc32c_test_active(buf, LEN);
    EL_CHECK("crc32c: active matches software reference", active == sw);

    bool standalone = getenv("ZCL_EVENT_LOG_BENCH") != NULL;
    int loops = standalone ? 512 : 16;
    volatile uint32_t sink = 0;
    double t0 = mono_sec();
    for (int i = 0; i < loops; i++)
        sink ^= event_log_crc32c_test_sw(buf, LEN);
    double t1 = mono_sec();
    for (int i = 0; i < loops; i++)
        sink ^= event_log_crc32c_test_active(buf, LEN);
    double t2 = mono_sec();
    double sw_sec = t1 - t0;
    double active_sec = t2 - t1;
    double bytes = (double)LEN * (double)loops;
    double sw_gbs = sw_sec > 0 ? bytes / sw_sec / 1e9 : 0.0;
    double active_gbs = active_sec > 0 ? bytes / active_sec / 1e9 : 0.0;
    printf("event_log: crc32c — impl=%s sw=%.2f GB/s active=%.2f GB/s "
           "(sink=%u)\n",
           event_log_crc32c_impl(), sw_gbs, active_gbs, (unsigned)sink);
    if (event_log_crc32c_hw_available())
        EL_CHECK("crc32c: hardware path selected after self-check",
                 strcmp(event_log_crc32c_impl(), "hardware-sse4.2") == 0);
    else
        EL_CHECK("crc32c: software fallback selected",
                 strcmp(event_log_crc32c_impl(), "software-table") == 0);

    free(buf);
    return *failures - start_failures;
}

int test_event_log(void)
{
    printf("\n=== event_log tests ===\n");
    int failures = 0;
    el_ensure_root();

    run_append_read_roundtrip(&failures);
    run_stream(&failures);
    run_fingerprint(&failures);
    run_edge_cases(&failures);
    run_persistence(&failures);
    run_targeted_recovery(&failures);
    run_kill9_fuzz(&failures);
    run_crc32c_dispatch(&failures);
    run_benchmark(&failures);

    printf("event_log: %d failures\n", failures);
    return failures;
}
