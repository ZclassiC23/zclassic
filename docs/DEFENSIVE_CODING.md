# Defensive Coding Standards — The Rails Way in C23

**Rule: if the compiler can't enforce it, it will be violated.**

These are architectural enforcement patterns that make it impossible for any
contributor (human or AI agent) to accidentally skip validation, swallow
errors, or leak memory. Read this before writing new code.

Modules prefixed `legacy_` are a compatibility layer with an external
`zclassicd` — see [`LEGACY_LIFECYCLE.md`](./LEGACY_LIFECYCLE.md) for which
paths are still load-bearing.

---

## 1. Every write goes through the AR lifecycle — no exceptions

**Problem:** `coins_view_sqlite.c` / `wallet_sqlite.c` once called
`sqlite3_step()` directly with no validation. That is how we lost 1.3M UTXOs
on 2026-04-10.

**Enforcement:** `activerecord.h` poisons raw `sqlite3_step` in app code at
compile time (a `_Pragma("GCC error …")` macro, guarded by `ZCL_AR_ENFORCE` /
opt-out `ZCL_AR_RAW_SQL` — see `app/models/include/models/activerecord.h`),
plus a CI lint that re-checks the same surface. The Makefile adds
`-DZCL_AR_ENFORCE` globally, so raw SQL becomes a conscious, visible decision.

**The three lifecycle entry points (all in `activerecord.h`).** All three run
the same `validate_*` → `before_save` → `after_save` chain, so model hooks fire
identically. Pick the one that fits the call site:

| Macro | When to use | What it does |
|-------|-------------|--------------|
| `AR_BEGIN_SAVE(cbs, name, rec, validate_fn)` + `AR_FINISH_SAVE(cbs, rec, ok)` | You build the statement yourself between the two macros (multi-statement transactions, conditional binds) | `AR_VALIDATE_RECORD` → `before_save` → your code → `after_save` → `return ok` |
| `AR_ADHOC_SAVE(ndb, stmt, sql, cbs, name, rec, validate_fn, bind_code)` | Single locally-prepared INSERT/UPDATE (the common case) | Wraps `AR_BEGIN_SAVE` + `AR_PREPARE_BOOL` + bind block + `AR_FINALIZE_STEP_DONE` + `AR_FINISH_SAVE` |
| `AR_CACHED_SAVE(stmt, cbs, name, rec, validate_fn, bind_code)` | Hot path with a cached prepared stmt owned by `node_db` | Same lifecycle, skips prepare — call `AR_RESET(stmt)` and bind |

For a worked call-site example (`db_wallet_key_save` using `AR_ADHOC_SAVE`),
see `lib/wallet/src/wallet_sqlite.c`.

Storage primitives that legitimately need raw SQL
(`lib/storage/src/coins_view_sqlite.c`) opt out with `#define ZCL_AR_RAW_SQL`.

**Status: shipped.** The ratchet allowlist
`tools/scripts/raw_sqlite_allowlist.txt` is empty. Raw `sqlite3_step()` is
linted across `app/`, `tools/`, `lib/`, `config/`, and `src/`; production step
calls must use `AR_STEP_ROW`, `AR_STEP_DONE`, `AR_STEP_ROW_READONLY`, or
`AR_STEP_WRITE` unless they carry a reviewed `// raw-sql-ok:<tag>`. Structured
domain-model saves still use the AR lifecycle entry points above so validation
and before/after hooks fire. Direct `sqlite3_exec(ndb->db|ndb.db, "...")` DML
(`INSERT`/`DELETE`/`UPDATE`/`REPLACE`) is also linted and must route through
`ar_exec_write_sql()` / `AR_STEP_WRITE` or a reviewed helper; transaction
control, PRAGMAs, ATTACH/DETACH, schema DDL, projection stores, and progress.kv
remain outside that narrow DML gate. `make lint` runs `check_raw_sqlite.sh`
(one of the gates in the canonical block below).

### The one principled exception: the `progress.kv` kernel store

The AR lifecycle is the law for **`node.db` domain models** — blocks, UTXOs,
wallet keys, peers, mempool entries. Those rows have an identity, a
`validate_*` function, and before/after-save hooks.

The reducer pipeline does **not** write its stage state to `node.db`. It writes
to `progress.kv` — a separate, singleton, WAL kernel store
(`lib/storage/src/progress_store.c`, opened once at boot) holding the F-2
`stage` primitive's `stage_cursor` table plus the per-stage `*_log` tables
(`header_admit_log`, `body_fetch_log`, `validate_headers_log`, `utxo_apply_log`,
`utxo_apply_delta`, `created_outputs`, `tip_finalize_log`, …). `progress.kv`
sits **below** the AR/domain-model layer by design (see
`storage/progress_store.h`, `docs/FRAMEWORK.md`): a `stage_cursor` row is not a
model, has no domain identity, no save hooks. Cursor commits are tiny, hot-path,
and want their own WAL out of the way of larger `node.db` transactions; the saga
atomicity contract (a stage advance and its log row commit together) lives in
`progress_store_tx_lock()` + `BEGIN IMMEDIATE`, not in AR.

Routing these through AR would be a **category error** (no model to validate, no
hook to fire). The discipline:

- A raw `sqlite3_step` on the `progress.kv` handle (always from
  `progress_store_db()`, never a `node_db`/`ndb` handle) is correct-by-design,
  not migration debt.
- Every such site MUST carry the canonical marker
  **`// raw-sql-ok:progress-kv-kernel-store`** (one no-space token after the
  colon). `progress_store.c` itself uses the equivalent `kernel-primitive` tag.
- This is a **bounded, stable exception**, not a deferred migration — the count
  changes only when the reducer gains/drops a stage table; it does not ratchet
  toward zero. `check_raw_sqlite.sh` treats `progress-kv-kernel-store` as the
  principled kernel-store hatch.

A reducer Job that writes a `node.db` **model** (not a progress.kv `*_log`/cursor
row) does NOT get the kernel-store marker — it goes through the AR lifecycle.

---

## 2. Every function that can fail returns a result type — not bare bool

**Problem:** `return false` with no context. Caller has no idea why.

**Enforcement:** standard result type `struct zcl_result` (`.ok`, `.code`,
`.message[256]`, `.source_file`, `.source_line`) with `ZCL_OK`, `ZCL_ERR`, and
`ZCL_CHECK` macros — see `lib/util/include/util/result.h`.

**Rule:** new service functions MUST return `struct zcl_result` instead of
`bool`. Existing code migrates incrementally.

**Why this works for agents:** an agent that writes `return false;` in a
function declared as returning `struct zcl_result` gets a compiler error. It
MUST write `return ZCL_ERR(-1, "reason: %s", detail);`, forcing it to explain
the failure.

---

## 3. Every malloc is checked — use zcl_malloc or die

**Problem:** 15+ unchecked malloc/calloc calls in sync services → silent NULL
dereference.

**Enforcement:** `lib/util/include/util/safe_alloc.h` provides:

- `zcl_malloc(size, label)` — logs (`malloc_failed` + `EV_OOM`) and returns
  NULL; use when graceful degradation is possible.
- `zcl_malloc_or_die(size, label)` — aborts via `zcl_oom_abort` on failure;
  use when there is no reasonable fallback.
- `zcl_realloc(ptr, size, label)` — never leaks the original pointer (does NOT
  free `ptr` on failure — caller decides).

**Makefile enforcement:** `check-malloc` greps `app/ lib/ config/ tools/` for
raw `malloc`/`calloc`/`realloc` outside the `zcl_*` wrappers, `safe_alloc.h`,
and `vendor/`. Files needing raw alloc add `// raw-alloc-ok` on the line.

---

## 4. Every error path logs with context — use the LOG_* macros

**Problem:** 100+ `return -1;` / `return false;` with no logging.

**Enforcement:** `lib/util/include/util/log_macros.h` provides log-and-return
macros (each logs `error` with `file`/`line`/`func` + varargs):

- `LOG_FAIL(domain, fmt, ...)` — log + `return false`.
- `LOG_ERR(domain, fmt, ...)` — log + `return -1` (for MCP handlers).
- `LOG_RETURN(val, domain, fmt, ...)` — log + `return (val)`.

**CI lint:** `check-silent-errors` greps `app/ tools/mcp/` for `return -1;` not
paired with `LOG_ERR`/`log_json`/`fprintf`.

---

## 5. MCP handlers must log on every error path

**Problem:** silent `return -1;` in MCP handlers leaves the caller with no
diagnostic info.

**Status: enforced by lint.** `make lint` runs five shape-tier gates. Each
requires a bare `return -1;` to be preceded by a logging call or carry an
explicit `// raw-return-ok:<reason>` marker (no space after the colon):

| Gate | Surface |
|------|---------|
| `check-silent-errors` | `tools/mcp/controllers/*.c` |
| `check-silent-errors-services` | `app/services/src/` |
| `check-silent-errors-controllers` | `app/controllers/src/` |
| `check-silent-errors-jobs` | `app/jobs/src/` |
| `check-silent-errors-conditions` | `app/conditions/src/` |
| `check-silent-errors-bool` | `app/{controllers,services,jobs,conditions,models}/src/` (RATCHET) |
| `check-wallet-raw-prepare-log` | raw `sqlite3_prepare_v2()` + unlogged NULL-check in `app/`, `lib/` (RATCHET) |

The service/controller/job/condition gates accept only an *error-level*
preceding log (`LOG_ERR`, `LOG_FAIL`, `LOG_RETURN`, or `log_json` at error
level) — a bare `printf`/`LOG_WARN` no longer satisfies the pairing, so a
silent failure can never masquerade as handled by a warn-level breadcrumb.

A future `mcp_fail(res, code, fmt, ...)` helper could enforce `res->body` is
populated on every error path by return-type discipline, but that is optional
polish — the lint already prevents the silent-fail class entirely.

---

## 6. Before/after save hooks — wired

**Status: shipped.** Every critical model wires `ar_register_before_save` and
`ar_register_after_save`. `check-before-save-hooks` enforces that `utxo`,
`block`, `wallet_key`, and `wallet_tx` keep these hooks — drop one and `make
lint` fails.

| Model | before_save | after_save |
|-------|-------------|------------|
| wallet_key | Log if `ZCL_WALLET_PASSPHRASE` set (keystore owns at-rest wrap) | Emit `EV_WALLET_KEY_SAVED` (`EV_SAPLING_KEY_SAVED` for sapling rows) |
| utxo | Validate money range + script coherence | Update UTXO commitment cache |
| block | Validate hash matches header | Emit `EV_BLOCK_SAVED` |
| wallet_tx | Validate txid format | Emit `EV_WALLET_TX_SAVED` |
| mempool_entry | Validate fee + size envelope | (none) |
| tx_index | Validate txid + block height | (none) |

---

## 7. CI gates — the final enforcer

`make lint` runs the gates listed in the canonical block below (the Makefile
`lint:` target is authoritative); `make ci` runs `lint test fuzz-ci coverage`.

An agent that pushes code with raw malloc, silent errors, bypassed AR
validation, unpaired stderr diagnostics, a critical model missing its
before_save hook, a model file with no `validates_*` call and no
`ar-validate-skip:<tag>` marker, a model save that hand-runs
`ar_run_before_save()` / `ar_run_after_save()` instead of the AR lifecycle
macros, or a controller/service function over 500 lines without a
`long-function-ok:<tag>` override, gets a red build before any human sees it.

Gates fall into three modes:

- **HARD / FAIL** — fails on any violation.
- **RATCHET** — fails on a *new* violation while tolerating a recorded
  baseline; the baseline file may only shrink (growing it requires an ADR).
- **WARN** — measures only (none currently; #18 and #20 graduated WARN →
  RATCHET as **E10**, 2026-05-26; #19 ratcheted WARN → FAIL in Phase 1).

Each gate's intent is one row below. Implementation scripts live under
`tools/scripts/` or `tools/lint/`. The E-series gates are tested in
`lib/test/src/test_make_lint_gates.c` (plant fixture → assert trip → remove →
assert green).

### §3–§6 core gates

| Gate | Mode | Intent / marker |
|------|------|-----------------|
| `check-blob-read-bounds` | HARD | Fixed-size SQLite blob reads in app models use `AR_READ_BLOB` or prove `sqlite3_column_bytes` before `memcpy`. |
| `check-malloc`, `check-raw-malloc` | HARD | Raw malloc/calloc/realloc outside `zcl_*` wrappers (§3). Override `// raw-alloc-ok:<tag>`. |
| `check-raw-sqlite` | HARD | Raw `sqlite3_step` outside `AR_STEP_*` (§1). Override `// raw-sql-ok:<tag>`. |
| `check-silent-errors` (+ `-services`/`-controllers`/`-jobs`/`-conditions`) | HARD | Bare `return -1;` with no error-level log (§4/§5). Override `// raw-return-ok:<tag>`. |
| `check-before-save-hooks` | HARD | `utxo`/`block`/`wallet_key`/`wallet_tx` keep before/after-save hooks (§6). |
| `check-coins-lookup-nullcheck` | HARD | Coins lookups null-check the returned coin before use. |
| `check-log-macro-return-type` | HARD | Returning `LOG_*` macros match the enclosing function return type (`LOG_FAIL` only in bool-returning functions, `LOG_ERR` only in int-returning functions, `LOG_NULL` only in pointer-returning functions). |
| `check-observability-pairing` | HARD | `fprintf(stderr,…)` pairs with an event emit / terminal propagation. Override `// obs-ok:<tag>`. |
| `check-pthread-create` | HARD | Thread spawns go through the sanctioned registry, not raw `pthread_create`. |

### Detailed gates

- **Gate #11: `check-model-validation`** (HARD) — every `app/models/src/*.c`
  has at least one `validates_*` call (from
  `app/models/include/models/activerecord.h`) OR a top-of-file
  `ar-validate-skip:<tag>` marker explaining why AR validation does not apply
  (e.g. `connection-handle-not-a-row`). Pins the wave-6 result. Impl:
  `tools/scripts/check_model_validation.sh`.

- **Gate #11b: `check-model-ar-lifecycle`** (HARD) — `app/models/src/*.c` may
  not call `ar_run_before_save()` or `ar_run_after_save()` directly. Save paths
  must use `AR_BEGIN_SAVE`, `AR_ADHOC_SAVE`, `AR_CACHED_SAVE`, and
  `AR_FINISH_SAVE` so `before_validate -> validate -> before_save -> SQL ->
  after_save` stays one mechanically enforced lifecycle. Impl:
  `tools/scripts/check_model_ar_lifecycle.sh`.

- **Gate #12: `check-long-functions`** (HARD) — flags any top-level function in
  `app/controllers/src/*.c` or `app/services/src/*.c` whose body spans >500
  lines. Long functions conceal multiple concerns. Override
  `// long-function-ok:<tag>` on the signature line (tag matches
  `[A-Za-z][A-Za-z0-9_-]+`). Impl: `tools/scripts/check_long_functions.sh`.

- **Gate #13: `check-rpc-registrar`** — every RPC handler declared in
  `lib/rpc/src/` must appear in the registrar table at the bottom of the same
  file, so "method not found" is caught at build time.

- **Gate #14: `check-lag-slo-observable`** — any code path that can produce
  SLO-relevant lag (block lag, peer floor, watchdog miss) must pair with a
  structured event emit and a Prometheus gauge.

- **Gate #15: `check-lib-layering`** (RATCHET) — flags any
  `#include "controllers/…"`, `"models/…"`, `"services/…"`, or `"views/…"` in
  `lib/**/*.c|.h` outside `lib/test/`. lib/ is the foundation; a backward
  include means a lib/ file is doing app/ work. Baseline
  `tools/scripts/lib_layering_baseline.txt` is empty (98 originals all
  remediated). Override `// lib-layer-ok:<tag>`. Impl:
  `tools/scripts/check_lib_layering.sh`.

- **Gate #45: `check-domain-purity`** (HARD) — `domain/` is the innermost
  layer. A `domain/**/*.c|.h` file (outside `*/test/*`) may only `#include` its
  own `"domain/…"` headers, C/system `<…>` headers, bare domain-local sibling
  files (a quoted include with no slash, e.g. `"reject_out.h"`), or one of the
  12 allowed lib subsystems (`bloom chain coins consensus core crypto keys
  primitives script support util validation`). Any include from an app/ shape
  (`controllers/`, `models/`, `services/`, `views/`) or an unlisted lib/
  subsystem (`storage/`, `ports/`, …) fails the build. No baseline (the tree is
  already clean). Override `// domain-purity-ok:<tag>`. Impl:
  `tools/scripts/check_domain_purity.sh`.

- **Gate #46: `check-core-include-boundary`** (HARD) — the sealed consensus
  core (top-level `core/`, populated by the Wave 1.1 physical split) is the
  innermost layer. A `core/**/*.c|.h|.inc` file may only `#include` its own
  preserved headers (`"domain/consensus/…"`, `"consensus/…"`, `"core/…"`,
  `"chainparams/…"`), C/system `<…>` headers, bare siblings, or a pure leaf lib
  subsystem — **never** `lib/validation` (validation *drives* consensus, it is
  not consensus) or any app/ shape. The gate is **exception-free** as of the
  Pre-W5 content fix: `check_block.c` now calls the core sigops predicate
  instead of `"validation/sigops.h"`, and `chainparams.h` gets
  `MESSAGE_START_SIZE` from `chain/chainparamsbase.h` instead of
  `"net/protocol.h"`. Any forbidden include now fails HARD. Override
  `// core-boundary-ok:<tag>`. Impl: `tools/scripts/check_core_include_boundary.sh`.

- **Gate #47: `check-core-seal`** (HARD — frozen at split wave W5) — pins
  the byte-integrity of `core/` to the SHA3-256 manifest `core/MANIFEST.sha3`
  (per-file digest + a ROOT digest over the sorted `(path, filehash)` stream).
  Any change to a sealed file changes ROOT. Regenerate with `make core-seal`;
  verify standalone with `make core-seal-check` (fails loud). A deliberate
  consensus-core change goes through the owner unseal ritual
  (`make core-unseal REASON=…` → append-only `core/UNSEAL.md` + one-shot
  `.core-unseal-token`). Tool: `tools/core_seal.c` (no external deps: links the
  in-tree FIPS-202 SHA3-256).

- **Gate #16: `check-supervisor-registration`** (RATCHET) — flags any
  `app/services/src/*_service.c` that spawns work (`pthread_create`,
  `thread_registry_spawn`, `health_register_periodic`) but does NOT call
  `supervisor_register(`, is not baselined, and has no `// supervisor-ok:<tag>`.
  Round 5 (2026-05-21) added the time-driven supervisor in
  `lib/util/supervisor.{c,h}` after the lib/health sweeper wedged 8.6 h leaving
  every periodic check silently dead; registered children fire `on_tick` /
  edge-trigger `on_stall` independently. Round-5 children:
  `sync.watchdog`, `net.outbound_floor`, `chain.coord_escalation`. Baseline
  `tools/scripts/supervisor_baseline.txt` (drained to 0 entries; Track C-3
  complete). Impl:
  `tools/scripts/check_supervisor_registration.sh`.

- **Gate #17: `check-typed-blocker`** — any code raising a `block_id` must use a
  typed kind from `enum blocker_kind` (`lib/util/src/blocker.c`, Round 6
  2026-05-21); raw string blockers fall in a baseline.

### Framework refactor gates (#18–#22)

- **Gate #18: `check-framework-shape`** (RATCHET, was WARN) —
  `tools/lint/framework_shape_check.sh`. Every `.c` under `app/` must live in
  one shape folder: `controllers`, `services`, `models`, `jobs`, `supervisors`,
  `conditions`, `events`, `views`. Baseline
  `tools/lint/framework_shape_allowlist.txt` (empty). Fix: move the file or
  split mixed responsibilities.

- **Gate #19: `check-no-raw-clock-outside-platform`** (FAIL, Phase 1
  2026-05-23) — `tools/lint/check_no_raw_clock_outside_platform.sh`. Bans direct
  `clock_gettime(`, `time(NULL)`, `getrandom(` outside `lib/platform/`. Route
  wall-clock/monotonic through the platform clock and entropy through platform
  RNG. Override `// platform-ok` on the line.

- **Gate: `check-no-shellouts`** (FAIL) —
  `tools/lint/check_no_shellouts.sh`. Bans `system(`, `popen(`, `execlp(` in
  the resident node binary's own code (`app/`, `lib/` excl. `lib/test/`,
  `src/`, `config/`). Every shell-out has been migrated onto the two in-tree
  primitives — `lib/util` `file_tree_ops` (`zcl_tree_copy` / `zcl_tree_remove`,
  fd-based, no shell) and `lib/util` `spawn` (`zcl_spawn_detached` /
  `zcl_spawn_capture`, argv-array, no `/bin/sh`). This is os-substrate Rung 0
  (`docs/work/os-substrate-plan.md` §1) and the precondition for a future
  seccomp `execve` deny-list. Standalone dev/bench/CLI binaries under `tools/`
  and the `lib/test/` fixtures are out of scope. Override `// shellout-ok` for
  a documented, reviewed exception.

- **Gate: `check-proc-self-shim`** (RATCHET) —
  `tools/lint/check_proc_self_shim.sh`. Bans raw `"/proc/self` / `"/proc/uptime`
  string literals in `app/`, `config/`, `lib/`, `tools/` outside
  `lib/platform/`. Route process/host memory, uptime, and exe-path reads
  through `platform/os_proc.h` (os-substrate Rung 1,
  `docs/work/os-substrate-plan.md` §2). Baseline
  `tools/lint/proc_self_shim_baseline.txt` (shrink-only); the one permanent
  exemption is `lib/sim/src/postmortem.c`'s async-signal-safe crash-handler
  copy, which cannot route through a non-signal-safe shim.

- **Gate #20: `check-no-raw-sqlite-in-controllers`** (RATCHET, was WARN) —
  `tools/lint/check_no_raw_sqlite_in_controllers.sh`. Bans
  `sqlite3_prepare_v2(` / `sqlite3_exec(` in `app/controllers/` and
  `tools/mcp/controllers/`. Baseline
  `tools/lint/no_raw_sqlite_in_controllers_baseline.txt` (may only shrink). Fix:
  move reads behind projections/models, writes through the AR lifecycle.
  Override `// raw-controller-sql-ok`.

- **Gate #21: `check-supervisor-domain`** (FAIL) —
  `tools/lint/check_supervisor_domain.sh`. Production `supervisor_register(`
  calls under `app/`, `config/`, `lib/` must use
  `supervisor_register_in_domain(...)` (`chain`, `net`, `mempool`, `wallet`,
  `feature`, `onion`, `op`). Deliberate root child: `// supervisor-root-ok:<tag>`.

- **Gate #22: `check-framework-filename-suffix`** (FAIL/HARD) —
  `tools/lint/check_framework_filename_suffix.sh`. No `.c` under a shape folder
  may end in a DIFFERENT shape's suffix (the eight: `controller`, `service`,
  `model`, `view`, `job`, `supervisor`, `condition`, `event`). A file may keep
  its own folder's suffix or a bare entity name (`models/block.c`); only a
  foreign suffix is rejected. `_store`/`_repository` name no shape. Recurrence
  guard for the S1 renames. If an entity name legitimately ends in a shape word
  (`models/file_service.c`), add `// suffix-ok:<tag>`.

- **Gate #23: `check-thread-supervision`** (RATCHET) —
  `tools/lint/check_thread_supervision.sh`. Universalizes Gate #16 from the
  services dir to EVERY `thread_registry_spawn(` call site under `app/`,
  `lib/`, `config/`. Each spawned long-running thread must be accounted for:
  (a) SUPERVISED — its TU registers a liveness contract
  (`supervisor_register{,_in_domain}(` or the lib/util adapter
  `thread_liveness_register(`), or the spawn line/line-above carries a
  `// supervised:<child>` marker; (b) MARKED-EXEMPT — a
  `// thread-supervision-ok:<reason>` marker (bounded/joined worker pool,
  one-shot job); or (c) BASELINED — the thread's name literal is in
  `tools/lint/thread_supervision_baseline.txt` with a disposition +
  justification. Anything else is a NEW unaccounted thread → FAIL. The
  baseline may only SHRINK: a stale entry (thread renamed/removed/now-covered)
  fails, forcing the count down as threads gain contracts via
  `util/thread_liveness.h` (exemplar `lib/health/src/heartbeat.c`). Closes the
  gap where the lib/ + config/ infrastructure threads (health sweep, metrics,
  event dispatch, RPC-timeout, DB worker/checkpoint) were spawned but never on
  the supervisor tree — a wedged loop there was silent, the exact Round-5
  failure mode. Raw `pthread_create` is separately gated by
  `check-pthread-create`. The `SUPERVISOR_CAP` static registry was raised
  32→64 to seat the new root children.

#### Bounded thread restart policy (Erlang/OTP), alongside Gate #23

Universal supervision (Gate #23) makes a wedged/dead infra thread a *named
blocker* instead of a silent stop. The restart policy goes one step further for
threads that are safe to auto-restart: it *respawns* them under a bounded
intensity cap (`lib/util/include/util/supervisor.h` +
`util/thread_liveness.h`). The policy is opt-in per child; the default is
`SUPERVISOR_RESTART_TEMPORARY`, i.e. unchanged named-blocker behavior.

- **Policy enum** (`enum supervisor_restart_policy`): `TEMPORARY` (never
  auto-restart — the default and the only policy for consensus/stateful
  threads), `TRANSIENT` (restart only on abnormal exit), `PERMANENT` (always
  restart while the node runs).
- **Which threads may be PERMANENT — the safe-restart criterion:** ONLY a pure,
  stateless *periodic-loop* worker with NO consensus/shared mutable state, where
  re-entering the loop from scratch is a no-op on correctness. Today exactly
  three qualify and opt in via `thread_liveness_register_restartable`:
  `zcl_metrics` (stats printer + Prometheus gauge setter),
  `zcl_health_sweep` (independent snapshot-and-dispatch each sweep), and
  `zcl_rpc_timeout` (periodic timeout sweep; all persistent state lives on the
  manager, not the thread). Everything else stays `TEMPORARY`. In particular
  the **8 reducer stages, the reducer drive, and the DB worker/checkpointer MUST
  stay TEMPORARY** — respawning them mid-fold is a correctness hazard; they keep
  the named-blocker + operator model.
- **Die-vs-stall detection:** the worker publishes an atomic
  `worker_state` (ALIVE at loop entry / on every heartbeat, EXITED just before
  it returns). A *slow* worker is still ALIVE (heartbeat lapsed → the existing
  stall/named-blocker path); a *dead* worker is EXITED → the restart path.
  Honesty caveat: a hard SIGSEGV takes down the whole process (POSIX C has no
  per-thread crash recovery), so the recoverable failure is an abnormal RETURN
  from the loop, not an independent segfault. The respawn additionally reaps and
  guards with `pthread_tryjoin_np` (EBUSY ⇒ the worker is actually still alive ⇒
  abort without spawning) so a false EXITED can never double-spawn a live
  thread; a supervisor-only `RESTARTING` claim state prevents the reverse
  lost-signal race.
- **Restart-storm cap (OTP intensity/period):** at most N restarts within M
  seconds per child (the three workers use N=5 / M=60 s). The (N+1)th death
  inside the window stops respawning and names a PERMANENT blocker
  `thread_restart_storm_<name>` — the node stays alive and named, and never
  infinite-spawns or crashes the process. The cap is sticky (a resumed
  heartbeat does not clear it); an operator diagnoses and restarts.

`ZCL_LINT_MODE` (`WARN` | `RATCHET` | `FAIL`) selects mode for #18/#20; `make
lint` runs them in `RATCHET`.

### Build-checklist gates (E-series, 2026-05-26)

Tooling-only: each turns the build red on a *regression* without breaking the
current green tree.

| Gate | Mode | Intent / baseline / override |
|------|------|------------------------------|
| **E1: `check-file-size-ceiling`** | RATCHET | No `app/**/*.c` exceeds **800 lines** (caps mega-modules hiding behind many <500-LOC functions). Baseline `file_size_ceiling_baseline.txt` (`<path> <max-loc>`; may only shrink) IS the visible escape hatch — no inline override. |
| **E2: `check-one-result-type`** | RATCHET | New `app/services/src/*.c` reference `struct zcl_result` (§2) instead of bare bool/int. File-granularity. Baseline `one_result_type_baseline.txt` (empty; 9 originals migrated). Override `// one-result-type-ok:<tag>` (pure table/registry helper). |
| **`check-service-result-convergence`** | RATCHET | Phase 3 sibling to E2: counts *exported* (non-static, top-level) bare-bool function DEFINITIONS per `app/services/src/*.c` file, so a file cannot stay E2-clean forever while still exporting legacy bool functions alongside its one `zcl_result` use. Baseline `service_result_convergence_baseline.txt` (`<path> <count>`; may only shrink; a file whose live count reaches 0, or that gains the `// one-result-type-ok:<tag>` marker, must be deleted from it). Same override marker as E2. Inventory + lane plan: `docs/work/service-result-convergence.md`. |
| **E3: `check-shape-includes-header`** | HARD | A shape file must include its shape contract header: conditions → `"framework/condition.h"` or `"conditions/"`; models → a `"models/"` header (pulls AR lifecycle); supervisors → `"supervisors/"` or `"util/supervisor.h"`. `app/jobs/` skipped (no `job.h` yet). Override `// shape-include-ok:<tag>`. |
| **E4: `check-projections-pure`** | HARD | A projection (`lib/storage/src/*_projection.c`) is a pure fold: no `#include` from `app/services/`-`app/controllers/`, and no AR save path (`AR_*_SAVE`, which would fire another model's hooks). Override `// projection-cache-ok:<tag>` (memoize a derived value into the projection's own table). |
| **E6: `check-one-write-path`** | RATCHET | New chain-state write surfaces forbidden unless they route through the reducer/log authority. Scans for legacy writers (`active_chain_set_tip`, `coins_view_*` flush/write, `process_new_block`, `connect_tip`, `disconnect_tip`, `utxo_projection_set_author`) vs `one_write_path_baseline.txt`. Override `// one-write-path-ok:<tag>` (compat wrapper, not a second consensus writer). |
| **E7: `check-no-authoritative-ram-state`** | RATCHET | No direct `active_chain` internals access / new global-static `struct active_chain`. Derived RAM indexes only via accessors; consensus authority is the log/projection/cursor surface. Baseline `no_authoritative_ram_state_baseline.txt` (empty). Override `// ram-state-ok:<tag>` (documented derived cache). |
| **E8: `check-no-silent-ready`** | HARD | The block-connection authority (`app/services/src/chain_activation_service.c`) must advance-the-tip OR name a typed blocker every tick (FRAMEWORK.md Prime Directive). Any `activation_set_state(…, ACTIVATION_READY, …)` must also route a typed blocker via `blocker_set(` (or `activation_set_behind_blocker(`). Closes the 2026-05-26 silent-ready hole (READY "behind_peers" while +950 behind). Override `// no-silent-ready-ok:<tag>`. |
| **E9: `check-operator-needed-sink`** | HARD | `EV_OPERATOR_NEEDED` ("auto-healing gave up, page a human") is emitted in production AND has a registered subscriber in `lib/util/src/alerts.c` (rule with `.trigger = EV_OPERATOR_NEEDED` via `event_observe(`). Prevents the silent-halt class where the loud signal reaches no sink. No override. |
| **P1-3: `check-systemd-memory-budget`** | HARD | Systemd service hard caps (`MemoryMax` plus finite `MemorySwapMax`) must stay below the host budget (default 70% of MemTotal); explicit `MemoryMax=infinity` fails. Prevents host-level OOM from cap drift. |
| **E11: `check-doc-accuracy`** | HARD | The canonical gate block below matches the `check-*` prerequisites of the Makefile `lint:` target by count AND name set. On mismatch, fix the doc block — the Makefile is authoritative. No override. |
| **`check-markdown-links`** | HARD | Every local file/directory target in tracked Markdown resolves inside the repository. Network/mail/app URIs, page anchors, images, code examples, and explicit generated placeholders are outside this filesystem-only contract. The gate fails loud on an empty scan/parser result and carries isolated positive/negative self-tests. No baseline or override. |
| **E12: `check-honest-witness`** | FAIL | Law 7 ("heal in the open, page when stuck"): a Condition's `witness_<name>()` must observe the symptom MOVE, not a constant, the pure inverse of `detect`, or an FSM/poison-flag the remedy itself set. Fails if TRIVIAL (every return a bare `true`/`false`), PURE-INVERSE (`return !detect_x()`), or NO-OBSERVABLE (references none of `active_chain_height`, reducer-frontier H\*, block_map iteration, a durable `SELECT`, a peer/inflight/staged/received progress counter). Exemplar: `app/conditions/src/block_failed_mask_at_tip.c`. Baseline `tools/lint/honest_witness_baseline.txt` (empty). Override `// honest-witness-ok:<reason>` (witness whose remedy returns `COND_REMEDY_FAILED` or re-verifies real structural state). |
| **E14: `check-condition-cooldown`** | HARD | The 2026-07-13 27h-page bug class: a `COND_CRITICAL` condition whose `detect()` calls a known peer/network-liveness primitive (`connman_max_peer_height`, `connman_get_node_count`, `sync_monitor_connman`, `sync_monitor_max_peer_height`) or the legacy `zclassicd` RPC oracle (`legacy_chain_rpc_*`) must set `.cooldown_secs > 0` (condition.c re-arms the remedy instead of latching permanently at `max_attempts`) or wire a `.progressing` callback (TL-1's alternate anti-latch mechanism, e.g. `reducer_frontier_reconcile_light.c`). Exemplar fix: `app/conditions/src/sync_violation_lag.c`. Self-tested against an isolated tmp dir (`ZCL_CONDITION_COOLDOWN_SELFTEST=1`), proven in `make test`/`make test-parallel` via `t_e14_condition_cooldown_gate()`. No baseline (structural, not a ratchet). |

E10 = the WARN→RATCHET graduation of #18 and #20 (above).

**Canonical lint-gate list (E11 source of truth).** This block is machine-checked
against the Makefile `lint:` target. Keep it sorted; edit it whenever you
add/remove a gate.

<!-- LINT-GATES-BEGIN -->
- `check-blob-read-bounds`
- `check-before-save-hooks`
- `check-build-epoch-integrity`
- `check-checkout-lock`
- `check-coins-lookup-nullcheck`
- `check-condition-cooldown`
- `check-consensus-parity`
- `check-core-include-boundary`
- `check-core-seal`
- `check-doc-accuracy`
- `check-doc-counts`
- `check-domain-purity`
- `check-file-purpose`
- `check-file-size-ceiling`
- `check-framework-filename-suffix`
- `check-framework-shape`
- `check-git-hooks-installed`
- `check-group-purpose`
- `check-honest-witness`
- `check-lag-slo-observable`
- `check-lib-layering`
- `check-log-macro-return-type`
- `check-long-functions`
- `check-markdown-links`
- `check-malloc`
- `check-model-ar-lifecycle`
- `check-model-validation`
- `check-no-raw-clock-outside-platform`
- `check-sysinit-ordering`
- `check-sandbox-wired`
- `check-no-raw-sqlite-in-controllers`
- `check-no-shellouts`
- `check-proc-self-shim`
- `check-no-authoritative-ram-state`
- `check-no-new-borrowed-seed`
- `check-no-new-coin-backfill-caller`
- `check-no-new-repair-rung`
- `check-no-orphan-placement`
- `check-no-silent-ready`
- `check-no-stray-untracked-source`
- `check-observability-pairing`
- `check-one-result-type`
- `check-one-write-path`
- `check-operator-needed-sink`
- `check-projections-pure`
- `check-pthread-create`
- `check-raw-malloc`
- `check-raw-sqlite`
- `check-rpc-registrar`
- `check-scanner-immunity`
- `check-service-result-convergence`
- `check-silent-errors`
- `check-silent-errors-bool`
- `check-wallet-raw-prepare-log`
- `check-silent-errors-conditions`
- `check-silent-errors-controllers`
- `check-silent-errors-jobs`
- `check-shape-includes-header`
- `check-silent-errors-services`
- `check-stage-advances-or-blocks`
- `check-supervisor-domain`
- `check-supervisor-registration`
- `check-systemd-memory-budget`
- `check-test-registration`
- `check-thread-supervision`
- `check-typed-blocker`
- `check-vendor-provenance`
- `check-doc-no-false-deleted`
- `check-stage-log-reorg-unsafe`
- `check-stable-publish-contained`
- `check-zclassicd-reach-allowlist`
- `check-no-csr-lock-on-finalize-drive`
- `check-mint-skip-crypto-offline-only`
- `check-wire-harness-security-gate`
- `check-hotswap-dev-only`
- `check-hotswap-eligible-scope`
- `check-hotswap-static-state`
- `check-hotswap-swappable-shape`
- `check-release-no-dev-symbols`
- `check-vcs-no-git`
- `check-vcs-no-sha1`
<!-- LINT-GATES-END -->

(`check-consensus-parity` [E13, the parity mechanism — see
`docs/CONSENSUS_PARITY_DOCTRINE.md`], `check-no-new-repair-rung`, and
`check-stage-advances-or-blocks` appear in the canonical block and run in `make
lint`; they are documented in their own docs rather than expanded here.)

---

## 8. Lint-override discipline — every escape hatch is named

Several lint gates accept an inline override marker when the rule cannot
mechanically hold:

| Marker | Where allowed | Lint gate |
|--------|---------------|-----------|
| `// obs-ok:<tag>` | line with `fprintf(stderr, …)` whose nearby code emits no event / does not terminally propagate | `check-observability-pairing` |
| `// raw-sql-ok:<tag>` | line with `sqlite3_step(…)` outside the `AR_STEP_*` wrappers | `check-raw-sqlite` |
| `// raw-return-ok:<tag>` | bare `return -1;` in MCP/service/controller code with no preceding log line | `check-silent-errors`, `-services`, `-controllers` |
| `// raw-alloc-ok:<tag>` | line with `malloc/calloc/realloc` outside the `zcl_*` wrappers | `check-raw-malloc` |
| `// long-function-ok:<tag>` | signature line of a controller/service function whose body spans >500 lines | `check-long-functions` |
| `// lib-layer-ok:<tag>` | line in `lib/` that includes a `controllers/`, `models/`, `services/`, or `views/` header | `check-lib-layering` |
| `// domain-purity-ok:<tag>` | line in `domain/` that includes an app/ shape or an unlisted lib/ subsystem header | `check-domain-purity` |
| `// supervisor-ok:<tag>` | any line in a long-running `app/services/src/*_service.c` that intentionally does not register a supervisor liveness contract | `check-supervisor-registration` |
| `// supervised:<child>` | a `thread_registry_spawn(` call site whose thread IS on the supervisor tree; names the watching contract | `check-thread-supervision` |
| `// thread-supervision-ok:<reason>` | a `thread_registry_spawn(` call site for a bounded/joined/one-shot thread that needs no liveness contract | `check-thread-supervision` |
| `// one-result-type-ok:<tag>` | top of an `app/services/src/*.c` that owns no fallible service surface (pure table/registry helper) | `check-one-result-type` |
| `// one-write-path-ok:<tag>` | chain-state compatibility wrapper that is not a second consensus writer | `check-one-write-path` |
| `// shape-include-ok:<tag>` | any line in a shape file (condition/model/supervisor) that is a genuine registry/aggregator and cannot include the shape header | `check-shape-includes-header` |
| `// projection-cache-ok:<tag>` | line in a `*_projection.c` with a legitimate cache write outside the strict fold | `check-projections-pure` |
| `// ram-state-ok:<tag>` | line with derived active-chain cache state that must stay non-authoritative | `check-no-authoritative-ram-state` |

**Syntax (machine-enforced).** Every marker requires a non-empty single-token
tag matching `[A-Za-z][A-Za-z0-9_-]+` immediately after the colon. The
space-after-colon form (`// raw-sql-ok: state-kv …`) and the bare form
(`// raw-alloc-ok`) are rejected — hyphen-join multi-word tags instead.

**Pairing rule.** A marker is a promise that the override is either:

1. **Logged at this site or nearby** — the diagnostic is already observable
   (LOG_FAIL above, fprintf on the previous line, the caller logs on receiving
   the propagated failure).
2. **Structurally safe by design** — qsort comparator, void-returning helper,
   pre-boot sentinel, build-time tool, test fixture.

If neither holds, the marker is a bug. Delete it, fix the underlying issue
(route through `AR_BEGIN_SAVE`, add `LOG_FAIL`, switch to `zcl_malloc`), and let
the lint go green naturally.

**Prefer reusable tags that name a structural property over one-off labels.**
`:debug` and `:operator` say nothing; `:helper-context-logged` and
`:bin-parser-bounds` describe a recognizable class of safe call sites. Reuse an
existing tag when the pattern matches; singleton tags survive only when they
name a genuinely unique structural property
(e.g. `fatal-true-triggers-rollback-and-partial-write-return`).

**Concrete tag taxonomy (existing usage at wave-7c):**

- `obs-ok:` — `pre-existing-diagnostic`, `helper-context-logged`,
  `helper-return-path`, `paired-with-return-false-below`,
  `paired-with-event_emitf-below`, `warning-only-on-best-effort-path`,
  `crash-dump-banner`.
- `raw-sql-ok:` — `progress-kv-kernel-store` (reducer `progress.kv` cursor +
  `*_log` tables, the kernel store below AR — see §1), `kernel-primitive`
  (inside `progress_store.c` itself), `kv-state-primitive`,
  `read-only-introspection`, `state-kv-write-caller-handles-rc`,
  `cvs-zcl-ar-raw-sql-rationale`, `test-fixture-setup`, `test-fixture-verify`,
  `standalone-dev-tool`.
- `raw-return-ok:` — `qsort-comparator`, `logged-above`, `sentinel`,
  `bin-parser-bounds`, `sentinel-no-compile-time-windows`.
- `raw-alloc-ok:` — `test-fixture`, `standalone-dev-tool`,
  `db-service-owns-heap-job`.
- `long-function-ok:` — `legacy-import-state-machine`.

Implementation: `tools/check_observability_pairing.c`,
`tools/scripts/check_raw_sqlite.sh`, `tools/scripts/check_raw_malloc.sh`,
`tools/scripts/check_long_functions.sh`, and the inline `check-silent-errors*`
recipes in `Makefile:1481+`.

---

## Summary: How agents learn to follow the Rails way

1. **Compiler errors** for raw `sqlite3_step` (unless opted out).
2. **Type system** forces `struct zcl_result` with a message on failure.
3. **CI lint** catches raw malloc, silent returns, missing error bodies,
   long-function bloat.
4. **Macros** make the right thing easier than the wrong thing.
5. **Before/after hooks** wired by default — agents see the pattern and follow it.
6. **This document** in `docs/` — agents read it on `cat docs/DEFENSIVE_CODING.md`.

The Rails philosophy isn't "write good code." It's "make it harder to write bad
code than good code." These patterns achieve that in C23.
