/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Canonical command registry shared by native, REST, and optional MCP
 * adapters.  The registry owns command identity and policy; transport
 * adapters only normalize input and render the bounded result.
 */

#ifndef ZCL_KERNEL_COMMAND_REGISTRY_H
#define ZCL_KERNEL_COMMAND_REGISTRY_H

#include "json/json.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZCL_COMMAND_ROOT_BUDGET 1200U
#define ZCL_COMMAND_BRANCH_BUDGET 1600U
/* Raised from 2400 to absorb the per-leaf `semantics` contract and effective
 * `budget_bytes` the describe document now emits. */
#define ZCL_COMMAND_SPEC_BUDGET 2816U
#define ZCL_COMMAND_STATUS_BUDGET 2048U
#define ZCL_COMMAND_ERROR_BUDGET 2048U
#define ZCL_COMMAND_RESULT_BUDGET 4096U
#define ZCL_COMMAND_LIST_BUDGET 8192U
#define ZCL_COMMAND_SEARCH_LIMIT 5U
#define ZCL_COMMAND_MAX_NEXT 3U
#define ZCL_COMMAND_MAX_PATH 128U
#define ZCL_COMMAND_MAX_INPUT 16384U

enum zcl_command_layer {
    ZCL_COMMAND_LAYER_ROOT = 0,
    ZCL_COMMAND_LAYER_CORE,
    ZCL_COMMAND_LAYER_APP,
    ZCL_COMMAND_LAYER_DEV,
    ZCL_COMMAND_LAYER_OPS,
    ZCL_COMMAND_LAYER_DISCOVER,
    ZCL_COMMAND_LAYER_CODE,
};

enum zcl_command_effect {
    ZCL_COMMAND_EFFECT_READ = 0,
    ZCL_COMMAND_EFFECT_MUTATE,
    ZCL_COMMAND_EFFECT_DESTRUCTIVE,
};

enum zcl_command_risk {
    ZCL_COMMAND_RISK_READ = 0,
    ZCL_COMMAND_RISK_APP_WRITE,
    ZCL_COMMAND_RISK_WALLET,
    ZCL_COMMAND_RISK_CORE_RECOVERY,
    ZCL_COMMAND_RISK_DESTRUCTIVE,
    ZCL_COMMAND_RISK_DEV_MUTATION,
};

enum zcl_command_scope {
    ZCL_COMMAND_SCOPE_LOCAL = 0,
    ZCL_COMMAND_SCOPE_NODE,
    ZCL_COMMAND_SCOPE_DEV_LANE,
    ZCL_COMMAND_SCOPE_OFFLINE_COPY,
};

enum zcl_command_authority {
    ZCL_COMMAND_AUTH_PUBLIC = 0,
    ZCL_COMMAND_AUTH_OPERATOR,
    ZCL_COMMAND_AUTH_OWNER,
};

enum zcl_command_availability {
    ZCL_COMMAND_READY = 0,
    ZCL_COMMAND_COMPAT,
    ZCL_COMMAND_PLANNED,
};

enum zcl_command_mode {
    ZCL_COMMAND_MODE_BRANCH = 0,
    ZCL_COMMAND_MODE_SYNC,
    ZCL_COMMAND_MODE_JOB,
    ZCL_COMMAND_MODE_STREAM,
};

enum zcl_command_latency {
    ZCL_COMMAND_LATENCY_INSTANT = 0,
    ZCL_COMMAND_LATENCY_FAST,
    ZCL_COMMAND_LATENCY_FOREGROUND,
    ZCL_COMMAND_LATENCY_BACKGROUND,
    ZCL_COMMAND_LATENCY_PERSISTENT,
};

/* Per-latency-bucket dispatch budget in milliseconds. Rehomes the legacy
 * agent_first_call.h budgets (250/500/750/900, MCP-only, deleted in zero-MCP
 * W3) as the kernel's own contract so every native leaf carries it, not just
 * the five MCP agent controllers that used to. */
#define ZCL_COMMAND_LATENCY_BUDGET_INSTANT_MS    50
#define ZCL_COMMAND_LATENCY_BUDGET_FAST_MS       250
#define ZCL_COMMAND_LATENCY_BUDGET_FOREGROUND_MS 750
#define ZCL_COMMAND_LATENCY_BUDGET_BACKGROUND_MS 900
#define ZCL_COMMAND_LATENCY_BUDGET_PERSISTENT_MS 900

/* >= the compiled catalog's leaf count; sized with headroom for the per-leaf
 * latency-sample ring (OS-B2 §2). config/src/command_catalog.c asserts against
 * this at compile time (size guard). */
#define ZCL_COMMAND_LATENCY_TABLE_MAX 512U

/* Maps a leaf's declared `latency` enum to its dispatch budget in ms. Pure,
 * total: an out-of-range value falls back to the PERSISTENT/900ms ceiling,
 * never 0 or undefined behavior. */
int64_t zcl_command_latency_budget_ms(enum zcl_command_latency latency);

enum zcl_command_cost {
    ZCL_COMMAND_COST_TINY = 0,
    ZCL_COMMAND_COST_LOW,
    ZCL_COMMAND_COST_MODERATE,
    ZCL_COMMAND_COST_HIGH,
    ZCL_COMMAND_COST_STREAM,
};

enum zcl_command_confirmation {
    ZCL_COMMAND_CONFIRM_NONE = 0,
    ZCL_COMMAND_CONFIRM_IDEMPOTENCY,
    ZCL_COMMAND_CONFIRM_PLAN_COMMIT,
};

enum zcl_command_status {
    ZCL_COMMAND_STATUS_PASSED = 0,
    ZCL_COMMAND_STATUS_ACCEPTED,
    ZCL_COMMAND_STATUS_BLOCKED,
    ZCL_COMMAND_STATUS_FAILED,
};

enum zcl_command_exit {
    ZCL_COMMAND_EXIT_OK = 0,
    ZCL_COMMAND_EXIT_FAILED = 1,
    ZCL_COMMAND_EXIT_INVALID = 2,
    ZCL_COMMAND_EXIT_BLOCKED = 3,
    ZCL_COMMAND_EXIT_DENIED = 4,
    ZCL_COMMAND_EXIT_TRANSIENT = 5,
    ZCL_COMMAND_EXIT_INTERNAL = 6,
};

enum zcl_command_lane {
    ZCL_COMMAND_LANE_LOCAL = 1U << 0,
    ZCL_COMMAND_LANE_DEV = 1U << 1,
    ZCL_COMMAND_LANE_CANONICAL = 1U << 2,
    ZCL_COMMAND_LANE_SOAK = 1U << 3,
    ZCL_COMMAND_LANE_OFFLINE_COPY = 1U << 4,
    ZCL_COMMAND_LANE_ALL_NODE = ZCL_COMMAND_LANE_DEV |
                                ZCL_COMMAND_LANE_CANONICAL |
                                ZCL_COMMAND_LANE_SOAK,
};

enum zcl_command_capability {
    ZCL_COMMAND_CAP_NONE = 0,
    ZCL_COMMAND_CAP_CHAIN_READ = UINT64_C(1) << 0,
    ZCL_COMMAND_CAP_APP_MANIFEST_READ = UINT64_C(1) << 1,
    ZCL_COMMAND_CAP_APP_SIMULATE = UINT64_C(1) << 2,
    ZCL_COMMAND_CAP_CHECKOUT_READ = UINT64_C(1) << 3,
    ZCL_COMMAND_CAP_CHECKOUT_WRITE = UINT64_C(1) << 4,
    ZCL_COMMAND_CAP_DEV_STATE_READ = UINT64_C(1) << 5,
    ZCL_COMMAND_CAP_DEV_STATE_WRITE = UINT64_C(1) << 6,
    ZCL_COMMAND_CAP_PROCESS_EXEC = UINT64_C(1) << 7,
    ZCL_COMMAND_CAP_TEST_RUN = UINT64_C(1) << 8,
    ZCL_COMMAND_CAP_COMPILER = UINT64_C(1) << 9,
    ZCL_COMMAND_CAP_HOTSWAP = UINT64_C(1) << 10,
    ZCL_COMMAND_CAP_DEV_ACTIVATE = UINT64_C(1) << 11,
    ZCL_COMMAND_CAP_WALLET_REQUEST = UINT64_C(1) << 12,
    ZCL_COMMAND_CAP_ZNAM = UINT64_C(1) << 13,
    ZCL_COMMAND_CAP_WEB = UINT64_C(1) << 14,
    ZCL_COMMAND_CAP_ONION = UINT64_C(1) << 15,
    ZCL_COMMAND_CAP_P2P_TOPIC = UINT64_C(1) << 16,
};

enum zcl_command_trait {
    ZCL_COMMAND_TRAIT_NONE = 0,
    ZCL_COMMAND_TRAIT_DETERMINISTIC = 1U << 0,
    ZCL_COMMAND_TRAIT_REVERSIBLE = 1U << 1,
    ZCL_COMMAND_TRAIT_IDEMPOTENT = 1U << 2,
    ZCL_COMMAND_TRAIT_DRY_RUN = 1U << 3,
    ZCL_COMMAND_TRAIT_DEV_ONLY = 1U << 4,
};

enum zcl_command_transport {
    ZCL_COMMAND_TRANSPORT_NONE = 0,
    ZCL_COMMAND_TRANSPORT_NATIVE = 1U << 0,
    ZCL_COMMAND_TRANSPORT_REST = 1U << 1,
    ZCL_COMMAND_TRANSPORT_MCP_COMPACT = 1U << 2,
    ZCL_COMMAND_TRANSPORT_MCP_LEGACY = 1U << 3,
    ZCL_COMMAND_TRANSPORT_RPC = 1U << 4,
};

/* Which transport carried one dispatch, recorded in the command interaction
 * ledger (util/command_ledger). Distinct from the `enum zcl_command_transport`
 * bitmask above (which advertises the set of transports a leaf may be reached
 * through in spec->transports): this is a single dense ordinal per call, 0 ==
 * the local native/CLI path so a zero-initialized context reads as native. */
enum zcl_cmd_transport {
    ZCL_CMD_TRANSPORT_NATIVE = 0,
    ZCL_CMD_TRANSPORT_RPC,
    ZCL_CMD_TRANSPORT_REST,
    ZCL_CMD_TRANSPORT_MCP,
};

struct zcl_command_spec;
struct zcl_command_registry;

struct zcl_command_context {
    const struct zcl_command_registry *registry;
    const char *source_root;
    const char *operator_lane;
    uint64_t granted_capabilities;
    /* The highest command authority this session may exercise. Dispatch fails
     * closed with AUTHORITY_DENIED when spec->authority exceeds it. A
     * zero-initialized context therefore defaults to ZCL_COMMAND_AUTH_PUBLIC
     * (the least-privilege floor); real sessions raise it from their role via
     * authz_ceiling_for_role(). A NULL context bypasses the check entirely. */
    enum zcl_command_authority authority_ceiling;
    /* Transport that carried this dispatch, stamped into each command-ledger
     * record (util/command_ledger). Default 0 == ZCL_CMD_TRANSPORT_NATIVE: a
     * zero-initialized context (local operator / plain CLI / in-process) reads
     * as native. RPC/REST/MCP adapters set it where they construct the context.
     */
    enum zcl_cmd_transport transport;
    bool dev_build;
};

struct zcl_command_request {
    const struct zcl_command_spec *spec;
    const struct zcl_command_context *context;
    const struct json_value *input;
    const char *view;         /* "summary" | "normal" | "full" (default) */
    size_t budget_bytes;      /* 0 = contract default (never raises the cap) */
    size_t max_items;         /* 0 = unbounded; bounds a --view=full page */
    const char *cursor;       /* opaque page cursor for --view=full, or NULL */
    bool invoked_by_alias;
    const char *invoked_name;
};

struct zcl_command_next {
    char command[ZCL_COMMAND_MAX_PATH];
    char input_json[512];
    char reason[160];
};

struct zcl_command_error {
    char code[64];
    char message[192];
    char phase[64];
    char evidence[256];
    char failure_id[96];
    bool retryable;
    bool mutated;
};

struct zcl_command_reply {
    enum zcl_command_status status;
    enum zcl_command_exit exit_code;
    const char *data_schema;
    struct json_value data;
    struct zcl_command_error error;
    struct zcl_command_next next[ZCL_COMMAND_MAX_NEXT];
    size_t next_count;
};

typedef void (*zcl_command_handler_fn)(const struct zcl_command_request *request,
                                       struct zcl_command_reply *reply);

struct zcl_command_spec {
    const char *path;
    const char *parent;
    const char *aliases;
    const char *summary;
    /* One-line OUTPUT-interpretation contract: the source, freshness, units,
     * and completeness of `data` — not a restatement of `summary`. Required
     * and distinct from summary on every READY leaf; empty on branches. */
    const char *semantics;
    const char *tags;
    const char *input_schema;
    const char *output_schema;
    const char *input_keys;
    const char *positional_keys;
    const char *example;
    const char *availability_reason;
    const char *compat_target;
    /* Per-leaf response byte budget: 0 selects the kind default (RESULT/ERROR),
     * else clamps the success envelope to this cap. Validated to 0 or
     * [256, 65536]. Set only where the default is obviously wrong (list-shaped
     * leaves that legitimately need the larger LIST budget). */
    int budget_bytes;
    enum zcl_command_layer layer;
    enum zcl_command_effect effect;
    enum zcl_command_risk risk;
    enum zcl_command_scope scope;
    enum zcl_command_authority authority;
    enum zcl_command_availability availability;
    enum zcl_command_mode mode;
    enum zcl_command_latency latency;
    enum zcl_command_cost cost;
    enum zcl_command_confirmation confirmation;
    uint32_t allowed_lanes;
    uint64_t required_capabilities;
    uint32_t traits;
    uint32_t transports;
    zcl_command_handler_fn handler;
};

struct zcl_command_registry {
    const struct zcl_command_spec *commands;
    size_t count;
};

void zcl_command_reply_init(struct zcl_command_reply *reply,
                            const char *data_schema);
void zcl_command_reply_free(struct zcl_command_reply *reply);
void zcl_command_reply_fail(struct zcl_command_reply *reply,
                            enum zcl_command_status status,
                            enum zcl_command_exit exit_code,
                            const char *code, const char *phase,
                            bool retryable, bool mutated,
                            const char *message, const char *evidence);
bool zcl_command_reply_add_next(struct zcl_command_reply *reply,
                                const char *command, const char *input_json,
                                const char *reason);

bool zcl_command_registry_validate(const struct zcl_command_registry *registry,
                                   char *why, size_t why_size);
const struct zcl_command_spec *zcl_command_registry_find(
    const struct zcl_command_registry *registry, const char *path_or_alias,
    bool *was_alias);
const struct zcl_command_spec *zcl_command_registry_resolve_words(
    const struct zcl_command_registry *registry,
    const char *const *words, size_t word_count, size_t *consumed,
    bool *was_alias, char *invoked, size_t invoked_size);
bool zcl_command_registry_input_validate(const struct zcl_command_spec *spec,
                                         const struct json_value *input,
                                         char *why, size_t why_size);
void zcl_command_registry_digest(const struct zcl_command_registry *registry,
                                 char out[72]);

size_t zcl_command_registry_menu_json(const struct zcl_command_registry *registry,
                                      const char *path, char *out,
                                      size_t out_size);
size_t zcl_command_registry_describe_json(
    const struct zcl_command_registry *registry, const char *path,
    char *out, size_t out_size);
size_t zcl_command_registry_search_json(
    const struct zcl_command_registry *registry, const char *query,
    char *out, size_t out_size);
size_t zcl_command_registry_execute_json(
    const struct zcl_command_registry *registry,
    const struct zcl_command_spec *spec,
    const struct zcl_command_context *context,
    const struct json_value *input,
    bool invoked_by_alias, const char *invoked_name,
    const char *view, size_t budget_bytes,
    size_t max_items, const char *cursor,
    char *out, size_t out_size, enum zcl_command_exit *exit_code);

/* ── Hot-swap leaf-handler override layer ─────────────────────────────
 *
 * A lock-free, all-or-nothing snapshot layer that re-points a bounded set of
 * READY read-only leaf handlers at runtime — the native analogue of the MCP
 * router's mcp_router_replace_batch (tools/mcp/router.c). Dispatch
 * (zcl_command_registry_execute_json) consults the active override snapshot
 * for the resolved leaf path before falling back to the immutable catalog
 * handler column; with no snapshot published the cost is a single atomic load
 * + NULL check. Published snapshots are NEVER freed — an in-flight dispatch
 * that acquired an older snapshot must be allowed to finish without a UAF
 * race (same discipline as router.c).
 */
#define ZCL_COMMAND_HANDLER_OVERRIDE_MAX 64U

struct zcl_command_handler_override {
    const char *path;                 /* canonical READY read-only leaf path */
    zcl_command_handler_fn handler;   /* replacement handler (must be non-NULL) */
};

/* Bind the canonical registry used to validate override paths. Idempotent;
 * pass NULL to unbind. Must be set before zcl_command_registry_replace_batch
 * can succeed. */
void zcl_command_registry_set_active(const struct zcl_command_registry *registry);

/* Atomically replace a batch of leaf handlers. All-or-nothing: every override
 * path must resolve to an existing READY, read-only leaf in the bound registry
 * (destructive/mutating leaves and aliases are rejected) BEFORE anything is
 * cloned or published. On any failure the active snapshot is untouched and
 * `why` (when non-NULL, size why_sz) carries a one-line reason. `generation`
 * must be strictly greater than the active generation, or 0 to auto-increment.
 * In-flight readers observe the entire old or entire new override set, never a
 * torn one. Returns true on publish. */
bool zcl_command_registry_replace_batch(
    uint32_t generation,
    const struct zcl_command_handler_override *overrides,
    size_t count, char *why, size_t why_sz);

/* Active override-snapshot generation (0 = none published). */
uint32_t zcl_command_registry_active_generation(void);

/* True iff every RETIRED override snapshot (published but no longer active) has
 * drained to a zero in-flight dispatch refcount — no dispatch can still be
 * executing a superseded handler. A hot-swap loader polls this before it
 * dlcloses a superseded module .so (epoch/refcount quiesce; see
 * hotswap_activate). Lock-free; the publish list is append-only, never freed. */
bool zcl_command_registry_all_retired_quiesced(void);

/* Effective handler for `spec`: the active override for spec->path when one is
 * published, else spec->handler. NULL when neither exists. */
zcl_command_handler_fn zcl_command_registry_effective_handler(
    const struct zcl_command_spec *spec);

/* Drop all overrides (revert to the immutable catalog). Reset hook; publishes
 * an empty (NULL) snapshot. The previous snapshot is retired per the
 * never-free discipline. */
void zcl_command_registry_reset_overrides(void);

const char *zcl_command_layer_name(enum zcl_command_layer value);
const char *zcl_command_effect_name(enum zcl_command_effect value);
const char *zcl_command_risk_name(enum zcl_command_risk value);
const char *zcl_command_scope_name(enum zcl_command_scope value);
const char *zcl_command_authority_name(enum zcl_command_authority value);
const char *zcl_command_availability_name(enum zcl_command_availability value);
const char *zcl_command_mode_name(enum zcl_command_mode value);
const char *zcl_command_latency_name(enum zcl_command_latency value);
const char *zcl_command_cost_name(enum zcl_command_cost value);
const char *zcl_command_confirmation_name(enum zcl_command_confirmation value);
const char *zcl_command_status_name(enum zcl_command_status value);

/* ── Command interaction ledger seam (Phase D — agent flight recorder) ──────
 *
 * The kernel stays dependency-pure: it does not know how the ledger is stored.
 * A util-layer sink (util/command_ledger) registers itself, and every dispatch
 * through zcl_command_registry_execute_json hands it ONE content-free record
 * describing that call. No sink registered == a single atomic load + NULL check
 * (zero overhead). Same registered-function-pointer idiom as the hot-swap
 * handler snapshot above.
 *
 * The record is a POD mirror of the durable zcl.cmd_ledger.v1 schema. It is
 * CONTENT-FREE by construction: it carries only byte COUNTS and typed metadata,
 * never the input or output bytes themselves. `leaf` and `request_id`/`code`
 * are the leaf's own static path and the dispatch's error code — never caller
 * data. The sink is called synchronously from the dispatching thread and must
 * copy anything it needs before returning (`leaf` points at the immutable
 * catalog string, valid for the process lifetime; the char arrays are inline). */
struct zcl_command_ledger_record {
    int64_t ts_unix_ms;      /* wall-clock append time (platform clock) */
    uint64_t seq;            /* process-local dispatch sequence */
    const char *leaf;        /* canonical leaf path (static catalog string) */
    enum zcl_cmd_transport transport;
    int64_t input_bytes;     /* serialized request size (count only) */
    int64_t output_bytes;    /* serialized reply size (count only) */
    int64_t budget_bytes;    /* effective response byte budget */
    bool budget_exceeded;    /* latency budget overrun (elapsed_us > budget) */
    int64_t elapsed_us;      /* dispatch duration */
    int64_t budget_ms;       /* declared latency-bucket budget */
    enum zcl_command_latency latency_class;
    bool ok;                 /* passed/accepted vs blocked/failed */
    char code[64];           /* error code, empty on success */
    char request_id[48];     /* "local-<seq>" envelope id */
};

typedef void (*zcl_command_ledger_sink_fn)(
    const struct zcl_command_ledger_record *record);

/* Register (or, with NULL, unregister) the durable ledger sink. Idempotent.
 * Once set, every execute_json dispatch hands the sink one record. */
void zcl_command_registry_set_ledger_sink(zcl_command_ledger_sink_fn fn);

/* A durable per-leaf p99 source. Returns true and fills the p99_us + samples
 * out-params for `leaf` over the trailing `window_s` seconds (<=0 == all
 * retained), false with samples==0 when it has no data. Same signature as
 * command_ledger_p99. When registered, discover.describe prefers it over the
 * in-process latency ring. */
typedef bool (*zcl_command_latency_source_fn)(
    const char *leaf, int64_t window_s, int64_t *p99_us, uint32_t *samples);

/* Register (or, with NULL, unregister) the durable latency source. Idempotent. */
void zcl_command_registry_set_latency_source(zcl_command_latency_source_fn fn);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_KERNEL_COMMAND_REGISTRY_H */
