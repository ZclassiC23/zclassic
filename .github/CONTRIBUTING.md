# Contributing to ZClassic23

## Prerequisites

- **gcc 14+** (or a clang with working `-std=c23` support) and GNU make.
- Vendored static libraries under `vendor/lib/` (LevelDB, SQLite, libevent,
  OpenSSL, zlib, Tor). Only `libsecp256k1.a` is tracked in-tree today; the
  remaining archives must currently be built locally and dropped into
  `vendor/lib/`. Expect friction building outside the maintainer's
  environment for now — this is a known gap.

## Build and test

```bash
make                     # test_zcl + zclassic23 + zclassic-cli
make test                # full test suite
make lint                # defensive-coding gates (must be clean)
make t ONLY=<group>      # one parallel test group, e.g. make t ONLY=service_state_driver
make ci                  # lint + full suite — what CI runs on every push
```

`make t ONLY=<group>` is the inner loop: it rebuilds the harness and runs
only the matching group(s) of the parallel suite. Never run the full
`test_zcl` binary in the inner loop.

## The defensive-coding contract

[`docs/DEFENSIVE_CODING.md`](../docs/DEFENSIVE_CODING.md) is mandatory
reading before writing any code. The rules are enforced by the compiler
and by the `make lint` gates (CI runs lint before tests), not by review
goodwill. The core of the contract:

- **Every `node.db` write goes through the ActiveRecord lifecycle** —
  `AR_BEGIN_SAVE` + `AR_FINISH_SAVE`, or the combined `AR_ADHOC_SAVE`
  (locally-prepared statement) / `AR_CACHED_SAVE` (cached statement).
  Raw `sqlite3_step()` in application code is a compile error
  (`-DZCL_AR_ENFORCE`); all three macros run the same
  `validate_*` → `before_save` → `after_save` hook chain.
- **Every error return logs context** — use `LOG_FAIL()`, `LOG_ERR()`,
  `LOG_NULL()` from `util/log_macros.h`. Never `return false;` silently.
- **Every allocation is checked** — use `zcl_malloc(size, "label")` from
  `util/safe_alloc.h`, never bare `malloc`.
- **Every MCP handler sets an error body** — never `return -1;` without
  explaining why.
- **Long-running loops register with the supervisor liveness tree**
  (`lib/util/include/util/supervisor.h`), so a stalled loop is detected
  instead of silently hanging.

Files under `app/` must live in exactly one of the eight shape folders
(`models/`, `views/`, `controllers/`, `services/`, `jobs/`, `conditions/`,
`events/`, `supervisors/`) — lint-enforced; see
[`docs/FRAMEWORK.md`](../docs/FRAMEWORK.md).

## Adding a test

Add `lib/test/src/test_<name>.c` and register its group in the
`TEST_LIST` X-macro in `lib/test/src/test_parallel.c`. Run it with
`make t ONLY=<name>`.

## Pull requests

Before opening a PR:

1. `make lint` — clean, no new gate violations or baseline regressions.
2. `make test` (or at minimum the groups you touched, then let CI run
   the rest) — green.

CI gates every push on `make ci`, so a PR that fails lint or tests will
not merge. Keep commits honest about what is proven versus scaffolding —
the project documents incomplete subsystems as incomplete, and PRs are
expected to do the same.
