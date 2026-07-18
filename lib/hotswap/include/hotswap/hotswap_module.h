/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tier-1 hot-swap — the REAL (activatable) single-handler module ABI.
 *
 * This is the single-handler successor to the native-leaf generation loader in
 * hotswap.h. Where that loader re-points a whole controller's leaf table via a
 * host vtable and
 * NEVER dlcloses (a deliberate permanent leak), this module ABI is:
 *
 *   - single-handler: one swappable leaf per .so, keyed by its canonical
 *     command path (`handler_name`);
 *   - self-describing under ONE known symbol (`zcl_hotswap_module`), version
 *     stamped (`abi_version`) — an ABI mismatch or a missing symbol is refused
 *     LOUDLY (logged + typed error), the handler is never called;
 *   - reclaimable: an activation commits into the kernel command-registry
 *     override layer and the superseded .so is dlclose'd AFTER in-flight
 *     dispatch drains (epoch/refcount quiesce in lib/kernel/command_registry.c).
 *
 * Dynamic loading is DEV-ONLY: every dlopen/dlsym/dlclose lives behind
 * `#ifdef ZCL_DEV_BUILD` in hotswap_activate.c; a release build links only a
 * refusal stub. Activation is gated OFF by default — see
 * hotswap_activation_authorized(): it requires BOTH the `-hotswap-activate`
 * flag AND `ZCL_HOTSWAP_ACTIVATE=1`, and REFUSES the canonical datadir.
 * Without authorization every call is verify-only (labeled as such).
 */

#ifndef ZCL_HOTSWAP_MODULE_H
#define ZCL_HOTSWAP_MODULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward decls only — lib/hotswap never pulls kernel/app headers. The concrete
 * request/reply structs are complete only in the swappable controller TU. */
struct zcl_command_request;
struct zcl_command_reply;
struct json_value;

typedef void (*zcl_hotswap_handler_fn)(const struct zcl_command_request *request,
                                       struct zcl_command_reply *reply);

/* Bump only on an incompatible layout change to struct zcl_hotswap_module. A
 * loaded .so whose abi_version != this is refused before its handler runs. */
#define ZCL_HOTSWAP_MODULE_ABI_V1 1u

/* The single data symbol every swappable module .so must export. */
#define ZCL_HOTSWAP_MODULE_SYMBOL "zcl_hotswap_module"

/* Exported (verbatim symbol name ZCL_HOTSWAP_MODULE_SYMBOL) by each swappable
 * .so. Resolved and fully validated before `fn` is ever called. */
struct zcl_hotswap_module {
    uint32_t abi_version;                     /* == ZCL_HOTSWAP_MODULE_ABI_V1 */
    const char *handler_name;                 /* canonical READY read-only leaf */
    zcl_hotswap_handler_fn fn;                /* replacement handler (non-NULL) */
    bool (*self_test)(char *err, size_t cap); /* true iff module is healthy */
};

/* ── Module emitter (invoke ONCE at file scope in a swappable TU) ──────────
 *
 * Under a module build (-DZCL_HOTSWAP_MODULE_GEN) this emits the exported
 * `zcl_hotswap_module` bound to the TU's freshly-compiled body + self-test. In
 * ordinary node and release builds it expands to nothing, so the symbol only
 * ever exists inside a module .so, never in the shipped binary. Example:
 *
 *     ZCL_HOTSWAP_MODULE("core.status", tramp_status, selftest_status)
 */
#ifdef ZCL_HOTSWAP_MODULE_GEN
#define ZCL_HOTSWAP_MODULE(handler_name_, fn_, self_test_)                   \
    const struct zcl_hotswap_module zcl_hotswap_module = {                   \
        .abi_version = ZCL_HOTSWAP_MODULE_ABI_V1,                            \
        .handler_name = (handler_name_),                                    \
        .fn = (fn_),                                                         \
        .self_test = (self_test_),                                          \
    };
#else
#define ZCL_HOTSWAP_MODULE(handler_name_, fn_, self_test_) /* node/release: omitted */
#endif

/* Result of hotswap_activate(). Always fully populated (even on failure). */
struct hotswap_activate_report {
    bool ok;              /* verify passed; and activated iff request+authorized */
    bool verify_only;     /* activation was not requested OR not authorized */
    bool activated;       /* the live registry slot was actually re-pointed */
    bool rolled_back;     /* a failure left the previous handler in place */
    uint32_t generation;  /* registry override generation after commit (else 0) */
    char handler_name[128];
    char artifact_sha256[65];
    char stage[64];       /* precheck|authorize|dlopen|abi|allowlist|self_test|commit */
    char error[256];      /* "" on ok */
};

/* Publish ONE {handler_name, fn} override into the resident command registry.
 * Supplied by the resident (tools/command) layer / test harness so lib/hotswap
 * stays free of kernel headers. Returns true on publish, filling *out_gen with
 * the new override generation; false leaves the active handler untouched. */
typedef bool (*hotswap_commit_handler_cb)(void *ctx,
                                          const char *handler_name,
                                          zcl_hotswap_handler_fn fn,
                                          uint32_t *out_gen,
                                          char *why, size_t why_sz);

/* Return true iff every RETIRED override snapshot has drained — i.e. no
 * in-flight dispatch can still enter a superseded handler, so the previous
 * module .so is safe to dlclose. Polled with a bounded backoff after commit. */
typedef bool (*hotswap_quiesced_cb)(void *ctx);

/* Load a single-handler module .so and, when authorized, activate it live.
 *
 *   so_path           absolute path to the module .so (hotswap_path_is_acceptable)
 *   resolved_datadir  must resolve to the exact worker path ~/.zclassic-c23-dev
 *   request_activate  false => verify-only (default). true => attempt a live
 *                     swap, which still requires hotswap_activation_authorized().
 *   commit_cb/ctx     publishes the override (required when actually activating;
 *                     ignored for verify-only). quiesced_cb gates the dlclose of
 *                     the superseded .so (NULL => keep it mapped, never close).
 *   report            out — always populated; on any failure the previous
 *                     handler is untouched (rolled_back=true) and error is set.
 *
 * Verify-only (or unauthorized activate): dlopen + ABI-validate + self_test,
 * NEVER commits, dlcloses the candidate, reports verify_only=true. Authorized
 * activate: also commits into the command registry override layer (generation
 * bumped) and dlcloses the superseded .so once quiesced_cb reports drain
 * (else keeps it mapped, the pilot's never-close fallback — always safe).
 *
 * DEV-ONLY: without ZCL_DEV_BUILD this refuses ("unavailable") and never
 * dlopens. Returns report->ok. */
bool hotswap_activate(const char *so_path,
                      const char *resolved_datadir,
                      bool request_activate,
                      hotswap_commit_handler_cb commit_cb,
                      hotswap_quiesced_cb quiesced_cb,
                      void *cb_ctx,
                      struct hotswap_activate_report *report);

/* Pure admission check for a resolved module: ABI version, required fields,
 * the swappable shape-leaf allowlist, and the module's own self_test — the
 * exact gauntlet hotswap_activate applies after dlsym, factored out so it is
 * unit-testable with a fabricated struct in ANY build (no dlopen). On failure
 * fills `stage` (one of "abi"|"fields"|"allowlist"|"self_test") and `why`.
 * Returns true iff the module is admissible. */
bool hotswap_module_admit(const struct zcl_hotswap_module *module,
                          char *stage, size_t stage_cap,
                          char *why, size_t why_cap);

/* Record the -hotswap-activate flag (argv parse, resident node process). */
void hotswap_set_activate_flag(bool enabled);
bool hotswap_activate_flag(void);

/* Activation gate. True ONLY when: the -hotswap-activate flag was set AND
 * env ZCL_HOTSWAP_ACTIVATE=1 AND resolved_datadir is the exact dev lane
 * (~/.zclassic-c23-dev). The canonical datadir (~/.zclassic-c23) is refused
 * with a loud, typed reason in `why`. */
bool hotswap_activation_authorized(const char *resolved_datadir,
                                   char *why, size_t why_sz);

/* Runtime mirror of the check-hotswap-swappable-shape lint gate: true iff
 * `handler_name` is on the compiled swappable allowlist (config/
 * hotswap_swappable.def) — a shape-leaf handler (controllers/views/conditions),
 * never a reducer/consensus/storage/supervisor path. */
bool hotswap_handler_is_swappable(const char *handler_name);

/* Number of successful live activations this process (monotone). */
uint64_t hotswap_activation_count(void);

/* Append the activation subsystem's telemetry into an already-open object.
 * Called by hotswap_dump_state_json() so `zclassic23 dumpstate hotswap` shows
 * both the generation loader and activation
 * slots/epochs/containment in one document. */
void hotswap_activate_dump_json(struct json_value *out);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_HOTSWAP_MODULE_H */
