# Contributing to ZClassic23

## Prerequisites

- **gcc 14+** (or a clang with working `-std=c23` support) and GNU make.
- For the one-time vendored-library build: `cmake`, `autoconf`, an autotools
  toolchain, `curl` or `wget`, `unzip`, and `sha256sum`.
- Vendored static libraries under `vendor/lib/` are built locally by
  `make vendor` from pinned source tarballs and SHA-256 hashes. A fresh clone
  links without manual archive copying; see [`docs/BUILD.md`](../docs/BUILD.md)
  for each dependency's source, version, and build notes.

## Build and test

```bash
make                     # test_zcl + zclassic23 + zclassic-cli
make vendor              # build missing vendor/lib archives from pinned sources
make build-only          # compile check, no final link
make dev-bin             # fast non-LTO local node binary: build/bin/zclassic23-dev
make t-fast ONLY=<group> # one fast test group, e.g. make t-fast ONLY=service_state_driver
make fast-ci             # cache-aware lint/build/focused-test agent loop
make test                # full test suite
make lint                # defensive-coding gates (must be clean)
make ci                  # local full gate: lint + tests + MVP slices + fuzz where available
```

`make dev-bin` is the normal way to get a changed local node/agent executable
without paying release LTO. `make t-fast ONLY=<group>` is the normal test inner
loop: it rebuilds the fast harness and runs only the matching group(s). `make
fast-ci` adds cache-aware lint/build/focused-test selection from changed files.
Never run the full `test_zcl` binary in the inner loop.

## The defensive-coding contract

[`docs/DEFENSIVE_CODING.md`](../docs/DEFENSIVE_CODING.md) is mandatory
reading before writing any code. The rules are enforced by the compiler
and by the `make lint` gates (`make ci` runs lint before tests), not by review
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
`make t-fast ONLY=<name>`.

## Pull requests

Before opening a PR:

1. `make lint` — clean, no new gate violations or baseline regressions.
2. `make t-fast ONLY=<group>` for focused groups you touched.
3. `make test` for broad shared behavior or before release-sized changes.

CI runs on the maintainers' own servers (`make ci` — lint + full suite),
not on GitHub Actions; maintainers run the full gate on every PR before
merging, so a PR that fails lint or tests will not merge. Keep commits
honest about what is proven versus scaffolding — the project documents
incomplete subsystems as incomplete, and PRs are expected to do the same.

## Consensus parity is inviolable

zclassic23 stays bit-for-bit consensus-compatible with `zclassicd`. A PR that
changes consensus (Equihash params, activation heights, block/tx validity) is
**declined on principle** — even if framed as opt-in, miner-signaled, or a
"sidegrade" — because a consensus change must never ship to zclassic23 first. We
will thank you, credit the idea, and decline the change (we may reimplement the
*non-consensus* part ourselves). Non-consensus PRs are judged purely on merit.
Enforced by the `check-consensus-parity` lint gate + the `test_consensus_parity`
golden values; full policy in
[`docs/CONSENSUS_PARITY_DOCTRINE.md`](../docs/CONSENSUS_PARITY_DOCTRINE.md).

## Licensing of contributions

ZClassic23 is licensed under the **Apache License 2.0**
([`LICENSE`](../LICENSE)), and contributions are accepted on
**inbound = outbound** terms:

- **By submitting a pull request, issue patch, or other contribution,
  you agree that your contribution is licensed under the Apache License
  2.0**, the same license as the project. (This is also the default
  under [GitHub's Terms of Service §D.6](https://docs.github.com/en/site-policy/github-terms/github-terms-of-service#6-contributions-under-repository-license);
  we state it explicitly so there is no ambiguity later.)
- You confirm you have the right to submit the work — it is your own,
  or you are authorized to contribute it under these terms.
- A `Signed-off-by` line (`git commit -s`, [DCO 1.1](https://developercertificate.org/))
  is welcome and encouraged, but a submitted PR constitutes agreement
  either way.

**Third-party code:** new original work defaults to Apache-2.0. Vendored
or ported third-party code under other **permissive** licenses (MIT,
BSD-2/3-Clause, ISC, Zlib, Blue Oak 1.0.0, Apache-2.0) is acceptable —
preserve the upstream copyright notice in [`NOTICE`](../NOTICE) and
credit the source in [`docs/ATTRIBUTIONS.md`](../docs/ATTRIBUTIONS.md).
Copyleft code (GPL/LGPL/AGPL/MPL) cannot be accepted into this tree.

**Attribution:** contributor authorship is preserved in git history —
PRs are merged with merge commits, never rewritten under someone else's
name — and contributors are recognized in
[`CONTRIBUTORS.md`](../CONTRIBUTORS.md) and GitHub's contributor graph.
