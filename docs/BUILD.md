# Building zclassic23

`zclassic23` is one whole-program C23 binary. The build is a single `cc` over
~660–1400 `.c` files with LTO, linked against a set of **static** third-party
archives in `vendor/lib/`.

## The vendored-library gap (read this first)

Only `vendor/lib/libsecp256k1.a` is tracked in git. The other archives below are
**not yet in the repo**, so a fresh clone **will not link** `make zclassic23`
until you build and drop them into `vendor/lib/`. Automating this as `make
vendor` is on the roadmap; until then, build each from source as follows.

`make build-only` (compile every `.o`, no link) works without the archives and
is the fastest way to confirm a clean checkout compiles.

## Prerequisites

- **gcc 14+** (or clang with working `-std=c23`) and **GNU make**.
- A POSIX toolchain to build the dependencies (`autoconf`/`cmake` as noted).
- For the embedded Tor onion service: the `vendor/tor` submodule
  (`git submodule update --init`).

## Vendored archives

Versions are what the maintainer's audited build currently ships
(`./tools/dep_audit.sh` re-detects them from the binaries and checks each against
its minimum-safe version). Build each as a **static** library and place the `.a`
in `vendor/lib/` with headers under `vendor/include/`.

| Archive | Upstream | Version | Source |
|---|---|---|---|
| `libsecp256k1.a` *(tracked)* | Bitcoin Core secp256k1 | fork | https://github.com/bitcoin-core/secp256k1 |
| `libcrypto.a`, `libssl.a` | OpenSSL | 3.0.13 | https://github.com/openssl/openssl |
| `libevent.a`, `libevent_openssl.a`, `libevent_pthreads.a` | libevent | 2.1.12 | https://github.com/libevent/libevent |
| `libleveldb.a` | LevelDB | 1.18 | https://github.com/google/leveldb |
| `libsqlite3.a` | SQLite | 3.53.0 | https://www.sqlite.org/ |
| `libz.a` | zlib | 1.3 | https://github.com/madler/zlib |
| `libtor_stub.a` | Tor (RhettCreighton fork) | — | https://github.com/RhettCreighton/tor |

> **Security note:** the currently vendored OpenSSL (3.0.13) is **below** this
> project's own minimum-safe floor (3.0.16) — see `tools/dep_audit.sh`. Prefer
> 3.0.16+ when you rebuild. `make audit` reports the gap.

### Typical build commands

```bash
# OpenSSL (static, no shared libs)
./Configure no-shared --prefix=$PWD/_install && make -j"$(nproc)" && make install_sw
# -> libcrypto.a, libssl.a + include/openssl/

# libevent (static, with OpenSSL support)
./configure --disable-shared --enable-static && make -j"$(nproc)"
# -> libevent.a, libevent_openssl.a, libevent_pthreads.a

# LevelDB
cmake -DBUILD_SHARED_LIBS=OFF -DLEVELDB_BUILD_TESTS=OFF . && make -j"$(nproc)"

# SQLite (amalgamation) — compiled into libsqlite3.a
# zlib
./configure --static && make -j"$(nproc)"   # -> libz.a
```

Then verify the toolchain detects safe versions:

```bash
make audit          # tools/dep_audit.sh — versions vs minimum-safe CVE floors
make build-only     # compile every .o (no link) — should be clean
make zclassic23     # full link — succeeds once vendor/lib/ is populated
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
