/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * reducer_stage_profile — bounded reducer hot-stage work/timing diagnostics. */

#include "util/reducer_stage_profile.h"

#include "json/json.h"
#include "util/stage.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

#define PROFILE_SCHEMA "zcl.reducer_stage_profile.v1"
#define PROFILE_HIST_BUCKETS 48u

struct profile_domain {
    _Atomic uint64_t cumulative[RPF_FIELD_COUNT];
    _Atomic uint64_t last_batch[RPF_FIELD_COUNT];
    _Atomic uint64_t histogram[RPF_FIELD_COUNT][PROFILE_HIST_BUCKETS];
    _Atomic uint64_t last_histogram[RPF_FIELD_COUNT][PROFILE_HIST_BUCKETS];
    _Atomic uint64_t present;
    _Atomic uint64_t last_present;
    _Atomic uint64_t generation;
    pthread_mutex_t rollover_lock;
};

static struct profile_domain g_profiles[REDUCER_PROFILE_DOMAIN_COUNT] = {
    [REDUCER_PROFILE_BODY_PERSIST] = {
        .rollover_lock = PTHREAD_MUTEX_INITIALIZER,
    },
    [REDUCER_PROFILE_SCRIPT_VALIDATE] = {
        .rollover_lock = PTHREAD_MUTEX_INITIALIZER,
    },
};

static const char *const g_field_names[RPF_FIELD_COUNT] = {
    [RPF_BLOCKS] = "blocks",
    [RPF_TOTAL_US] = "total_us",
    [RPF_UPSTREAM_US] = "upstream_cursor_log_us",
    [RPF_CACHE_HITS] = "parsed_cache_hits",
    [RPF_CACHE_MISSES] = "parsed_cache_misses",
    [RPF_CACHE_PROBES] = "parsed_cache_lookup_probes",
    [RPF_CACHE_LOCK_WAIT_US] = "parsed_cache_lock_wait_us",
    [RPF_DISK_READ_US] = "parsed_disk_read_us",
    [RPF_PARSE_US] = "parsed_parse_us",
    [RPF_DEEP_CLONES] = "parsed_deep_clones",
    [RPF_DEEP_CLONE_BYTES] = "parsed_deep_clone_estimated_bytes",
    [RPF_BLOCK_HASH_US] = "block_hash_verify_us",
    [RPF_MERKLE_US] = "merkle_verify_us",
    [RPF_MERKLE_ALLOCS] = "merkle_temporary_allocations",
    [RPF_MERKLE_BYTES] = "merkle_temporary_bytes",
    [RPF_EVENT_ENCODE_US] = "block_event_wire_encode_us",
    [RPF_EVENT_APPEND_US] = "block_event_append_us",
    [RPF_CREATED_INDEX_BLOCKS] = "created_index_blocks",
    [RPF_CREATED_INDEX_TXS] = "created_index_transactions",
    [RPF_CREATED_INDEX_OUTPUTS] = "created_index_outputs",
    [RPF_CREATED_INDEX_PREPARES] = "created_index_statement_prepares",
    [RPF_CREATED_INDEX_STEPS] = "created_index_sqlite_steps",
    [RPF_CREATED_INDEX_US] = "created_index_us",
    [RPF_CONTEXTUAL_US] = "contextual_gate_us",
    [RPF_TX_PRECOMPUTE_US] = "transaction_precompute_us",
    [RPF_JOB_ARRAY_ALLOCS] = "job_array_allocations",
    [RPF_JOB_ARRAY_BYTES] = "job_array_bytes",
    [RPF_PREVOUT_CREATED_LOOKUPS] = "prevout_created_index_lookups",
    [RPF_PREVOUT_COINS_FALLBACKS] = "prevout_coins_kv_fallbacks",
    [RPF_PREVOUT_PREPARES] = "prevout_statement_prepares",
    [RPF_PREVOUT_HITS] = "prevout_hits",
    [RPF_PREVOUT_MISSES] = "prevout_misses",
    [RPF_PREVOUT_US] = "prevout_resolution_us",
    [RPF_POOL_SETUP_US] = "worker_pool_setup_us",
    [RPF_POOL_WAKE_US] = "worker_pool_wakeup_us",
    [RPF_VERIFY_SCRIPT_CPU_US] = "verify_script_cpu_us",
    [RPF_WORKER_WAIT_US] = "worker_join_wait_us",
    [RPF_ORDERED_REDUCTION_US] = "ordered_reduction_us",
    [RPF_HEADER_EVENT_US] = "header_event_emission_us",
    [RPF_STAGE_LOG_CURSOR_US] = "stage_log_cursor_us",
};

static void rollover_if_needed(struct profile_domain *p, uint64_t generation)
{
    if (atomic_load_explicit(&p->generation, memory_order_acquire) == generation)
        return;
    pthread_mutex_lock(&p->rollover_lock);
    if (atomic_load_explicit(&p->generation, memory_order_relaxed) != generation) {
        for (size_t i = 0; i < RPF_FIELD_COUNT; i++)
            atomic_store_explicit(&p->last_batch[i], 0, memory_order_relaxed);
        for (size_t i = 0; i < RPF_FIELD_COUNT; i++)
            for (size_t b = 0; b < PROFILE_HIST_BUCKETS; b++)
                atomic_store_explicit(&p->last_histogram[i][b], 0,
                                      memory_order_relaxed);
        atomic_store_explicit(&p->last_present, 0, memory_order_relaxed);
        atomic_store_explicit(&p->generation, generation, memory_order_release);
    }
    pthread_mutex_unlock(&p->rollover_lock);
}

static struct profile_domain *profile_for_add(
    enum reducer_profile_domain domain, enum reducer_profile_field field)
{
    if (domain < 0 || domain >= REDUCER_PROFILE_DOMAIN_COUNT ||
        field < 0 || field >= RPF_FIELD_COUNT)
        return NULL;
    struct profile_domain *p = &g_profiles[domain];
    uint64_t generation = stage_batch_active() ? stage_batch_generation() : 0;
    rollover_if_needed(p, generation);
    return p;
}

static void add_value(struct profile_domain *p,
                      enum reducer_profile_field field, uint64_t value)
{
    atomic_fetch_add_explicit(&p->cumulative[field], value,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&p->last_batch[field], value,
                              memory_order_relaxed);
    uint64_t bit = UINT64_C(1) << (unsigned)field;
    atomic_fetch_or_explicit(&p->present, bit, memory_order_relaxed);
    atomic_fetch_or_explicit(&p->last_present, bit, memory_order_relaxed);
}

void reducer_stage_profile_add(enum reducer_profile_domain domain,
                               enum reducer_profile_field field,
                               uint64_t value)
{
    struct profile_domain *p = profile_for_add(domain, field);
    if (p)
        add_value(p, field, value);
}

static size_t histogram_bucket(uint64_t value)
{
    if (value == 0)
        return 0;
    size_t bucket = 64u - (size_t)__builtin_clzll(value);
    return bucket < PROFILE_HIST_BUCKETS ? bucket
                                         : PROFILE_HIST_BUCKETS - 1u;
}

void reducer_stage_profile_observe_us(enum reducer_profile_domain domain,
                                      enum reducer_profile_field field,
                                      uint64_t value)
{
    struct profile_domain *p = profile_for_add(domain, field);
    if (!p)
        return;
    add_value(p, field, value);
    size_t bucket = histogram_bucket(value);
    atomic_fetch_add_explicit(&p->histogram[field][bucket], 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&p->last_histogram[field][bucket], 1,
                              memory_order_relaxed);
}

void reducer_stage_profile_reset(void)
{
    for (size_t d = 0; d < REDUCER_PROFILE_DOMAIN_COUNT; d++) {
        pthread_mutex_lock(&g_profiles[d].rollover_lock);
        for (size_t i = 0; i < RPF_FIELD_COUNT; i++) {
            atomic_store(&g_profiles[d].cumulative[i], 0);
            atomic_store(&g_profiles[d].last_batch[i], 0);
            for (size_t b = 0; b < PROFILE_HIST_BUCKETS; b++) {
                atomic_store(&g_profiles[d].histogram[i][b], 0);
                atomic_store(&g_profiles[d].last_histogram[i][b], 0);
            }
        }
        atomic_store(&g_profiles[d].present, 0);
        atomic_store(&g_profiles[d].last_present, 0);
        atomic_store(&g_profiles[d].generation, 0);
        pthread_mutex_unlock(&g_profiles[d].rollover_lock);
    }
}

static uint64_t bucket_upper_bound(size_t bucket)
{
    if (bucket == 0)
        return 0;
    return (UINT64_C(1) << bucket) - 1u;
}

static bool histogram_quantile(struct profile_domain *p, size_t field,
                               bool last, unsigned percentile,
                               uint64_t *value_out, uint64_t *samples_out)
{
    uint64_t samples = 0;
    for (size_t b = 0; b < PROFILE_HIST_BUCKETS; b++)
        samples += atomic_load(last ? &p->last_histogram[field][b]
                                    : &p->histogram[field][b]);
    if (samples_out)
        *samples_out = samples;
    if (samples == 0)
        return false;
    uint64_t target = (samples * percentile + 99u) / 100u;
    uint64_t seen = 0;
    for (size_t b = 0; b < PROFILE_HIST_BUCKETS; b++) {
        seen += atomic_load(last ? &p->last_histogram[field][b]
                                 : &p->histogram[field][b]);
        if (seen >= target) {
            *value_out = bucket_upper_bound(b);
            return true;
        }
    }
    return false;
}

static void push_latency_quantiles(struct json_value *out,
                                   struct profile_domain *p, bool last)
{
    struct json_value quantiles;
    json_init(&quantiles);
    json_set_object(&quantiles);
    for (size_t i = 0; i < RPF_FIELD_COUNT; i++) {
        uint64_t p50 = 0, p95 = 0, samples = 0;
        if (!histogram_quantile(p, i, last, 50, &p50, &samples))
            continue;
        (void)histogram_quantile(p, i, last, 95, &p95, NULL);
        struct json_value field;
        json_init(&field);
        json_set_object(&field);
        json_push_kv_int(&field, "samples", (int64_t)samples);
        json_push_kv_int(&field, "p50", (int64_t)p50);
        json_push_kv_int(&field, "p95", (int64_t)p95);
        json_push_kv(&quantiles, g_field_names[i], &field);
    }
    json_push_kv(out, "latency_quantiles_us", &quantiles);
}

static void push_nullable_fields(struct json_value *out,
                                 struct profile_domain *p, bool last)
{
    uint64_t present = atomic_load(last ? &p->last_present : &p->present);
    for (size_t i = 0; i < RPF_FIELD_COUNT; i++) {
        struct json_value v;
        json_init(&v);
        if ((present & (UINT64_C(1) << i)) != 0) {
            uint64_t n = atomic_load(last ? &p->last_batch[i]
                                          : &p->cumulative[i]);
            json_set_int(&v, (int64_t)n);
        } else {
            json_set_null(&v);
        }
        json_push_kv(out, g_field_names[i], &v);
    }
}

static void push_domain(struct json_value *out, struct profile_domain *p)
{
    struct json_value cumulative, last;
    json_init(&cumulative);
    json_init(&last);
    json_set_object(&cumulative);
    json_set_object(&last);
    push_nullable_fields(&cumulative, p, false);
    push_nullable_fields(&last, p, true);
    push_latency_quantiles(&cumulative, p, false);
    push_latency_quantiles(&last, p, true);
    json_push_kv_int(&last, "batch_generation",
                     (int64_t)atomic_load(&p->generation));
    json_push_kv(out, "cumulative", &cumulative);
    json_push_kv(out, "last_batch", &last);
}

bool reducer_stage_profile_dump_state_json(struct json_value *out,
                                           const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);
    json_push_kv_str(out, "schema", PROFILE_SCHEMA);
    struct json_value body, script;
    json_init(&body);
    json_init(&script);
    json_set_object(&body);
    json_set_object(&script);
    push_domain(&body, &g_profiles[REDUCER_PROFILE_BODY_PERSIST]);
    push_domain(&script, &g_profiles[REDUCER_PROFILE_SCRIPT_VALIDATE]);
    json_push_kv(out, "body_persist", &body);
    json_push_kv(out, "script_validate", &script);
    return true;
}
