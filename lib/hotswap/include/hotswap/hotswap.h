/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tier 1 in-process hot-swap loader (DEV-ONLY).
 *
 * An AI agent edits an app-layer MCP controller .c, `make hotswap-so`
 * compiles the eligible stateless MCP TU into a generation .so, and this loader
 * dlopen's it into the RUNNING dev node and re-points the affected MCP
 * routes atomically — no restart. The release binary stays 100% static:
 * every dlopen/dlsym here lives inside `#ifdef ZCL_DEV_BUILD`, so a
 * release build links zero dynamic-loading code (the whole load path
 * compiles down to a stub that returns "hotswap unavailable in release").
 *
 * Generations are NEVER dlclose'd (a deliberate leak): an in-flight call
 * that already entered old code stays valid because the old .so text is
 * never unmapped.
 *
 * See docs/work/HOTSWAP.md for the end-to-end recipe + the ephemerality
 * warning (hot-swaps revert on restart; persist via a normal rebuild).
 */

#ifndef ZCL_HOTSWAP_H
#define ZCL_HOTSWAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward decls only — lib/hotswap must not include tools/mcp/router.h
 * (lib-layer purity) nor pull the full json header into consumers that
 * only need the dump signature. */
struct mcp_tool_route;
struct json_value;

/* Forward decls only — the native command surface (kernel/app types) is never
 * pulled into lib/hotswap. A `native.leaves` generation stages replacement
 * command handlers by dotted path; the concrete request/reply structs are
 * complete only in the controller TU that invokes ZCL_HOTSWAP_EXPORT_LEAVES. */
struct zcl_command_request;
struct zcl_command_reply;
typedef void (*zcl_command_handler_fn)(const struct zcl_command_request *,
                                       struct zcl_command_reply *);

/* v2 is deliberately small: the only admitted provider class is a stateless
 * MCP route set.  REST/diagnostics and every stateful provider remain reload
 * required until they can participate in the same generation transaction. */
#define ZCL_HOTSWAP_MANIFEST_SCHEMA_V2 2u
#define ZCL_HOTSWAP_HOST_ABI_V2        2u
/* v3 host: same manifest schema (v2), but the host vtable additionally exposes
 * leaf_stage for the native.leaves provider class. Strictly additive — the v3
 * struct appends one field past the v2 layout, so a v2 consumer that only reads
 * the first ZCL_HOTSWAP_HOST_STRUCT_SIZE_V2 bytes is unaffected. */
#define ZCL_HOTSWAP_HOST_ABI_V3        3u

enum zcl_hotswap_host_capability {
    ZCL_HOTSWAP_CAP_MCP_STAGE          = UINT64_C(1) << 0,
    ZCL_HOTSWAP_CAP_ATOMIC_COMMIT      = UINT64_C(1) << 1,
    /* v3: stage a native command leaf (dotted path → handler). */
    ZCL_HOTSWAP_CAP_LEAF_STAGE         = UINT64_C(1) << 2,
    /* Deliberately unsupported in the v2 pilot.  The legacy provider macro
     * requests this bit, so direct REST/diagnostics publication fails closed
     * before generation code runs. */
    ZCL_HOTSWAP_CAP_DIRECT_PROVIDER    = UINT64_C(1) << 63,
};

#define ZCL_HOTSWAP_V2_HOST_CAPABILITIES \
    (ZCL_HOTSWAP_CAP_MCP_STAGE | ZCL_HOTSWAP_CAP_ATOMIC_COMMIT)

#define ZCL_HOTSWAP_V3_HOST_CAPABILITIES \
    (ZCL_HOTSWAP_CAP_LEAF_STAGE | ZCL_HOTSWAP_CAP_ATOMIC_COMMIT)

enum zcl_hotswap_quiescence {
    ZCL_HOTSWAP_QUIESCENCE_NONE = 0,
};

/* Host vtable handed to a generation's zcl_hotswap_gen_init(). Calls to
 * mcp_stage only append to the loader-owned transaction; they NEVER publish.
 * The resident host validates the complete batch and commits one immutable
 * router snapshot only after gen_init and the generation self-test succeed. */
struct zcl_hotswap_host {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t gen;
    uint64_t capabilities;
    bool (*mcp_stage)(const char *name, const struct mcp_tool_route *route);
    /* v3 (appended — DO NOT reorder the fields above; a v2 consumer sees only
     * the first ZCL_HOTSWAP_HOST_STRUCT_SIZE_V2 bytes). Present only when the
     * host advertises ZCL_HOTSWAP_HOST_ABI_V3 / ZCL_HOTSWAP_CAP_LEAF_STAGE. */
    bool (*leaf_stage)(const char *path, zcl_command_handler_fn handler);
};

/* The v2 host layout is exactly the prefix through mcp_stage; the v3 layout is
 * the whole struct. Freezing the v2 size as an offsetof (rather than a bare
 * sizeof) keeps a routes generation compiled against this header declaring the
 * SAME host_struct_size a pre-v3 generation declared, so the byte path stays
 * identical after the struct grows. */
#define ZCL_HOTSWAP_HOST_STRUCT_SIZE_V2 \
    ((uint32_t)offsetof(struct zcl_hotswap_host, leaf_stage))
#define ZCL_HOTSWAP_HOST_STRUCT_SIZE_V3 \
    ((uint32_t)sizeof(struct zcl_hotswap_host))

/* The symbol every generation .so must export. */
typedef bool (*zcl_hotswap_gen_init_fn)(const struct zcl_hotswap_host *host);

typedef bool (*zcl_hotswap_self_test_fn)(
    const struct zcl_hotswap_host *host, char *why, size_t why_sz);

/* Exported as the data symbol `zcl_hotswap_manifest_v2`.  It is resolved and
 * validated before gen_init or any other generation function is called. */
struct zcl_hotswap_manifest_v2 {
    uint32_t schema_version;
    uint32_t struct_size;
    uint32_t host_abi_version;
    uint32_t host_struct_size;
    uint64_t required_host_capabilities;
    const char *provider_id;
    const char *build_identity;
    const char *source_identity;
    const char *input_digest;       /* exact inputs, SHA-256 lowercase hex */
    uint32_t state_schema_version;
    bool stateless;
    enum zcl_hotswap_quiescence quiescence;
    const char *mapped_tests_csv;
    const char *probe_tools_csv;
    zcl_hotswap_self_test_fn self_test;
};

#define ZCL_HOTSWAP_GEN_MAX_REPLACED 64

/* Result of a load attempt. Always fully populated (even on failure). */
struct hotswap_load_report {
    bool     ok;                  /* full transaction committed */
    uint32_t gen;                 /* assigned generation id (0 if not loaded) */
    char     error[256];          /* human-readable failure reason ("" on ok) */
    char     replaced[ZCL_HOTSWAP_GEN_MAX_REPLACED][64]; /* tool names published */
    size_t   replaced_count;
    bool     replaced_overflow;   /* reserved; v2 rejects oversized batches */
    char     rejection_stage[64]; /* precheck/manifest/init/self_test/commit */
    char     provider_id[96];
    char     build_identity[96];
    char     source_identity[256];
    char     input_digest[128];
    char     artifact_sha256[65]; /* resident hash of the .so bytes */
};

struct zcl_hotswap_mcp_replacement {
    const char *name;
    const struct mcp_tool_route *route;
};

/* One all-or-zero publication callback supplied by the resident MCP
 * controller.  The loader never links upward against tools/mcp/router. */
typedef bool (*zcl_hotswap_commit_cb)(
    void *context,
    uint32_t gen,
    const struct zcl_hotswap_mcp_replacement *replacements,
    size_t replacement_count,
    char *why,
    size_t why_sz);

/* Parallel to zcl_hotswap_mcp_replacement for the native.leaves provider: a
 * dotted command path bound to the generation's freshly-compiled handler. */
struct zcl_hotswap_leaf_replacement {
    const char *path;
    zcl_command_handler_fn handler;
};

/* All-or-zero publication callback supplied by the resident native command
 * router. Signature is depended on verbatim by downstream (W1-B/C) code. */
typedef bool (*zcl_hotswap_leaf_commit_cb)(
    void *ctx,
    uint32_t gen,
    const struct zcl_hotswap_leaf_replacement *reps,
    size_t n,
    char *why,
    size_t why_sz);

/* Load a generation .so and stage its route replacements.
 *
 *   so_path          absolute path to the gen .so (see hotswap_path_is_acceptable)
 *   resolved_datadir must resolve to the exact worker path
 *                    ~/.zclassic-c23-dev; every other lane is refused
 *   required_probe   canonical manifest probe selected by the planner
 *   commit_cb        validates + publishes one immutable route snapshot
 *   commit_context   opaque caller context passed to commit_cb
 *   report           out — always populated
 *
 * Returns true iff the .so loaded, manifest/init/self-test passed, and the
 * complete staged batch committed. DEV-ONLY: without ZCL_DEV_BUILD this refuses with an
 * "unavailable" error and performs NO dlopen. */
bool hotswap_load(const char *so_path,
                  const char *resolved_datadir,
                  const char *required_probe,
                  zcl_hotswap_commit_cb commit_cb,
                  void *commit_context,
                  struct hotswap_load_report *report);

/* Load a `native.leaves` generation .so and stage its command-handler
 * replacements. Same fail-closed precheck/dlopen/hash/manifest-validate core
 * as hotswap_load(); diverges only in staging native leaves (v3 host,
 * leaf_stage) and invoking a zcl_hotswap_leaf_commit_cb. DEV-ONLY: without
 * ZCL_DEV_BUILD it refuses with an "unavailable" error and performs NO dlopen. */
bool hotswap_load_leaves(const char *so_path,
                         const char *datadir,
                         const char *probe_leaf,
                         zcl_hotswap_leaf_commit_cb commit_cb,
                         void *ctx,
                         struct hotswap_load_report *report);

/* ── Pure predicates (compiled in ALL builds; unit-tested w/o dlopen) ── */

/* True if so_path is safe to load: absolute, ends ".so", exists, and its
 * realpath sits under /tmp or a repo build/hotswap dir. On false, `why`
 * (if non-NULL) gets a short reason. */
bool hotswap_path_is_acceptable(const char *so_path, char *why, size_t why_sz);

/* True only for the exact registered worker datadir ~/.zclassic-c23-dev.
 * NULL, empty, canonical, soak, test/copy, and arbitrary paths fail closed. */
bool hotswap_datadir_is_dev(const char *resolved_datadir);

/* Pure fail-closed v2 manifest validation. Does not call generation code. */
bool hotswap_manifest_v2_validate(
    const struct zcl_hotswap_manifest_v2 *manifest,
    char *why,
    size_t why_sz);

/* Number of generations loaded this process (monotone; never decremented). */
size_t hotswap_generation_count(void);

/* zcl_state subsystem=hotswap. See CLAUDE.md "Adding state introspection".
 * Reentrant-safe; `out` is initialized (json_set_object) by this function. */
bool hotswap_dump_state_json(struct json_value *out, const char *key);

/* ── Generation entrypoint emitter ─────────────────────────────────────
 *
 * Invoke ONCE at file scope in a swap-eligible controller TU, passing the
 * controller's static route table and its element count, e.g.:
 *
 *     ZCL_HOTSWAP_EXPORT_ROUTES(k_routes, PARAM_COUNT(k_routes))
 *
 * (no trailing semicolon). Under a generation .so build (-DZCL_HOTSWAP_GEN)
 * it emits the manifest plus `zcl_hotswap_gen_init`, which stages every route
 * the controller owns — the routes (and their handlers) resolve to the .so's
 * OWN freshly-compiled copies, so dispatch runs the edited code. In the node
 * build AND in release it expands to nothing, so the symbol only ever exists
 * inside a gen .so (never in the shipped binary).
 *
 * The body is expanded in the controller TU, where `struct mcp_tool_route`
 * is complete — that is why lib/hotswap needs only a forward declaration. */
#ifdef ZCL_HOTSWAP_GEN
#ifndef ZCL_HOTSWAP_BUILD_IDENTITY
/* The generation build must bind this independently of the host's runtime
 * getter. Empty fails manifest validation instead of freezing the host build
 * macro into another production translation unit. */
#define ZCL_HOTSWAP_BUILD_IDENTITY ""
#endif
#ifndef ZCL_HOTSWAP_SOURCE_ID
#define ZCL_HOTSWAP_SOURCE_ID __FILE__
#endif
#ifndef ZCL_HOTSWAP_INPUT_DIGEST
/* Fail closed until the generation build supplies a SHA-256 over its exact
 * source/header/flag inputs. The loader rejects this empty sentinel. */
#define ZCL_HOTSWAP_INPUT_DIGEST ""
#endif
#ifndef ZCL_HOTSWAP_MAPPED_TESTS
#define ZCL_HOTSWAP_MAPPED_TESTS "hotswap_loader,hotswap_simnet"
#endif
#ifndef ZCL_HOTSWAP_PROBE_TOOLS
#define ZCL_HOTSWAP_PROBE_TOOLS "zcl_name_list"
#endif
#ifndef ZCL_HOTSWAP_PROBE_LEAF
#define ZCL_HOTSWAP_PROBE_LEAF "node.status"
#endif

/* Generalized manifest emitter, parameterized by the host ABI/size/caps/probe
 * so both the v2 (mcp.routes) and v3 (native.leaves) provider classes share
 * one body. The self-test validates the host against the SAME abi/size/caps the
 * manifest declares. A TU invokes exactly one EXPORT_* macro, so the single
 * zcl_hotswap_default_self_test / zcl_hotswap_manifest_v2 symbols never clash. */
#define ZCL_HOTSWAP_EXPORT_MANIFEST(host_abi_, host_size_, required_caps_,   \
                                    provider_id_, probe_csv_)                \
    static bool zcl_hotswap_default_self_test(                              \
        const struct zcl_hotswap_host *host, char *why, size_t why_sz)       \
    {                                                                        \
        if (!host || host->abi_version != (host_abi_) ||                    \
            host->struct_size != (host_size_) ||                             \
            (host->capabilities & (required_caps_)) != (required_caps_)) {   \
            if (why && why_sz)                                               \
                (void)snprintf(why, why_sz, "generation host contract mismatch"); \
            return false;                                                    \
        }                                                                    \
        if (why && why_sz) why[0] = '\0';                                   \
        return true;                                                         \
    }                                                                        \
    const struct zcl_hotswap_manifest_v2 zcl_hotswap_manifest_v2 = {        \
        .schema_version = ZCL_HOTSWAP_MANIFEST_SCHEMA_V2,                   \
        .struct_size = sizeof(struct zcl_hotswap_manifest_v2),              \
        .host_abi_version = (host_abi_),                                    \
        .host_struct_size = (host_size_),                                   \
        .required_host_capabilities = (required_caps_),                     \
        .provider_id = (provider_id_),                                      \
        .build_identity = ZCL_HOTSWAP_BUILD_IDENTITY,                       \
        .source_identity = ZCL_HOTSWAP_SOURCE_ID,                           \
        .input_digest = ZCL_HOTSWAP_INPUT_DIGEST,                           \
        .state_schema_version = 0,                                          \
        .stateless = true,                                                   \
        .quiescence = ZCL_HOTSWAP_QUIESCENCE_NONE,                          \
        .mapped_tests_csv = ZCL_HOTSWAP_MAPPED_TESTS,                       \
        .probe_tools_csv = (probe_csv_),                                    \
        .self_test = zcl_hotswap_default_self_test,                         \
    };

/* v2 alias (unchanged emitted bytes: host_struct_size resolves to the frozen
 * v2 prefix size, identical to the pre-v3 sizeof). */
#define ZCL_HOTSWAP_EXPORT_V2_MANIFEST(required_caps_, provider_id_)         \
    ZCL_HOTSWAP_EXPORT_MANIFEST(ZCL_HOTSWAP_HOST_ABI_V2,                     \
                                ZCL_HOTSWAP_HOST_STRUCT_SIZE_V2,             \
                                required_caps_, provider_id_,                \
                                ZCL_HOTSWAP_PROBE_TOOLS)

#define ZCL_HOTSWAP_EXPORT_ROUTES(routes_arr, routes_count)                  \
    ZCL_HOTSWAP_EXPORT_V2_MANIFEST(                                          \
        ZCL_HOTSWAP_V2_HOST_CAPABILITIES, "mcp.routes")                     \
    bool zcl_hotswap_gen_init(const struct zcl_hotswap_host *host);          \
    bool zcl_hotswap_gen_init(const struct zcl_hotswap_host *host)           \
    {                                                                        \
        if (!host || !host->mcp_stage) return false;                         \
        bool zcl__all = true;                                                \
        for (size_t zcl__i = 0; zcl__i < (size_t)(routes_count); zcl__i++) { \
            if (!host->mcp_stage((routes_arr)[zcl__i].name,                  \
                                 &(routes_arr)[zcl__i]))                      \
                zcl__all = false;                                            \
        }                                                                    \
        return zcl__all;                                                     \
    }

/* ── native.leaves (v3 host) generation entrypoint emitter ─────────────
 *
 * Invoke ONCE at file scope in a swap-eligible native command controller TU,
 * passing the controller's static {path,handler} table and its element count:
 *
 *     ZCL_HOTSWAP_EXPORT_LEAVES(k_leaves, PARAM_COUNT(k_leaves))
 *
 * (no trailing semicolon). Under a generation .so build it emits the manifest
 * plus zcl_hotswap_gen_init, which stages every command leaf the controller
 * owns via the host's v3 leaf_stage vtable entry — the handlers resolve to the
 * .so's OWN freshly-compiled copies. In node/release builds it expands to
 * nothing. struct zcl_command_request/reply are complete only here, which is
 * why lib/hotswap needs only a forward declaration. */
#define ZCL_HOTSWAP_EXPORT_LEAVES(leaves_arr, leaves_count)                  \
    ZCL_HOTSWAP_EXPORT_MANIFEST(ZCL_HOTSWAP_HOST_ABI_V3,                     \
                                ZCL_HOTSWAP_HOST_STRUCT_SIZE_V3,             \
                                ZCL_HOTSWAP_V3_HOST_CAPABILITIES,            \
                                "native.leaves", ZCL_HOTSWAP_PROBE_LEAF)     \
    bool zcl_hotswap_gen_init(const struct zcl_hotswap_host *host);          \
    bool zcl_hotswap_gen_init(const struct zcl_hotswap_host *host)           \
    {                                                                        \
        if (!host || !host->leaf_stage) return false;                        \
        bool zcl__all = true;                                                \
        for (size_t zcl__i = 0; zcl__i < (size_t)(leaves_count); zcl__i++) { \
            if (!host->leaf_stage((leaves_arr)[zcl__i].path,                 \
                                  (leaves_arr)[zcl__i].handler))             \
                zcl__all = false;                                            \
        }                                                                    \
        return zcl__all;                                                     \
    }
#else
#define ZCL_HOTSWAP_EXPORT_ROUTES(routes_arr, routes_count) /* node/release: omitted */
#define ZCL_HOTSWAP_EXPORT_LEAVES(leaves_arr, leaves_count) /* node/release: omitted */
#endif

/* ── Legacy direct-provider marker (reload-required under v2) ──────────
 *
 * REST/diagnostics still carry this source marker pending a unified provider
 * registry. A generation build exports an explicitly unsupported capability
 * and an init function that never evaluates the old immediate-install
 * expression. The v2 loader therefore rejects it before generation code.
 * Invoke once at file scope as before, e.g.:
 *
 *     ZCL_HOTSWAP_EXPORT_PROVIDER(
 *         api_resource_dispatch_replace(api_resource_route_dispatch_builtin))
 *
 *
 * In ordinary node and release builds it remains a no-op. */
#ifdef ZCL_HOTSWAP_GEN
#define ZCL_HOTSWAP_EXPORT_PROVIDER(install_expr)                            \
    ZCL_HOTSWAP_EXPORT_V2_MANIFEST(                                          \
        ZCL_HOTSWAP_CAP_DIRECT_PROVIDER, "reload_required.direct_provider")  \
    bool zcl_hotswap_gen_init(const struct zcl_hotswap_host *host);          \
    bool zcl_hotswap_gen_init(const struct zcl_hotswap_host *host)           \
    {                                                                        \
        (void)host;                                                          \
        return false;                                                        \
    }
#else
#define ZCL_HOTSWAP_EXPORT_PROVIDER(install_expr) /* node/release: omitted */
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZCL_HOTSWAP_H */
