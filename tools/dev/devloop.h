/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_TOOLS_DEVLOOP_H
#define ZCL_TOOLS_DEVLOOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZCL_DEVLOOP_MAX_FILES 256
#define ZCL_DEVLOOP_PATH_MAX 1024
#define ZCL_DEVLOOP_OUTPUT_MAX 65536
#define ZCL_DEVLOOP_WATCH_LOCK_REL ".cache/zcl-dev-watch.lock"

enum zcl_devloop_action {
    ZCL_DEVLOOP_CHECK = 0,
    ZCL_DEVLOOP_HOTSWAP,
    ZCL_DEVLOOP_RELOAD,
};

/* Publication intent is an explicit caller decision.  Persistent watchers use
 * VERIFY_ONLY by default; only an operator-invoked apply/auto path may select
 * APPLY.  Keeping this separate from the file-classification action prevents a
 * consensus/reload classification from bypassing watcher containment. */
enum zcl_devloop_publish_mode {
    ZCL_DEVLOOP_PUBLISH_VERIFY_ONLY = 0,
    ZCL_DEVLOOP_PUBLISH_APPLY,
};

struct zcl_devloop_plan {
    enum zcl_devloop_action action;
    const char *action_name;
    const char *reason;
    const char *probe_tool;
    const char *proof_group;
    char proof_group_storage[64];
    bool consensus_risk;
    bool docs_only;
    /* True iff any changed file lives under the SEALED consensus core (core/
     * — the surface core/MANIFEST.sha3 pins). Sealed files are always
     * consensus_risk too (heaviest proof), and the fast loop structurally
     * REFUSES to auto-publish them unless the owner unseal token is present.
     * See zcl_devloop_path_is_sealed_core() / zcl_devloop_refusal_json(). */
    bool sealed_core;
    size_t file_count;
};

struct zcl_devloop_process_result {
    int exit_code;
    int term_signal;
    bool timed_out;
    int64_t elapsed_ms;
    char output[ZCL_DEVLOOP_OUTPUT_MAX];
    size_t output_len;
    bool output_truncated;
};

bool zcl_devloop_is_method(const char *method);
int zcl_devloop_cli_main(const char **args, int nargs);

bool zcl_devloop_plan_files(const char *const *files, size_t file_count,
                            struct zcl_devloop_plan *out);
size_t zcl_devloop_plan_json(const char *const *files, size_t file_count,
                             char *out, size_t out_sz);

/* True iff the persistent watcher should react to a change at `path`
 * (repo-relative): a .c/.h/.def/.md/.mk/.service source or the Makefile,
 * excluding editor temp files, build/, .git/, and — critically — the
 * transient `_*fixture*` lint/shape-gate fixtures that
 * test_make_lint_gates.c writes under app/, lib/, and domain/ then deletes.
 * Reacting to those fixtures fires a phantom reload cycle on every
 * test-suite run (they no longer exist by the time the cycle rebuilds), so
 * the watcher must ignore them. Pure: no I/O. Shared by the watcher and its
 * unit test. */
bool zcl_devloop_path_is_relevant(const char *path);

/* True iff `path` is under the sealed consensus core (the `core/` prefix —
 * the exact surface `core/MANIFEST.sha3` seals). Broader than the
 * consensus-risk prefix list: it covers ALL of core/ (incl. core/math). */
bool zcl_devloop_path_is_sealed_core(const char *path);

/* True iff an owner-minted one-shot unseal token (`.core-unseal-token`, the
 * file `make core-unseal` writes) is present at `repo_root`. READ-ONLY: this
 * never mints or consumes the token — `make core-seal` is the sole consumer,
 * so one unseal authorizes one LANDED COMMIT, not one dev-cycle. */
bool zcl_devloop_unseal_token_present(const char *repo_root);

/* Build the structured sealed-core refusal envelope (a zcl.dev_cycle.v1
 * document with status "refused") into `out`. `files` lists the full changed
 * set; the envelope's "paths" array carries only the sealed-core members that
 * triggered the refusal. Returns the byte count, or 0 on overflow/bad args.
 * Sealed != frozen: the envelope always names the elevated procedure. */
size_t zcl_devloop_refusal_json(const char *const *files, size_t file_count,
                                char *out, size_t out_sz);

size_t zcl_devloop_menu_json(const char *path, char *out, size_t out_sz);
size_t zcl_devloop_menu_search_json(const char *query,
                                    char *out, size_t out_sz);

bool zcl_devloop_process_run(const char *cwd,
                             const char *const argv[],
                             int timeout_ms,
                             struct zcl_devloop_process_result *out);

/* Detach-launch the generation-neutral initial ZVCS baseline: double-fork +
 * setsid, grandchild runs vcs_devloop_run_initial_baseline() with stdio
 * redirected to <repo_root>/.zvcs/bootstrap.log, then _exit()s. Reaps the
 * short-lived launcher child itself so a persistent dev-loop watcher never
 * accumulates a zombie. Returns true iff the launcher was forked and
 * exited 0 (the baseline itself may still have failed — check
 * .zvcs/bootstrap.log or the next vcs_devloop_anchor_cycle() call). Dev-only
 * (tools/dev/devloop_baseline.c); a release build never links this. */
bool zcl_devloop_baseline_launch(const char *repo_root);

int zcl_devloop_run_cycle(const char *repo_root,
                          const char *const *files,
                          size_t file_count);
int zcl_devloop_run_cycle_mode(const char *repo_root,
                               const char *const *files,
                               size_t file_count,
                               enum zcl_devloop_publish_mode publish_mode);
bool zcl_devloop_publish_mode_applies(
    enum zcl_devloop_publish_mode publish_mode);
const char *zcl_devloop_publish_mode_name(
    enum zcl_devloop_publish_mode publish_mode);
enum zcl_devloop_publish_mode zcl_devloop_default_watch_publish_mode(void);
/* Pure path builder shared by the native watcher's flock acquisition and its
 * regression tests.  repo_root must already identify the worktree whose lane
 * is being watched; distinct worktrees consequently receive distinct locks. */
bool zcl_devloop_watch_lock_path(const char *repo_root,
                                 char *out, size_t out_sz);
int zcl_devloop_watch(const char *repo_root);
int zcl_devloop_watch_mode(const char *repo_root,
                           enum zcl_devloop_publish_mode publish_mode);
int zcl_devloop_print_status(void);
int zcl_devloop_run_sim(const char *repo_root);
int zcl_devloop_app_describe(const char *repo_root, const char *app_id);
int zcl_devloop_app_plan(const char *repo_root, const char *app_id,
                         const char *resource);
int zcl_devloop_app_simulate(const char *app_id, uint64_t seed);

/* Release-safe bounded JSON buffer producers backing both the stdout print
 * wrappers above and the Wave 2.2 registry dev handlers. Return bytes written,
 * or 0 on invalid arguments / overflow. */
size_t zcl_devloop_app_describe_json(const char *repo_root, const char *app_id,
                                     char *out, size_t out_sz);
size_t zcl_devloop_app_plan_json(const char *repo_root, const char *app_id,
                                 const char *resource, char *out, size_t out_sz);
size_t zcl_devloop_app_simulate_json(const char *app_id, uint64_t seed,
                                     char *out, size_t out_sz);

/* ── Wave 3.2 native activation engine — dev-lane wiring ───────────────
 *
 * The implementation remains testable while all public callers are hard-
 * contained. A future complete publication transaction may decide whether to
 * drive the native transactional activation
 * engine (tools/dev/dev_activation.h: dev_activation_run() /
 * dev_activation_activate_generation()) or keep shelling out to the proven
 * backend or a replacement. The three helpers below are pure (no process
 * exec, no disk I/O beyond getenv()) glue used to build/interpret its request and
 * result -- see docs/work/HOTSWAP.md "Transactional reload: native engine".
 *
 * Guarded the same way dev_activation.h's own entry points are
 * (ZCL_DEV_BUILD || ZCL_TESTING): a release build sees no declaration at
 * all, and the hermetic ZCL_TESTING harness can unit-test the pure builders
 * directly (no fake ops vtable needed -- they never call one). */
#if defined(ZCL_DEV_BUILD) || defined(ZCL_TESTING)
#include <limits.h>

#include "dev_activation.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* True iff ZCL_DEV_NATIVE_ACTIVATION selects the retained native engine in
 * code paths that already possess internal test authority. This runtime
 * selector is NOT activation authority: public watcher/apply entrypoints
 * refuse before consulting it, and deploy-dev-lane.sh accepts machinery tests
 * only through its inherited-FD, fixture-bound self-test capability. */
bool dev_activation_native_enabled(void);

/* Caller-owned storage backing a struct dev_activation_request built by
 * dev_activation_request_from_cycle() below -- the struct's string fields
 * point directly into these buffers (and, for repo_root/build_commit, into
 * the caller's own arguments), so `out` must outlive any use of out->req. */
struct dev_activation_cycle_request {
    struct dev_activation_request req;
    char artifact_path[PATH_MAX];
    char gen_root[PATH_MAX];
    char datadir[PATH_MAX];
};

/* Build *out from the dev lane's fixed constants -- the exact values
 * tools/dev/deploy-dev-lane.sh hard-codes (GEN_ROOT defaulting to
 * ~/.local/lib/zclassic23-dev, DEV_DATADIR ~/.zclassic-c23-dev, UNIT
 * zcl23-dev.service, DEV_RPCPORT 18252, build_type "fast") plus the
 * fast-lane artifact at <repo_root>/build/bin/zclassic23-dev. Pure: reads
 * only HOME and ZCL_DEV_GENERATION_ROOT, never touches disk beyond that and
 * never spawns a process. `repo_root` and display-only `build_commit` are
 * stored by pointer (not copied) and must outlive `out`; `build_commit` may
 * be "" but not NULL. It is never a preflight or activation decision input.
 * The caller binds req.source_identity separately before activation. Returns
 * false, leaving *out unusable, iff HOME is unset/empty or an argument is
 * NULL/oversized. */
bool dev_activation_request_from_cycle(const char *repo_root,
                                       const char *build_commit,
                                       struct dev_activation_cycle_request *out);

/* Outcome of a completed dev_activation_result, mapped into the shape
 * finish_cycle()/the vcs.vcs.revert reply need: pass/fail plus a short
 * human-readable failure capsule and the candidate's generation hex (the
 * ZVCS auto-anchor's generation binding -- see devloop_cycle.c:finish_cycle
 * and vcs.h's generation_sha256). */
struct dev_activation_cycle_outcome {
    bool ok;
    char capsule[256];
    char generation_hex[65];
};

/* Pure: maps `r` into `out`. `r` NULL => *out zeroed (ok=false). */
void dev_activation_map_result(const struct dev_activation_result *r,
                               struct dev_activation_cycle_outcome *out);

/* Testable wrapper over devloop_cycle.c's static distill_first_error(): scan
 * [out, out+len) FORWARD for the first actionable line — a compiler
 * diagnostic (contains ": error:") or a test failure (contains "FAIL",
 * "Assertion", or "EXPECT") — and copy it (newline stripped, bounded by
 * dstcap, always NUL-terminated) into dst. Returns true iff one was found.
 * Backs the dense output_capsule "first_error=" prefix. Pure: no I/O. */
bool zcl_devloop_distill_first_error(const char *out, size_t len,
                                     char *dst, size_t dstcap);
#endif /* ZCL_DEV_BUILD || ZCL_TESTING */

#ifdef __cplusplus
}
#endif

#endif /* ZCL_TOOLS_DEVLOOP_H */
