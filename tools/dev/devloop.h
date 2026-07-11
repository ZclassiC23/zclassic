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

#ifdef __cplusplus
}
#endif

#endif /* ZCL_TOOLS_DEVLOOP_H */
