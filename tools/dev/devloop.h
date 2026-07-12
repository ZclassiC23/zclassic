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

enum zcl_devloop_action {
    ZCL_DEVLOOP_CHECK = 0,
    ZCL_DEVLOOP_HOTSWAP,
    ZCL_DEVLOOP_RELOAD,
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
int zcl_devloop_watch(const char *repo_root);
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
 * devloop_cycle.c's transactional_reload site (the RELOAD action's finish
 * line) and native_dev_command.c's dev.vcs.revert relink seam both need to
 * decide, per cycle, whether to drive the native transactional activation
 * engine (tools/dev/dev_activation.h: dev_activation_run() /
 * dev_activation_activate_generation()) or keep shelling out to the proven
 * `make agent-deploy-fast` path. The three helpers below are the shared,
 * pure (no process exec, no disk I/O beyond getenv()) glue both call sites
 * use to make that decision and build/interpret the engine's request and
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

/* True iff ZCL_DEV_NATIVE_ACTIVATION selects the native engine for this
 * process instead of the shell path. A RUNTIME env check, not a second
 * compile-time gate: the whole engine already compiles only into
 * ZCL_DEV_BUILD/ZCL_TESTING binaries, so gating it a second way at compile
 * time would only let one flag value exist per already-built binary. A
 * runtime check matches the idiom the dev lane already uses for adjacent
 * A/B switches (ZCL_DEV_USE_PREBUILT, ZCL_DEV_SKIP_BUILD in
 * deploy-dev-lane.sh) and lets one already-deployed zclassic23-dev flip
 * between the native engine and the proven shell path with no rebuild.
 * Default OFF (unset, empty, or any value other than "1"/"true"/"yes"):
 * today's `make agent-deploy-fast` shell path runs byte-identically. */
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
 * never spawns a process. `repo_root` and `build_commit` are stored by
 * pointer (not copied) and must outlive `out`; `build_commit` may be ""
 * (dev_activation's preflight skips the build-commit comparison when
 * empty) but not NULL. Returns false, leaving *out unusable, iff HOME is
 * unset/empty or an argument is NULL/oversized. */
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
#endif /* ZCL_DEV_BUILD || ZCL_TESTING */

#ifdef __cplusplus
}
#endif

#endif /* ZCL_TOOLS_DEVLOOP_H */
