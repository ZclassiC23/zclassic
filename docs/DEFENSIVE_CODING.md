# Defensive Coding Standards — The Rails Way in C23

**Rule: if the compiler can't enforce it, it will be violated.**

This document defines architectural enforcement patterns that make it
impossible for any contributor (human or AI agent) to accidentally skip
validation, swallow errors, or leak memory. Read this before writing
any new code.

Modules prefixed `legacy_` are a compatibility layer with an external
`zclassicd`. See [`LEGACY_LIFECYCLE.md`](./LEGACY_LIFECYCLE.md) for
which paths are still load-bearing.

---

## 1. Every write goes through the AR lifecycle — no exceptions

**Problem:** `coins_view_sqlite.c` and `wallet_sqlite.c` historically
called `sqlite3_step()` directly with no validation. This is how we lost
1.3M UTXOs on 2026-04-10.

**Enforcement:** Compile-time ban on raw `sqlite3_step` in application
code, plus a CI lint that re-checks the same surface.

```c
/* In activerecord.h — poison raw sqlite3_step in app code */
#ifdef ZCL_AR_ENFORCE
  /* Any file that includes activerecord.h cannot call sqlite3_step directly.
   * Use the AR lifecycle macros instead. To opt out (e.g. in
   * lib/storage internals), #define ZCL_AR_RAW_SQL before include. */
  #ifndef ZCL_AR_RAW_SQL
    #define sqlite3_step(x) \
      _Pragma("GCC error \"Use the AR lifecycle macros, not raw sqlite3_step\"")
  #endif
#endif
```

**The three lifecycle entry points (all defined in `activerecord.h`):**

| Macro | When to use | What it does |
|-------|-------------|--------------|
| `AR_BEGIN_SAVE(cbs, name, rec, validate_fn)` + `AR_FINISH_SAVE(cbs, rec, ok)` | You build the statement yourself between the two macros (e.g. multi-statement transactions, conditional binds) | `AR_VALIDATE_RECORD` → `before_save` hook → your code → `after_save` hook → `return ok` |
| `AR_ADHOC_SAVE(ndb, stmt, sql, cbs, name, rec, validate_fn, bind_code)` | Single locally-prepared INSERT/UPDATE statement (the common case) | Wraps `AR_BEGIN_SAVE` + `AR_PREPARE_BOOL` + your bind block + `AR_FINALIZE_STEP_DONE` + `AR_FINISH_SAVE` |
| `AR_CACHED_SAVE(stmt, cbs, name, rec, validate_fn, bind_code)` | Hot path with a cached prepared statement already owned by `node_db` | Same lifecycle, skips the prepare — call `AR_RESET(stmt)` and bind |

All three invoke the same `validate_*` + `before_save` + `after_save`
chain, so model hooks fire identically regardless of which one the
caller picked. Pick the one that fits the call site.

**Minimal call-site (the common case — `AR_ADHOC_SAVE`):**

```c
bool db_wallet_key_save(struct node_db *ndb, const struct db_wallet_key *k) {
    if (!ndb->open) return false;
    wallet_key_init_hooks();
    struct ar_callbacks *cbs = db_wallet_key_callbacks();
    sqlite3_stmt *s = NULL;
    AR_ADHOC_SAVE(ndb, s,
        "INSERT OR REPLACE INTO wallet_keys(pubkey_hash,pubkey,privkey,"
        "compressed,created_at) VALUES(?,?,?,?,?)",
        cbs, "wallet_key", k, db_wallet_key_validate,
        AR_BIND_BLOB(s, 1, k->pubkey_hash, 20);
        AR_BIND_BLOB(s, 2, k->pubkey, (int)k->pubkey_len);
        AR_BIND_BLOB(s, 3, k->privkey, 32);
        AR_BIND_INT(s, 4, k->compressed ? 1 : 0);
        AR_BIND_INT(s, 5, k->created_at));
}
```

Files that legitimately need raw SQL (storage primitives in
`lib/storage/src/coins_view_sqlite.c`) opt out with `#define
ZCL_AR_RAW_SQL`. The Makefile adds `-DZCL_AR_ENFORCE` globally. Raw SQL
becomes a conscious, visible decision.

**Status: shipped.** The ratchet allowlist at
`tools/scripts/raw_sqlite_allowlist.txt` is empty. All production
writes across `app/models/src/`, `app/controllers/src/`,
`app/services/src/`, and `lib/wallet/src/wallet_sqlite.c` route through
the AR lifecycle (one of the three macros above). `make lint` runs
`check_raw_sqlite.sh` (one of the 34 lint gates defined in the Makefile).

### The one principled exception: the `progress.kv` kernel store

The AR lifecycle is the law for **`node.db` domain models** — blocks,
UTXOs, wallet keys, peers, mempool entries. Those rows have an identity,
a `validate_*` function, and before/after-save hooks; AR exists to make
every such write run that chain.

The reducer pipeline does **not** write its stage state to `node.db`. It
writes to `progress.kv` — a *separate, singleton, WAL kernel store*
(`lib/storage/src/progress_store.c`, opened once at boot) that holds the
F-2 `stage` primitive's `stage_cursor` table plus the per-stage `*_log`
tables (`header_admit_log`, `body_fetch_log`, `validate_headers_log`,
`utxo_apply_log`, `utxo_apply_delta`, `created_outputs`,
`tip_finalize_log`, …). `progress.kv` sits **below** the AR/domain-model
layer by design (see `storage/progress_store.h` and `docs/FRAMEWORK.md`):
a `stage_cursor` row is not a model, it has no domain identity and no
save hooks. Cursor commits are tiny, on the hot path, and want their own
WAL out of the way of the much larger `node.db` transactions; the saga
atomicity contract (a stage advance and its log row commit together)
lives in `progress_store_tx_lock()` + `BEGIN IMMEDIATE`, not in AR.

Routing these writes through AR would be a **category error** — there is
no model to validate and no hook to fire. The correct discipline is:

- A raw `sqlite3_step` on the `progress.kv` handle (always sourced from
  `progress_store_db()`, never a `node_db`/`ndb` handle) is
  **correct-by-design**, not migration debt.
- Every such site MUST carry the single canonical marker
  **`// raw-sql-ok:progress-kv-kernel-store`** (one no-space token after
  the colon). The marker is self-documenting: it asserts "this is the
  kernel cursor store, below AR." The home module
  `progress_store.c` itself uses the equivalent `kernel-primitive` tag
  for the same reason.
- This is a **bounded, stable exception**, not a deferred migration. The
  count is documented and changes only when the reducer gains or drops a
  stage table — it does not ratchet toward zero the way the `node.db`
  allowlist did. `check_raw_sqlite.sh` treats the
  `progress-kv-kernel-store` tag as the principled kernel-store hatch.

If a reducer Job ever needs to write a `node.db` **model** (not a
progress.kv `*_log`/cursor row), that write does NOT get the kernel-store
marker — it goes through the AR lifecycle like any other model write.

---

## 2. Every function that can fail returns a result type — not bare bool

**Problem:** `return false` with no context. Caller has no idea why.

**Enforcement:** Standard result type for all service/model functions.

```c
/* lib/util/include/util/result.h */

struct zcl_result {
    bool        ok;
    int         code;          /* 0 = success, negative = error category */
    char        message[256];  /* human-readable, always populated on failure */
    const char *source_file;   /* __FILE__ */
    int         source_line;   /* __LINE__ */
};

#define ZCL_OK ((struct zcl_result){.ok = true, .code = 0})

#define ZCL_ERR(err_code, fmt, ...) ((struct zcl_result){ \
    .ok = false, \
    .code = (err_code), \
    .source_file = __FILE__, \
    .source_line = __LINE__, \
    .message = "" \
})
/* message populated via snprintf in the macro expansion */

#define ZCL_CHECK(result) do { \
    struct zcl_result _r = (result); \
    if (!_r.ok) { \
        log_json("error", "zcl_check_failed", \
                 "code", _r.code, \
                 "message", _r.message, \
                 "file", _r.source_file, \
                 "line", _r.source_line); \
        return _r; \
    } \
} while (0)
```

**Rule:** New service functions MUST return `struct zcl_result` instead
of `bool`. Existing code migrates incrementally.

**Why this works for agents:** An agent that writes `return false;` in a
function declared as returning `struct zcl_result` gets a compiler error.
They MUST write `return ZCL_ERR(-1, "reason: %s", detail);` which forces
them to explain the failure.

---

## 3. Every malloc is checked — use zcl_malloc or die

**Problem:** 15+ unchecked malloc/calloc calls in sync services.
Silent NULL dereference.

**Enforcement:**

```c
/* lib/util/include/util/safe_alloc.h */

/* Checked malloc — logs and returns NULL (caller must handle).
 * Use when graceful degradation is possible. */
static inline void *zcl_malloc(size_t size, const char *label)
{
    void *p = malloc(size);
    if (!p && size > 0) {
        log_json("error", "malloc_failed",
                 "size", (int64_t)size,
                 "label", label);
        event_emitf(EV_OOM, 0, "label=%s size=%zu", label, size);
    }
    return p;
}

/* Checked malloc — aborts on failure.
 * Use when there's no reasonable fallback. */
[[noreturn]] static inline void zcl_oom_abort(size_t size, const char *label);

static inline void *zcl_malloc_or_die(size_t size, const char *label)
{
    void *p = malloc(size);
    if (!p && size > 0) zcl_oom_abort(size, label);
    return p;
}

/* Checked realloc — never leaks the original pointer. */
static inline void *zcl_realloc(void *ptr, size_t size, const char *label)
{
    void *p = realloc(ptr, size);
    if (!p && size > 0) {
        log_json("error", "realloc_failed",
                 "size", (int64_t)size,
                 "label", label);
        /* Original ptr is NOT freed — caller decides. */
    }
    return p;
}
```

**Makefile enforcement:**

```makefile
# In CI / make lint:
check-malloc:
	@echo "Checking for raw malloc/calloc/realloc..."
	@grep -rn '\bmalloc\b\|bcalloc\b\|\brealloc\b' \
	    app/ lib/ config/ tools/ \
	    --include='*.c' \
	    | grep -v 'zcl_malloc\|zcl_calloc\|zcl_realloc' \
	    | grep -v 'safe_alloc.h' \
	    | grep -v 'vendor/' \
	    | grep -v '// raw-alloc-ok' \
	    && echo "FAIL: use zcl_malloc/zcl_calloc/zcl_realloc" && exit 1 \
	    || echo "OK: no raw allocations"
```

Files that need raw malloc (e.g. vendor code, allocator internals) add
`// raw-alloc-ok` comment on the line.

---

## 4. Every error path logs with context — use LOG_ERR macro

**Problem:** 100+ `return -1;` or `return false;` with no logging.

**Enforcement:**

```c
/* lib/util/include/util/log_macros.h */

/* Log + return false */
#define LOG_FAIL(domain, fmt, ...) do { \
    log_json("error", domain, \
             "file", __FILE__, \
             "line", __LINE__, \
             "func", __func__, \
             ##__VA_ARGS__); \
    return false; \
} while (0)

/* Log + return -1 (for MCP handlers) */
#define LOG_ERR(domain, fmt, ...) do { \
    log_json("error", domain, \
             "file", __FILE__, \
             "line", __LINE__, \
             "func", __func__, \
             ##__VA_ARGS__); \
    return -1; \
} while (0)

/* Log + return custom value */
#define LOG_RETURN(val, domain, fmt, ...) do { \
    log_json("error", domain, \
             "file", __FILE__, \
             "line", __LINE__, \
             "func", __func__, \
             ##__VA_ARGS__); \
    return (val); \
} while (0)
```

**CI lint rule:**

```makefile
check-silent-errors:
	@echo "Checking for silent error returns..."
	@grep -rn 'return -1;' app/ tools/mcp/ --include='*.c' \
	    | grep -v 'LOG_ERR\|log_json\|fprintf' \
	    && echo "FAIL: silent error returns found" && exit 1 \
	    || echo "OK: all error returns logged"
```

---

## 5. MCP handlers must log on every error path

**Problem:** silent `return -1;` in MCP handlers leaves the caller with
no diagnostic info.

**Status: enforced by lint.** `make lint` runs five shape-tier gates:

- `check-silent-errors` — every bare `return -1;` in
  `tools/mcp/controllers/*.c` must either be preceded by a logging
  call (`LOG_ERR`, `log_json`, `fprintf`) or carry an explicit
  `// raw-return-ok:<reason>` marker (no space after the colon).
- `check-silent-errors-services` — same rule for `app/services/src/`.
- `check-silent-errors-controllers` — same rule for
  `app/controllers/src/`.
- `check-silent-errors-jobs` — same rule for `app/jobs/src/`.
- `check-silent-errors-conditions` — same rule for
  `app/conditions/src/`.

The service/controller/job/condition gates accept only an *error-level*
preceding log (`LOG_ERR`, `LOG_FAIL`, `LOG_RETURN`, or `log_json` with an
error level) — a bare `printf`/`LOG_WARN` no longer satisfies the
pairing, so a silent failure can never masquerade as handled by a
warn-level breadcrumb.

A future `mcp_fail(res, code, fmt, ...)` helper could enforce that
`res->body` is populated on every error path by return-type discipline,
but that's optional polish — the lint already prevents the silent-fail
class entirely.

---

## 6. Before/after save hooks — wired

**Status: shipped.** Every critical model wires `ar_register_before_save`
and `ar_register_after_save`. The `check-before-save-hooks` lint
enforces that `utxo`, `block`, `wallet_key`, and `wallet_tx` keep these
hooks — drop one and `make lint` fails.

| Model | before_save | after_save |
|-------|-------------|------------|
| wallet_key | Log if `ZCL_WALLET_PASSPHRASE` set (keystore owns at-rest wrap) | Emit `EV_WALLET_KEY_SAVED` (`EV_SAPLING_KEY_SAVED` for sapling rows) |
| utxo | Validate money range + script coherence | Update UTXO commitment cache |
| block | Validate hash matches header | Emit `EV_BLOCK_SAVED` |
| wallet_tx | Validate txid format | Emit `EV_WALLET_TX_SAVED` |
| mempool_entry | Validate fee + size envelope | (no after_save) |
| tx_index | Validate txid + block height | (no after_save) |

---

## 7. CI gates — the final enforcer

Add to `Makefile`:

```makefile
lint: check-malloc check-silent-errors check-raw-sqlite \
      check-raw-malloc check-coins-lookup-nullcheck \
      check-observability-pairing check-silent-errors-services \
      check-silent-errors-controllers check-silent-errors-jobs \
      check-silent-errors-conditions check-before-save-hooks \
      check-pthread-create check-model-validation \
      check-long-functions check-rpc-registrar \
      check-lag-slo-observable check-lib-layering \
      check-supervisor-registration check-typed-blocker \
      check-framework-shape check-no-raw-clock-outside-platform \
      check-no-raw-sqlite-in-controllers check-supervisor-domain \
      check-file-size-ceiling check-operator-needed-sink \
      check-framework-filename-suffix \
      check-doc-accuracy
	@echo "All lint checks passed"

ci: lint test fuzz-ci coverage
```

**Status: 34 gates active.** Gates #18 and #20 graduated from WARN to
RATCHET (E10, 2026-05-26) — they fail `make ci` on any *new* off-shape
file / raw-sqlite controller while tolerating the recorded baseline.
Gate #19 and #21 are FAIL. The ten E-series gates (E1–E9 and E11:
`check-file-size-ceiling`, `check-one-result-type`,
`check-shape-includes-header`, `check-projections-pure`,
`check-stage-advances-or-blocks`, `check-one-write-path`,
`check-no-authoritative-ram-state`, `check-no-silent-ready`,
`check-operator-needed-sink`, `check-doc-accuracy`) are detailed
under "Build-checklist gates" below. An agent that pushes code with raw malloc, silent errors,
bypassed AR validation, unpaired stderr diagnostics, a critical model
missing its before_save hook, a model file with no `validates_*` call
and no `ar-validate-skip:<tag>` marker, or a controller/service
function over 500 lines without a `long-function-ok:<tag>` override,
gets a red build before any human sees it.

Detailed sections below cover gates #11 (`check-model-validation`),
#12 (`check-long-functions`), #15 (`check-lib-layering`, was numbered
#14 in earlier drafts of this doc) and #16
(`check-supervisor-registration`, was numbered #15). Three more gates
are live in `make lint` but not yet detailed in this doc — they
remain on the doc-accuracy backlog:

- **Gate #13: `check-rpc-registrar`** — every RPC handler declared in
  `lib/rpc/src/` must appear in the registrar table at the bottom of
  the same file; "method not found" failures are caught at build time
  rather than at runtime.
- **Gate #14: `check-lag-slo-observable`** — any code path that can
  produce SLO-relevant lag (block lag, peer floor, watchdog miss)
  must pair with a structured event emit and a Prometheus gauge.
- **Gate #17: `check-typed-blocker`** — Round 6 (2026-05-21) introduced
  `lib/util/src/blocker.c` (typed blocker primitive). Any code raising
  a `block_id` must use a typed kind from `enum blocker_kind`; raw
  string blockers fall in a baseline (see Layer baselines below).

### Framework refactor gates (#18-#20, ratcheting)

These three gates shipped with the Phase 0 framework refactor
scaffold. Gates #18 and #20 still default to WARN mode through
`ZCL_LINT_MODE=WARN` in `make lint` so the current tree gets measured
before the refactor starts deleting debt. Gate #19 ratcheted to FAIL in
Phase 1 after the platform clock/RNG migration landed.

- **Gate #18: `framework_shape_check`**
  - Path: `tools/lint/framework_shape_check.sh`
  - Checks: every `.c` file under `app/` must live under one framework
    shape source folder: `controllers`, `services`, `models`, `jobs`,
    `supervisors`, `conditions`, `events`, or `views`.
  - Current mode: WARN.
  - Ratchets to FAIL: Phase 2, after the Wave S job cutover has removed
    any remaining off-shape app code.
  - Fix: move the file into the correct shape folder or split mixed
    responsibilities into separate shape files.
  - Allowlist: add one relative path per line to
    `tools/lint/framework_shape_allowlist.txt` for grandfathered
    violations only. The file is a ratchet and should shrink to empty.

- **Gate #19: `check_no_raw_clock_outside_platform`**
  - Path: `tools/lint/check_no_raw_clock_outside_platform.sh`
  - Checks: direct `clock_gettime(`, `time(NULL)`, and `getrandom(`
    calls outside `lib/platform/`.
  - Current mode: FAIL — added Phase 1 (2026-05-23).
  - Fix: route wall-clock and monotonic reads through the platform clock
    abstraction, and route entropy reads through platform RNG. For
    deliberate one-off exceptions, add
    `// platform-ok` on the line with a nearby explanation.

- **Gate #20: `check_no_raw_sqlite_in_controllers`**
  - Path: `tools/lint/check_no_raw_sqlite_in_controllers.sh`
  - Checks: direct `sqlite3_prepare_v2(` or `sqlite3_exec(` use in
    `app/controllers/` and `tools/mcp/controllers/`.
  - Current mode: WARN.
  - Ratchets to FAIL: Phase 1, after read paths move behind
    projections or models.
  - Fix: move reads into projection helpers or model queries; move
    writes through models and the ActiveRecord lifecycle. For a
    temporary documented exception, add `// raw-controller-sql-ok` on
    the line.

- **Gate #21: `check_supervisor_domain`**
  - Path: `tools/lint/check_supervisor_domain.sh`
  - Checks: production `supervisor_register(` calls under `app/`,
    `config/`, and `lib/` must use `supervisor_register_in_domain(...)`.
  - Current mode: FAIL — added with the supervisor tree split.
  - Fix: classify the child into one of the boot-created domains
    (`chain`, `net`, `mempool`, `wallet`, `feature`, `onion`, `op`) and
    register through that domain. For a deliberate root child, add
    `// supervisor-root-ok:<tag>` on the registration line.

- **Gate #22: `check_framework_filename_suffix`**
  - Path: `tools/lint/check_framework_filename_suffix.sh`
  - Checks: no `.c` under a shape folder may end in a DIFFERENT shape's
    suffix — the eight suffixes are `controller`, `service`, `model`,
    `view`, `job`, `supervisor`, `condition`, `event`. A file may keep
    its own folder's suffix (`services/foo_service.c`) or a bare entity
    name (`models/block.c`, `jobs/validate_headers_stage.c`); only a
    foreign suffix (e.g. `*_controller.c` in `services/`) is rejected.
    This is the recurrence guard for the S1 service renames. `_store` /
    `_repository` name no shape and are not flagged.
  - Current mode: FAIL (HARD) — added with the S1 conformance close-out.
  - Fix: rename to the folder's own suffix or a bare entity name, or move
    the file to the folder its suffix names. If the entity name
    legitimately ends in a shape word (e.g. `models/file_service.c`, a
    "file service" Model row), add a top-of-file `// suffix-ok:<tag>`.

### Gate #11: every model is either validated or explicitly skipped

`check-model-validation` walks every `app/models/src/*.c` and
requires one of:

1. At least one `validates_*` call (the macros from
   `app/models/include/models/activerecord.h` — `validates_presence_of`,
   `validates_range`, `validates_zcl_address`, etc.).
2. A top-of-file marker `ar-validate-skip:<tag>` (no space after the
   colon, non-empty tag) explaining why the AR validation lifecycle
   does not apply — e.g. `connection-handle-not-a-row`,
   `registry-module-not-a-row`, `shared-helpers-not-a-row`.

This pins the wave-6 result: validations are required for every row
model, and infrastructure / registry / helper files declare their
exemption in code rather than by silent omission. Implementation:
`tools/scripts/check_model_validation.sh`.

### Gate #12: controller / service functions stay under 500 lines

`check-long-functions` walks every `app/controllers/src/*.c` and
`app/services/src/*.c` and flags any top-level function whose body
spans more than 500 lines from signature to closing `}` on column 0.

Long functions are hard to test in isolation, hard to read in one
sitting, and almost always conceal two or more concerns waiting to
be split.  The two report builders that broke this cap before wave
7d — `explorer_factoids_build` (1389L, 17 archaeology sections) and
`explorer_stats_build` (1011L, 10 statistics sections) — have been
refactored into per-section emit helpers, each under ~120 lines.

**Override marker.** A single state machine that genuinely belongs
as one function may carry `// long-function-ok:<tag>` on its
signature line.  The tag must be a non-empty single token matching
`[A-Za-z][A-Za-z0-9_-]+` (same syntax as the other lint overrides)
and describe WHY the rule does not apply.

Implementation: `tools/scripts/check_long_functions.sh`.

### Gate #14: lib/ layer purity (regression ratchet)

`check-lib-layering` walks every `lib/**/*.c` and `lib/**/*.h` outside
`lib/test/` and flags any `#include "controllers/..."`, `"models/..."`,
`"services/..."`, or `"views/..."` directive. lib/ is the foundation —
controllers/models/services/views are upstream consumers, and a
backward include typically means the lib/ file is doing something that
belongs in app/ or relying on a symbol that should live in lib/.

The gate ships with a baseline file at
`tools/scripts/lib_layering_baseline.txt`, now empty — the 98 violations
that pre-existed when the gate was introduced (round 4) have all been
remediated. Each
entry is `<file>:<exact #include directive>`. Any *new* violation not
in the baseline fails CI. The list is a ratchet: shrinking it is
permanent progress; **growing it requires an ADR justifying the
regression.**

To pay down debt: pick a baseline entry, replace the include with a
forward declaration (or move the symbol into lib/, or delete it if
unused), delete the matching baseline line, and re-run `make lint`.

**Override marker.** `// lib-layer-ok:<tag>` on the include line keeps
the include while documenting that the deviation is intentional
(e.g. `file_manifest-struct-defn` where lib/net needs the full struct
definition that happens to live in a controller header). Use sparingly.

Implementation: `tools/scripts/check_lib_layering.sh`.

### Gate #15: supervisor registration for long-running services (ratchet)

`check-supervisor-registration` walks every
`app/services/src/*_service.c` and flags any file that contains
`pthread_create(`, `thread_registry_spawn`, or
`health_register_periodic(` but does NOT contain `supervisor_register(`,
is not in `tools/scripts/supervisor_baseline.txt`, and does not carry
a `// supervisor-ok:<tag>` override.

Why: Round 5 (2026-05-21) introduced a dedicated time-driven
supervisor in `lib/util/supervisor.{c,h}` after the lib/health sweeper
wedged for 8.6 h, leaving every periodic check silently dead. Services
that register a `struct liveness_contract` are protected by an
independent driver: even if their normal scheduler stalls, the
supervisor still fires `on_tick` and edge-triggers `on_stall` on
deadline or progress-frozen.

Three children were registered in Round 5:
* `sync.watchdog` — tick rescue for `sync_watchdog_periodic_tick`
  (R5 C2, sync_watchdog_service.c).
* `net.outbound_floor` — emits `EV_PEER_FLOOR_BREACH` + calls
  `connman_kick_seed_discovery` when outbound peers stay below 2 for
  60 s (R5 C3, boot_services.c).
* `chain.coord_escalation` — calls
  `chain_advance_coordinator_force_mirror_promotion` (300 s bounded
  window, bypasses `mir->blocked` short-circuit) when mirror lag is
  "fatal" and local height is frozen for 900 s (R5 C4,
  boot_services.c).

The gate ships with a 9-entry baseline of services that need
contracts but weren't migrated in Round 5. Track C-3 of the master
plan drains these. The highest blast-radius targets are
`block_sync_service`, `header_sync_service`, and
`legacy_mirror_sync_service` (see
`tools/scripts/supervisor_baseline.txt`).

**Override marker.** `// supervisor-ok:<tag>` on any line in the file
exempts it (use when the service intentionally manages its own
lifecycle — e.g. main-thread workers, signal-driven helpers).

Implementation: `tools/scripts/check_supervisor_registration.sh`.

### Build-checklist gates (E-series, 2026-05-26)

Four gates from the "beauty by the build" checklist. Tooling-only:
each turns the build red on a *regression* without breaking the
current green tree (RATCHET-with-baseline or a pairing that the tree
already satisfies). Tested in `lib/test/src/test_make_lint_gates.c`
(plant fixture → assert trip → remove → assert green).

- **Gate E1: `check-file-size-ceiling`** (RATCHET)
  - Path: `tools/scripts/check_file_size_ceiling.sh`
  - Checks: no `app/**/*.c` file exceeds **800 lines**. Mega-modules
    can otherwise hide behind a wall of <500-LOC functions that each
    pass `check-long-functions`; this caps the whole file.
  - Baseline: `tools/scripts/file_size_ceiling_baseline.txt` records
    each pre-existing oversized file as `<path> <max-loc>`. A NEW file
    over the ceiling fails; a baselined file that grows ABOVE its
    recorded LOC fails. The baseline may only shrink.
  - Override: none inline — the baseline line IS the visible,
    reviewable escape hatch.

- **Gate E9: `check-operator-needed-sink`** (HARD)
  - Path: `tools/scripts/check_operator_needed_sink.sh`
  - Checks: `EV_OPERATOR_NEEDED` (the "auto-healing gave up, page a
    human" signal) is emitted in production code AND has a registered
    subscriber in `lib/util/src/alerts.c` (an alert rule with
    `.trigger = EV_OPERATOR_NEEDED` wired via `event_observe(`).
    Models the pairing style of `check-lag-slo-observable`. Prevents
    the silent-halt class where the loud signal reaches no sink.
  - Override: none — a missing sink is always a bug to fix.

- **Gate E11: `check-doc-accuracy`** (HARD)
  - Path: `tools/scripts/check_doc_accuracy.sh`
  - Checks: the canonical gate list in this file (the
    `<!-- LINT-GATES-BEGIN/END -->` block below) matches the `check-*`
    prerequisites of the `lint:` target in the Makefile, by both
    count and name set. Catches doc rot. On mismatch, fix the doc
    block — the Makefile is authoritative for what runs.
  - Override: none.

- **Gate E2: `check-one-result-type`** (RATCHET)
  - Path: `tools/scripts/check_one_result_type.sh`
  - Checks: new `app/services/src/*.c` files return `struct zcl_result`
    (§2, `util/result.h`) instead of bare `bool`/`int`, so a failure
    reason always travels with the failure. The tree is ~98% bare-bool
    today, so the gate ratchets at FILE granularity: a service file is
    "result-clean" if it references `struct zcl_result` anywhere; every
    other service file is grandfathered.
  - Baseline: `tools/scripts/one_result_type_baseline.txt` is now empty —
    the 9 formerly-grandfathered files have all migrated to
    `struct zcl_result`. A NEW service file not in
    the baseline that does not use `struct zcl_result` fails. The
    baseline may only shrink — migrate a file to `zcl_result`, delete
    its line, and the gate then enforces it stays migrated.
  - Override: `// one-result-type-ok:<tag>` (top of a service file that
    genuinely owns no fallible service surface — a pure table/registry
    helper).

- **Gate E3: `check-shape-includes-header`** (HARD)
  - Path: `tools/scripts/check_shape_includes_header.sh`
  - Checks: upgrades the path-only shape map (Gate #18) so a shape file
    must include the header that defines its shape contract — closing
    the "mislabeled Service in a shape folder" hole:
    * `app/conditions/src/*.c` → `"framework/condition.h"` (the Condition
      shape contract) **or** a `"conditions/"` header.
    * `app/models/src/*.c` → a `"models/"` header (each model header pulls
      in `models/activerecord.h`, the AR lifecycle).
    * `app/supervisors/src/*.c` → a `"supervisors/"` header **or**
      `"util/supervisor.h"` (the supervisor liveness contract).
    `app/jobs/` is skipped — its `job.h` shape header does not exist yet.
    The tree fully satisfies this gate today, so it runs HARD.
  - Override: `// shape-include-ok:<tag>` (anywhere in a shape file that
    is a genuine registry/aggregator and cannot include the shape header).

- **Gate E4: `check-projections-pure`** (HARD)
  - Path: `tools/scripts/check_projections_pure.sh`
  - Checks: a projection (`lib/storage/src/*_projection.c`) is a pure fold
    over the event/storage log into its own table(s). It must NOT
    `#include` anything from `app/services/` or `app/controllers/` (that
    inverts the dependency arrow), and must NOT write through the AR model
    save path (`AR_ADHOC_SAVE` / `AR_CACHED_SAVE` / `AR_BEGIN_SAVE`, which
    would fire another model's hooks). The current projection set fully
    complies, so this gate runs HARD.
  - Override: `// projection-cache-ok:<tag>` (on the line of a legitimate
    cache write — memoizing a derived value back into the projection's
    own table outside the strict fold).

- **Gate E6: `check-one-write-path`** (RATCHET)
  - Path: `tools/scripts/check_one_write_path.sh`
  - Checks: new production chain-state write surfaces are forbidden unless
    they route through the existing reducer/log authority or carry a narrow
    `// one-write-path-ok:<tag>` marker. The gate scans for the legacy write
    APIs (`active_chain_set_tip`, `coins_view_*` flush/write calls,
    `process_new_block`, `connect_tip`, `disconnect_tip`,
    `utxo_projection_set_author`) and compares them to
    `tools/scripts/one_write_path_baseline.txt`.
  - Baseline: grandfathered B8 debt. Delete lines as legacy writers are
    removed; growing the file means a new writer appeared and requires an ADR.
  - Override: `// one-write-path-ok:<tag>` only for a compatibility wrapper
    that is demonstrably not a second consensus writer.

- **Gate E7: `check-no-authoritative-ram-state`** (RATCHET)
  - Path: `tools/scripts/check_no_authoritative_ram_state.sh`
  - Checks: direct access to `active_chain` internals and new global/static
    `struct active_chain` instances. Derived in-RAM indexes are allowed only
    through accessors; consensus authority must be the log/projection/cursor
    surface.
  - Baseline: `tools/scripts/no_authoritative_ram_state_baseline.txt`
    (currently empty). Delete lines as debt shrinks; do not add without ADR.
  - Override: `// ram-state-ok:<tag>` on a line that is explicitly a derived
    cache with a documented invariant.

- **Gate E8: `check-no-silent-ready`** (HARD)
  - Path: `tools/scripts/check_no_silent_ready.sh`
  - Checks: the block-connection authority
    (`app/services/src/chain_activation_controller.c`) must
    advance-the-tip OR name-a-typed-blocker every tick (FRAMEWORK.md Prime
    Directive). Any file that performs an
    `activation_set_state(..., ACTIVATION_READY, ...)` transition must also
    route a typed blocker through `blocker_set(` (or a helper such as
    `activation_set_behind_blocker(`), so a non-progress stall is always
    visible in `zcl_state subsystem=blocker` and reaches the supervisor
    escape / operator sink. Closes the 2026-05-26 silent-ready hole where
    the authority went READY with reason "behind_peers" while +950 behind
    the most-work header chain, naming no actionable reason. The tree
    satisfies it today, so it runs HARD.
  - Override: `// no-silent-ready-ok:<tag>` (on a READY transition line that
    provably cannot be a non-progress stall — e.g. a clean caught-up path).

- **Gate E12: `check-honest-witness`** (HARD, FAIL mode)
  - Path: `tools/lint/check_honest_witness.sh`
  - Checks: Law 7 ("heal in the open, page when stuck") — a Condition's
    `witness_<name>()` post-condition must observe the symptom MOVE, not a
    constant, the pure inverse of `detect`, or an FSM / poison-flag the
    remedy itself set (which lets a no-op or self-certifying remedy report
    "cleared" while the tip stays frozen — the exact lie W2 fixed for
    `stale_validate_headers_repair` / `peer_floor_violated` /
    `sync_state_stuck`). A witness fails the gate if it is TRIVIAL (every
    return is a bare `true`/`false`), PURE-INVERSE (`return !detect_x()`),
    or NO-OBSERVABLE (references none of the observable-progress tokens:
    `active_chain_height`, block_map iteration, a durable `SELECT`, a peer /
    inflight / staged / received progress counter). The exemplar honest
    witness is `app/conditions/src/block_failed_mask_at_tip.c`
    (`current_tip_height(ms) > g_tip_at_detect` — the tip MOVED).
  - Baseline: `tools/lint/honest_witness_baseline.txt` (EMPTY; may only
    shrink). The gate runs in `FAIL` mode in `make lint` — the tree is
    clean with no grandfathered entries.
  - Override: `// honest-witness-ok:<reason>` on a line inside a witness body
    whose remedy returns `COND_REMEDY_FAILED` (cannot self-certify) or which
    re-verifies real structural state independently of any remedy-set flag.

`check-framework-shape` (Gate #18) and `check-no-raw-sqlite-in-controllers`
(Gate #20) were **graduated WARN → RATCHET (E10)**. Each now fails on a
new violation while tolerating its baseline:
- Gate #18 baseline = `tools/lint/framework_shape_allowlist.txt`
  (currently empty — all app `.c` files already live in a shape folder).
- Gate #20 baseline = `tools/lint/no_raw_sqlite_in_controllers_baseline.txt`
  (grandfathered controller files with raw sqlite; may only shrink).
Both honor `ZCL_LINT_MODE` (`WARN` | `RATCHET` | `FAIL`); `make lint`
runs them in `RATCHET`.

**Canonical lint-gate list (E11 source of truth).** This block is
machine-checked against the Makefile `lint:` target. Keep it sorted;
edit it whenever you add/remove a gate.

<!-- LINT-GATES-BEGIN -->
- `check-before-save-hooks`
- `check-coins-lookup-nullcheck`
- `check-consensus-parity`
- `check-doc-accuracy`
- `check-file-size-ceiling`
- `check-framework-filename-suffix`
- `check-framework-shape`
- `check-honest-witness`
- `check-lag-slo-observable`
- `check-lib-layering`
- `check-long-functions`
- `check-malloc`
- `check-model-validation`
- `check-no-raw-clock-outside-platform`
- `check-no-raw-sqlite-in-controllers`
- `check-no-authoritative-ram-state`
- `check-no-silent-ready`
- `check-observability-pairing`
- `check-one-result-type`
- `check-one-write-path`
- `check-operator-needed-sink`
- `check-projections-pure`
- `check-pthread-create`
- `check-raw-malloc`
- `check-raw-sqlite`
- `check-rpc-registrar`
- `check-silent-errors`
- `check-silent-errors-conditions`
- `check-silent-errors-controllers`
- `check-silent-errors-jobs`
- `check-shape-includes-header`
- `check-silent-errors-services`
- `check-stage-advances-or-blocks`
- `check-supervisor-domain`
- `check-supervisor-registration`
- `check-typed-blocker`
<!-- LINT-GATES-END -->

---

## 8. Lint-override discipline — every escape hatch is named

Ten lint gates accept an inline override marker when the underlying
rule cannot mechanically hold. The ten marker classes:

| Marker | Where allowed | Lint gate |
|--------|---------------|-----------|
| `// obs-ok:<tag>` | line with `fprintf(stderr, ...)` whose nearby code does not emit an event or terminally propagate | `check-observability-pairing` |
| `// raw-sql-ok:<tag>` | line with `sqlite3_step(...)` outside the `AR_STEP_*` wrappers | `check-raw-sqlite` |
| `// raw-return-ok:<tag>` | bare `return -1;` in MCP / service / controller code with no preceding log line | `check-silent-errors`, `-services`, `-controllers` |
| `// raw-alloc-ok:<tag>` | line with `malloc/calloc/realloc` outside the `zcl_*` wrappers | `check-raw-malloc` |
| `// long-function-ok:<tag>` | signature line of a top-level controller/service function whose body spans >500 lines | `check-long-functions` |
| `// lib-layer-ok:<tag>` | line in `lib/` that includes a `controllers/`, `models/`, `services/`, or `views/` header | `check-lib-layering` |
| `// supervisor-ok:<tag>` | any line in a long-running `app/services/src/*_service.c` that intentionally does not register a supervisor liveness contract | `check-supervisor-registration` |
| `// one-result-type-ok:<tag>` | top of an `app/services/src/*.c` file that owns no fallible service surface (pure table/registry helper) | `check-one-result-type` |
| `// one-write-path-ok:<tag>` | chain-state compatibility wrapper that is not a second consensus writer | `check-one-write-path` |
| `// shape-include-ok:<tag>` | any line in a shape file (condition/model/supervisor) that is a genuine registry/aggregator and cannot include the shape header | `check-shape-includes-header` |
| `// projection-cache-ok:<tag>` | line in a `*_projection.c` with a legitimate cache write outside the strict fold | `check-projections-pure` |
| `// ram-state-ok:<tag>` | line with derived active-chain cache state that must stay non-authoritative | `check-no-authoritative-ram-state` |

**Syntax (machine-enforced).** Every marker requires a non-empty
single-token tag matching `[A-Za-z][A-Za-z0-9_-]+` immediately after
the colon. The space-after-colon form (`// raw-sql-ok: state-kv …`)
and the bare form (`// raw-alloc-ok`) are rejected by the lint —
hyphen-join multi-word tags instead.

**Pairing rule.** A marker is not a free pass; it is a promise that
the override is either:

1. **Logged at this site or one nearby** — the diagnostic is already
   observable (LOG_FAIL above, fprintf on the previous line, the
   caller logs on receiving the propagated failure).
2. **Structurally safe by design** — qsort comparator, void-returning
   helper, pre-boot sentinel, build-time tool, test fixture.

If neither holds, the marker is a bug. Delete it, fix the underlying
issue (route through `AR_BEGIN_SAVE`, add `LOG_FAIL`, switch to
`zcl_malloc`), and let the lint go green naturally.

**Prefer reusable tags that name a structural property over one-off
labels.** `:debug` and `:operator` say nothing about why the call is
safe; `:helper-context-logged` and `:bin-parser-bounds` describe a
class of safe call sites that a future reader can recognize. When the
override pattern at hand matches one already in use (see taxonomy
below), reuse that tag rather than coining a fresh one. Singleton tags
should only survive when they name a genuinely unique structural
property (e.g. `fatal-true-triggers-rollback-and-partial-write-return`)
— ad-hoc labels get folded back into the shared vocabulary.

**Concrete tag taxonomy (existing usage at wave-7c):**

- `obs-ok:` — `pre-existing-diagnostic`, `helper-context-logged`,
  `helper-return-path`, `paired-with-return-false-below`,
  `paired-with-event_emitf-below`, `warning-only-on-best-effort-path`,
  `crash-dump-banner`.
- `raw-sql-ok:` — `progress-kv-kernel-store` (the reducer pipeline's
  `progress.kv` stage cursor + per-stage `*_log` tables — a kernel store
  below the AR layer; see §1 "The one principled exception"),
  `kernel-primitive` (the equivalent tag used inside
  `lib/storage/src/progress_store.c` itself), `kv-state-primitive`,
  `read-only-introspection`, `state-kv-write-caller-handles-rc`,
  `cvs-zcl-ar-raw-sql-rationale`, `test-fixture-setup`,
  `test-fixture-verify`, `standalone-dev-tool`.
- `raw-return-ok:` — `qsort-comparator`, `logged-above`, `sentinel`,
  `bin-parser-bounds`, `sentinel-no-compile-time-windows`.
- `raw-alloc-ok:` — `test-fixture`, `standalone-dev-tool`,
  `db-service-owns-heap-job`.
- `long-function-ok:` — `legacy-import-state-machine`.

Implementation: `tools/check_observability_pairing.c`,
`tools/scripts/check_raw_sqlite.sh`,
`tools/scripts/check_raw_malloc.sh`,
`tools/scripts/check_long_functions.sh`, and the inline
`check-silent-errors*` recipes in `Makefile:654+`.

---

## Summary: How agents learn to follow the Rails way

1. **Compiler errors** for raw `sqlite3_step` (unless opted out)
2. **Type system** forces `struct zcl_result` with message on failure
3. **CI lint** catches raw malloc, silent returns, missing error bodies,
   long-function bloat
4. **Macros** make the right thing easier than the wrong thing
5. **Before/after hooks** wired by default — agents see the pattern and follow it
6. **This document** in `docs/` — agents read it on `cat docs/DEFENSIVE_CODING.md`

The Rails philosophy isn't "write good code." It's "make it harder
to write bad code than good code." These 8 patterns achieve that in C23.
