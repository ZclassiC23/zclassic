/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_cold_start.h — the `-cold-start` staged, resumable one-command driver.
 *
 * WHAT IT REPLACES. The proven cold-start recipe was hand-driven in four/five
 * separate process launches, each an existing verb the operator had to sequence
 * correctly (and one, `--importblockindex`, that silently no-ops unless it is
 * argv[1]):
 *   (1) --importblockindex <src>         header-chain bulk import (argv[1]!)
 *   (2) -load-snapshot-at-own-height=<s> seed boot, then a manual clean stop
 *   (3) -install-consensus-bundle=<b>    checkpoint-authority install (terminal)
 *   (4) plain boot                        serve
 * Step (2) cannot self-seed a truly fresh datadir in ONE boot because the
 * snapshot gate (config/src/boot.c boot_snapshot_install_gate_boot) evaluates
 * BEFORE the header chain is loaded, so the seed anchor has no header to bind to.
 *
 * WHAT `-cold-start` IS. A single foreground driver process that runs those same
 * existing verbs IN ORDER, each as a child process (it COMPOSES the stages, it
 * does not fork/duplicate them), writing a durable per-stage receipt after each
 * success. Because the header-import stage runs and commits FIRST, the seed
 * stage's snapshot gate always sees a populated header chain — the self-seed
 * ordering bug is fixed by sequencing, not by moving the gate. A killed cold
 * start resumes at the first stage whose receipt is absent (or whose bound
 * parameter changed), never from scratch.
 *
 * RECEIPT IDIOM — dedicated per-stage files under <datadir>/coldstart/, written
 * with the shutdown_stagewatch temp+fsync+rename crash-safe idiom (NOT the
 * progress.kv stage cursor). Justification: the driver runs BEFORE progress.kv
 * is opened and its stages span multiple child process lifetimes, so a
 * standalone, directly-inspectable durable file per stage is the natural fit;
 * progress.kv is a single-process in-boot cursor and is the wrong lifetime.
 * Each receipt records the stage's bound parameter (source/seed/bundle path) so
 * a resume with a CHANGED parameter re-runs that stage instead of falsely
 * skipping it.
 *
 * This file declares (a) the pure, side-effect-free planning + receipt helpers
 * (unit tested directly, no child spawn), and (b) the live driver entry point.
 */
#ifndef ZCL_BOOT_COLD_START_H
#define ZCL_BOOT_COLD_START_H

#include <stdbool.h>
#include <stddef.h>

/* Ordered prep stages plus the terminal SERVE target. HEADERS/SEED/BUNDLE each
 * bear a durable receipt; SERVE is the plain serving boot the driver exec()s
 * into once every configured prep stage has a matching receipt. */
enum cold_start_stage {
    COLD_START_STAGE_HEADERS = 0, /* --importblockindex <source>          */
    COLD_START_STAGE_SEED    = 1, /* -load-snapshot-at-own-height=<seed>  */
    COLD_START_STAGE_BUNDLE  = 2, /* -install-consensus-bundle=<bundle>   */
    COLD_START_STAGE_SERVE   = 3, /* plain serving boot (exec target)     */
};
#define COLD_START_PREP_STAGE_COUNT 3 /* HEADERS, SEED, BUNDLE */

/* Immutable description of one cold-start run. A NULL/empty parameter means that
 * prep stage is NOT configured for this run and is skipped entirely (e.g. no
 * seed_snapshot => rely on P2P for state; no header_source => rely on P2P header
 * sync). datadir must be non-empty. All pointers are borrowed (argv-lifetime). */
struct cold_start_plan {
    const char *datadir;
    const char *header_source;  /* --importblockindex source datadir, or NULL */
    const char *seed_snapshot;  /* -load-snapshot-at-own-height path, or NULL  */
    const char *install_bundle; /* -install-consensus-bundle path, or NULL     */
};

/* Stable lower-case stage name ("headers"/"seed"/"bundle"/"serve"), or "?" for
 * an out-of-range value. Used in receipt filenames, logs, and tests. */
const char *cold_start_stage_name(enum cold_start_stage stage);

/* Is this prep stage part of the plan? SERVE is always "configured" (the driver
 * always ends by serving). HEADERS/SEED/BUNDLE are configured iff their
 * corresponding plan parameter is non-NULL and non-empty. */
bool cold_start_stage_configured(const struct cold_start_plan *plan,
                                 enum cold_start_stage stage);

/* The bound parameter for a prep stage (source/seed/bundle path), or NULL for
 * SERVE / an unconfigured stage. This is the string a receipt records and
 * re-validates against on resume. */
const char *cold_start_stage_param(const struct cold_start_plan *plan,
                                   enum cold_start_stage stage);

/* Compose <datadir>/coldstart/<stage>.receipt into buf. Returns the number of
 * bytes written (excluding the NUL), or -1 on truncation / bad args. */
int cold_start_receipt_path(const char *datadir, enum cold_start_stage stage,
                            char *buf, size_t n);

/* Durably record that `stage` completed with bound parameter `param` (may be
 * NULL). Creates <datadir>/coldstart/ as needed, then temp+fsync+rename+dir-
 * fsync so a crash never leaves a torn receipt. Returns false (logged) on any
 * I/O failure — the caller MUST treat a write failure as a stage failure so the
 * stage is retried rather than silently skipped. */
bool cold_start_receipt_write(const char *datadir, enum cold_start_stage stage,
                              const char *param);

/* True iff a receipt for `stage` exists AND its recorded parameter equals
 * `param` (both NULL compares equal). A present-but-parameter-mismatched receipt
 * returns false so a changed source/seed/bundle re-runs that stage. */
bool cold_start_receipt_matches(const char *datadir, enum cold_start_stage stage,
                                const char *param);

/* Pure resume decision: the first configured prep stage (in HEADERS, SEED,
 * BUNDLE order) whose receipt does not match its bound parameter. If every
 * configured prep stage already matches, returns COLD_START_STAGE_SERVE. */
enum cold_start_stage cold_start_plan_next(const struct cold_start_plan *plan);

/* Runner invoked by cold_start_drive() to execute one prep stage. Returns 0 on
 * success (the driver then writes the receipt), non-zero on failure (the driver
 * stops WITHOUT writing a receipt, so the stage is retried on the next run).
 * The live runner fork/exec()s the corresponding existing verb; tests inject a
 * recording fake so the drive loop is exercised without spawning a node. */
typedef int (*cold_start_stage_runner_fn)(const struct cold_start_plan *plan,
                                          enum cold_start_stage stage,
                                          void *user);

/* Drive the plan to SERVE using `runner`. Skips already-receipted stages, runs
 * each remaining configured prep stage in order, and writes its receipt on
 * success. On reaching SERVE, sets *out_reached (if non-NULL) to
 * COLD_START_STAGE_SERVE and returns 0 WITHOUT serving — the caller performs the
 * exec. On a stage-runner failure returns that non-zero code and sets
 * *out_reached to the failed stage. On a receipt-write failure returns -1. */
int cold_start_drive(const struct cold_start_plan *plan,
                     cold_start_stage_runner_fn runner, void *user,
                     enum cold_start_stage *out_reached);

/* Live `-cold-start` entry point, dispatched from main() before app_init. Parses
 * -datadir=/-cold-start-source=/-cold-start-seed=/-install-consensus-bundle=
 * out of argv, drives every configured prep stage via child processes, then
 * exec()s a plain serving boot (argv minus the cold-start-only flags). Returns a
 * non-zero exit code only when a prep stage fails; on success it exec()s and
 * does not return. */
int boot_cold_start_run(int argc, char **argv);

#endif /* ZCL_BOOT_COLD_START_H */
