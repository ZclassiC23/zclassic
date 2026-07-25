/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_validate_headers_tuning — the PARITY-SAFETY guard for the
 * validate_headers pool-width / forward-batch override
 * (app/jobs/src/validate_headers_tuning.c).
 *
 * The override changes HOW MANY headers the stage Equihash-verifies at once.
 * The load-bearing safety property is that on a NORMAL live node — no refold in
 * progress, no -mint-anchor fold ceiling — it is INERT: vh_runtime_pool_size()
 * is VH_POOL_SIZE and vh_runtime_batch_size() is VH_BATCH_SIZE, and NEITHER
 * ZCL_VH_POOL nor ZCL_VH_BATCH can widen them. This test pins that: an edit
 * that lets the environment reach the live path fails here.
 *
 * It also proves the override FIRES with its accelerated defaults under either
 * fold gate, honors + clamps its env knobs there, and that clearing the gate
 * RESTORES the inert identity. */

#include "test/test_core.h"

#include "../../../app/jobs/src/validate_headers_internal.h"

#include "jobs/validate_headers_stage.h"
#include "jobs/refold_cadence.h"
#include "jobs/refold_progress.h"     /* refold_progress_test_set_cached */
#include "jobs/mint_fold_ceiling.h"   /* mint_fold_ceiling_set, MINT_FOLD_NO_CEILING */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define VHT_CHECK(name, expr) do {                                 \
    printf("validate_headers_tuning: %s... ", (name));             \
    if (expr) { printf("OK\n"); }                                  \
    else { printf("FAIL\n"); failures++; }                         \
} while (0)

/* Force BOTH fold gates off and the knobs unset — the live-node state. */
static void vht_clear_gates(void)
{
    mint_fold_ceiling_set(MINT_FOLD_NO_CEILING);
    refold_progress_test_set_cached(false);
    unsetenv("ZCL_VH_POOL");
    unsetenv("ZCL_VH_BATCH");
}

/* (1) NORMAL live node: compile-time widths, and the env cannot reach them. */
static int case_normal_inert(void)
{
    int failures = 0;
    vht_clear_gates();

    VHT_CHECK("normal: not active", !refold_cadence_active());
    VHT_CHECK("normal: pool = VH_POOL_SIZE",
              vh_runtime_pool_size() == VH_POOL_SIZE);
    VHT_CHECK("normal: batch = VH_BATCH_SIZE",
              vh_runtime_batch_size() == VH_BATCH_SIZE);

    setenv("ZCL_VH_POOL", "64", 1);
    setenv("ZCL_VH_BATCH", "2048", 1);
    VHT_CHECK("normal: env ignored when inactive (pool)",
              vh_runtime_pool_size() == VH_POOL_SIZE);
    VHT_CHECK("normal: env ignored when inactive (batch)",
              vh_runtime_batch_size() == VH_BATCH_SIZE);

    vht_clear_gates();
    return failures;
}

/* (2) -mint-anchor active: accelerated defaults, env tuning, clamps. */
static int case_mint_active(void)
{
    int failures = 0;
    vht_clear_gates();

    mint_fold_ceiling_set(3056758);   /* the real anchor; any real height arms it */
    VHT_CHECK("mint: active", refold_cadence_active());

    VHT_CHECK("mint: default pool 16",
              vh_runtime_pool_size() == VH_FOLD_POOL_DEFAULT);
    VHT_CHECK("mint: default batch 256",
              vh_runtime_batch_size() == VH_FOLD_BATCH_DEFAULT);

    setenv("ZCL_VH_POOL", "32", 1);
    setenv("ZCL_VH_BATCH", "1024", 1);
    VHT_CHECK("mint: env pool 32",  vh_runtime_pool_size()  == 32);
    VHT_CHECK("mint: env batch 1024", vh_runtime_batch_size() == 1024);

    /* Clamps: absurd values are bounded, never returned raw. */
    setenv("ZCL_VH_POOL", "0", 1);
    VHT_CHECK("mint: pool clamp low", vh_runtime_pool_size() == 1);
    setenv("ZCL_VH_POOL", "999999", 1);
    VHT_CHECK("mint: pool clamp high", vh_runtime_pool_size() == VH_MAX_POOL);
    setenv("ZCL_VH_BATCH", "-5", 1);
    VHT_CHECK("mint: batch clamp low", vh_runtime_batch_size() == 1);
    setenv("ZCL_VH_BATCH", "999999", 1);
    VHT_CHECK("mint: batch clamp high",
              vh_runtime_batch_size() == VH_MAX_BATCH);

    /* Garbage falls back to the accelerated default, never to 0. */
    setenv("ZCL_VH_POOL", "not-a-number", 1);
    setenv("ZCL_VH_BATCH", "", 1);
    VHT_CHECK("mint: garbage pool -> default",
              vh_runtime_pool_size() == VH_FOLD_POOL_DEFAULT);
    VHT_CHECK("mint: empty batch -> default",
              vh_runtime_batch_size() == VH_FOLD_BATCH_DEFAULT);

    vht_clear_gates();
    return failures;
}

/* (3) -refold-* active, then clearing the gate RESTORES the inert identity. */
static int case_refold_active_then_restore(void)
{
    int failures = 0;
    vht_clear_gates();

    refold_progress_test_set_cached(true);
    VHT_CHECK("refold: active", refold_cadence_active());
    VHT_CHECK("refold: default pool 16",
              vh_runtime_pool_size() == VH_FOLD_POOL_DEFAULT);
    VHT_CHECK("refold: default batch 256",
              vh_runtime_batch_size() == VH_FOLD_BATCH_DEFAULT);

    /* An env knob set DURING the fold must not survive the gate clearing. */
    setenv("ZCL_VH_POOL", "128", 1);
    setenv("ZCL_VH_BATCH", "4096", 1);
    refold_progress_test_set_cached(false);
    VHT_CHECK("restore: not active", !refold_cadence_active());
    VHT_CHECK("restore: pool = VH_POOL_SIZE",
              vh_runtime_pool_size() == VH_POOL_SIZE);
    VHT_CHECK("restore: batch = VH_BATCH_SIZE",
              vh_runtime_batch_size() == VH_BATCH_SIZE);

    /* The pool array is statically sized to VH_MAX_POOL — a fold width can
     * never index past it. */
    VHT_CHECK("bound: fold pool default within VH_MAX_POOL",
              VH_FOLD_POOL_DEFAULT <= VH_MAX_POOL);
    VHT_CHECK("bound: fold batch default within VH_MAX_BATCH",
              VH_FOLD_BATCH_DEFAULT <= VH_MAX_BATCH);

    vht_clear_gates();
    return failures;
}

int test_validate_headers_tuning(void)
{
    int failures = 0;
    failures += case_normal_inert();
    failures += case_mint_active();
    failures += case_refold_active_then_restore();
    if (failures == 0)
        printf("test_validate_headers_tuning: ALL PASSED\n");
    else
        printf("test_validate_headers_tuning: %d FAILURE(S)\n", failures);
    return failures;
}
