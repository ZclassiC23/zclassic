#!/usr/bin/env bash
# tools/scripts/build_vendor.sh — produce every vendor/lib/*.a from source.
#
# Goal: `git clone && make vendor && make zclassic23` LINKS in one shot.
# zclassic23 links against a fixed set of static third-party archives in
# vendor/lib/. Only libsecp256k1.a is committed to git; the rest are built
# here, from source, with pinned versions + SHA256-verified downloads.
#
# Two source classes:
#   IN-TREE  — built from a source file already in the repo (no network):
#                libtor_stub.a  <- vendor/tor_stub.c
#   FETCHED  — no in-tree source; tarball pulled from a pinned URL, verified
#              against a pinned SHA256, then built static:
#                libsqlite3.a                          (SQLite amalgamation)
#                libcrypto.a, libssl.a                 (OpenSSL)
#                libevent.a, libevent_openssl.a,
#                libevent_pthreads.a                   (libevent)
#                libleveldb.a                          (LevelDB)
#                librustzcash.a                       (Sapling prover)
#                libz.a                                (zlib)
#
# Idempotent by PROVENANCE, not existence: an archive is skipped only when its
# deterministic stamp matches the source pin, recipe revision/flags, relevant
# dependency stamps, toolchain identity, and the archive's current SHA256.
# Missing/mismatched stamps rebuild. Force a full rebuild with VENDOR_FORCE=1.
# Downloads are cached under vendor/.cache/ (gitignored).
#
# Usage:
#   tools/scripts/build_vendor.sh            # build only what's missing
#   VENDOR_FORCE=1 tools/scripts/build_vendor.sh
#   tools/scripts/build_vendor.sh libz.a libsqlite3.a   # build a subset
#   tools/scripts/build_vendor.sh --check-provenance    # read-only verification

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
VENDOR="$REPO_ROOT/vendor"
LIB="$VENDOR/lib"
INC="$VENDOR/include"
CACHE="$VENDOR/.cache"
WORK="$VENDOR/.build"
SECP_MANIFEST="$VENDOR/provenance/libsecp256k1.manifest"
LIBEVENT_PATCH="$VENDOR/patches/libevent-2.1.12-secure-rng-abi.patch"

# shellcheck source=tools/scripts/vendor_provenance_lib.sh
. "$SCRIPT_DIR/vendor_provenance_lib.sh"

JOBS="$(nproc 2>/dev/null || echo 4)"
FORCE="${VENDOR_FORCE:-0}"
VENDOR_LOCK_DIR="${VENDOR_LOCK_DIR:-$VENDOR/.build.lock}"
VENDOR_LOCK_TIMEOUT_SEC="${VENDOR_LOCK_TIMEOUT_SEC:-600}"

# --- pinned versions + SHA256 (verified upstream-published hashes) ----------
SQLITE_YEAR="2025"
SQLITE_AMALG="sqlite-amalgamation-3490000"   # SQLite 3.49.0
SQLITE_URL="https://www.sqlite.org/${SQLITE_YEAR}/${SQLITE_AMALG}.zip"
SQLITE_SHA="cb6851ebad74913672014c20f642bbd7883552c4747780583a54ee1cd493f13b"

OPENSSL_VER="3.0.16"                          # >= project min-safe floor (3.0.16)
OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VER}/openssl-${OPENSSL_VER}.tar.gz"
OPENSSL_SHA="57e03c50feab5d31b152af2b764f10379aecd8ee92f16c985983ce4a99f7ef86"

LIBEVENT_VER="2.1.12"
LIBEVENT_URL="https://github.com/libevent/libevent/releases/download/release-${LIBEVENT_VER}-stable/libevent-${LIBEVENT_VER}-stable.tar.gz"
LIBEVENT_SHA="92e6de1be9ec176428fd2367677e61ceffc2ee1cb119035037a27d346b0403bb"

LEVELDB_VER="1.23"                            # C API (leveldb/c.h) compatible w/ tracked 1.18 headers
LEVELDB_URL="https://github.com/google/leveldb/archive/refs/tags/${LEVELDB_VER}.tar.gz"
LEVELDB_SHA="9a37f8a6174f09bd622bc723b55881dc541cd50747cbd08831c2a82d620f6d76"

ZLIB_VER="1.3.1"                              # 1.3 line, clean of CVE-2022-37434
ZLIB_URL="https://github.com/madler/zlib/releases/download/v${ZLIB_VER}/zlib-${ZLIB_VER}.tar.gz"
ZLIB_SHA="9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"

# Exact Sapling circuit/prover revision linked by the canonical ZClassic
# daemon.  Its C ABI stays behind lib/sapling/sapling_prover.h; zclassic23
# continues to use the independent C23 verifier for consensus.
RUSTZCASH_COMMIT="06da3b9ac8f278e5d4ae13088cf0a4c03d2c13f5"
RUSTZCASH_URL="https://github.com/zcash/librustzcash/archive/${RUSTZCASH_COMMIT}.tar.gz"
RUSTZCASH_SHA="9909ec59fa7a411c2071d6237b3363a0bc6e5e42358505cf64b7da0f58a7ff5a"

# Reproducibility: pin the build epoch + strip nondeterministic ar metadata.
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1700000000}"
export TZ=UTC LC_ALL=C
ARFLAGS_DET="Dcr"   # D = deterministic (zero mtime/uid/gid) when supported
VENDOR_CC="${VENDOR_CC:-cc}"
VENDOR_AR="${VENDOR_AR:-ar}"

# Recipe revisions are part of every expected stamp. Bump the affected value
# whenever its commands or semantic flags change. Exact flags and toolchain
# identities are bound separately, so pin/tool upgrades invalidate without a
# manual revision bump.
PROVENANCE_CONTRACT_REV="vp3"
RECIPE_TOR_STUB="tor-stub-r2"
RECIPE_SQLITE="sqlite-r2"
RECIPE_ZLIB="zlib-r2"
RECIPE_OPENSSL="openssl-r3"
RECIPE_LIBEVENT="libevent-r5"
RECIPE_LEVELDB="leveldb-r4"
RECIPE_RUSTZCASH="rustzcash-r3"

# --- logging (to stderr; stdout is reserved for fetch() to echo a path) -----
say()  { printf '\033[36m[vendor]\033[0m %s\n' "$*" >&2; }
ok()   { printf '\033[32m[vendor]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31m[vendor] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

need() { command -v "$1" >/dev/null 2>&1 || die "required tool not found: $1"; }

release_vendor_lock() {
    if [[ "${VENDOR_LOCK_HELD:-0}" == "1" ]]; then
        rmdir "$VENDOR_LOCK_DIR" 2>/dev/null || true
        VENDOR_LOCK_HELD=0
    fi
}

acquire_vendor_lock() {
    local waited=0
    while ! mkdir "$VENDOR_LOCK_DIR" 2>/dev/null; do
        if (( waited >= VENDOR_LOCK_TIMEOUT_SEC )); then
            die "timed out waiting for vendor build lock: $VENDOR_LOCK_DIR"
        fi
        sleep 1
        waited=$((waited + 1))
    done
    VENDOR_LOCK_HELD=1
    trap release_vendor_lock EXIT INT TERM
}

# --- download + verify ------------------------------------------------------
fetch() {
    # fetch <url> <sha256> <dest-filename>  -> echoes cached path
    local url="$1" sha="$2" dest="$CACHE/$3"
    mkdir -p "$CACHE"
    if [[ -f "$dest" ]] && echo "$sha  $dest" | sha256sum -c - >/dev/null 2>&1; then
        say "cached  $(basename "$dest")"
    else
        say "fetch   $url"
        if command -v curl >/dev/null 2>&1; then
            curl -fsSL --retry 3 -o "$dest.tmp" "$url"
        else
            need wget; wget -q -O "$dest.tmp" "$url"
        fi
        echo "$sha  $dest.tmp" | sha256sum -c - >/dev/null 2>&1 \
            || die "SHA256 mismatch for $url (expected $sha)"
        mv "$dest.tmp" "$dest"
    fi
    echo "$dest"
}

archive_group() {
    case "$1" in
        libtor_stub.a) printf 'tor_stub' ;;
        libsqlite3.a) printf 'sqlite' ;;
        libz.a) printf 'zlib' ;;
        libcrypto.a|libssl.a) printf 'openssl' ;;
        libevent.a|libevent_openssl.a|libevent_pthreads.a) printf 'libevent' ;;
        libleveldb.a) printf 'leveldb' ;;
        librustzcash.a) printf 'rustzcash' ;;
        *) return 1 ;;
    esac
}

recipe_revision() {
    case "$1" in
        tor_stub) printf '%s' "$RECIPE_TOR_STUB" ;;
        sqlite) printf '%s' "$RECIPE_SQLITE" ;;
        zlib) printf '%s' "$RECIPE_ZLIB" ;;
        openssl) printf '%s' "$RECIPE_OPENSSL" ;;
        libevent) printf '%s' "$RECIPE_LIBEVENT" ;;
        leveldb) printf '%s' "$RECIPE_LEVELDB" ;;
        rustzcash) printf '%s' "$RECIPE_RUSTZCASH" ;;
        *) return 1 ;;
    esac
}

recipe_source_fields() {
    case "$1" in
        tor_stub)
            printf 'version=in-tree\nsource_url=in-tree:vendor/tor_stub.c\nsource_sha256=%s\n' \
                "$(vp_sha256_file "$VENDOR/tor_stub.c")"
            ;;
        sqlite)
            printf 'version=%s\nsource_url=%s\nsource_sha256=%s\n' \
                "${SQLITE_AMALG#sqlite-amalgamation-}" "$SQLITE_URL" "$SQLITE_SHA"
            ;;
        zlib)
            printf 'version=%s\nsource_url=%s\nsource_sha256=%s\n' \
                "$ZLIB_VER" "$ZLIB_URL" "$ZLIB_SHA"
            ;;
        openssl)
            printf 'version=%s\nsource_url=%s\nsource_sha256=%s\n' \
                "$OPENSSL_VER" "$OPENSSL_URL" "$OPENSSL_SHA"
            ;;
        libevent)
            printf 'version=%s\nsource_url=%s\nsource_sha256=%s\nsource_patch=%s\nsource_patch_sha256=%s\n' \
                "$LIBEVENT_VER" "$LIBEVENT_URL" "$LIBEVENT_SHA" \
                "vendor/patches/$(basename "$LIBEVENT_PATCH")" \
                "$(vp_sha256_file "$LIBEVENT_PATCH")"
            ;;
        leveldb)
            printf 'version=%s\nsource_url=%s\nsource_sha256=%s\n' \
                "$LEVELDB_VER" "$LEVELDB_URL" "$LEVELDB_SHA"
            ;;
        rustzcash)
            printf 'version=%s\nsource_url=%s\nsource_sha256=%s\n' \
                "$RUSTZCASH_COMMIT" "$RUSTZCASH_URL" "$RUSTZCASH_SHA"
            ;;
        *) return 1 ;;
    esac
}

recipe_flags() {
    local group="$1" leveldb_route="direct-cxx11"
    command -v cmake >/dev/null 2>&1 && leveldb_route="cmake-release"
    case "$group" in
        tor_stub) printf '%s' '-std=c23 -O2 -fPIC; ar=Dcr' ;;
        sqlite) printf '%s' '-O2 -fPIC -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE -DSQLITE_ENABLE_JSON1 -DSQLITE_ENABLE_COLUMN_METADATA -DSQLITE_OMIT_DEPRECATED -DSQLITE_DEFAULT_FOREIGN_KEYS=1; ar=Dcr' ;;
        zlib) printf '%s' 'CFLAGS=-O2 -fPIC; ./configure --static; make libz.a' ;;
        openssl) printf '%s' './Configure no-shared no-tests --prefix=/usr/local --openssldir=/etc/ssl --libdir=lib; make build_libs' ;;
        libevent) printf '%s' 'apply pinned secure-rng ABI patch; CFLAGS=-O2 -fPIC -Ivendor/include; LDFLAGS=-Lvendor/lib; CPPFLAGS=-Ivendor/include; ./configure --disable-shared --enable-static --disable-samples --disable-libevent-regress; require=evutil_secure_rng_add_bytes' ;;
        leveldb) printf '%s' "route=$leveldb_route; Release PIC static; tests=off; benchmarks=off; direct=-std=c++11 -O2 -DNDEBUG -fPIC -fno-exceptions -fno-rtti"
            ;;
        rustzcash) printf '%s' 'cargo build --locked --release --package librustzcash; CARGO_INCREMENTAL=0; RUSTFLAGS=remap-source-and-cargo-home:/usr/src/zclassic23,-C debuginfo=0; reject-build-host-paths' ;;
        *) return 1 ;;
    esac
}

recipe_toolchain_sha() {
    local group="$1" cxx identities
    identities="cc=$(vp_compiler_identity_sha "$VENDOR_CC")
ar=$(vp_tool_identity_sha "$VENDOR_AR")"
    case "$group" in
        openssl)
            identities="$identities
perl=$(vp_tool_identity_sha perl)
make=$(vp_tool_identity_sha make)"
            ;;
        libevent|zlib)
            identities="$identities
make=$(vp_tool_identity_sha make)"
            ;;
        leveldb)
            cxx="$(leveldb_cxx_compiler)"
            identities="cxx=$(vp_compiler_identity_sha "$cxx")
ar=$(vp_tool_identity_sha "$VENDOR_AR")
make=$(vp_tool_identity_sha make)"
            if command -v cmake >/dev/null 2>&1; then
                identities="$identities
cmake=$(vp_tool_identity_sha cmake)"
            fi
            ;;
        rustzcash)
            identities="rustc=$(vp_tool_identity_sha rustc)
cargo=$(vp_tool_identity_sha cargo)"
            ;;
        tor_stub|sqlite) ;;
        *) return 1 ;;
    esac
    vp_sha256_text "$identities"
}

recipe_dependencies() {
    local group="$1" archive sha
    if [[ "$group" != "libevent" ]]; then
        printf 'none'
        return
    fi
    for archive in libcrypto.a libssl.a; do
        sha="$(vp_stamp_sha256 "$LIB" "$archive" 2>/dev/null || printf 'missing')"
        printf '%s=%s\n' "$archive" "$sha"
    done
}

archive_descriptor() {
    local archive="$1" group source_fields flags dependencies
    group="$(archive_group "$archive")" || return 1
    source_fields="$(recipe_source_fields "$group")" || return 1
    flags="$(recipe_flags "$group")" || return 1
    dependencies="$(recipe_dependencies "$group")"
    printf 'schema=%s\n' "$VP_SCHEMA"
    printf 'archive=%s\n' "$archive"
    printf 'component=%s\n' "$group"
    printf '%s\n' "$source_fields"
    printf 'provenance_contract_revision=%s\n' "$PROVENANCE_CONTRACT_REV"
    printf 'recipe_revision=%s\n' "$(recipe_revision "$group")"
    printf 'recipe_flags_sha256=%s\n' "$(vp_sha256_text "$flags")"
    printf 'source_date_epoch=%s\n' "$SOURCE_DATE_EPOCH"
    printf 'toolchain_sha256=%s\n' "$(recipe_toolchain_sha "$group")"
    printf 'dependencies_sha256=%s' "$(vp_sha256_text "$dependencies")"
}

archive_current() {
    local archive="$1" descriptor
    [[ -f "$LIB/$archive" ]] || return 1
    descriptor="$(archive_descriptor "$archive")" || return 1
    vp_verify_stamp "$LIB" "$archive" "$descriptor"
}

have() {
    [[ "$FORCE" != "1" ]] && archive_current "$1"
}

invalidate_stamps() {
    local archive
    for archive in "$@"; do
        rm -f "$(vp_stamp_path "$LIB" "$archive")"
    done
}

stamp_archives() {
    local archive descriptor
    for archive in "$@"; do
        descriptor="$(archive_descriptor "$archive")" ||
            die "cannot compute provenance descriptor for $archive"
        vp_write_stamp "$LIB" "$archive" "$descriptor" ||
            die "cannot write provenance stamp for $archive"
    done
}

install_archive() {
    local source="$1" archive="$2" tmp
    tmp="$LIB/.${archive}.tmp.$$"
    cp -f "$source" "$tmp"
    chmod 0644 "$tmp"
    mv -f "$tmp" "$LIB/$archive"
}

verify_committed_secp() {
    vp_verify_locked_manifest "$LIB/libsecp256k1.a" "$SECP_MANIFEST" \
        libsecp256k1.a
}

# --- per-library builders ---------------------------------------------------

build_tor_stub() {     # IN-TREE: vendor/tor_stub.c
    have libtor_stub.a && { say "skip    libtor_stub.a (provenance current)"; return; }
    say "build   libtor_stub.a  (in-tree: vendor/tor_stub.c)"
    [[ -f "$VENDOR/tor_stub.c" ]] || die "vendor/tor_stub.c missing (tracked source expected)"
    invalidate_stamps libtor_stub.a
    local o="$WORK/tor_stub.o" built="$WORK/libtor_stub.a"
    mkdir -p "$WORK"
    "$VENDOR_CC" -std=c23 -O2 -fPIC -c "$VENDOR/tor_stub.c" -o "$o"
    rm -f "$built"
    "$VENDOR_AR" $ARFLAGS_DET "$built" "$o" 2>/dev/null ||
        "$VENDOR_AR" cr "$built" "$o"
    install_archive "$built" libtor_stub.a
    stamp_archives libtor_stub.a
    ok "built   libtor_stub.a"
}

build_sqlite() {       # FETCHED: SQLite amalgamation
    have libsqlite3.a && { say "skip    libsqlite3.a (provenance current)"; return; }
    say "build   libsqlite3.a  (SQLite ${SQLITE_AMALG#sqlite-amalgamation-})"
    invalidate_stamps libsqlite3.a
    local zip; zip="$(fetch "$SQLITE_URL" "$SQLITE_SHA" "${SQLITE_AMALG}.zip")"
    local d="$WORK/$SQLITE_AMALG"
    rm -rf "$d"; need unzip; unzip -q -o "$zip" -d "$WORK"
    # Build flags mirror a typical Bitcoin/Zcash sqlite vendor build.
    local FLAGS="-DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE \
        -DSQLITE_ENABLE_JSON1 -DSQLITE_ENABLE_COLUMN_METADATA -DSQLITE_OMIT_DEPRECATED \
        -DSQLITE_DEFAULT_FOREIGN_KEYS=1"
    "$VENDOR_CC" -O2 -fPIC $FLAGS -c "$d/sqlite3.c" -o "$WORK/sqlite3.o"
    local built="$WORK/libsqlite3.a"
    rm -f "$built"
    "$VENDOR_AR" $ARFLAGS_DET "$built" "$WORK/sqlite3.o" 2>/dev/null ||
        "$VENDOR_AR" cr "$built" "$WORK/sqlite3.o"
    install_archive "$built" libsqlite3.a
    # Keep the amalgamation source in vendor/ (gitignored) so the rest of the
    # build (tools/sqlq.c etc.) and the header stay in sync.
    cp -f "$d/sqlite3.c" "$VENDOR/sqlite3.c"
    cp -f "$d/sqlite3.h" "$INC/sqlite3.h"
    stamp_archives libsqlite3.a
    ok "built   libsqlite3.a"
}

build_zlib() {         # FETCHED: zlib
    have libz.a && { say "skip    libz.a (provenance current)"; return; }
    say "build   libz.a  (zlib ${ZLIB_VER})"
    invalidate_stamps libz.a
    local tb; tb="$(fetch "$ZLIB_URL" "$ZLIB_SHA" "zlib-${ZLIB_VER}.tar.gz")"
    local d="$WORK/zlib-${ZLIB_VER}"
    rm -rf "$d"; tar -C "$WORK" -xzf "$tb"
    ( cd "$d" && CC="$VENDOR_CC" AR="$VENDOR_AR" CFLAGS="-O2 -fPIC" \
        ./configure --static >/dev/null \
        && make -j"$JOBS" libz.a >/dev/null )
    install_archive "$d/libz.a" libz.a
    cp -f "$d/zlib.h" "$d/zconf.h" "$INC/"
    stamp_archives libz.a
    ok "built   libz.a"
}

build_openssl() {      # FETCHED: OpenSSL -> libcrypto.a + libssl.a
    need perl
    { have libcrypto.a && have libssl.a; } && { say "skip    libcrypto.a/libssl.a (provenance current)"; return; }
    say "build   libcrypto.a + libssl.a  (OpenSSL ${OPENSSL_VER}) — this is the slow one"
    invalidate_stamps libcrypto.a libssl.a
    local tb; tb="$(fetch "$OPENSSL_URL" "$OPENSSL_SHA" "openssl-${OPENSSL_VER}.tar.gz")"
    local d="$WORK/openssl-${OPENSSL_VER}"
    rm -rf "$d"; tar -C "$WORK" -xzf "$tb"
    # no-docs/no-apps don't exist in OpenSSL 3.0.x; build only the libs target.
    #
    # Use NEUTRAL, non-$HOME prefix/openssldir so libcrypto.a carries NO
    # absolute build-machine path. OpenSSL bakes OPENSSLDIR / ENGINESDIR /
    # MODULESDIR string constants — derived from --prefix/--openssldir — into
    # the static library; with the old --prefix="$d/_install" (under the build
    # tree, i.e. /home/<user>/...) those leaked the build user's $HOME into the
    # final zclassic23 binary, failing test_no_hardcoded_home. This leakage is
    # exactly what the build-twice byte-identity gate catches
    # (tools/scripts/check_reproducible_build.sh, exposed as `make
    # ci-reproducible`): a non-neutral prefix bakes a per-build path into the
    # .a, so two builds of zclassic23 fail byte-identity. We only ever copy the
    # .a files (never `make install`), so these paths affect ONLY the embedded strings — the
    # canonical /usr/local + /etc/ssl values are relocatable and operator-
    # agnostic. (Do NOT pass -DOPENSSLDIR etc — Configure already defines them
    # from --prefix/--openssldir, and a -D redefine errors the build.)
    ( cd "$d" \
        && export CC="$VENDOR_CC" AR="$VENDOR_AR" \
        && ./Configure no-shared no-tests \
             --prefix=/usr/local --openssldir=/etc/ssl --libdir=lib >/dev/null \
        && make -j"$JOBS" build_libs >/dev/null 2>&1 )
    install_archive "$d/libcrypto.a" libcrypto.a
    install_archive "$d/libssl.a" libssl.a
    rm -rf "$INC/openssl"; mkdir -p "$INC/openssl"
    cp -f "$d/include/openssl/"*.h "$INC/openssl/" 2>/dev/null || true
    stamp_archives libcrypto.a libssl.a
    ok "built   libcrypto.a + libssl.a"
}

build_libevent() {     # FETCHED: libevent -> libevent.a + libevent_openssl.a + libevent_pthreads.a
    # Validate/rebuild OpenSSL first. Its stamp digests are bound into every
    # libevent descriptor, so a dependency upgrade invalidates all outputs.
    build_openssl
    { have libevent.a && have libevent_openssl.a && have libevent_pthreads.a; } \
        && { say "skip    libevent*.a (provenance current)"; return; }
    say "build   libevent.a + libevent_openssl.a + libevent_pthreads.a  (libevent ${LIBEVENT_VER})"
    need nm; need patch
    [[ -f "$LIBEVENT_PATCH" ]] || die "missing libevent patch: $LIBEVENT_PATCH"
    invalidate_stamps libevent.a libevent_openssl.a libevent_pthreads.a
    local tb; tb="$(fetch "$LIBEVENT_URL" "$LIBEVENT_SHA" "libevent-${LIBEVENT_VER}.tar.gz")"
    local d="$WORK/libevent-${LIBEVENT_VER}-stable"
    local build_log="$WORK/libevent-build.log"
    local symbols="$WORK/libevent.symbols"
    rm -rf "$d"; tar -C "$WORK" -xzf "$tb"
    # Newer glibc exposes arc4random() but not arc4random_addrandom(). In that
    # combination libevent 2.1.12 omits evutil_secure_rng_add_bytes even though
    # its public header declares it; the embedded Tor archive requires that
    # symbol. The pinned patch keeps the API present and makes it a no-op only
    # on platforms whose system arc4random has no entropy-injection primitive,
    # matching current upstream behavior while retaining the pinned release.
    if ! ( cd "$d" \
            && patch -p1 --forward <"$LIBEVENT_PATCH" \
            && CC="$VENDOR_CC" AR="$VENDOR_AR" \
               CFLAGS="-O2 -fPIC -I$INC" LDFLAGS="-L$LIB" \
               CPPFLAGS="-I$INC" \
               ./configure --disable-shared --enable-static \
                 --disable-samples --disable-libevent-regress \
            && make -j"$JOBS" \
        ) >"$build_log" 2>&1; then
        tail -200 "$build_log" >&2 || true
        die "libevent build failed (log: $build_log)"
    fi
    nm -g --defined-only "$d/.libs/libevent.a" >"$symbols" 2>/dev/null ||
        die "could not inspect rebuilt libevent.a"
    grep -qE ' [Tt] evutil_secure_rng_add_bytes$' "$symbols" ||
        die "libevent.a lacks Tor-required evutil_secure_rng_add_bytes"
    install_archive "$d/.libs/libevent.a" libevent.a
    install_archive "$d/.libs/libevent_openssl.a" libevent_openssl.a
    install_archive "$d/.libs/libevent_pthreads.a" libevent_pthreads.a
    # event2/* headers are consumed via the vendored tor build, not directly by
    # app code, so we do not need to install them for the zclassic23 link.
    stamp_archives libevent.a libevent_openssl.a libevent_pthreads.a
    ok "built   libevent*.a"
}

leveldb_cxx_compiler() {
    if [[ -n "${CXX:-}" ]]; then
        command -v "$CXX" >/dev/null 2>&1 || die "CXX not found: $CXX"
        printf '%s' "$CXX"
        return
    fi
    if command -v c++ >/dev/null 2>&1; then
        printf '%s' c++
        return
    fi
    if command -v g++ >/dev/null 2>&1; then
        printf '%s' g++
        return
    fi
    die "required tool not found: c++ or g++ (LevelDB direct fallback)"
}

build_leveldb_direct() {
    local d="$1" cxx gen objdir src obj
    local objs=()
    local sources=(
        db/builder.cc
        db/c.cc
        db/db_impl.cc
        db/db_iter.cc
        db/dbformat.cc
        db/dumpfile.cc
        db/filename.cc
        db/log_reader.cc
        db/log_writer.cc
        db/memtable.cc
        db/repair.cc
        db/table_cache.cc
        db/version_edit.cc
        db/version_set.cc
        db/write_batch.cc
        table/block_builder.cc
        table/block.cc
        table/filter_block.cc
        table/format.cc
        table/iterator.cc
        table/merger.cc
        table/table_builder.cc
        table/table.cc
        table/two_level_iterator.cc
        util/arena.cc
        util/bloom.cc
        util/cache.cc
        util/coding.cc
        util/comparator.cc
        util/crc32c.cc
        util/env.cc
        util/filter_policy.cc
        util/hash.cc
        util/logging.cc
        util/options.cc
        util/status.cc
        util/env_posix.cc
        helpers/memenv/memenv.cc
    )

    cxx="$(leveldb_cxx_compiler)"
    say "build   libleveldb.a  (direct C++11 fallback, no cmake)"
    gen="$WORK/leveldb-direct/include/port"
    objdir="$WORK/leveldb-direct/obj"
    rm -rf "$WORK/leveldb-direct"
    mkdir -p "$gen" "$objdir"
    cat > "$gen/port_config.h" <<'EOF'
#ifndef STORAGE_LEVELDB_PORT_PORT_CONFIG_H_
#define STORAGE_LEVELDB_PORT_PORT_CONFIG_H_
#ifndef HAVE_FDATASYNC
#define HAVE_FDATASYNC 1
#endif
#ifndef HAVE_FULLFSYNC
#define HAVE_FULLFSYNC 0
#endif
#ifndef HAVE_O_CLOEXEC
#define HAVE_O_CLOEXEC 1
#endif
#ifndef HAVE_CRC32C
#define HAVE_CRC32C 0
#endif
#ifndef HAVE_SNAPPY
#define HAVE_SNAPPY 0
#endif
#endif
EOF

    for src in "${sources[@]}"; do
        obj="$objdir/${src//\//_}.o"
        "$cxx" -std=c++11 -O2 -DNDEBUG -fPIC -fno-exceptions -fno-rtti \
            -DLEVELDB_PLATFORM_POSIX=1 -DLEVELDB_COMPILE_LIBRARY \
            -I"$WORK/leveldb-direct/include" -I"$d" -I"$d/include" \
            -c "$d/$src" -o "$obj"
        objs+=("$obj")
    done
    rm -f "$WORK/libleveldb.a"
    "$VENDOR_AR" $ARFLAGS_DET "$WORK/libleveldb.a" "${objs[@]}" 2>/dev/null ||
        "$VENDOR_AR" cr "$WORK/libleveldb.a" "${objs[@]}"
}

build_leveldb() {      # FETCHED: LevelDB -> libleveldb.a
    have libleveldb.a && { say "skip    libleveldb.a (provenance current)"; return; }
    say "build   libleveldb.a  (LevelDB ${LEVELDB_VER})"
    invalidate_stamps libleveldb.a
    local tb; tb="$(fetch "$LEVELDB_URL" "$LEVELDB_SHA" "leveldb-${LEVELDB_VER}.tar.gz")"
    local d="$WORK/leveldb-${LEVELDB_VER}"
    rm -rf "$d"; tar -C "$WORK" -xzf "$tb"
    if command -v cmake >/dev/null 2>&1; then
        local cxx ar_executable cmake_log
        cxx="$(leveldb_cxx_compiler)"
        ar_executable="$(command -v "$VENDOR_AR" 2>/dev/null || true)"
        [[ -n "$ar_executable" ]] ||
            die "LevelDB: could not resolve archiver '$VENDOR_AR'"
        cmake_log="$WORK/leveldb-cmake.log"
        if ! ( cd "$d" && cmake -S . -B build_static \
                -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_CXX_COMPILER="$cxx" \
                -DCMAKE_AR="$ar_executable" \
                -DBUILD_SHARED_LIBS=OFF \
                -DLEVELDB_BUILD_TESTS=OFF \
                -DLEVELDB_BUILD_BENCHMARKS=OFF \
                -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
                && cmake --build build_static -j"$JOBS" --target leveldb \
            ) >"$cmake_log" 2>&1; then
            tail -200 "$cmake_log" >&2 || true
            die "LevelDB CMake build failed (log: $cmake_log)"
        fi
        install_archive "$d/build_static/libleveldb.a" libleveldb.a
    else
        build_leveldb_direct "$d"
        install_archive "$WORK/libleveldb.a" libleveldb.a
    fi
    # Tracked vendor/include/leveldb/*.h (1.18) expose the same stable C API
    # (leveldb/c.h) the repo uses; we intentionally do NOT overwrite them.
    stamp_archives libleveldb.a
    ok "built   libleveldb.a"
}

build_rustzcash() {    # FETCHED: canonical Zcash Sapling prover -> librustzcash.a
    need cargo; need rustc; need nm
    have librustzcash.a && { say "skip    librustzcash.a (provenance current)"; return; }
    say "build   librustzcash.a  (Zcash ${RUSTZCASH_COMMIT:0:12})"
    invalidate_stamps librustzcash.a

    local tb; tb="$(fetch "$RUSTZCASH_URL" "$RUSTZCASH_SHA" \
        "librustzcash-${RUSTZCASH_COMMIT}.tar.gz")"
    local d="$WORK/librustzcash-${RUSTZCASH_COMMIT}"
    rm -rf "$d"
    mkdir -p "$d"
    tar -C "$d" -xzf "$tb" --strip-components=1

    # Cargo.lock pins registry checksums and the sole git dependency to an
    # exact revision.  Disable incremental state, use the upstream release
    # profile's single codegen unit + LTO, and remap the throwaway extraction
    # directory so neither the archive nor the final one-binary artifact
    # embeds $HOME or vendor/.build paths. SOURCE_DATE_EPOCH is shared with
    # the C archives above for the build-twice reproducibility contract.
    (
        cd "$d"
        CARGO_HOME="$CACHE/cargo-home" \
        CARGO_TARGET_DIR="$d/target" \
        CARGO_INCREMENTAL=0 \
        RUSTFLAGS="--remap-path-prefix=$d=/usr/src/zclassic23/librustzcash --remap-path-prefix=$CACHE/cargo-home=/usr/src/zclassic23/cargo-home -C debuginfo=0" \
            cargo build --locked --release --package librustzcash
    )

    local built="$d/target/release/librustzcash.a"
    local symbols="$d/target/release/librustzcash.symbols"
    [[ -f "$built" ]] || die "cargo succeeded but librustzcash.a is missing"
    nm -g --defined-only "$built" >"$symbols" 2>/dev/null ||
        die "could not inspect librustzcash.a symbols"
    if ! grep -q 'librustzcash_sapling_spend_proof' "$symbols"; then
        die "librustzcash.a lacks the Sapling proving C ABI"
    fi
    # A static archive can be functionally correct while still leaking the
    # build user's home through Rust panic/debug file names.  Reject it here,
    # before installation or final linking, so test_no_hardcoded_home is a
    # backstop rather than the first place the leak is discovered.
    local forbidden
    for forbidden in "$REPO_ROOT" "$WORK" "$CACHE" "${HOME:-}"; do
        [[ -n "$forbidden" ]] || continue
        if LC_ALL=C grep -aF "$forbidden" "$built" >/dev/null; then
            die "librustzcash.a embeds build-host path: $forbidden"
        fi
    done
    install_archive "$built" librustzcash.a
    stamp_archives librustzcash.a
    ok "built   librustzcash.a"
}

# --- orchestration ----------------------------------------------------------
need "$VENDOR_CC"; need "$VENDOR_AR"; need sha256sum; need tar; need make
mkdir -p "$LIB" "$INC" "$WORK"
acquire_vendor_lock

REQUIRED=(libsecp256k1.a libcrypto.a libssl.a libevent.a libevent_openssl.a
          libevent_pthreads.a libleveldb.a libsqlite3.a libz.a
          librustzcash.a libtor_stub.a)

check_one_provenance() {
    local archive="$1" descriptor
    if [[ "$archive" == "libsecp256k1.a" ]]; then
        verify_committed_secp
        return
    fi
    descriptor="$(archive_descriptor "$archive")" || return 1
    vp_verify_stamp "$LIB" "$archive" "$descriptor"
}

check_provenance_set() {
    local archive failed=0
    for archive in "$@"; do
        if check_one_provenance "$archive"; then
            [[ "${VENDOR_PROVENANCE_QUIET:-0}" == "1" ]] ||
                ok "verify  $archive provenance current"
        else
            [[ "${VENDOR_PROVENANCE_QUIET:-0}" == "1" ]] ||
                say "STALE   $archive (missing/mismatched provenance or bytes)"
            failed=1
        fi
    done
    return "$failed"
}

if [[ "${1:-}" == "--check-provenance" ]]; then
    shift
    if [[ $# -gt 0 ]]; then
        check_provenance_set "$@" ||
            die "vendor provenance verification failed"
    else
        check_provenance_set "${REQUIRED[@]}" ||
            die "vendor provenance verification failed"
    fi
    ok "vendor provenance verification passed"
    exit 0
fi

# Build order: openssl before libevent (libevent_openssl needs its headers).
ALL=(build_tor_stub build_zlib build_sqlite build_openssl build_libevent build_leveldb build_rustzcash)

# Map .a names -> builder for the subset form.
declare -A BUILDER=(
    [libtor_stub.a]=build_tor_stub
    [libz.a]=build_zlib
    [libsqlite3.a]=build_sqlite
    [libcrypto.a]=build_openssl [libssl.a]=build_openssl
    [libevent.a]=build_libevent [libevent_openssl.a]=build_libevent [libevent_pthreads.a]=build_libevent
    [libleveldb.a]=build_leveldb
    [librustzcash.a]=build_rustzcash
)

if [[ $# -gt 0 ]]; then
    seen=""
    for a in "$@"; do
        b="${BUILDER[$a]:-}"
        [[ -n "$b" ]] || die "unknown vendor archive: $a"
        case " $seen " in *" $b "*) ;; *) "$b"; seen="$seen $b";; esac
    done
else
    for b in "${ALL[@]}"; do "$b"; done
fi

# --- verify the full set ----------------------------------------------------
stale=()
for a in "${REQUIRED[@]}"; do
    check_one_provenance "$a" || stale+=("$a")
done
if [[ ${#stale[@]} -gt 0 ]]; then
    if [[ $# -gt 0 ]]; then
        say "subset build complete; still missing/stale: ${stale[*]}"
    else
        die "vendor build finished but provenance is stale: ${stale[*]}"
    fi
else
    rm -rf "$WORK"
    ok "all vendor/lib archives provenance-verified:"
    ( cd "$LIB" && ls -1 *.a | sed 's/^/        /' )
fi
