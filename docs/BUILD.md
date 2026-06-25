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

## Prerequisites

- **gcc 14+** (or clang with working `-std=c23`) and **GNU make**.
- For `make vendor`: **`cmake`** (LevelDB), **`autoconf`** + an autotools
  toolchain (libevent, zlib), **`curl`** or **`wget`**, **`unzip`** (SQLite
  amalgamation zip), and **`sha256sum`**.
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

That is 11 archives total (the 10 `make vendor` builds + the committed
`libsecp256k1.a`).

Notes:
- **OpenSSL pinned to 3.0.16** — the project's minimum-safe floor (the older
  vendored 3.0.13 was below it). `make audit` reports the version.
- **LevelDB 1.23** is built, while the committed `vendor/include/leveldb/*.h`
  headers are 1.18. That is intentional and safe: the repo uses only LevelDB's
  stable C API (`<leveldb/c.h>`), which is unchanged across 1.18→1.23, so the
  headers and the built library stay compatible.
- **SQLite 3.49.0** amalgamation; `make vendor` also refreshes
  `vendor/include/sqlite3.h` and `vendor/sqlite3.c` so the rest of the build
  (e.g. `tools/sqlq.c`) stays in sync.
- Downloads are cached under `vendor/.cache/` (gitignored); build trees live in
  `vendor/.build/` (removed on a clean full run). To bump a version, edit the
  pinned version + SHA256 in `tools/scripts/build_vendor.sh`.

### Verify

```bash
make audit          # tools/dep_audit.sh — versions vs minimum-safe CVE floors
make build-only     # compile every .o (no link) — should be clean
make vendor         # build the vendored archives from source
make zclassic23     # full link
```

## Build, test, deploy

```bash
make -j"$(nproc)"   # test_zcl + zclassic23 + zclassic-cli
make test           # full suite (~426 parallel groups)
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
