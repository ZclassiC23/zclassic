/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_operator_ux — the operator-UX native commands (wf/operator-ux):
 *   explain (pure topic composers), profile (in-process thread sampler),
 *   ops.producer.status (node-free producer datadir reader), and the brief
 *   status prose renderer. Each is exercised without a running node using
 *   fabricated fixtures / a synthetic datadir.
 */

#include "test/test_helpers.h"

#include "controllers/explain_native_handlers.h"
#include "config/consensus_state_producer_receipt.h"
#include "command/native_command.h"
#include "util/thread_profile.h"
#include "util/stage.h"
#include "storage/progress_store.h"
#include "platform/time_compat.h"
#include "json/json.h"

#include <pthread.h>
#include <string.h>
#include <sys/stat.h>

/* ── explain ───────────────────────────────────────────────────────── */

static int test_explain_sync_names_dominant_blocker(void)
{
    int failures = 0;
    TEST("explain sync names the dominant blocker on a known fixture") {
        /* Frontier: H*=100. */
        struct json_value frontier;
        json_init(&frontier);
        json_set_object(&frontier);
        json_push_kv_int(&frontier, "hstar", 100);
        json_push_kv_int(&frontier, "served_floor", 100);
        json_push_kv_str(&frontier, "hstar_next_primary_stage", "utxo_apply");
        json_push_kv_str(&frontier, "hstar_next_primary_kind", "DEPENDENCY");
        json_push_kv_str(&frontier, "hstar_next_primary_detail",
                         "anchor backfill gap");

        /* Chain: validated header tip 200 -> gap 100. */
        struct json_value chain;
        json_init(&chain);
        json_set_object(&chain);
        json_push_kv_int(&chain, "best_header_height", 200);

        /* One active blocker with a distinctive id. */
        struct json_value blockers, b;
        json_init(&blockers);
        json_set_array(&blockers);
        json_init(&b);
        json_set_object(&b);
        json_push_kv_str(&b, "id", "anchor_backfill_gap");
        json_push_kv_str(&b, "class", "PERMANENT");
        json_push_kv_str(&b, "reason", "missing historical shielded anchors");
        json_push_kv_int(&b, "fire_count", 3);
        json_push_back(&blockers, &b);
        json_free(&b);

        struct explain_inputs in = {
            .frontier = &frontier, .blockers = &blockers, .chain = &chain,
            .block_height = 100, .block_height_known = true,
        };
        struct json_value out;
        json_init(&out);
        explain_compose_sync(&in, &out);

        const char *text = json_get_str(json_get(&out, "text"));
        ASSERT(text != NULL);
        ASSERT(strstr(text, "anchor_backfill_gap") != NULL);
        ASSERT(strstr(text, "BEHIND") != NULL);
        ASSERT(json_get_int(json_get(&out, "gap")) == 100);
        ASSERT(strcmp(json_get_str(json_get(&out, "dominant_blocker")),
                      "anchor_backfill_gap") == 0);

        json_free(&out);
        json_free(&blockers);
        json_free(&chain);
        json_free(&frontier);
        PASS();
    } _test_next:;
    return failures;
}

static int test_explain_topics_and_health(void)
{
    int failures = 0;
    TEST("explain topic table is non-empty and blockers/health compose") {
        ASSERT(explain_topic_count() == 3);
        char csv[128];
        size_t n = explain_topics_csv(csv, sizeof(csv));
        ASSERT(n > 0);
        ASSERT(strstr(csv, "sync") != NULL);
        ASSERT(strstr(csv, "blockers") != NULL);
        ASSERT(strstr(csv, "health") != NULL);

        /* health compose with a fabricated healthcheck: unhealthy. */
        struct json_value health, checks, chk;
        json_init(&health);
        json_set_object(&health);
        json_push_kv_bool(&health, "healthy", false);
        json_push_kv_bool(&health, "serving", true);
        json_push_kv_int(&health, "memory_rss_mb", 4096);
        json_init(&checks);
        json_set_object(&checks);
        json_init(&chk);
        json_set_object(&chk);
        json_push_kv_bool(&chk, "ok", false);
        json_push_kv(&checks, "sync_lag", &chk);
        json_free(&chk);
        json_push_kv(&health, "checks", &checks);
        json_free(&checks);

        struct explain_inputs in = { .health = &health };
        struct json_value out;
        json_init(&out);
        explain_compose_health(&in, &out);
        const char *text = json_get_str(json_get(&out, "text"));
        ASSERT(text != NULL && strstr(text, "healthy: no") != NULL);
        ASSERT(strstr(text, "sync_lag") != NULL);
        ASSERT(json_get_int(json_get(&out, "unhealthy_checks")) == 1);
        json_free(&out);

        /* blockers compose with no active blockers. */
        struct json_value empty;
        json_init(&empty);
        json_set_array(&empty);
        struct explain_inputs in2 = { .blockers = &empty };
        struct json_value out2;
        json_init(&out2);
        explain_compose_blockers(&in2, &out2);
        const char *t2 = json_get_str(json_get(&out2, "text"));
        ASSERT(t2 != NULL && strstr(t2, "dominant: none") != NULL);
        json_free(&out2);
        json_free(&empty);
        json_free(&health);
        PASS();
    } _test_next:;
    return failures;
}

/* ── profile ───────────────────────────────────────────────────────── */

static void *tp_racer(void *arg)
{
    (void)arg;
    platform_sleep_ms(20); /* exit mid-window so the tid races the sample */
    return NULL;
}

static int test_profile_samples_threads(void)
{
    int failures = 0;
    TEST("profile returns >=1 thread and a verdict, never crashes on a race") {
        pthread_t racer;
        int rc = pthread_create(&racer, NULL, tp_racer, NULL);
        ASSERT(rc == 0);

        struct thread_profile_opts opts = { .sample_ms = 120, .top_n = 8 };
        struct json_value out;
        json_init(&out);
        bool ok = thread_profile_sample(&opts, &out);
        pthread_join(racer, NULL);

        ASSERT(ok);
        ASSERT(json_get_int(json_get(&out, "sampled_threads")) >= 1);
        const char *verdict = json_get_str(json_get(&out, "verdict"));
        ASSERT(verdict != NULL && verdict[0] != '\0');
        const struct json_value *threads = json_get(&out, "threads");
        ASSERT(threads != NULL && threads->type == JSON_ARR);
        ASSERT(threads->num_children >= 1);
        json_free(&out);
        PASS();
    } _test_next:;
    return failures;
}

/* ── producer.status ───────────────────────────────────────────────── */

static int test_producer_status_synthetic(void)
{
    int failures = 0;
    TEST("producer status reads a synthetic progress.kv datadir") {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "operator_ux_producer", "seed");
        mkdir(dir, 0700);

        ASSERT(progress_store_open(dir));
        sqlite3 *db = progress_store_db();
        ASSERT(db != NULL);
        ASSERT(stage_set_named_cursor(db, "utxo_apply", 12345));
        ASSERT(stage_set_named_cursor(db, "tip_finalize", 12300));
        progress_store_close();

        struct producer_status_read st;
        char err[256];
        ASSERT(consensus_state_producer_status_read(dir, &st, err,
                                                     sizeof(err)));
        ASSERT(st.progress_kv_present);
        ASSERT(st.utxo_apply_cursor == 12345);
        ASSERT(st.tip_finalize_cursor == 12300);
        /* No producer session/receipt seeded -> absent. */
        ASSERT(!st.session_open);
        ASSERT(!st.receipt_finalized);

        test_cleanup_tmpdir(dir);
        PASS();
    } _test_next:;
    return failures;
}

static int test_producer_status_absent(void)
{
    int failures = 0;
    TEST("producer status on a datadir with no progress.kv is not an error") {
        char dir[256];
        test_fmt_tmpdir(dir, sizeof(dir), "operator_ux_producer", "empty");
        mkdir(dir, 0700);

        struct producer_status_read st;
        char err[256];
        ASSERT(consensus_state_producer_status_read(dir, &st, err,
                                                     sizeof(err)));
        ASSERT(!st.progress_kv_present);
        ASSERT(st.utxo_apply_cursor == -1);

        test_cleanup_tmpdir(dir);
        PASS();
    } _test_next:;
    return failures;
}

/* ── brief status prose ────────────────────────────────────────────── */

static int test_brief_status_contains_gap(void)
{
    int failures = 0;
    TEST("brief status prose contains the gap number") {
        struct json_value d;
        json_init(&d);
        json_set_object(&d);
        json_push_kv_int(&d, "served_height", 100);
        json_push_kv_int(&d, "header_height", 142);
        json_push_kv_int(&d, "gap", 42);
        json_push_kv_str(&d, "sync_state", "syncing");
        json_push_kv_bool(&d, "serving", true);
        json_push_kv_bool(&d, "healthy", false);
        json_push_kv_int(&d, "peer_count", 8);
        json_push_kv_str(&d, "primary_blocker", "none");
        json_push_kv_int(&d, "tip_advance_age_seconds", 5);

        char buf[1024];
        zcl_native_status_brief_render(&d, buf, sizeof(buf));
        ASSERT(strstr(buf, "42") != NULL);
        ASSERT(strstr(buf, "BEHIND") != NULL);
        ASSERT(strstr(buf, "peers: 8") != NULL);
        json_free(&d);
        PASS();
    } _test_next:;
    return failures;
}

int test_operator_ux(void)
{
    int failures = 0;
    failures += test_explain_sync_names_dominant_blocker();
    failures += test_explain_topics_and_health();
    failures += test_profile_samples_threads();
    failures += test_producer_status_synthetic();
    failures += test_producer_status_absent();
    failures += test_brief_status_contains_gap();
    printf("=== operator_ux: %d failures ===\n", failures);
    return failures;
}
