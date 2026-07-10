/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tier 1 in-process hot-swap loader (DEV-ONLY).
 *
 * An AI agent edits an app-layer MCP controller .c, `make hotswap-so`
 * compiles the changed TU(s) into a generation .so, and this loader
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

#ifdef __cplusplus
extern "C" {
#endif

/* Forward decls only — lib/hotswap must not include tools/mcp/router.h
 * (lib-layer purity) nor pull the full json header into consumers that
 * only need the dump signature. */
struct mcp_tool_route;
struct json_value;

/* Host vtable handed to a generation's zcl_hotswap_gen_init(). The
 * generation STAGES its replacements by calling host->mcp_replace() for
 * each tool it carries; mcp_replace validates the new route and only then
 * atomically publishes it into the live router slot. */
struct zcl_hotswap_host {
    uint32_t gen;
    bool (*mcp_replace)(const char *name, const struct mcp_tool_route *route);
};

/* The symbol every generation .so must export. */
typedef bool (*zcl_hotswap_gen_init_fn)(const struct zcl_hotswap_host *host);

#define ZCL_HOTSWAP_GEN_MAX_REPLACED 64

/* Result of a load attempt. Always fully populated (even on failure). */
struct hotswap_load_report {
    bool     ok;                  /* gen_init returned true, all replaces ok */
    uint32_t gen;                 /* assigned generation id (0 if not loaded) */
    char     error[256];          /* human-readable failure reason ("" on ok) */
    char     replaced[ZCL_HOTSWAP_GEN_MAX_REPLACED][64]; /* tool names published */
    size_t   replaced_count;
    bool     replaced_overflow;   /* > MAX_REPLACED tools re-pointed */
};

/* Route-replacement callback supplied by the caller (the MCP dev-hotswap
 * controller wraps mcp_router_replace). Kept as a caller-supplied pointer
 * so this lib TU never links against the tools/mcp router. */
typedef bool (*zcl_hotswap_replace_cb)(const char *name,
                                       const struct mcp_tool_route *route);

/* Load a generation .so and stage its route replacements.
 *
 *   so_path          absolute path to the gen .so (see hotswap_path_is_acceptable)
 *   resolved_datadir the node's resolved datadir (the canonical live datadir
 *                    ~/.zclassic-c23 and legacy ~/.zclassic are REFUSED)
 *   replace_cb       publishes one route (wraps mcp_router_replace)
 *   report           out — always populated
 *
 * Returns true iff the .so loaded, gen_init ran, and every staged replace
 * succeeded. DEV-ONLY: without ZCL_DEV_BUILD this refuses with an
 * "unavailable" error and performs NO dlopen. */
bool hotswap_load(const char *so_path,
                  const char *resolved_datadir,
                  zcl_hotswap_replace_cb replace_cb,
                  struct hotswap_load_report *report);

/* ── Pure predicates (compiled in ALL builds; unit-tested w/o dlopen) ── */

/* True if so_path is safe to load: absolute, ends ".so", exists, and its
 * realpath sits under /tmp or a repo build/hotswap dir. On false, `why`
 * (if non-NULL) gets a short reason. */
bool hotswap_path_is_acceptable(const char *so_path, char *why, size_t why_sz);

/* True if resolved_datadir is a dev/test/soak datadir (i.e. NOT the
 * canonical live ~/.zclassic-c23 nor legacy ~/.zclassic). */
bool hotswap_datadir_is_dev(const char *resolved_datadir);

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
 * it emits the well-known `zcl_hotswap_gen_init` that re-points every route
 * the controller owns — the routes (and their handlers) resolve to the .so's
 * OWN freshly-compiled copies, so dispatch runs the edited code. In the node
 * build AND in release it expands to nothing, so the symbol only ever exists
 * inside a gen .so (never in the shipped binary).
 *
 * The body is expanded in the controller TU, where `struct mcp_tool_route`
 * is complete — that is why lib/hotswap needs only a forward declaration. */
#ifdef ZCL_HOTSWAP_GEN
#define ZCL_HOTSWAP_EXPORT_ROUTES(routes_arr, routes_count)                  \
    bool zcl_hotswap_gen_init(const struct zcl_hotswap_host *host);          \
    bool zcl_hotswap_gen_init(const struct zcl_hotswap_host *host)           \
    {                                                                        \
        if (!host || !host->mcp_replace) return false;                       \
        bool zcl__all = true;                                                \
        for (size_t zcl__i = 0; zcl__i < (size_t)(routes_count); zcl__i++) { \
            if (!host->mcp_replace((routes_arr)[zcl__i].name,                \
                                   &(routes_arr)[zcl__i]))                    \
                zcl__all = false;                                            \
        }                                                                    \
        return zcl__all;                                                     \
    }
#else
#define ZCL_HOTSWAP_EXPORT_ROUTES(routes_arr, routes_count) /* node/release: omitted */
#endif

/* ── Provider-trampoline entrypoint emitter ────────────────────────────
 *
 * The generic sibling of ZCL_HOTSWAP_EXPORT_ROUTES for swap-eligible TUs
 * that publish a single atomic PROVIDER function pointer (REST resource
 * dispatch, diagnostics `dumpstate`) rather than an MCP route table. Invoke
 * ONCE at file scope, passing a boolean install EXPRESSION that re-points the
 * resident provider at this TU's freshly-compiled implementation, e.g.:
 *
 *     ZCL_HOTSWAP_EXPORT_PROVIDER(
 *         api_resource_dispatch_replace(api_resource_route_dispatch_builtin))
 *
 * (no trailing semicolon). Under a generation .so build (-DZCL_HOTSWAP_GEN)
 * it emits the well-known `zcl_hotswap_gen_init` that evaluates the install
 * expression. CORRECTNESS CONTRACT: the install function named in the
 * expression MUST live in a RESIDENT (non-swap-eligible) TU — it is an
 * undefined symbol in the .so and so binds to the executable's resident copy
 * (RTLD_LOCAL), which mutates the resident provider the live read path reads.
 * The implementation it installs (e.g. the *_builtin) is defined in THIS TU,
 * so it resolves to the .so's own freshly-compiled copy. In the node build
 * AND in release the macro expands to nothing (symbol lives only inside a
 * gen .so, never in the shipped binary). */
#ifdef ZCL_HOTSWAP_GEN
#define ZCL_HOTSWAP_EXPORT_PROVIDER(install_expr)                            \
    bool zcl_hotswap_gen_init(const struct zcl_hotswap_host *host);          \
    bool zcl_hotswap_gen_init(const struct zcl_hotswap_host *host)           \
    {                                                                        \
        (void)host;                                                          \
        return (install_expr);                                               \
    }
#else
#define ZCL_HOTSWAP_EXPORT_PROVIDER(install_expr) /* node/release: omitted */
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZCL_HOTSWAP_H */
