/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_TOOLS_NATIVE_COMMAND_H
#define ZCL_TOOLS_NATIVE_COMMAND_H

#include "kernel/command_registry.h"
#include "controllers/native_handler_body.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool zcl_native_command_is_root(const char *word);

/* Resolve argv under a canonical root through the registry and print exactly
 * one bounded JSON document (branch menu, discovery document, common result
 * envelope, or structured unknown-command error). Returns a contract exit code
 * (0..6). A typo under a canonical branch returns the structured unknown-branch
 * error and NEVER falls through to the arbitrary RPC method fallback.
 * datadir/rpc_port target the running node for READ-ONLY bridge leaves. */
int zcl_native_command_main(const char *root_word,
                            const char *const *args, int nargs,
                            const char *datadir, int rpc_port);

/* Ensure the one-shot JSON-RPC client (datadir cookie + port) is initialized
 * from the CLI-resolved -datadir/-rpcport. Handlers that call node_rpc_call()
 * or node_rpc_client_datadir() WITHOUT going through the bridge dispatch
 * (e.g. the dev hot-swap handlers) must call this first — the bridge would
 * otherwise leave the client global empty in a fresh CLI process. */
void zcl_native_bridge_ensure_rpc(void);

/* Generic transport binding for READ-ONLY Core/Ops leaves. Resolves the
 * leaf's canonical path to exactly one dispatch: either a transport-neutral
 * body function or, for a pure pass-through leaf, the backing JSON-RPC method
 * directly. The body is wrapped in the common zcl.result.v1 envelope. Bound
 * by config/src/command_catalog.c. */
void zcl_native_bridge_command(const struct zcl_command_request *request,
                               struct zcl_command_reply *reply);

/* Run a bridged leaf with an EXPLICIT body function — the reusable core of
 * zcl_native_bridge_command: build args from the request, dispatch the passed
 * `body` (or, when `body` is NULL and the leaf is a pure 1:1 proxy, the
 * backing JSON-RPC method directly), then project the body into the reply
 * envelope. zcl_native_bridge_command is a thin wrapper that supplies
 * zcl_native_bridge_body_for_path(path); a hot-swap generation instead
 * supplies its own freshly-compiled body for an existing body-backed bridge
 * path. Non-bridge paths and ambiguous/missing bindings fail closed with
 * NO_BRIDGE_BINDING. */
void zcl_native_bridge_run(const struct zcl_command_request *request,
                           zcl_native_body_fn body,
                           struct zcl_command_reply *reply);

/* Project a bridged command body into reply->data bounded by request->view
 * (summary|normal|full), request->budget_bytes, request->max_items, and
 * request->cursor, emitting an explicit `_page` descriptor and — when
 * truncated — one structured retrieval next-command. Exposed for golden tests
 * so progressive disclosure can be proven without contacting a node. */
void zcl_native_bridge_project(const struct zcl_command_request *request,
                               const struct json_value *body,
                               struct zcl_command_reply *reply);

/* Dispatch lookups. Every bridged leaf resolves to exactly
 * one of the two: a re-homed body function OR a direct JSON-RPC method.
 * Pure lookups — no node contact. The golden catalog test proves the union
 * covers every bridged leaf and never overlaps. */
zcl_native_body_fn zcl_native_bridge_body_for_path(const char *path);
const char *zcl_native_bridge_rpc_for_path(const char *path);

void zcl_native_handle_discover_help(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_discover_search(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_discover_describe(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_discover_schema(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_status(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
/* dev.ff — the fail-fast edit-loop ladder (`make ff`: compile -> focused
 * tests -> lint-fast, see tools/command/native_dev_command.c). A release
 * build's copy of this function is a `#ifndef ZCL_DEV_BUILD` stub that fails
 * BLOCKED without spawning anything (zcl_devloop_process_run is dev-only
 * linked, see Makefile DEV_ONLY_SRCS). */
void zcl_native_handle_dev_ff(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_core_boundary(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_app_describe(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_app_plan(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_app_simulate(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_change_plan(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
/* dev.vcs.revert — one-command source+binary revert (see
 * tools/command/native_dev_command.c). A release build's copy of this
 * function is a `#ifndef ZCL_DEV_BUILD` stub that fails BLOCKED without
 * touching lib/vcs/ or spawning anything. */
void zcl_native_handle_dev_vcs_revert(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
/* dev.vcs.seal.grant — owner-run ZVCS unseal-token ritual (see
 * tools/command/native_dev_command.c). Mirrors dev.vcs.revert's shape: a
 * release build's copy of this function is a `#ifndef ZCL_DEV_BUILD` stub
 * that fails BLOCKED without touching lib/vcs/ or spawning anything. */
void zcl_native_handle_dev_vcs_seal_grant(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_app_list(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* ── code.* — the source-code navigator (tools/command/native_code_command.c).
 * Local, read-only, deterministic leaves backed by the in-binary lib/codeindex
 * index. Each renders one bounded JSON document (structured array + human
 * one-liners) well within ZCL_COMMAND_RESULT_BUDGET. */
void zcl_native_handle_code_group(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_code_file(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_code_sym(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_code_refs(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_code_find(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
/* code.map — the whole-tree map: the 9 root groups (aggregate file counts +
 * purposes) and the 8 app/ shapes (direct file counts), plus a total. */
void zcl_native_handle_code_map(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
/* code.tests — the routing link: which focused test group a change to one file
 * routes to, mirroring `dev test plan` (tools/dev/devloop_plan.c). */
void zcl_native_handle_code_tests(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
/* code.room — the unified single-room view: composes shape + purpose + group +
 * neighbors + tests/route for one path into one bounded document (palace-design
 * §2). The command→file join is degraded to null (registry stores handler
 * pointers, not symbol names). */
void zcl_native_handle_code_room(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* Resolve the focused-test proof group for a changed source `path`, mirroring
 * tools/dev/devloop_plan.c:171-185 so `code tests` and `dev test plan` never
 * disagree. When non-NULL, `acc` is filled with the matched shared-rule groups
 * (caller may enumerate acc->groups[0..groups_len]) and `consensus_risk`
 * reports whether the path is a consensus / sealed-core surface. Returns a
 * static string owned by the registry ("consensus_parity" / a shared-rule
 * group / "make_lint_gates") — pure, no node contact, no allocation. */
struct agent_impact_acc;
const char *zcl_native_code_route_for_path(const char *path,
                                           struct agent_impact_acc *acc,
                                           bool *consensus_risk);
void zcl_native_handle_app_inspect(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* ops.state — generic subsystem state dump. Dispatches the `dumpstate` RPC
 * method directly. `subsystem` (required) selects the
 * owning module's *_dump_state_json; `key` is subsystem-specific (e.g. a
 * block_index height/hash). Bound by config/src/command_catalog.c. */
void zcl_native_handle_ops_state(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* core.network.chain_view — the reachable-network chain view (modal tip, max
 * advertised height, our delta, fork clusters) from the node's network_monitor.
 * Reads the running node's network_monitor dumpstate over the read-only RPC.
 * Bound by
 * config/src/command_catalog.c. */
void zcl_native_handle_network_chain_view(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* core.network.census / .node / .versions / .graph — READ-ONLY operator
 * surface over the banked network census + topology stores (node_census,
 * topology_edges, census_observations under <datadir>/peers_projection.db +
 * topology.db). These
 * open the census SQLite files with SQLITE_OPEN_READONLY in the one-shot CLI
 * process; no running node is required and consensus is never touched. When the
 * indexer lane has not yet created a table they degrade gracefully
 * ("census empty: indexer not yet populated"), never error. Every result is
 * bounded (paginated node list, bounded observation/edge history, bounded
 * distribution). Bound by config/commands/core.def. */
void zcl_native_handle_network_census(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_network_node(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_network_versions(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_network_graph(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* Resolved datadir for the one-shot native CLI process (the --datadir value
 * captured by zcl_native_command_main, or "" when none was given). Read-only
 * accessor for handlers that open a datadir-relative store directly. */
const char *zcl_native_command_datadir(void);

/* ops.selftest — node-free registry self-test. Sweeps every catalog leaf for
 * the static
 * well-formedness the registry guarantees (READY ⇒ dispatchable handler +
 * schema/example + read-effect/risk agreement + a bound bridge tool) and
 * reports total/pass/fail/skip + the failing paths. Deterministic and
 * node-independent so the dev-lane deploy verify can gate on fail == 0. */
void zcl_native_handle_ops_selftest(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* ops.debug.backtrace — dump a live backtrace for every thread of the running
 * node. Dispatches the `selfbacktrace` RPC method directly and
 * projects { path, thread_count }. Answers "what is every thread doing right
 * now" where perf/gdb/ptrace are blocked. Bound by config/commands/ops.def. */
void zcl_native_handle_ops_debug_backtrace(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* ops.debug.bundle — write a one-shot debug bundle (every registered state
 * dumper + build identity + supervisor stall summary) as ONE JSON document
 * to <datadir>/debug-bundle-<utc>.json on the running node. Dispatches the
 * `debugbundle` RPC method directly and projects { path, bytes,
 * subsystems_captured, subsystems_failed }. Bound by config/commands/ops.def. */
void zcl_native_handle_ops_debug_bundle(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* ops.explain <topic> — compose one prose-like diagnostic from four surfaces
 * (reducer frontier, blocker registry, condition engine, health/sync RPCs).
 * Topics: sync, blockers, health (table-dispatched, see
 * app/controllers/src/explain_native_handlers.c). Reply carries data.text +
 * structured fields; the CLI prints text unless --format=json. */
void zcl_native_handle_ops_explain(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* ops.profile [seconds] [top_n] — dispatch the `profile` RPC (in-process
 * /proc/self/task sampling) and render the busiest threads + verdict + reducer
 * stage step-EWMA. */
void zcl_native_handle_ops_profile(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* ops.producer.status --datadir= — read a mint/anchor producer's progress.kv
 * (stage cursors + session/receipt lifecycle) and mint-progress.log tail with
 * NO node contact. Read-only. */
void zcl_native_handle_ops_producer_status(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* ops.rom — dispatch the `dumpstate rom_compile` RPC against THIS running
 * node and render its ROM-compilation-fold telemetry (zcl.rom_compile.v1) as
 * rich ASCII: a fold progress bar, a horizontal bar chart of the eight
 * reducer stages' step_us_ewma (the bottleneck stage highlighted), and the
 * layer ladder (ROM checkpoint / sealed segment history / sealed state-seal
 * ring / delta frontier / tip ring), each filled or empty. The structured
 * fields are also returned verbatim in reply->data for machine consumers;
 * the CLI prints data.text unless --format=json. See
 * app/jobs/src/rom_compile_status.c for the data source (all EXISTING
 * telemetry — no second producer). */
void zcl_native_handle_ops_rom(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* Render the CLI UX contract's ONE-LINE status brief: a single line, <=200
 * bytes, stable `key=value` pairs (no JSON braces), from the flat brief body
 * core.status.brief emits (see zcl_native_status_brief_body): hstar, gap,
 * peer_best, sync_state, primary_blocker, blocker_age_s, active_conditions,
 * peer_count, rss_mb. `buf`/`cap` must be at least 256 bytes; the line is
 * always NUL-terminated and never exceeds 200 visible bytes before the
 * terminator. Exposed for test_operator_ux. See docs/NATIVE_COMMAND_INTERFACE.md
 * "CLI UX contract" for the frozen field list. */
struct json_value;
void zcl_native_status_brief_render(const struct json_value *data, char *buf,
                                    size_t cap);

/* Pick one short, deterministic next-step command from the same brief body
 * (dominant blocker present -> explain it; still behind -> explain sync;
 * otherwise -> a general healthcheck). Never allocates; returns a pointer to
 * a static string. Backs the bare no-arg `zclassic23` entry point and any
 * leaf invoked with --next. */
const char *zcl_native_status_brief_next_command(const struct json_value *data);

/* Select named top-level fields out of a JSON object and render each as one
 * "key=value\n" line (bools -> true/false, ints -> decimal, strings verbatim,
 * null -> "null", real -> "%g", an object/array value -> compact JSON on that
 * same line). `fields_csv` is a comma-separated, whitespace-tolerant list of
 * top-level key names (max 24, no duplicates). Returns true and fills `out`
 * (NUL-terminated, each line ending in '\n') only when every requested name
 * exists in `obj` — never a partial selection. On any unknown/duplicate name,
 * an empty list, or an output overflow, returns false and fills `err` with a
 * short, human-readable reason (naming the bad field and, space permitting,
 * up to 12 of the object's own known top-level keys); `out` is left
 * untouched. This is the ONE implementation `status field=` and
 * `dumpstate ... field=` both call — see docs/NATIVE_COMMAND_INTERFACE.md
 * "CLI UX contract". */
bool zcl_native_render_field_selection(const struct json_value *obj,
                                       const char *fields_csv,
                                       char *out, size_t out_cap,
                                       char *err, size_t err_cap);

/* Build the CLI UX contract's unrecognized-command diagnostic (see
 * docs/NATIVE_COMMAND_INTERFACE.md "CLI UX contract"): one typed
 * `error=UNKNOWN_COMMAND detail=... try=...` line, plus (when the existing
 * command-search index returns any hit — reused, never a new fuzzy matcher)
 * a `did you mean: ...` line naming up to 3 candidate paths. Writes into
 * `out` (NUL-terminated, newline after each line) and returns the byte
 * count written (0 on a NULL/empty method or an output overflow — the
 * caller should fall back to a minimal one-liner in that case). Pure:
 * takes the registry explicitly so a test can pass a fixture instead of the
 * live catalog. */
size_t zcl_native_render_unknown_command(
    const struct zcl_command_registry *reg, const char *method, char *out,
    size_t out_cap);

/* core.node.bootstatus / core.node.bootwait — pre-RPC boot observability. Both
 * read <datadir>/boot_status.json directly off disk (util/boot_status.h): no
 * node contact, no RPC. bootstatus returns the current beacon (or BLOCKED when
 * none exists yet); bootwait polls until phase=serving or a bounded timeout.
 * This is the typed replacement for ss/ps/tail node.log boot watching. Bound
 * by config/src/command_catalog.c. */
void zcl_native_handle_core_node_bootstatus(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_core_node_bootwait(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* app.auth.* / app.account.* — the multi-user-server identity surface
 * (app/controllers/src/account_controller.c), mounted under the `app` root.
 * app.auth.challenge/app.auth.verify are PUBLIC (no capability):
 * challenge/response public-key login. app.account.* manage principals:
 * list/show/whoami are reads, add/role/suspend/unsuspend are the first
 * executable mutating native leaves (OWNER authority). Each renders one bounded
 * JSON document. Bound by config/commands/accounts.def. */
void zcl_native_handle_auth_challenge(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_auth_verify(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_account_list(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_account_show(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_account_whoami(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_account_add(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_account_role(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_account_suspend(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_account_unsuspend(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* ROM-seed policy/ledger surface (app/controllers/src/rom_seed_controller.c)
 * — see config/commands/ops.def `ops.rom_seed.*` and docs/ROM_DELIVERY.md. */
void zcl_native_handle_rom_seed_status(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_rom_seed_enable(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_rom_seed_disable(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_rom_seed_artifacts(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* ROM-fetch engine surface (app/controllers/src/rom_fetch_controller.c) —
 * see config/commands/ops.def `ops.debug.rom_fetch.*` and
 * docs/ROM_DELIVERY.md. The fetch leaf downloads + content-verifies bytes
 * only; activation stays with the unified -install-consensus-bundle path. */
void zcl_native_handle_rom_fetch_status(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_rom_fetch_bundle(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);

/* Dev-build-only executors.  The catalog binds these only when
 * ZCL_DEV_BUILD is set; release objects neither reference nor link them. */
#ifdef ZCL_DEV_BUILD
void zcl_native_handle_dev_change_apply(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_loop_ensure(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_loop_status(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_loop_wait(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_loop_stop(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_test_run(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_test_sim(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_generation_current(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_generation_history(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
/* Both hot-swap commands are hard-contained compatibility entrypoints; probe
 * must not dlopen candidates in the resident node before ELF admission. */
void zcl_native_handle_dev_hotswap_apply(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_hotswap_probe(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
#endif

#if defined(ZCL_DEV_BUILD) || defined(ZCL_TESTING)
void zcl_native_handle_dev_diagnose_latest(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
void zcl_native_handle_dev_diagnose_show(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZCL_TOOLS_NATIVE_COMMAND_H */
