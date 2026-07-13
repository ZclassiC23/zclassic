/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for the always-on SAMPLED re-verify loop that bg_validation enters
 * after the genesis→tip walk completes. We exercise the outcome-recording seam
 * (bg_validation_record_reverify) directly:
 *   - a healthy sample advances reverify_passes and keeps state COMPLETE;
 *   - a planted re-verify FAILURE flips state to FAILED and raises the
 *     PERMANENT `bg_validation.reverify_failed` blocker naming the height.
 * The seam is what the loop calls for every sampled height, so this proves the
 * branch the loop depends on without needing an on-disk historical chain. */

#include "test/test_helpers.h"
#include "services/bg_validation_service.h"
#include "util/blocker.h"

#include <stdatomic.h>
#include <string.h>

static bool find_reverify_blocker(struct blocker_snapshot *out)
{
    struct blocker_snapshot snaps[BLOCKER_CAP];
    int n = blocker_snapshot_all(snaps, BLOCKER_CAP);
    for (int i = 0; i < n; i++) {
        if (strcmp(snaps[i].id, "bg_validation.reverify_failed") == 0) {
            if (out) *out = snaps[i];
            return true;
        }
    }
    return false;
}

static int test_bg_validation_reverify_healthy_advances(void)
{
    int failures = 0;

    TEST("bg_validation: healthy sampled re-verify advances reverify_passes") {
        blocker_clear("bg_validation.reverify_failed");
        struct bg_validation_service svc;
        memset(&svc, 0, sizeof(svc));
        atomic_store(&svc.progress.state, BG_VALIDATION_COMPLETE);

        for (int i = 0; i < 5; i++)
            ASSERT(bg_validation_record_reverify(&svc, 1000 + i, true));

        struct bg_validation_progress p = bg_validation_get_progress(&svc);
        ASSERT(p.reverify_passes == 5);
        ASSERT(p.reverify_fails == 0);
        ASSERT(p.reverify_height == 1004);
        /* Healthy re-verify never regresses the COMPLETE state. */
        ASSERT(p.state == BG_VALIDATION_COMPLETE);
        ASSERT(!find_reverify_blocker(NULL));
        PASS();
    } _test_next:;
    return failures;
}

static int test_bg_validation_reverify_failure_raises_blocker(void)
{
    int failures = 0;

    TEST("bg_validation: planted re-verify FAIL raises PERMANENT blocker") {
        blocker_clear("bg_validation.reverify_failed");
        struct bg_validation_service svc;
        memset(&svc, 0, sizeof(svc));
        atomic_store(&svc.progress.state, BG_VALIDATION_COMPLETE);

        /* One healthy sample, then a planted failure at a named height. */
        ASSERT(bg_validation_record_reverify(&svc, 2000, true));
        ASSERT(!bg_validation_record_reverify(&svc, 2500, false));

        struct bg_validation_progress p = bg_validation_get_progress(&svc);
        ASSERT(p.reverify_passes == 1);
        ASSERT(p.reverify_fails == 1);
        ASSERT(p.reverify_height == 2500);
        ASSERT(p.state == BG_VALIDATION_FAILED);

        struct blocker_snapshot snap;
        ASSERT(find_reverify_blocker(&snap));
        ASSERT(snap.class == BLOCKER_PERMANENT);
        ASSERT(strstr(snap.reason, "2500") != NULL);

        blocker_clear("bg_validation.reverify_failed");
        PASS();
    } _test_next:;
    return failures;
}

int test_bg_validation_reverify(void)
{
    int failures = 0;
    failures += test_bg_validation_reverify_healthy_advances();
    failures += test_bg_validation_reverify_failure_raises_blocker();
    return failures;
}
