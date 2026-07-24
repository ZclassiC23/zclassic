/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_subsystem_snapshot — the seqlock publish envelope (util/subsystem_snapshot.h).
 * Proves the two contract properties the diagnostic snapshot plane relies on:
 *
 *   1. Multi-field coherence under a hammering writer: a reader bracket that
 *      completes (read_ok == true) NEVER observes a torn multi-field snapshot;
 *      when it cannot get a coherent read it falls back to the LAST published
 *      (still-coherent) values labeled stale — never an empty body.
 *   2. The uniform staleness label reports {stale, age_us, last_publish_height,
 *      generation, warning_reason} correctly for never-published, fresh, and
 *      torn reads.
 */

#include "test/test_helpers.h"

#include "json/json.h"
#include "platform/time_compat.h"
#include "util/subsystem_snapshot.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SS_CHECK(name, expr) do {                     \
    printf("subsystem_snapshot: %s... ", (name));     \
    if (expr) { printf("OK\n"); }                     \
    else { printf("FAIL\n"); failures++; }            \
} while (0)

/* A two-field payload the writer keeps EQUAL (a == b) at all times; a coherent
 * read must therefore always see a == b. A torn read (writer mid-publish) would
 * see a != b — which the seqlock bracket must reject. */
struct hammer_payload {
    struct zcl_snapshot_env env;
    _Atomic int64_t a;
    _Atomic int64_t b;
};

struct hammer_ctl {
    struct hammer_payload *p;
    _Atomic int stop;
    _Atomic uint64_t writes;
};

static void *hammer_writer(void *arg)
{
    struct hammer_ctl *c = arg;
    int64_t v = 0;
    while (!atomic_load_explicit(&c->stop, memory_order_acquire)) {
        v++;
        zcl_snapshot_publish_begin(&c->p->env);
        atomic_store_explicit(&c->p->a, v, memory_order_relaxed);
        /* A deliberate gap between the two field stores widens the torn window
         * a naive (non-seqlock) reader would fall into. */
        for (volatile int spin = 0; spin < 8; spin++) { }
        atomic_store_explicit(&c->p->b, v, memory_order_relaxed);
        zcl_snapshot_publish_end(&c->p->env, v);
        atomic_fetch_add_explicit(&c->writes, 1, memory_order_relaxed);
    }
    return NULL;
}

/* One reader bracket (<= ZCL_SNAPSHOT_READ_MAX_RETRIES). On success the out
 * params hold a coherent pair; on failure they hold the last-attempt (possibly
 * torn) values and the caller must fall back to last-known. True iff coherent. */
static bool read_pair(struct hammer_payload *p, int64_t *a, int64_t *b)
{
    for (int i = 0; i < ZCL_SNAPSHOT_READ_MAX_RETRIES; i++) {
        uint64_t seq;
        if (!zcl_snapshot_read_try(&p->env, &seq))
            continue;  /* writer active — retry */
        *a = atomic_load_explicit(&p->a, memory_order_relaxed);
        *b = atomic_load_explicit(&p->b, memory_order_relaxed);
        if (zcl_snapshot_read_ok(&p->env, seq))
            return true;
    }
    return false;
}

static int case_concurrent_coherence(void)
{
    int failures = 0;
    struct hammer_payload p;
    memset(&p, 0, sizeof(p));
    struct zcl_snapshot_env init = ZCL_SNAPSHOT_ENV_INIT;
    p.env = init;

    /* Seed one coherent publish so the fallback always has last-known values. */
    zcl_snapshot_publish_begin(&p.env);
    atomic_store_explicit(&p.a, 0, memory_order_relaxed);
    atomic_store_explicit(&p.b, 0, memory_order_relaxed);
    zcl_snapshot_publish_end(&p.env, 0);

    struct hammer_ctl ctl = { .p = &p };
    atomic_store(&ctl.stop, 0);
    atomic_store(&ctl.writes, 0);

    pthread_t th;
    int rc = pthread_create(&th, NULL, hammer_writer, &ctl);
    SS_CHECK("concurrent: writer thread starts", rc == 0);
    if (rc != 0)
        return failures;

    int64_t coherent_reads = 0, torn_fallbacks = 0;
    int64_t last_a = 0, last_b = 0;  /* last-known-good fallback */
    bool ever_incoherent = false;    /* a coherent read that saw a != b */
    bool fallback_ever_incoherent = false;

    for (int i = 0; i < 200000; i++) {
        int64_t a = -1, b = -2;
        if (read_pair(&p, &a, &b)) {
            coherent_reads++;
            if (a != b)
                ever_incoherent = true;   /* MUST never happen */
            last_a = a;
            last_b = b;                   /* update last-known-good */
        } else {
            torn_fallbacks++;
            zcl_snapshot_note_torn(&p.env);
            /* Fallback path: serve last-known-good — which is always coherent. */
            if (last_a != last_b)
                fallback_ever_incoherent = true;
        }
    }

    atomic_store_explicit(&ctl.stop, 1, memory_order_release);
    pthread_join(th, NULL);

    SS_CHECK("concurrent: writer actually published",
             atomic_load(&ctl.writes) > 0);
    SS_CHECK("concurrent: some reads completed", coherent_reads > 0);
    SS_CHECK("concurrent: NO coherent read ever saw a torn (a!=b) pair",
             !ever_incoherent);
    SS_CHECK("concurrent: fallback last-known value is always coherent",
             !fallback_ever_incoherent);
    /* torn_reads_total must equal the number of times we fell back. */
    SS_CHECK("concurrent: torn_reads_total accounts every fallback",
             atomic_load_explicit(&p.env.torn_reads_total,
                                  memory_order_relaxed) ==
             (uint64_t)torn_fallbacks);
    printf("subsystem_snapshot: concurrent stats coherent=%lld torn_fallback=%lld writes=%llu\n",
           (long long)coherent_reads, (long long)torn_fallbacks,
           (unsigned long long)atomic_load(&ctl.writes));
    return failures;
}

/* Deterministic torn detection: leaving the env mid-publish (begin without end)
 * makes read_try report the writer is active; read_ok on a stale even seq is
 * false. */
static int case_deterministic_bracket(void)
{
    int failures = 0;
    struct zcl_snapshot_env env = ZCL_SNAPSHOT_ENV_INIT;
    uint64_t seq = 0;

    SS_CHECK("bracket: fresh env read_try succeeds (even seq)",
             zcl_snapshot_read_try(&env, &seq) && (seq & 1U) == 0);

    zcl_snapshot_publish_begin(&env);
    SS_CHECK("bracket: mid-publish read_try fails (odd seq)",
             !zcl_snapshot_read_try(&env, &seq));
    zcl_snapshot_publish_end(&env, 123);

    SS_CHECK("bracket: after publish read_try succeeds",
             zcl_snapshot_read_try(&env, &seq));
    SS_CHECK("bracket: read_ok true when no writer intervened",
             zcl_snapshot_read_ok(&env, seq));

    /* A publish between read_try and read_ok invalidates the sample. */
    uint64_t seq2 = 0;
    SS_CHECK("bracket: read_try before an intervening publish",
             zcl_snapshot_read_try(&env, &seq2));
    zcl_snapshot_publish_begin(&env);
    zcl_snapshot_publish_end(&env, 456);
    SS_CHECK("bracket: read_ok false after an intervening publish",
             !zcl_snapshot_read_ok(&env, seq2));

    return failures;
}

static const char *label_reason(const struct json_value *label)
{
    const struct json_value *wr = json_get(label, "warning_reason");
    return wr ? json_get_str(wr) : NULL;
}

static int case_label(void)
{
    int failures = 0;
    int64_t now = platform_time_monotonic_us();

    /* Never published: stale, warning_reason never_published, age -1, gen 0. */
    struct zcl_snapshot_env env = ZCL_SNAPSHOT_ENV_INIT;
    struct json_value label;
    json_init(&label);
    json_set_object(&label);
    zcl_snapshot_emit_label(&label, &env, /*torn=*/false, now);
    SS_CHECK("label(never): stale true",
             json_get_bool(json_get(&label, "stale")));
    SS_CHECK("label(never): generation 0",
             json_get_int(json_get(&label, "generation")) == 0);
    SS_CHECK("label(never): age_us -1",
             json_get_int(json_get(&label, "age_us")) == -1);
    { const char *r = label_reason(&label);
      SS_CHECK("label(never): warning_reason never_published",
               r && strcmp(r, "never_published") == 0); }
    json_free(&label);

    /* Fresh publish: not stale, ok, generation 1, last_publish_height records. */
    zcl_snapshot_publish_begin(&env);
    zcl_snapshot_publish_end(&env, 987654);
    json_init(&label);
    json_set_object(&label);
    zcl_snapshot_emit_label(&label, &env, /*torn=*/false,
                            platform_time_monotonic_us());
    SS_CHECK("label(fresh): stale false",
             !json_get_bool(json_get(&label, "stale")));
    SS_CHECK("label(fresh): generation 1",
             json_get_int(json_get(&label, "generation")) == 1);
    SS_CHECK("label(fresh): last_publish_height 987654",
             json_get_int(json_get(&label, "last_publish_height")) == 987654);
    { const char *r = label_reason(&label);
      SS_CHECK("label(fresh): warning_reason ok", r && strcmp(r, "ok") == 0); }
    json_free(&label);

    /* Torn read: even on a published env, torn=true labels the fallback stale. */
    json_init(&label);
    json_set_object(&label);
    zcl_snapshot_emit_label(&label, &env, /*torn=*/true,
                            platform_time_monotonic_us());
    SS_CHECK("label(torn): stale true",
             json_get_bool(json_get(&label, "stale")));
    { const char *r = label_reason(&label);
      SS_CHECK("label(torn): warning_reason snapshot_torn_read",
               r && strcmp(r, "snapshot_torn_read") == 0); }
    /* The fallback still carries the LAST published values, never empty. */
    SS_CHECK("label(torn): last_publish_height still 987654 (not empty)",
             json_get_int(json_get(&label, "last_publish_height")) == 987654);
    json_free(&label);

    return failures;
}

int test_subsystem_snapshot(void)
{
    int failures = 0;
    failures += case_deterministic_bracket();
    failures += case_label();
    failures += case_concurrent_coherence();
    if (failures == 0)
        printf("test_subsystem_snapshot: ALL PASSED\n");
    else
        printf("test_subsystem_snapshot: %d FAILURE(S)\n", failures);
    return failures;
}
