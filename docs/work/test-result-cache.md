# Content-addressed per-group test result cache

Bazel-style caching for `make test-parallel`: a test **group** is returned from
cache — never forked — when its exact transitive **input closure** is
byte-identical to the last time that group passed. Editing one leaf re-runs only
the handful of groups downstream of it; every unrelated group is a ~0-time cache
hit.

## The invariant: the canonical gate runs COLD

The cache is **OFF by default**. `make test-parallel` with no flags/env is
byte-for-byte the historical runner: it forks and runs every group, prints
`ALL TESTS PASSED`, and returns `failed_groups == 0 ? 0 : 1`. A cached SKIP can
therefore never gate a push. **The push gate is always cold; the cache only
accelerates the inner dev loop.**

## Using it (inner dev loop)

```bash
ZCL_TEST_CACHE=1 make test-parallel   # opt in: skip unchanged groups, store passes
make test-parallel                    # default: cold, runs everything
make test-parallel TEST_PARALLEL_ARGS=--no-cache   # force cold even if ZCL_TEST_CACHE set
```

The final summary gains one line: `cached N / ran M`.

Inspect what a group keys on (the operator/proof lens):

```bash
ZCL_TEST_CACHE_DUMP=test_hkdf_sha256_rfc5869 <test_parallel binary>
# prints the toolchain key, the content key, cacheable=yes/no, and the closure
```

## How the key is computed

For group `test_<x>` the key is `SHA3-256` over, in order: a domain tag; the
compiled-in **toolchain fingerprint** (`BUILD_COMPILER_ID`, a compiler/flags
change busts the whole cache); the group name; and, for every file in the
group's **forward (callee) input closure** sorted by path, the file's path and
its `SHA3-256` content hash.

The forward closure is `codeindex_forward_closure()`
(`lib/codeindex/src/codeindex_impact.c`) — the mirror of the `code impact`
reverse-closure engine. From the entry symbol it walks the callee call graph and
collects every in-tree file that **defines** a reachable symbol, plus every
in-tree header those files include (compiler depfile edges). That is exactly the
set of bytes whose change can alter the group's verdict. A stored `PASS` record
addressed by the key lives in the `.zvcs` object store
(`vcs_object_put_addressed`); only `PASS` is ever stored.

## Soundness: a cached SKIP is provably equivalent to a fresh PASS

A group is **UNCACHEABLE (always runs)** when its inputs cannot be bounded:

- the forward closure came back `truncated` (a cap / fan-out / depth limit),
- the entry symbol does not resolve in the code index,
- the group is on the **external-input denylist** — it reads fixtures, the live
  node DB, an external `zclassicd`, `~/.zcash-params`, built binary artifacts, or
  a legacy datadir, i.e. inputs outside its source closure
  (`group_reads_external_inputs()` in `lib/test/src/testcache.c`).

The one residual assumption is the standard one for any source-based
"affected-tests" analysis: the call graph captures a test's dependency edges by
name (an indirect/function-pointer/dlopen edge is invisible to source scanning,
exactly as it is to `code impact`). That assumption is backed by two nets:

1. **Default OFF** — the canonical gate is cold, so the cache never gates a push.
2. **`--cold-audit`** — runs *every* group fresh (cache disabled for execution)
   and asserts that every group carrying a stored `PASS` at its current key also
   passes the fresh run. A divergence is a closure/cache soundness bug and fails
   the run loudly:

   ```bash
   ZCL_TEST_CACHE=1 make test-parallel                        # populate
   make test-parallel TEST_PARALLEL_ARGS=--cold-audit         # verify: 0 divergences
   ```

## Files

- `lib/codeindex/src/codeindex_impact.c` — `codeindex_forward_closure()`.
- `lib/test/src/testcache.c` + `lib/test/include/test/testcache.h` — the cache.
- `lib/test/src/test_parallel.c` — dispatch wiring, `--cache/--no-cache/--cold-audit`.
- `lib/test/src/test_testcache.c` — the cache's own contract test group.
- `Makefile` — bakes `BUILD_COMPILER_ID` into `testcache.o` as the toolchain key.
