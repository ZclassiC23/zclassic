/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Self-tests for the size and shape gates: the file-size ceiling and
 * long-function ratchets (E1, enforced tier plus the lib/ warn tier), and the
 * eight-shape purity rules — E3 (a shape .c includes its own header), E4
 * (projections stay pure), E5 (a reducer stage advances or names a blocker),
 * E6 (one write path), E7 (no authoritative RAM state), E12 (honest witness),
 * domain purity, and shape include direction.
 *
 * Each check plants a violating fixture in the scanned tree, asserts the gate
 * trips, removes it, and asserts the gate recovers. */

#define _POSIX_C_SOURCE 200809L

#include "test/test_core.h"

#ifdef ZCL_TESTING

#include "lint_gate_selftests.h"

/* Gate #12 (check_long_functions.sh) ENFORCED tier, extended to config/src/
 * (this test exercises the mechanism via an isolated test-tmp/ scan dir +
 * baseline, so it stands in for either real ENFORCED root — controllers/
 * services/config-src share one code path). Matches the
 * shrinking-floor breadth of t_service_result_convergence_ratchet above:
 * baseline-matched is clean, a lowered baseline trips as a REGRESSION, an
 * empty baseline trips as NEW, and the `// long-function-ok:` tag exempts
 * the same function entirely — then recovery. */
int t_long_functions_enforced_ratchet(void)
{
    int failures = 0;
    char dir_path[PATH_MAX];
    if (repo_path(dir_path, sizeof(dir_path), LONGFN_ENFORCED_DIR_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve long-fn enforced dir path\n");
        return 1;
    }
    (void)mkdir(dir_path, 0700);
    unlink_rel(LONGFN_ENFORCED_FIXTURE_REL);
    unlink_rel(LONGFN_ENFORCED_BASELINE_REL);
    /* keep.c: a permanent, always-clean (well under the cap) sibling file
     * so the gate's non-empty-scan-set floor stays satisfied through the
     * recovery case below, independent of whatever fixture.c is doing. */
    int wrote_keep = write_file(LONGFN_ENFORCED_KEEP_REL,
        "void longfn_enforced_keep_placeholder(void) {}\n");

    /* Case A: fixture is 520 lines; baseline records it at 520 — matched,
     * must be clean. */
    int planted_a = plant_long_function_file(
        LONGFN_ENFORCED_FIXTURE_REL, "longfn_case_matched", 520, NULL);
    char baseline_matched[PATH_MAX + 32];
    (void)snprintf(baseline_matched, sizeof(baseline_matched),
                   "%s longfn_case_matched 520\n", LONGFN_ENFORCED_FIXTURE_REL);
    int wrote_baseline_a = write_file(LONGFN_ENFORCED_BASELINE_REL, baseline_matched);
    int matched_rc =
        (planted_a == 0 && wrote_baseline_a == 0)
            ? run_gate_script_with_env2(LONGFN_SCRIPT_REL,
                  "ZCL_LONGFN_ENFORCED_ROOTS", LONGFN_ENFORCED_DIR_REL,
                  "ZCL_LONGFN_BASELINE", LONGFN_ENFORCED_BASELINE_REL)
            : -999;

    /* Case B: same fixture (still 520 lines), baseline lowered to 400 —
     * must trip as a REGRESSION (grew past its recorded length). */
    char baseline_grown[PATH_MAX + 32];
    (void)snprintf(baseline_grown, sizeof(baseline_grown),
                   "%s longfn_case_matched 400\n", LONGFN_ENFORCED_FIXTURE_REL);
    int wrote_baseline_b = write_file(LONGFN_ENFORCED_BASELINE_REL, baseline_grown);
    int grown_rc =
        wrote_baseline_b == 0
            ? run_gate_script_with_env2(LONGFN_SCRIPT_REL,
                  "ZCL_LONGFN_ENFORCED_ROOTS", LONGFN_ENFORCED_DIR_REL,
                  "ZCL_LONGFN_BASELINE", LONGFN_ENFORCED_BASELINE_REL)
            : -999;
    char grown_out_path[PATH_MAX];
    char *grown_out = NULL;
    int grown_read =
        (grown_rc >= 0 &&
         lint_gate_out_path(grown_out_path, sizeof(grown_out_path)) == 0)
            ? read_entire_file(grown_out_path, &grown_out)
            : -1;

    /* Case C: EMPTY baseline, no tag — must trip as a NEW unlisted long
     * function. */
    int wrote_baseline_empty = write_file(LONGFN_ENFORCED_BASELINE_REL, "");
    int new_rc =
        wrote_baseline_empty == 0
            ? run_gate_script_with_env2(LONGFN_SCRIPT_REL,
                  "ZCL_LONGFN_ENFORCED_ROOTS", LONGFN_ENFORCED_DIR_REL,
                  "ZCL_LONGFN_BASELINE", LONGFN_ENFORCED_BASELINE_REL)
            : -999;
    char new_out_path[PATH_MAX];
    char *new_out = NULL;
    int new_read =
        (new_rc >= 0 &&
         lint_gate_out_path(new_out_path, sizeof(new_out_path)) == 0)
            ? read_entire_file(new_out_path, &new_out)
            : -1;

    /* Case D: same EMPTY baseline, but the signature now carries
     * `// long-function-ok:<tag>` — the override must exempt it entirely,
     * no baseline entry required. */
    int planted_d = plant_long_function_file(
        LONGFN_ENFORCED_FIXTURE_REL, "longfn_case_matched", 520,
        "test_fixture_reason");
    int tagged_rc =
        planted_d == 0
            ? run_gate_script_with_env2(LONGFN_SCRIPT_REL,
                  "ZCL_LONGFN_ENFORCED_ROOTS", LONGFN_ENFORCED_DIR_REL,
                  "ZCL_LONGFN_BASELINE", LONGFN_ENFORCED_BASELINE_REL)
            : -999;

    /* Recovery: remove fixture + baseline entirely. */
    unlink_rel(LONGFN_ENFORCED_FIXTURE_REL);
    int wrote_baseline_recover = write_file(LONGFN_ENFORCED_BASELINE_REL, "");
    int recover_rc =
        wrote_baseline_recover == 0
            ? run_gate_script_with_env2(LONGFN_SCRIPT_REL,
                  "ZCL_LONGFN_ENFORCED_ROOTS", LONGFN_ENFORCED_DIR_REL,
                  "ZCL_LONGFN_BASELINE", LONGFN_ENFORCED_BASELINE_REL)
            : -999;

    unlink_rel(LONGFN_ENFORCED_FIXTURE_REL);
    unlink_rel(LONGFN_ENFORCED_BASELINE_REL);
    unlink_rel(LONGFN_ENFORCED_KEEP_REL);
    (void)rmdir(dir_path);

    TEST("[lint-gate] gate#12 ENFORCED (config/src+controllers/services): matched clean, grown/new trip, tag exempts, recovers") {
        ASSERT(wrote_keep == 0);
        ASSERT(planted_a == 0);
        ASSERT(matched_rc == 0);
        ASSERT(grown_rc != 0);
        ASSERT(grown_read == 0);
        ASSERT(grown_out != NULL && strstr(grown_out, "REGRESSION") != NULL);
        ASSERT(new_rc != 0);
        ASSERT(new_read == 0);
        ASSERT(new_out != NULL && strstr(new_out, "NEW long function") != NULL);
        ASSERT(planted_d == 0);
        ASSERT(tagged_rc == 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;

    free(grown_out);
    free(new_out);
    return failures;
}

/* Gate #12 WARN tier — lib/ (excl. lib/test/). Same mechanism, but a
 * violation must PRINT (WARN) and never fail the build (exit 0), mirroring
 * E1's lib/domain WARN tier. */
int t_long_functions_lib_warn_tier(void)
{
    int failures = 0;
    char dir_path[PATH_MAX];
    if (repo_path(dir_path, sizeof(dir_path), LONGFN_LIB_DIR_REL) != 0) {
        fprintf(stderr, "[lint-gate] could not resolve long-fn lib dir path\n");
        return 1;
    }
    (void)mkdir(dir_path, 0700);
    unlink_rel(LONGFN_LIB_FIXTURE_REL);
    unlink_rel(LONGFN_LIB_BASELINE_REL);
    /* keep.c: a permanent, always-clean sibling file so the gate's
     * non-empty-scan-set floor stays satisfied through the recovery case
     * below, independent of whatever fixture.c is doing. */
    int wrote_keep = write_file(LONGFN_LIB_KEEP_REL,
        "void longfn_lib_keep_placeholder(void) {}\n");

    /* Case A: EMPTY baseline, unbaselined 520-line function — must WARN
     * (print) but exit 0. */
    int planted_a = plant_long_function_file(
        LONGFN_LIB_FIXTURE_REL, "longfn_lib_case_new", 520, NULL);
    int wrote_baseline_empty = write_file(LONGFN_LIB_BASELINE_REL, "");
    int new_rc =
        (planted_a == 0 && wrote_baseline_empty == 0)
            ? run_gate_script_with_env2(LONGFN_SCRIPT_REL,
                  "ZCL_LONGFN_LIB_ROOTS", LONGFN_LIB_DIR_REL,
                  "ZCL_LONGFN_LIB_BASELINE", LONGFN_LIB_BASELINE_REL)
            : -999;
    char new_out_path[PATH_MAX];
    char *new_out = NULL;
    int new_read =
        (new_rc >= 0 &&
         lint_gate_out_path(new_out_path, sizeof(new_out_path)) == 0)
            ? read_entire_file(new_out_path, &new_out)
            : -1;

    /* Case B: baseline lowered below the fixture's actual length — must
     * WARN as "grew past its baselined length" but still exit 0. */
    char baseline_grown[PATH_MAX + 32];
    (void)snprintf(baseline_grown, sizeof(baseline_grown),
                   "%s longfn_lib_case_new 400\n", LONGFN_LIB_FIXTURE_REL);
    int wrote_baseline_grown = write_file(LONGFN_LIB_BASELINE_REL, baseline_grown);
    int grown_rc =
        wrote_baseline_grown == 0
            ? run_gate_script_with_env2(LONGFN_SCRIPT_REL,
                  "ZCL_LONGFN_LIB_ROOTS", LONGFN_LIB_DIR_REL,
                  "ZCL_LONGFN_LIB_BASELINE", LONGFN_LIB_BASELINE_REL)
            : -999;
    char grown_out_path[PATH_MAX];
    char *grown_out = NULL;
    int grown_read =
        (grown_rc >= 0 &&
         lint_gate_out_path(grown_out_path, sizeof(grown_out_path)) == 0)
            ? read_entire_file(grown_out_path, &grown_out)
            : -1;

    /* Recovery: remove fixture + baseline entirely. */
    unlink_rel(LONGFN_LIB_FIXTURE_REL);
    int wrote_baseline_recover = write_file(LONGFN_LIB_BASELINE_REL, "");
    int recover_rc =
        wrote_baseline_recover == 0
            ? run_gate_script_with_env2(LONGFN_SCRIPT_REL,
                  "ZCL_LONGFN_LIB_ROOTS", LONGFN_LIB_DIR_REL,
                  "ZCL_LONGFN_LIB_BASELINE", LONGFN_LIB_BASELINE_REL)
            : -999;
    char recover_out_path[PATH_MAX];
    char *recover_out = NULL;
    int recover_read =
        (recover_rc >= 0 &&
         lint_gate_out_path(recover_out_path, sizeof(recover_out_path)) == 0)
            ? read_entire_file(recover_out_path, &recover_out)
            : -1;

    unlink_rel(LONGFN_LIB_FIXTURE_REL);
    unlink_rel(LONGFN_LIB_BASELINE_REL);
    unlink_rel(LONGFN_LIB_KEEP_REL);
    (void)rmdir(dir_path);

    /* NOTE: the gate's own CLEAN line for this tier reads "...(cap 500,
     * lib/, WARN tier)" — the bare word "WARN" appears there as a tier
     * label even when nothing tripped. Assert on the violation HEADING
     * ("WARN — long-function watch"), never the bare word, or a clean run
     * false-passes/false-fails this check. */
    TEST("[lint-gate] gate#12 WARN (lib/, excl. lib/test/): new/grown WARN-print but exit 0, recovers") {
        ASSERT(wrote_keep == 0);
        ASSERT(planted_a == 0);
        ASSERT(new_rc == 0);
        ASSERT(new_read == 0);
        ASSERT(new_out != NULL &&
               strstr(new_out, "WARN — long-function watch") != NULL);
        ASSERT(new_out != NULL && strstr(new_out, "NEW long function") != NULL);
        ASSERT(grown_rc == 0);
        ASSERT(grown_read == 0);
        ASSERT(grown_out != NULL &&
               strstr(grown_out, "grew past its baselined length") != NULL);
        ASSERT(recover_rc == 0);
        ASSERT(recover_read == 0);
        ASSERT(recover_out != NULL &&
               strstr(recover_out, "WARN — long-function watch") == NULL);
        PASS();
    } _test_next:;

    free(new_out);
    free(grown_out);
    free(recover_out);
    return failures;
}

/* E1 — file-size ceiling is an ENFORCED RATCHET (hard FAIL, not advisory):
 * a NEW (non-baselined) app/.c file over the 800-line ceiling trips the
 * gate (nonzero exit) and prints the violation report; removing it returns
 * to a clean, zero-exit run. This complements (does not replace) the hard
 * correctness gate check_long_functions.sh (<=500 lines/function). */
int t_e1_file_size_ceiling(void)
{
    int failures = 0;
    unlink_rel(E1_FIXTURE_DST);
    int baseline_rc = run_gate_script(E1_SCRIPT_REL, NULL);
    int planted = plant_oversized_file(E1_FIXTURE_DST, 900);
    int trip_rc = planted == 0 ? run_gate_script(E1_SCRIPT_REL, NULL) : -1;
    /* Capture the FAIL run's stdout so we can prove the report still PRINTS
     * the violation detail alongside the nonzero exit. */
    char *fail_out = NULL;
    char fail_path[PATH_MAX];
    int fail_read = (planted == 0 &&
                     lint_gate_out_path(fail_path, sizeof(fail_path)) == 0)
                        ? read_entire_file(fail_path, &fail_out)
                        : -1;
    unlink_rel(E1_FIXTURE_DST);
    int recover_rc = run_gate_script(E1_SCRIPT_REL, NULL);
    TEST("[lint-gate] E1 file-size ceiling: clean, FAILS (exit != 0) on oversized, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        /* FAIL, not WARN: oversized file now exits nonzero (enforced). */
        ASSERT(trip_rc != 0);
        /* ...and the violation report must be printed alongside the fail. */
        ASSERT(fail_read == 0);
        ASSERT(fail_out != NULL && strstr(fail_out, "FAIL") != NULL);
        ASSERT(fail_out != NULL &&
               strstr(fail_out, "NEW oversized file") != NULL);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    free(fail_out);
    return failures;
}

/* E1 lib/domain WARN tier: a SINGLE oversized, unbaselined lib/ file must
 * WARN (print) but never fail the build on its own (exit 0), mirroring the
 * WARN-vs-ENFORCED split proven for gate #12 below. The tier's aggregate
 * drift-count ratchet (see E1_LIB_FIXTURE_DST's comment above) is pointed at
 * an isolated tmp file for the duration of this test so the assertion holds
 * regardless of how much real, already-reviewed drift the live tree
 * carries. */
int t_e1_lib_warn_tier(void)
{
    int failures = 0;
    unlink_rel(E1_LIB_FIXTURE_DST);

    char ratchet_path[PATH_MAX];
    int ratchet_rc = repo_path(ratchet_path, sizeof(ratchet_path),
                                E1_LIB_DRIFT_RATCHET_TMP_REL);
    if (ratchet_rc == 0) {
        FILE *rf = fopen(ratchet_path, "wb");
        if (rf) {
            fputs("999999\n", rf);
            fclose(rf);
        } else {
            ratchet_rc = -1;
        }
    }

    int baseline_rc = ratchet_rc == 0
        ? run_gate_script_with_env(E1_SCRIPT_REL, E1_LIB_DRIFT_RATCHET_ENV,
                                    ratchet_path)
        : -1;
    int planted = plant_oversized_file(E1_LIB_FIXTURE_DST, 900);
    int warn_rc = (planted == 0 && ratchet_rc == 0)
        ? run_gate_script_with_env(E1_SCRIPT_REL, E1_LIB_DRIFT_RATCHET_ENV,
                                    ratchet_path)
        : -1;
    char *warn_out = NULL;
    char warn_path[PATH_MAX];
    int warn_read = (planted == 0 &&
                     lint_gate_out_path(warn_path, sizeof(warn_path)) == 0)
                        ? read_entire_file(warn_path, &warn_out)
                        : -1;
    unlink_rel(E1_LIB_FIXTURE_DST);
    int recover_rc = ratchet_rc == 0
        ? run_gate_script_with_env(E1_SCRIPT_REL, E1_LIB_DRIFT_RATCHET_ENV,
                                    ratchet_path)
        : -1;
    unlink_rel(E1_LIB_DRIFT_RATCHET_TMP_REL);
    TEST("[lint-gate] E1 lib/domain WARN tier: clean, WARN-prints (exit 0) on oversized lib file, recovers") {
        ASSERT(ratchet_rc == 0);
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        /* WARN, not FAIL: a single new WARN-tier violation never fails the
         * build on its own (the isolated ratchet above keeps this test
         * independent of the real tree's already-reviewed aggregate drift). */
        ASSERT(warn_rc == 0);
        ASSERT(warn_read == 0);
        ASSERT(warn_out != NULL && strstr(warn_out, "WARN") != NULL);
        ASSERT(warn_out != NULL &&
               strstr(warn_out, "NEW oversized file") != NULL);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    free(warn_out);
    return failures;
}

/* E3 — shape-includes-header HARD: a condition file that includes neither
 * framework/condition.h nor a conditions/ header trips the gate; removing
 * it restores green. */
int t_e3_shape_includes_header(void)
{
    int failures = 0;
    unlink_rel(E3_FIXTURE_DST);
    int baseline_rc = run_gate_script(E3_SCRIPT_REL, NULL);
    char path[PATH_MAX];
    int planted = (repo_path(path, sizeof(path), E3_FIXTURE_DST) == 0 &&
                   write_file(path,
                       "/* mislabeled condition: no shape header */\n"
                       "int e3_fixture;\n") == 0)
                  ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(E3_SCRIPT_REL, NULL) : -1;
    unlink_rel(E3_FIXTURE_DST);
    int recover_rc = run_gate_script(E3_SCRIPT_REL, NULL);
    TEST("[lint-gate] E3 shape-includes-header HARD: clean, trips headerless condition, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* E4 — projections-pure HARD: a projection file that includes an app-layer
 * (services/) header trips the gate; removing it restores green. */
int t_e4_projections_pure(void)
{
    int failures = 0;
    unlink_rel(E4_FIXTURE_DST);
    int baseline_rc = run_gate_script(E4_SCRIPT_REL, NULL);
    char path[PATH_MAX];
    int planted = (repo_path(path, sizeof(path), E4_FIXTURE_DST) == 0 &&
                   write_file(path,
                       "#include \"services/sync_monitor.h\"\n"
                       "int e4_fixture;\n") == 0)
                  ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(E4_SCRIPT_REL, NULL) : -1;
    unlink_rel(E4_FIXTURE_DST);
    int recover_rc = run_gate_script(E4_SCRIPT_REL, NULL);
    TEST("[lint-gate] E4 projections-pure HARD: clean, trips app-layer include, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* Gate #45 — domain/ source purity HARD: a domain/ file that includes an
 * app-layer (services/) header trips the gate (rule a); an unlisted lib/
 * subsystem prefix also trips it (rule b); a bare domain-local sibling include
 * stays clean. Removing the fixture restores green. */
int t_domain_purity(void)
{
    int failures = 0;
    char path[PATH_MAX];

    unlink_rel(DOMAIN_PURITY_FIXTURE_DST);
    int baseline_rc = run_gate_script(DOMAIN_PURITY_SCRIPT_REL, NULL);

    /* Rule (a): an app-layer (services/) include must trip the gate. */
    int planted_app = (repo_path(path, sizeof(path),
                                 DOMAIN_PURITY_FIXTURE_DST) == 0 &&
                       write_file(path,
                           "#include \"services/foo.h\"\n"
                           "int domain_purity_fixture;\n") == 0)
                      ? 0 : -1;
    int trip_app_rc = planted_app == 0
                      ? run_gate_script(DOMAIN_PURITY_SCRIPT_REL, NULL) : -1;
    unlink_rel(DOMAIN_PURITY_FIXTURE_DST);

    /* Rule (b): an unlisted lib/ subsystem prefix must also trip the gate. */
    int planted_lib = (repo_path(path, sizeof(path),
                                 DOMAIN_PURITY_FIXTURE_DST) == 0 &&
                       write_file(path,
                           "#include \"storage/foo.h\"\n"
                           "int domain_purity_fixture;\n") == 0)
                      ? 0 : -1;
    int trip_lib_rc = planted_lib == 0
                      ? run_gate_script(DOMAIN_PURITY_SCRIPT_REL, NULL) : -1;
    unlink_rel(DOMAIN_PURITY_FIXTURE_DST);

    /* A bare domain-local sibling include (no slash) must NOT trip the gate. */
    int planted_sib = (repo_path(path, sizeof(path),
                                 DOMAIN_PURITY_FIXTURE_DST) == 0 &&
                       write_file(path,
                           "#include \"reject_out.h\"\n"
                           "int domain_purity_fixture;\n") == 0)
                      ? 0 : -1;
    int sibling_rc = planted_sib == 0
                     ? run_gate_script(DOMAIN_PURITY_SCRIPT_REL, NULL) : -1;
    unlink_rel(DOMAIN_PURITY_FIXTURE_DST);

    int recover_rc = run_gate_script(DOMAIN_PURITY_SCRIPT_REL, NULL);

    TEST("[lint-gate] #45 domain-purity HARD: clean, trips app+lib includes, allows sibling, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted_app == 0);
        ASSERT(trip_app_rc != 0);
        ASSERT(planted_lib == 0);
        ASSERT(trip_lib_rc != 0);
        ASSERT(planted_sib == 0);
        ASSERT(sibling_rc == 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* Gate #49 — check-shape-include-direction RATCHET: a models/ file with an
 * upward #include "services/..." trips the gate; removing it restores
 * green. (The services/ -> controllers/ edge already carries 13
 * grandfathered baseline entries — see shape_include_direction_baseline.txt
 * — so this fixture targets the models/ edge instead, which is the one
 * this gate's own introduction paid down to zero.) */
int t_shape_include_direction(void)
{
    int failures = 0;
    char path[PATH_MAX];

    unlink_rel(SHAPE_DIR_FIXTURE_DST);
    int baseline_rc = run_gate_script(SHAPE_DIR_SCRIPT_REL, NULL);

    int planted = (repo_path(path, sizeof(path), SHAPE_DIR_FIXTURE_DST) == 0 &&
                   write_file(path,
                       "#include \"services/foo.h\"\n"
                       "int shape_dir_fixture;\n") == 0)
                  ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(SHAPE_DIR_SCRIPT_REL, NULL) : -1;
    unlink_rel(SHAPE_DIR_FIXTURE_DST);
    int recover_rc = run_gate_script(SHAPE_DIR_SCRIPT_REL, NULL);

    TEST("[lint-gate] #49 shape-include-direction RATCHET: clean, trips models->services include, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* E5 — stage-advances-or-blocks HARD: a Job step file that only ever returns
 * JOB_ADVANCED and references no cursor trips the gate; removing it restores
 * green. */
int t_e5_stage_advances_or_blocks(void)
{
    int failures = 0;
    unlink_rel(E5_FIXTURE_DST);
    int baseline_rc = run_gate_script(E5_SCRIPT_REL, NULL);
    char path[PATH_MAX];
    int planted = (repo_path(path, sizeof(path), E5_FIXTURE_DST) == 0 &&
                   write_file(path,
                       "/* mislabeled Job stage: advances only, no cursor */\n"
                       "typedef int job_result_t;\n"
                       "#define JOB_ADVANCED 0\n"
                       "job_result_t fixture_stage_step_once(void){ return JOB_ADVANCED; }\n") == 0)
                  ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(E5_SCRIPT_REL, NULL) : -1;
    unlink_rel(E5_FIXTURE_DST);
    int recover_rc = run_gate_script(E5_SCRIPT_REL, NULL);
    TEST("[lint-gate] E5 stage-advances-or-blocks HARD: clean, trips advance-only stage, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* E6 — one-write-path RATCHET: a new production write surface outside the
 * baseline trips the gate; removing it restores green. */
int t_e6_one_write_path(void)
{
    int failures = 0;
    unlink_rel(E6_FIXTURE_DST);
    int baseline_rc = run_gate_script(E6_SCRIPT_REL, NULL);
    char path[PATH_MAX];
    int planted = (repo_path(path, sizeof(path), E6_FIXTURE_DST) == 0 &&
                   write_file(path,
                       "struct active_chain; struct block_index;\n"
                       "int active_chain_set_tip(struct active_chain *, struct block_index *);\n"
                       "int e6_fixture(struct active_chain *c, struct block_index *b){ return active_chain_set_tip(c, b); }\n") == 0)
                  ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(E6_SCRIPT_REL, NULL) : -1;
    unlink_rel(E6_FIXTURE_DST);
    int recover_rc = run_gate_script(E6_SCRIPT_REL, NULL);
    TEST("[lint-gate] E6 one-write-path RATCHET: clean, trips new writer, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* E7 — no-authoritative-RAM-state RATCHET: a new direct active_chain
 * internals access trips the gate; removing it restores green. */
int t_e7_no_authoritative_ram_state(void)
{
    int failures = 0;
    unlink_rel(E7_FIXTURE_DST);
    int baseline_rc = run_gate_script(E7_SCRIPT_REL, NULL);
    char path[PATH_MAX];
    int planted = (repo_path(path, sizeof(path), E7_FIXTURE_DST) == 0 &&
                   write_file(path,
                       "struct main_state { struct { int height; } chain_active; };\n"
                       "int e7_fixture(struct main_state *s){ return s->chain_active.height; }\n") == 0)
                  ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(E7_SCRIPT_REL, NULL) : -1;
    unlink_rel(E7_FIXTURE_DST);
    int recover_rc = run_gate_script(E7_SCRIPT_REL, NULL);
    TEST("[lint-gate] E7 no-authoritative-RAM-state RATCHET: clean, trips direct RAM state, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* E12 — honest witness (Law 7). The live tree is clean (FAIL mode passes:
 * every witness reads observable progress or carries a reviewed
 * // honest-witness-ok hatch). Plant a condition .c whose witness is a
 * PURE-INVERSE of detect ("return !detect_x()") — the canonical Law-7 lie
 * a no-op/self-certifying remedy hides behind — and assert the gate trips;
 * removing it restores green. This proves the gate has teeth and the
 * baseline is honestly empty. */
int t_e12_honest_witness(void)
{
    int failures = 0;
    unlink_rel(E12_FIXTURE_DST);
    int baseline_rc = run_gate_script(E12_SCRIPT_REL, "FAIL");
    char path[PATH_MAX];
    int planted_good = (repo_path(path, sizeof(path), E12_FIXTURE_DST) == 0 &&
                        write_file(path,
                            "#include <stdbool.h>\n"
                            "#include <stdint.h>\n"
                            "static bool reducer_frontier_compute_hstar(void *db, int32_t *h, int32_t *s){\n"
                            "    (void)db; *h = 42; *s = 42; return true;\n"
                            "}\n"
                            "static bool witness_e12_frontier(int64_t t){\n"
                            "    int32_t hstar = -1;\n"
                            "    int32_t served = -1;\n"
                            "    return reducer_frontier_compute_hstar(0, &hstar, &served) && hstar >= (int)t;\n"
                            "}\n") == 0)
                       ? 0 : -1;
    int good_rc = planted_good == 0 ? run_gate_script(E12_SCRIPT_REL, "FAIL") : -1;
    unlink_rel(E12_FIXTURE_DST);
    int planted = (repo_path(path, sizeof(path), E12_FIXTURE_DST) == 0 &&
                   write_file(path,
                       "#include <stdbool.h>\n"
                       "#include <stdint.h>\n"
                       "static bool detect_e12_fixture(void){ return true; }\n"
                       "static bool witness_e12_fixture(int64_t t){\n"
                       "    (void)t;\n"
                       "    return !detect_e12_fixture();\n"
                       "}\n") == 0)
                  ? 0 : -1;
    int trip_rc = planted == 0 ? run_gate_script(E12_SCRIPT_REL, "FAIL") : -1;
    unlink_rel(E12_FIXTURE_DST);
    int recover_rc = run_gate_script(E12_SCRIPT_REL, "FAIL");
    TEST("[lint-gate] E12 honest-witness FAIL: accepts reducer H*, trips pure-inverse witness, recovers") {
        ASSERT(baseline_rc == 0);
        ASSERT(planted_good == 0);
        ASSERT(good_rc == 0);
        ASSERT(planted == 0);
        ASSERT(trip_rc != 0);
        ASSERT(recover_rc == 0);
        PASS();
    } _test_next:;
    return failures;
}

#else  /* !ZCL_TESTING */

/* Without ZCL_TESTING the lint-gate self-tests compile to nothing; this
 * keeps the translation unit non-empty. */
typedef int zcl_lint_gate_sz_unit;

#endif /* ZCL_TESTING */
