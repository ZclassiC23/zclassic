# Building zclassic23

`zclassic23` is one whole-program C23 binary. The build is a single `cc` over
~660–1400 `.c` files with LTO, linked against a set of **static** third-party
archives in `vendor/lib/`.

## One command: `make vendor`

The static third-party archives in `vendor/lib/` are **built from source** by
`make vendor`. Only `vendor/lib/libsecp256k1.a` (a custom Bitcoin Core fork
build) is committed to git; everything else is produced locally from
SHA256-pinned sources. A fresh clone links in one shot:

```bash
git clone https://github.com/ZclassiC23/zclassic.git && cd zclassic
make zclassic23     # auto-runs `make vendor` if vendor/lib/ archives are absent
```

`make zclassic23` first crosses a Make restart barrier when vendor archives are
missing. The first parse invokes `tools/scripts/build_vendor.sh`; the restarted
parse then captures source identity from the final archive bytes. Link targets
also retain the archives as order-only prerequisites. This prevents a fresh
clone from baking an identity that omitted inputs generated later in the same
build. To build them explicitly:

```bash
make vendor         # build every missing vendor/lib/*.a (idempotent — no-op if present)
make vendor-force   # rebuild all of them from scratch
tools/scripts/build_vendor.sh libz.a libsqlite3.a   # a subset
```

The exact source identity also recursively covers generated headers under
`vendor/include/`, including ignored SQLite, OpenSSL, and zlib headers used by
the global include path. Unsupported file types and symlinks there fail closed.
The generated wallet-template and explorer-CSS headers use the same ordering
rule: an included-Makefile barrier regenerates stale view outputs and restarts
parsing before the source record is captured.

`make build-only` (compile every `.o`, no link) does not need the archives and
is the fastest way to confirm a clean checkout compiles.

## Fast development binary

Release builds intentionally use one whole-program LTO link. For day-to-day C
development, use the non-release dev binary instead:

```bash
make dev-watch
make agent-loop
make fast-rebuild
make agent-index
make dev-loop-bench
build/bin/zclassic23 discover help          # enumerate native commands
build/bin/zclassic23 status                 # local node status
build/bin/zclassic23-dev status             # dev-lane status
make agent-doctor
make agent-dev-status
build/bin/zclassic23-dev agentdevstatus
build/bin/zclassic23-dev agentbuild
```

`make dev-watch` is the save-driven AI/operator loop. It debounces and
coalesces changed files, asks the shared `agentimpact` rules for the smallest
focused verification set, and defaults to verify-only mode:

- `MODE=verify` (the default) runs `make ff` and records a verdict without
  linking, hot-swapping, or activating a runtime generation.
- `MODE=auto`, `MODE=apply`, `MODE=hotswap`, `MODE=reload`, and `MODE=stage`
  are Phase-0 contained and fail before any runtime publication or staging.
  They do not fall back to another activation path.
- `MODE=check` runs classification and focused checks without activation.

Only `verify` and `check` are public watcher modes today. The historical
activation machinery remains in-tree for tests and completion of the unified
transaction, but it is not an operator authority. Watch-command override
environment variables are accepted only by the hermetic `--self-test` path.

Each attempted save writes one authoritative `zcl.dev_cycle.v1` record under
`~/.local/state/zclassic23-dev/cycles/`, atomically refreshes
`latest-cycle.json`, and updates the watcher heartbeat. The record contains the
changed files, impact plan, selected path/reason, per-phase timings, candidate,
running, and last-good generations, test/probe results, rollback result, a
failure capsule, resident hot-swap response/provenance when applicable, and one
executable `agent_next_action`. `make dev-watch` uses
`inotifywait` when available and otherwise retains a stock-toolchain polling
fallback. `make dev-watch-selftest` is the deterministic, node-free contract
test.

`make dev-activation-selftest` exercises the otherwise unreachable activation
machinery only inside a mode-0700 `/tmp` fixture. The deploy script requires a
mode-0600 sentinel through a bound inherited file descriptor, exact
fixture-local paths, an inert candidate, and allowlisted fake service commands.
No public environment variable can enable this mode or target the real dev
HOME/unit. The retained generation manifest, candidate preflight, post-restart
probe, recovery plan, and systemd intent all bind the baked 64-hex
`source_id_sha256`; `build_commit` is optional GitHub trace metadata and never
a freshness or activation decision.

`make agent-dev-recover` is likewise read-only planning. Public `ARGS=--apply`
and direct `recover-dev-lane.sh --apply` refuse before generation relinking,
datadir replacement, or service commands. `make dev-recovery-selftest` proves
the retained transaction only through its own inherited-FD, fixture-bound
capability and fake service-command allowlist.

The registry-owned C23 development plane is the preferred direct interface:

```bash
build/bin/zclassic23-dev dev loop ensure \
  --input='{"root":"/home/rhett/github/zclassic23","mode":"verify"}'
build/bin/zclassic23-dev dev loop status
build/bin/zclassic23-dev dev loop wait \
  --input='{"after_epoch":0,"timeout_ms":30000}'
make hotswap-so FILES=app/controllers/src/status_native_handlers.c
make t ONLY=hotswap_loader
make hotswap-sim
```

`ensure` is singleton/idempotent, defaults to `mode:"verify"`, and returns the
watcher ID used by `dev loop stop`. Changing an active watcher's mode requires
stopping it first. The dev executable defaults this confined command tree to the
`dev` operator lane, so the documented commands work without an environment
prefix; an explicit canonical or soak lane still fails closed. The native
cycle returns one bounded verdict after classify, proof, and build. Attempts to
select `auto`/`apply`, call `dev.change.apply`, or publish through hot-swap
return a structured containment refusal. The shell watcher remains a
verify/check compatibility surface.

The intended process-reload transaction is content-addressed under
`~/.local/lib/zclassic23-dev/<generation>/`. Candidate build identity, native
command catalog, and `ops selftest` are checked before the old process is
disturbed. Activation takes a nonblocking lock and flips atomic `current` and
`last-good` links. The bounded warm probe verifies the exact `/proc` executable,
RPC, agent/operator contracts, and native-registry health. A failed candidate is
quarantined; the activator restores `last-good`, restarts once, and verifies the
recovery. Canonical and soak services, ports, and datadirs are rejected by the
dev-lane guards. During Phase-0, `make deploy-dev`, `make deploy-dev-fast`,
`make agent-deploy-fast`, `make agent-stage-dev`, and the direct deploy/hot-swap
scripts hard-refuse unconditionally. A caller-supplied source identity cannot
re-enable them. `dev.vcs.revert` remains source-only with
`relink_generation=false`; `true` refuses before the source revert. This does
not relax release, consensus, or full-suite gates.

The native cycle no longer derives proof authority from a HEAD-relative Git
dirty set. Its exact SHA-256 source epoch supplies supersession CAS; until a
signed prior epoch can authenticate a content diff, every nonempty save event
is conservatively routed through the reload/parity verification lane. This may
cost more local CPU, but a path hint cannot downgrade the proof or authorize
publication.

`make agent-loop` remains the manual one-shot loop. It runs the cache-aware fast
checks; set `ZCL_AGENT_LOOP_BIN=1` to also link the local dev binary.
`ZCL_AGENT_LOOP_DEPLOY=stage|dev` reaches a contained backend and cannot stage
or reload the dev lane.
Native commands (`zclassic23 status`, `zclassic23 dumpstate <subsystem>`,
`zclassic23 discover help`) are the agent interface. Use
`build/bin/zclassic23-dev <leaf>` when checking the current development binary
without paying the release LTO link.
`make agent-doctor` is the no-build combined development check: build binary,
dev-lane status, recent focused-test failure hints, dirty-file count, and the
next safe command. Use `make agent-doctor ARGS=--json` for
`zcl.agent_doctor.v1`.
`make agent-dev-status` is the no-build read-only lane check: it reports the
explicit `worker_lane` contract (`role=worker`,
`mutation_policy=noncanonical_dev_only`, and
`canonical_guard=never_touches_live_or_soak`), source/staged binaries, linger
service PID, RPC readiness, the current/running/last-good/staged generations,
activation lock, rejected generations, rollback availability, saved deploy
state, current cycle and watcher heartbeat, background-quality freshness,
latency-SLO status, auto-reindex marker, deploy blocker/reason, stale-marker
candidate, and next safe action. Use
`make agent-dev-status ARGS=--json` or `build/bin/zclassic23-dev status` for a
machine-readable status form.
When that status reports `auto_reindex_stale_candidate=true`, run
`make agent-clear-stale-dev-reindex`; it archives the dev-lane marker only after
the dev RPC serves at or above the marker anchor, and never touches canonical or
soak.
No busy-service path stages a generation during containment. `make
agent-stage-dev` refuses; build and preflight must remain non-publishing until
the complete transaction is implemented.

Foreground candidate preflight uses `build/bin/zclassic23 ops selftest`. It
validates every native leaf and generated input schema inside the candidate
without depending on the health of the process being replaced. The exhaustive
handler-dispatch self-test remains a background/live diagnostic.

`make agent-index` generates root `compile_commands.json` by dry-running the
real `DEV_OBJS` recipes. It therefore preserves generated-header prerequisites,
the exact C23 flags, compiler/cache wrapper, and the normal `-Og` versus hot
consensus/crypto/script/validation `-O2` split. It atomically records hash and
freshness metadata under `.cache/zcl-agent-index/`; clangd is an optional
consumer, not a build requirement. `zclassic23 agentbuild` embeds the current
indexing status and an executable refresh command.

`make dev-loop-bench` runs controlled no-op, controller, service, header,
hot-swap, and process-reload cases and writes `zcl.dev_loop_bench.v1` with raw
samples plus p50/p95 values. Hot-swap and reload are skipped unless the operator
explicitly opts in with `ZCL_DEV_BENCH_ACTIVATE=1`, which measures the armed
dev-lane hot-swap activate path, so an SLO is never claimed from build-only
timings. `zclassic23 agentbuild` also exposes the latest benchmark status.

In-process generation v2 is deliberately narrower than process reload. Its
loader, simulator, and build paths remain available, but resident probing and
every commit/publication entry point are Phase-0 contained. The intended mechanism
admits only manifest-eligible, stateless native leaf providers; it stages the
whole handler batch, validates ABI/capabilities/provenance, runs the generation
self-test, and atomically publishes one resident registry snapshot. REST,
diagnostics, services, models, storage, events, conditions, supervisors,
network/wallet/crypto state, reducers, consensus, and bootstrap ownership remain
`reload_required`. If publication is re-enabled after the transactional gates
land, a committed hot-swap will remain process-local and disappear on restart;
durable convergence must use the same receipt-bound activation transaction.
No watcher currently schedules or stages a post-swap generation. Inspect any
pre-containment provenance with
`zclassic23 dumpstate hotswap` (`zcl.hotswap_generation.v2`). The resident
mutation RPC (`dev_hotswap_native`) is dev-build only — release, canonical,
and soak nodes never register it — and whole-generation publication entry
points stay Phase-0 contained. Build and verify without resident
loading:

```bash
make hotswap-so FILES=app/controllers/src/status_native_handlers.c
make t ONLY=hotswap_loader
make hotswap-sim
```

`make hotswap` and
`tools/dev/hotswap-running-dev.sh` always
refuse and never call the loader. The live runtime surface is the single-leaf
module loop: `make hotswap-try HANDLER=<leaf> ARGS="<cmd>"` rebuilds one
swappable leaf's module `.so` and runs the command in a one-shot CLI with
`ZCL_HOTSWAP_PRELOAD` against the dev lane; `make hotswap-apply HANDLER=<leaf>`
commits the override in the running `zcl23-dev` node, gated on
`-hotswap-activate` + `ZCL_HOTSWAP_ACTIVATE=1` + the exact dev datadir
(canonical refused). Only the six read-only leaves on
`config/hotswap_swappable.def` are eligible. Run
`make hotswap-sim` for the focused deterministic simulated-network proof, and
`make sim-fast` for the broader checked-in scenarios plus seeded replay sweep.
Each successful mapping keeps both its code mapping and the exact artifact
descriptor pinned for the process lifetime. This prevents `/proc/self/fd/N`
loader-cache aliasing when several different providers are swapped in
sequence; `zclassic23 dumpstate hotswap` reports
`artifact_inode_pinned=true` for every accepted generation.

`make fast-rebuild` is an alias for the local dev binary (`make dev-bin`). It
writes per-file objects under
`build/dev-obj/epochs/<compile-epoch>/`, links the exact candidate under
`build/bin/dev/epochs/<compile-epoch>/`, then atomically refreshes the familiar
`build/bin/zclassic23-dev` alias. It links without LTO, keeps symbols, and
defaults most code to
`ZCL_DEV_OPT=-Og` while compiling consensus/crypto/script/validation hot paths
at `ZCL_DEV_HOT_OPT=-O2`; both are overrideable. The link step auto-selects
`mold` or `ld.lld` through `ZCL_DEV_LINKER` when available; set
`ZCL_DEV_LINKER=` to force the platform linker. When `ccache` is installed, the
Makefile automatically wraps `CC` with it for rebuild speed; set
`ZCL_USE_CCACHE=0` to opt out.

For the absolute cheapest edit check, run `make fast-changed-compile`. It
uses changed paths only as classification hints and always resolves the complete
current dev source inventory through `make fast-compile`. Any source mutation
selects a new compile epoch; `ccache`/`sccache` recovers unchanged translation
units without reusing an object from a different source epoch.

The host-local compile-epoch key binds the exact source SHA-256 and capture
completeness, mutation token, compiler/toolchain fingerprint, profile, and
effective compile/link flags. Per-TU object and depfile publication, candidate
linking, and stable-alias publication are atomic. Final publication re-verifies
the complete source/compiler/session record, so concurrent builds and an
A→B→A edit/revert cannot publish a candidate from the wrong epoch.
Epoch retention is bounded by `BUILD_EPOCH_KEEP` (default `3`). Lease
acquisition prunes older inactive object trees and their matching candidates,
but a lease is live only when both its `/proc` PID and process start tick still
match; a concurrent Make epoch is therefore never garbage-collected.

This binary is for local AI/operator iteration only. `make zclassic23`,
`make deploy`, reproducible builds, and releases continue to use
`build/bin/zclassic23` with the release flag profile.

## Cached full test suite (`test_parallel`)

`build/bin/test_parallel` — the binary `make t`, `make test`, and `make ci`
run — is built from a **cached per-TU object tree**
(`build/test-rel-obj/epochs/<compile-epoch>/`),
not one whole-program `cc`. Each source is addressed under
`build/test-rel-obj/epochs/<compile-epoch>/` with `-MD -MP` depfiles, and the
exact candidate is linked under `build/bin/test-strict/epochs/`. Consequences:

- **Every edit gets a fresh immutable epoch.** Make resolves every current
  source in that epoch; compiler-cache hits recover unchanged TU work before
  one plain link. A no-edit invocation reuses the exact verified epoch.
- **Header/`.def` and system-header inputs are tracked.** The retired
  whole-program rule listed only `.c` files as prerequisites, so a header-only
  edit did **not** rebuild `test_parallel` at all. Complete depfiles plus the
  epoch key close that false-green path.
- **`ccache` makes it cacheable.** A giant multi-source `cc` invocation cannot
  be cached; per-TU `.o` compiles hit the cache, so a clean object tree with a
  warm cache rebuilds in a few seconds. `ccache` stays optional (auto-detected
  via `ZCL_USE_CCACHE`); everything works without it, just slower on the first
  build.

**Flag profile.** The cached objects use the identical release flags of the old
whole-program `test_parallel` (`-O3 -Werror -pedantic`, the hardening flags,
`-DZCL_TESTING`) with two documented, semantics-neutral deltas: `-flto=auto` is
dropped (LTO is a link-time whole-program optimization — caching per-TU GIMPLE
would still force the slow whole-program codegen at every link; dropping it lets
each TU be cached and code-generated independently), and the `-O3`+FORTIFY
heuristic-warning family (`-Wformat-truncation`/`-overflow`, `-Warray-bounds`,
`-Wstringop-truncation`/`-overread`, `-Wrestrict`, `-Wnonnull`,
`-Wmaybe-uninitialized`) is `-Wno`'d — those fire only once real per-TU codegen
runs at `-O3`, and no other build in the tree enforces them (release and
`build-only` defer codegen to the LTO link; `test_parallel_fast` runs at `-O1`),
so excluding them keeps the enforced warning set a superset-or-equal of the
retired monolith's. Neither delta can change test behavior.

**Whole-program variant for debugging.** `make test_parallel_wpo` still builds
the original monolithic whole-program LTO binary at
`build/bin/test_parallel_wpo`. Use it to rule out any per-TU-vs-LTO divergence
if a test ever behaves differently between the two (it should not). `test_zcl`
(the serial runner) also remains a whole-program build.

**Fast inner-loop variant.** `make t-fast ONLY=<group>` uses the separate
exact candidate and object tree (`build/bin/test-fast/epochs/<compile-epoch>/`
and `build/test-obj/epochs/<compile-epoch>/`, `-O1`, non-`-Werror`) for the
tightest edit loop; run strict `make t` / `make test` before commit.

## Sanitizer profiles (opt-in)

Two ASan/UBSan profiles extend the fuzz-only sanitizer coverage to the test
suite and the dev node. Both compile with
`-fsanitize=address,undefined -fno-omit-frame-pointer` (plus
`-fno-sanitize=alignment`, mirroring the fuzz harnesses' established UBSan
profile), no LTO, into their own epoch-keyed object trees — the flags are
referenced only by these profiles and can never leak into the
release/dev/test default builds.

- **`make t-asan ONLY=<group>`** — one test group under the instrumented
  harness (`build/bin/test-asan`, object tree `build/test-asan-obj/`).
  ASan aborts the failing child, so a memory error is a red group with the
  full report in its replayed log; UBSan stays in gcc's recover-and-continue
  default so one run collects every finding (export
  `UBSAN_OPTIONS=halt_on_error=1` to make reports fatal). Findings are the
  point — fix forward, don't suppress.
- **`make asan-ci`** — opt-in smoke: a small set of fast, params-free groups
  under test-asan with `UBSAN_OPTIONS=halt_on_error=1` so any report fails
  the run. Deliberately **not** wired into `make ci` (instrumented runs are
  several times slower and push times must stay stable). Override the set
  with `ASAN_CI_GROUPS="..."`.
- **`make dev-asan`** — the dev node under ASan/UBSan
  (`build/bin/zclassic23-dev-asan`, `-Og`, non-LTO, object tree
  `build/dev-asan-obj/`). For local memory/UB debugging on a scratch
  datadir; boot with `ASAN_OPTIONS=detect_leaks=0` until leak triage is
  done.

The test runners set a large **finite** stack limit (1 GiB) rather than the
usual `ulimit -s unlimited`: ASan + PIE with an unlimited stack
intermittently aborts at startup with "Shadow memory range interleaves with
an existing memory mapping" (google/sanitizers#856).

**Known-good as of 2026-07-18 — UBSan "left shift" in `lib/crypto/src/ed25519.c`
and `curve25519.c`:** a `make t-asan` pass flags UBSan shift-base reports at
`ed25519.c:81`/`:304` and `curve25519.c:50` — the TweetNaCl `int64_t`
carry-propagation idiom (`o[i] -= c << 16;`), never the ref10
sign-bit-into-`<<24` pattern. The left operand is legitimately negative (gf
limbs are signed by design), so a pre-C23 compiler calls this UB; the project
builds `-std=c23`, which redefines signed left shift as modular (the C++20
rule), so it is no longer UB at the language level even though gcc's UBSan
instrumentation still flags it under the old rule. Not a live miscompilation:
both expressions lower to a plain `shlq`, identical to the C23-defined
result, and the reachable magnitudes (`|c| <= 2^47`, `|carry| << 2^24`) never
approach overflow. Fix (not yet applied — Ed25519 is consensus-adjacent per
`docs/CONSENSUS_PARITY_DOCTRINE.md`, so it needs the full replay-canary bar,
not just green ASan): cast through `uint64_t` before the shift,
`(int64_t)((uint64_t)c << 16)` — bit-exact for all inputs (defined
conversions + a defined unsigned shift either side), same `shlq` codegen
(objdump-diff verifiable), silencing the sanitizer noise without touching
consensus-relevant math.

## ThreadSanitizer profiles (opt-in)

Two TSan profiles mirror the ASan ones above for data-race detection. Both
compile with `-fsanitize=thread -fno-omit-frame-pointer`, `-g`, **no LTO**,
into their own epoch-keyed object trees. LTO is deliberately off (beyond
mirroring every instrumented profile here): race reports need precise per-TU
PC/stack attribution, whole-program LTO inlining degrades exactly that, and
`-fsanitize=thread` under `-flto=auto` is a far less-traveled gcc path.
`-fsanitize=thread` is mutually exclusive with address/undefined, so these
are sibling profiles, not an extension of the ASan flag set. Vendored static
archives (OpenSSL/leveldb/libevent/rustzcash) are NOT rebuilt — TSan works
fine against uninstrumented libraries, with proportionally less coverage
inside them.

- **`make t-tsan ONLY=<group>`** — one test group under the
  thread-instrumented harness (`build/bin/test-tsan`, object tree
  `build/test-tsan-obj/`). TSan's default report-and-continue mode collects
  every race in one run, then exits the failing child with `exitcode=66`,
  so a group with reports surfaces red with the full stacks in its replayed
  log. Findings are the point — fix forward, don't suppress.
- **`make tsan-ci`** — opt-in smoke: a small set of fast, thread-spawning
  groups (supervisor / workpool / mailbox / parallel fold / parallel
  validation / net bootstrap / cpu topology) with
  `TSAN_OPTIONS=halt_on_error=1` so the first report fails the run.
  Deliberately **not** wired into `make ci` (instrumented runs are several
  times slower and push times must stay stable). Green as of the R1 fix
  below. Override the set with `TSAN_CI_GROUPS="..."`.
- **`make dev-tsan`** — the dev node under TSan
  (`build/bin/zclassic23-dev-tsan`, `-Og`, non-LTO, object tree
  `build/dev-tsan-obj/`). For local data-race debugging on a scratch
  datadir; race reports go to stderr.

`t-tsan` and `tsan-ci` both read `tools/tsan.supp` via
`TSAN_OPTIONS=suppressions=...`. Every active entry there must be confirmed
benign with a written justification — never suppress an untriaged report;
it ships with zero active suppressions (comments only) — the one race found
so far (below) was fixed in code, not hidden.

**Known-good as of 2026-07-18 — first TSan sweep, R1 fixed:** a ~23 s
thread-relevant subset of `test_parallel` (supervisor incl. its production
tree / workpool / mailbox / parallel range-fold / parallel validation
determinism / net bootstrap / cpu topology / net + peer-lifecycle / header
sync / chain-advance atomicity / connman locking / service-state-driver /
reducer-drive watchdog / sync-watchdog conditions, the 7 `tsan-ci` groups run
3x for timing sensitivity) found **one unique real race**: R1 —
`struct thread_liveness_child.id` (`lib/util/src/thread_liveness.c`)
published via a plain (non-atomic) store in `thread_liveness_register()`
while an already-spawned worker thread plain-reads it in
`thread_liveness_worker_alive()` — the documented
spawn-then-register contract, and exactly what production callers do
(`rpc_timeout_start_watchdog`, `heartbeat.c`, `metrics.c`). Formally a C11
data race; benign-looking on x86-64 (aligned 4-byte load/store can't tear,
so the worst pre-fix outcome was one skipped, self-healing liveness beat) but
real UB and a genuine missing-synchronization risk on weaker memory orders.
**Fixed** (`lane/fix-tsan-r1`, merged): `id` is now `_Atomic
supervisor_child_id`, release-stored after `supervisor_register()` completes
and acquire-loaded by every reader (beat, worker_alive/_exited,
stop_begin/_finish, retire, idempotent guards) — a worker observing a valid
id also observes the completed registry insertion. Verified:
`make t-tsan ONLY=test_supervisor` x3 -> 0 reports (was 3/4 flaky-red);
`make tsan-ci` green. This was a THIN baseline (short unit-style runs, small
race windows) — a full-suite TSan pass and a `dev-tsan` boot on a scratch
datadir are still open follow-ups, not yet run.

Both runners wrap the harness in `setarch -R` (ASLR off): TSan reserves
fixed shadow address ranges and the default-ASLR PIE/mmap placement
intermittently collides at startup (`FATAL: ThreadSanitizer: unexpected
memory mapping`). These are opt-in triage binaries, never release artifacts,
so no-ASLR is an acceptable trade.

## Prerequisites

- **gcc 14+** (or clang with working `-std=c23`) and **GNU make**.
- For `make vendor`: a C++11 compiler (`c++` or `g++`) for LevelDB, **`cargo`**
  + **`rustc`** for the canonical Sapling prover, optional **`cmake`** for the
  preferred LevelDB build path, **`autoconf`** + an
  autotools toolchain (libevent, zlib), **`curl`** or **`wget`**, **`unzip`**
  (SQLite amalgamation zip), **`patch`** (pinned libevent compatibility patch),
  and **`sha256sum`**.
- For the embedded Tor onion service (optional): the `vendor/tor` submodule
  (`git submodule update --init`). When that submodule is built, the Makefile
  links the real Tor; otherwise it links the in-tree `libtor_stub.a` that
  `make vendor` builds from `vendor/tor_stub.c`.

## Vendored archives

`make vendor` produces every archive below. **Source class** is either
*in-tree* (a source file already in the repo) or *fetched* (pulled from the
pinned URL, verified against the pinned SHA256 in
`tools/scripts/build_vendor.sh`, then built static). `./tools/dep_audit.sh`
(`make audit`) re-detects versions from the built archives and checks each
against its minimum-safe version.

| Archive | Upstream | Version | Source class | Origin |
|---|---|---|---|---|
| `libsecp256k1.a` *(committed)* | Bitcoin Core secp256k1 | fork | in git | https://github.com/bitcoin-core/secp256k1 |
| `libtor_stub.a` | Tor stub (`vendor/tor_stub.c`) | — | in-tree (built) | https://github.com/RhettCreighton/tor |
| `libcrypto.a`, `libssl.a` | OpenSSL | 3.0.16 | fetched + built | https://github.com/openssl/openssl |
| `libevent.a`, `libevent_openssl.a`, `libevent_pthreads.a` | libevent | 2.1.12 | fetched + built | https://github.com/libevent/libevent |
| `libleveldb.a` | LevelDB | 1.23 | fetched + built | https://github.com/google/leveldb |
| `libsqlite3.a` | SQLite (amalgamation) | 3.49.0 | fetched + built | https://www.sqlite.org/ |
| `libz.a` | zlib | 1.3.1 | fetched + built | https://github.com/madler/zlib |
| `librustzcash.a` | Zcash Sapling prover | `06da3b9ac8f2` | fetched + built | https://github.com/zcash/librustzcash |

That is 11 archives total (the 10 `make vendor` builds + the committed
`libsecp256k1.a`).

Notes:
- **OpenSSL pinned to 3.0.16** — the project's minimum-safe floor (the older
  vendored 3.0.13 was below it). `make audit` reports the version.
- **libevent 2.1.12** carries the pinned, digest-bound
  `vendor/patches/libevent-2.1.12-secure-rng-abi.patch`. It preserves the
  public `evutil_secure_rng_add_bytes` symbol required by embedded Tor on
  newer glibc systems where libevent otherwise omits it; the vendor builder
  asserts the symbol before installing the archive.
- **LevelDB 1.23** is built, while the committed `vendor/include/leveldb/*.h`
  headers are 1.18. That is intentional and safe: the repo uses only LevelDB's
  stable C API (`<leveldb/c.h>`), which is unchanged across 1.18→1.23, so the
  headers and the built library stay compatible. `cmake` is used when present;
  otherwise `tools/scripts/build_vendor.sh` builds the same static source set
  directly with a C++11 compiler and a generated POSIX `port_config.h`. Final
  node links still use `cc`; the Makefile asks `c++` for libstdc++'s directory
  and adds it to the linker search path so mixed distro compiler packages do
  not break cold remote builds.
- **SQLite 3.49.0** amalgamation; `make vendor` also refreshes
  `vendor/include/sqlite3.h` and `vendor/sqlite3.c` so the rest of the build
  (e.g. `tools/sqlq.c`) stays in sync.
- **librustzcash is proving-only.** It is the exact, SHA256-pinned revision
  used by the canonical ZClassic daemon and is linked statically behind the
  repository's C ABI. Sapling block/transaction verification stays in the
  independent C23 verifier. `Cargo.lock` pins registry checksums and the git
  dependency revision; build paths are remapped before the archive is linked.
- Downloads are cached under `vendor/.cache/` (gitignored); build trees live in
  `vendor/.build/` (removed on a clean full run). To bump a version, edit the
  pinned version + SHA256 in `tools/scripts/build_vendor.sh`.

### Verify

```bash
make audit          # tools/dep_audit.sh — versions vs minimum-safe CVE floors
make build-only     # compile every .o (no link) — should be clean
make dev-bin        # fast non-LTO local node binary: build/bin/zclassic23-dev
make vendor         # build the vendored archives from source
make zclassic23     # full link
```

## Build, test, deploy

```bash
make -j"$(nproc)"   # test_zcl + zclassic23 + zclassic-cli
make dev-bin        # fast local node executable, not for deploy/release
make test           # full parallel suite via the cached per-TU test_parallel
make test_parallel_wpo  # whole-program LTO test binary (debug per-TU/LTO divergence)
make lint           # 40 defensive-coding gates
make ci             # local gate: lint + tests + MVP slices (runs locally, not on GitHub Actions)
make deploy         # rebuild + restart; verify exact source ID and running executable SHA-256
```

`make deploy` pins its outer `BUILD_SOURCE_RECORD` into every recursive Make,
then freezes one candidate before any install or restart. Candidate-local
`agentbuild`, the current source record, the installed executable digest, and
the post-restart digest must all identify that same source/artifact pair. The
post-restart verifier ignores inherited `ZCL_DATADIR`, `ZCL_RPCPORT`, and
`ZCL_RPCCONNECT`: it derives the loopback RPC endpoint from the canonical
systemd service's captured `MainPID`/`ExecStart`/process argv, confirms that PID
owns the RPC listener, and rejects a PID or executable change during the proof.

Default target is `-march=x86-64-v3` (portable AVX2/FMA/BMI2); pass `ZCL_NATIVE=1`
to build for the host CPU only.

## Reproducible / signed releases

`tools/release.sh` is a legacy local packaging primitive with deterministic
flags and GPG support. `--unsigned` produces only a local-development artifact;
it is not eligible for stable publication. Stable release publication remains
contained until exact-candidate evidence, two independently provisioned
byte-identical builds, complete manifests/SBOM/provenance, and the required
offline signatures are enforced. See `docs/SECURITY_AND_INTEGRITY.md` for the
integrity model.
