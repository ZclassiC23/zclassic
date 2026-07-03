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
#                libz.a                                (zlib)
#
# Idempotent: a lib whose .a already exists in vendor/lib/ is skipped, so
# re-running is a no-op once vendor/lib/ is populated. Force a full rebuild
# with VENDOR_FORCE=1. Downloads are cached under vendor/.cache/ (gitignored).
#
# Usage:
#   tools/scripts/build_vendor.sh            # build only what's missing
#   VENDOR_FORCE=1 tools/scripts/build_vendor.sh
#   tools/scripts/build_vendor.sh libz.a libsqlite3.a   # build a subset

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
VENDOR="$REPO_ROOT/vendor"
LIB="$VENDOR/lib"
INC="$VENDOR/include"
CACHE="$VENDOR/.cache"
WORK="$VENDOR/.build"

JOBS="$(nproc 2>/dev/null || echo 4)"
FORCE="${VENDOR_FORCE:-0}"

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

# Reproducibility: pin the build epoch + strip nondeterministic ar metadata.
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1700000000}"
export TZ=UTC LC_ALL=C
ARFLAGS_DET="Dcr"   # D = deterministic (zero mtime/uid/gid) when supported

# --- logging (to stderr; stdout is reserved for fetch() to echo a path) -----
say()  { printf '\033[36m[vendor]\033[0m %s\n' "$*" >&2; }
ok()   { printf '\033[32m[vendor]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31m[vendor] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

need() { command -v "$1" >/dev/null 2>&1 || die "required tool not found: $1"; }

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

have() { [[ -f "$LIB/$1" && "$FORCE" != "1" ]]; }

# --- per-library builders ---------------------------------------------------

build_tor_stub() {     # IN-TREE: vendor/tor_stub.c
    have libtor_stub.a && { say "skip    libtor_stub.a (present)"; return; }
    say "build   libtor_stub.a  (in-tree: vendor/tor_stub.c)"
    [[ -f "$VENDOR/tor_stub.c" ]] || die "vendor/tor_stub.c missing (tracked source expected)"
    local o="$WORK/tor_stub.o"
    mkdir -p "$WORK"
    cc -std=c23 -O2 -fPIC -c "$VENDOR/tor_stub.c" -o "$o"
    rm -f "$LIB/libtor_stub.a"
    ar $ARFLAGS_DET "$LIB/libtor_stub.a" "$o" 2>/dev/null || ar cr "$LIB/libtor_stub.a" "$o"
    ok "built   libtor_stub.a"
}

build_sqlite() {       # FETCHED: SQLite amalgamation
    have libsqlite3.a && { say "skip    libsqlite3.a (present)"; return; }
    say "build   libsqlite3.a  (SQLite ${SQLITE_AMALG#sqlite-amalgamation-})"
    local zip; zip="$(fetch "$SQLITE_URL" "$SQLITE_SHA" "${SQLITE_AMALG}.zip")"
    local d="$WORK/$SQLITE_AMALG"
    rm -rf "$d"; need unzip; unzip -q -o "$zip" -d "$WORK"
    # Build flags mirror a typical Bitcoin/Zcash sqlite vendor build.
    local FLAGS="-DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE \
        -DSQLITE_ENABLE_JSON1 -DSQLITE_ENABLE_COLUMN_METADATA -DSQLITE_OMIT_DEPRECATED \
        -DSQLITE_DEFAULT_FOREIGN_KEYS=1"
    cc -O2 -fPIC $FLAGS -c "$d/sqlite3.c" -o "$WORK/sqlite3.o"
    rm -f "$LIB/libsqlite3.a"
    ar $ARFLAGS_DET "$LIB/libsqlite3.a" "$WORK/sqlite3.o" 2>/dev/null || ar cr "$LIB/libsqlite3.a" "$WORK/sqlite3.o"
    # Keep the amalgamation source in vendor/ (gitignored) so the rest of the
    # build (tools/sqlq.c etc.) and the header stay in sync.
    cp -f "$d/sqlite3.c" "$VENDOR/sqlite3.c"
    cp -f "$d/sqlite3.h" "$INC/sqlite3.h"
    ok "built   libsqlite3.a"
}

build_zlib() {         # FETCHED: zlib
    have libz.a && { say "skip    libz.a (present)"; return; }
    say "build   libz.a  (zlib ${ZLIB_VER})"
    local tb; tb="$(fetch "$ZLIB_URL" "$ZLIB_SHA" "zlib-${ZLIB_VER}.tar.gz")"
    local d="$WORK/zlib-${ZLIB_VER}"
    rm -rf "$d"; tar -C "$WORK" -xzf "$tb"
    ( cd "$d" && CFLAGS="-O2 -fPIC" ./configure --static >/dev/null \
        && make -j"$JOBS" libz.a >/dev/null )
    cp -f "$d/libz.a" "$LIB/libz.a"
    cp -f "$d/zlib.h" "$d/zconf.h" "$INC/"
    ok "built   libz.a"
}

build_openssl() {      # FETCHED: OpenSSL -> libcrypto.a + libssl.a
    { have libcrypto.a && have libssl.a; } && { say "skip    libcrypto.a/libssl.a (present)"; return; }
    say "build   libcrypto.a + libssl.a  (OpenSSL ${OPENSSL_VER}) — this is the slow one"
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
        && ./Configure no-shared no-tests \
             --prefix=/usr/local --openssldir=/etc/ssl --libdir=lib >/dev/null \
        && make -j"$JOBS" build_libs >/dev/null 2>&1 )
    cp -f "$d/libcrypto.a" "$LIB/libcrypto.a"
    cp -f "$d/libssl.a"    "$LIB/libssl.a"
    rm -rf "$INC/openssl"; mkdir -p "$INC/openssl"
    cp -f "$d/include/openssl/"*.h "$INC/openssl/" 2>/dev/null || true
    ok "built   libcrypto.a + libssl.a"
}

build_libevent() {     # FETCHED: libevent -> libevent.a + libevent_openssl.a + libevent_pthreads.a
    { have libevent.a && have libevent_openssl.a && have libevent_pthreads.a; } \
        && { say "skip    libevent*.a (present)"; return; }
    # libevent_openssl needs the OpenSSL headers we just installed.
    { have libcrypto.a && have libssl.a; } || build_openssl
    say "build   libevent.a + libevent_openssl.a + libevent_pthreads.a  (libevent ${LIBEVENT_VER})"
    local tb; tb="$(fetch "$LIBEVENT_URL" "$LIBEVENT_SHA" "libevent-${LIBEVENT_VER}.tar.gz")"
    local d="$WORK/libevent-${LIBEVENT_VER}-stable"
    rm -rf "$d"; tar -C "$WORK" -xzf "$tb"
    ( cd "$d" \
        && CFLAGS="-O2 -fPIC -I$INC" LDFLAGS="-L$LIB" \
           ./configure --disable-shared --enable-static --disable-samples \
             --disable-libevent-regress \
             CPPFLAGS="-I$INC" >/dev/null \
        && make -j"$JOBS" >/dev/null 2>&1 )
    cp -f "$d/.libs/libevent.a"          "$LIB/libevent.a"
    cp -f "$d/.libs/libevent_openssl.a"  "$LIB/libevent_openssl.a"
    cp -f "$d/.libs/libevent_pthreads.a" "$LIB/libevent_pthreads.a"
    # event2/* headers are consumed via the vendored tor build, not directly by
    # app code, so we do not need to install them for the zclassic23 link.
    ok "built   libevent*.a"
}

build_leveldb() {      # FETCHED: LevelDB -> libleveldb.a
    have libleveldb.a && { say "skip    libleveldb.a (present)"; return; }
    say "build   libleveldb.a  (LevelDB ${LEVELDB_VER})"
    local tb; tb="$(fetch "$LEVELDB_URL" "$LEVELDB_SHA" "leveldb-${LEVELDB_VER}.tar.gz")"
    local d="$WORK/leveldb-${LEVELDB_VER}"
    rm -rf "$d"; tar -C "$WORK" -xzf "$tb"
    need cmake
    ( cd "$d" && cmake -S . -B build_static \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DLEVELDB_BUILD_TESTS=OFF \
        -DLEVELDB_BUILD_BENCHMARKS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON >/dev/null \
        && cmake --build build_static -j"$JOBS" --target leveldb >/dev/null 2>&1 )
    cp -f "$d/build_static/libleveldb.a" "$LIB/libleveldb.a"
    # Tracked vendor/include/leveldb/*.h (1.18) expose the same stable C API
    # (leveldb/c.h) the repo uses; we intentionally do NOT overwrite them.
    ok "built   libleveldb.a"
}

# --- orchestration ----------------------------------------------------------
need cc; need ar; need sha256sum; need tar; need make
mkdir -p "$LIB" "$INC" "$WORK"

# Build order: openssl before libevent (libevent_openssl needs its headers).
ALL=(build_tor_stub build_zlib build_sqlite build_openssl build_libevent build_leveldb)

# Map .a names -> builder for the subset form.
declare -A BUILDER=(
    [libtor_stub.a]=build_tor_stub
    [libz.a]=build_zlib
    [libsqlite3.a]=build_sqlite
    [libcrypto.a]=build_openssl [libssl.a]=build_openssl
    [libevent.a]=build_libevent [libevent_openssl.a]=build_libevent [libevent_pthreads.a]=build_libevent
    [libleveldb.a]=build_leveldb
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
REQUIRED=(libsecp256k1.a libcrypto.a libssl.a libevent.a libevent_openssl.a
          libevent_pthreads.a libleveldb.a libsqlite3.a libz.a libtor_stub.a)
missing=()
for a in "${REQUIRED[@]}"; do [[ -f "$LIB/$a" ]] || missing+=("$a"); done
if [[ ${#missing[@]} -gt 0 ]]; then
    if [[ $# -gt 0 ]]; then
        say "subset build complete; still absent: ${missing[*]}"
    else
        die "vendor build finished but these archives are still missing: ${missing[*]}"
    fi
else
    rm -rf "$WORK"
    ok "all vendor/lib archives present:"
    ( cd "$LIB" && ls -1 *.a | sed 's/^/        /' )
fi
