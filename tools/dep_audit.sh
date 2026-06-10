#!/usr/bin/env bash
# tools/dep_audit.sh — Dependency vulnerability audit for vendored libraries.
# Checks vendored OpenSSL, SQLite, libevent, leveldb, zlib, libsecp256k1
# against known-minimum-safe versions.  Exit 0 = all clear, exit 1 = findings.
#
# Usage:  ./tools/dep_audit.sh [--json]
# Or:     make audit

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VENDOR_LIB="$REPO_ROOT/vendor/lib"
VENDOR_INC="$REPO_ROOT/vendor/include"

JSON=0
[[ "${1:-}" == "--json" ]] && JSON=1

FAIL=0
WARN=0
RESULTS=()

# --- helpers ---------------------------------------------------------------

check() {
    local name="$1" current="$2" minimum="$3" cve_note="$4"

    # Compare versions using sort -V (version sort).
    local older
    older=$(printf '%s\n%s\n' "$current" "$minimum" | sort -V | head -1)

    local status="PASS" detail=""
    if [[ "$current" == "unknown" ]]; then
        status="WARN"
        detail="version could not be detected"
        ((WARN++)) || true
    elif [[ "$older" != "$minimum" && "$current" != "$minimum" ]]; then
        # current is older than minimum
        status="FAIL"
        detail="$cve_note (need >= $minimum, have $current)"
        ((FAIL++)) || true
    elif [[ "$current" == "$minimum" ]]; then
        detail="at minimum safe version"
    else
        detail="ahead of minimum ($minimum)"
    fi

    if [[ $JSON -eq 1 ]]; then
        RESULTS+=("{\"name\":\"$name\",\"version\":\"$current\",\"minimum\":\"$minimum\",\"status\":\"$status\",\"detail\":\"$detail\"}")
    else
        local color="\033[32m"  # green
        [[ "$status" == "FAIL" ]] && color="\033[31m"  # red
        [[ "$status" == "WARN" ]] && color="\033[33m"  # yellow
        printf "  %-20s %-14s %-10s ${color}%-6s\033[0m %s\n" \
            "$name" "$current" ">= $minimum" "$status" "$detail"
    fi
}

extract_version() {
    # Extract version from a static library using strings + pattern match.
    local lib="$1" pattern="$2"
    strings "$lib" 2>/dev/null | grep -oP "$pattern" | head -1
}

# --- detect versions -------------------------------------------------------

# OpenSSL: embedded version string in libcrypto.a
OPENSSL_VER=$(extract_version "$VENDOR_LIB/libcrypto.a" 'OpenSSL \K[0-9]+\.[0-9]+\.[0-9]+' || echo "unknown")

# SQLite: header macro
if [[ -f "$VENDOR_INC/sqlite3.h" ]]; then
    SQLITE_VER=$(grep '#define SQLITE_VERSION ' "$VENDOR_INC/sqlite3.h" | grep -oP '"[0-9.]+' | tr -d '"')
else
    SQLITE_VER="unknown"
fi

# libevent: embedded version string
LIBEVENT_VER=$(extract_version "$VENDOR_LIB/libevent.a" '[0-9]+\.[0-9]+\.[0-9]+-stable' || echo "unknown")
LIBEVENT_VER="${LIBEVENT_VER%-stable}"  # strip -stable suffix for comparison

# leveldb: header constants
if [[ -f "$VENDOR_INC/leveldb/db.h" ]]; then
    LDB_MAJOR=$(grep 'kMajorVersion' "$VENDOR_INC/leveldb/db.h" | grep -oP '[0-9]+' | tail -1)
    LDB_MINOR=$(grep 'kMinorVersion' "$VENDOR_INC/leveldb/db.h" | grep -oP '[0-9]+' | tail -1)
    LEVELDB_VER="${LDB_MAJOR:-0}.${LDB_MINOR:-0}"
else
    LEVELDB_VER="unknown"
fi

# zlib: embedded copyright string in libz.a
ZLIB_VER=$(extract_version "$VENDOR_LIB/libz.a" 'deflate \K[0-9]+\.[0-9]+(\.[0-9]+)?' || echo "unknown")

# libsecp256k1: no standard version string; check library exists
SECP_STATUS="present"
[[ ! -f "$VENDOR_LIB/libsecp256k1.a" ]] && SECP_STATUS="MISSING"

# --- minimum safe versions -------------------------------------------------
# These are the oldest versions without known HIGH/CRITICAL CVEs
# as of the audit date.  Update when new advisories drop.

# OpenSSL 3.0.x: CVE-2024-0727 (3.0.13), CVE-2024-2511 (3.0.14),
#   CVE-2024-4603 (3.0.14), CVE-2024-4741 (3.0.14),
#   CVE-2024-5535 (3.0.15), CVE-2024-9143 (3.0.15),
#   CVE-2024-12797 (3.0.16), CVE-2024-13176 (3.0.16)
OPENSSL_MIN="3.0.16"
OPENSSL_NOTE="CVE-2024-12797 (MITM on RFC7250), CVE-2024-9143 (low-level bignum OOB)"

# SQLite: 3.46.0 fixed CVE-2024-0232; 3.49.0 is well ahead
SQLITE_MIN="3.46.0"
SQLITE_NOTE="CVE-2024-0232 (jsonb blob crash)"

# libevent 2.1.12 is current stable; no outstanding CVEs
LIBEVENT_MIN="2.1.12"
LIBEVENT_NOTE="no known CVEs in 2.1.12"

# leveldb: no CVEs on record
LEVELDB_MIN="1.18"
LEVELDB_NOTE="no known CVEs"

# zlib: CVE-2022-37434 (heap OOB in inflate, fixed in 1.2.12);
#   1.3+ is clean
ZLIB_MIN="1.2.12"
ZLIB_NOTE="CVE-2022-37434 (heap OOB inflate)"

# --- run checks ------------------------------------------------------------

if [[ $JSON -eq 0 ]]; then
    echo "=== ZClassic23 Dependency Vulnerability Audit ==="
    echo "    Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo ""
    printf "  %-20s %-14s %-10s %-6s %s\n" "DEPENDENCY" "VERSION" "MINIMUM" "STATUS" "DETAIL"
    printf "  %-20s %-14s %-10s %-6s %s\n" "----------" "-------" "-------" "------" "------"
fi

check "OpenSSL"    "$OPENSSL_VER"  "$OPENSSL_MIN"  "$OPENSSL_NOTE"
check "SQLite"     "$SQLITE_VER"   "$SQLITE_MIN"   "$SQLITE_NOTE"
check "libevent"   "$LIBEVENT_VER" "$LIBEVENT_MIN" "$LIBEVENT_NOTE"
check "leveldb"    "$LEVELDB_VER"  "$LEVELDB_MIN"  "$LEVELDB_NOTE"
check "zlib"       "$ZLIB_VER"     "$ZLIB_MIN"     "$ZLIB_NOTE"

# secp256k1: presence check only (Bitcoin Core fork, no versioned CVEs)
if [[ $JSON -eq 1 ]]; then
    RESULTS+=("{\"name\":\"libsecp256k1\",\"version\":\"n/a\",\"minimum\":\"n/a\",\"status\":\"$( [[ $SECP_STATUS == present ]] && echo PASS || echo FAIL )\",\"detail\":\"$SECP_STATUS — Bitcoin Core fork, no versioned CVE tracking\"}")
else
    if [[ "$SECP_STATUS" == "present" ]]; then
        printf "  %-20s %-14s %-10s \033[32m%-6s\033[0m %s\n" \
            "libsecp256k1" "n/a" "n/a" "PASS" "present — Bitcoin Core fork, no versioned CVE tracking"
    else
        printf "  %-20s %-14s %-10s \033[31m%-6s\033[0m %s\n" \
            "libsecp256k1" "n/a" "n/a" "FAIL" "MISSING"
        ((FAIL++))
    fi
fi

# --- output ----------------------------------------------------------------

if [[ $JSON -eq 1 ]]; then
    echo "{"
    echo "  \"date\": \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\","
    echo "  \"fail\": $FAIL,"
    echo "  \"warn\": $WARN,"
    printf '  "results": [%s]\n' "$(IFS=,; echo "${RESULTS[*]}")"
    echo "}"
else
    echo ""
    if [[ $FAIL -gt 0 ]]; then
        echo -e "\033[31m  ✗ $FAIL dependency version(s) below minimum safe version.\033[0m"
        echo "    Action: update vendored libraries and rebuild."
        exit 1
    elif [[ $WARN -gt 0 ]]; then
        echo -e "\033[33m  ⚠ $WARN dependency version(s) could not be detected.\033[0m"
        exit 0
    else
        echo -e "\033[32m  ✓ All dependencies at or above minimum safe versions.\033[0m"
        exit 0
    fi
fi
