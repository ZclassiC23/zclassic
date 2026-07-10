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

`make zclassic23` declares the vendor archives as order-only prerequisites, so
the first build transparently invokes `tools/scripts/build_vendor.sh` to produce
any that are missing. To build them explicitly:

```bash
make vendor         # build every missing vendor/lib/*.a (idempotent — no-op if present)
make vendor-force   # rebuild all of them from scratch
tools/scripts/build_vendor.sh libz.a libsqlite3.a   # a subset
```

`make build-only` (compile every `.o`, no link) does not need the archives and
is the fastest way to confirm a clean checkout compiles.

## Fast development binary

Release builds intentionally use one whole-program LTO link. For day-to-day C
development, use the non-release dev binary instead:

```bash
make agent-loop
make fast-rebuild
make agent-mcp-call TOOL=zcl_tools_list
make agent-mcp-call-hot TOOL=zcl_status
make agent-mcp-call-dev TOOL=zcl_status
make agent-doctor
make agent-dev-status
build/bin/zclassic23-dev agentdevstatus
make agent-stage-dev
build/bin/zclassic23-dev agentbuild
```

`make agent-loop` is the default AI/operator edit loop. It runs the cache-aware
fast checks; set `ZCL_AGENT_LOOP_BIN=1` to also link the local dev binary, or
`ZCL_AGENT_LOOP_DEPLOY=dev` to hot-swap the dev lane with the fast dev build.
`make agent-mcp-call` is the fresh source-tree typed MCP path; it refreshes
`build/bin/zclassic23-dev` before calling `mcpcall`, so API smoke checks after a
code edit use the current local code without paying the release LTO link. For
read-only status and schema checks where recompiling would be noise,
`make agent-mcp-call-hot TOOL=<tool>` reuses the existing source-tree dev
binary, and `make agent-mcp-call-dev TOOL=<tool>` reuses the installed
`~/.local/bin/zclassic23-dev` binary with the dev-lane datadir and RPC port.
`make agent-doctor` is the no-build combined development check: build binary,
dev-lane status, recent focused-test failure hints, dirty-file count, and the
next safe command. Use `make agent-doctor ARGS=--json` for
`zcl.agent_doctor.v1`.
`make agent-dev-status` is the no-build read-only lane check: it reports the
explicit `worker_lane` contract (`role=worker`,
`mutation_policy=noncanonical_dev_only`, and
`canonical_guard=never_touches_live_or_soak`), source/staged binaries, linger
service PID, RPC readiness, saved deploy state, auto-reindex marker, deploy
blocker/reason, stale-marker candidate, and next safe action. Use
`make agent-dev-status ARGS=--json`, native `zclassic23 agentdevstatus`, or MCP
`zcl_agent_dev_status` for the machine-readable `zcl.agent_dev_status.v1` form.
When that status reports `auto_reindex_stale_candidate=true`, run
`make agent-clear-stale-dev-reindex`; it archives the dev-lane marker only after
the dev RPC serves at or above the marker anchor, and never touches canonical or
soak.
When the dev service is busy and should not be restarted, `make agent-stage-dev`
builds the fast dev binary and atomically stages it at
`~/.local/bin/zclassic23-dev` for the next `zcl23-dev` restart.

`make fast-rebuild` is an alias for the local dev binary (`make dev-bin`). It
writes cached per-file objects under `build/dev-obj/`, links without LTO, keeps
symbols, and emits `build/bin/zclassic23-dev`. It defaults most code to
`ZCL_DEV_OPT=-Og` while compiling consensus/crypto/script/validation hot paths
at `ZCL_DEV_HOT_OPT=-O2`; both are overrideable. The link step auto-selects
`mold` or `ld.lld` through `ZCL_DEV_LINKER` when available; set
`ZCL_DEV_LINKER=` to force the platform linker. When `ccache` is installed, the
Makefile automatically wraps `CC` with it for rebuild speed; set
`ZCL_USE_CCACHE=0` to opt out.

For the absolute cheapest edit check, run `make fast-changed-compile`. It
compiles only changed node `.c` files into `build/dev-obj/` when that is safe,
and automatically falls back to `make fast-compile` for header, template,
Makefile, removed-source, or broad edits.

This binary is for local AI/operator iteration only. `make zclassic23`,
`make deploy`, reproducible builds, and releases continue to use
`build/bin/zclassic23` with the release flag profile.

## Cached full test suite (`test_parallel`)

`build/bin/test_parallel` — the binary `make t`, `make test`, and `make ci`
run — is built from a **cached per-TU object tree** (`build/test-rel-obj/`),
not one whole-program `cc`. Each of the
~1,300 node + test `.c` files compiles to its own `.o` with `-MMD -MP`
depfiles, and the binary is one plain link over those objects. Consequences:

- **One-file edit → one recompile + one link** (≈2 s here), not a full
  ~1,300-TU rebuild (≈90 s). Repeated full-suite gate runs with no edits
  re-link nothing (`make test_parallel` is a no-op when up to date).
- **Header/`.def` edits are tracked.** The retired whole-program rule listed
  only `.c` files as prerequisites, so a header-only edit did **not** rebuild
  `test_parallel` at all (a false green). The per-TU depfiles now recompile
  exactly the dependents of any changed header.
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
`build/bin/test_parallel_fast` object tree (`build/test-obj/`, `-O1`, non-
`-Werror`) for the tightest edit loop; run strict `make t` / `make test` before
commit.

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
make deploy         # force-fresh rebuild + restart the live service + verify build_commit
```

Default target is `-march=x86-64-v3` (portable AVX2/FMA/BMI2); pass `ZCL_NATIVE=1`
to build for the host CPU only.

## Reproducible / signed releases

`tools/release.sh` produces a reproducible build and an optional GPG signature
(waivable with `--unsigned`). See `docs/SECURITY_AND_INTEGRITY.md` for the
integrity model.
