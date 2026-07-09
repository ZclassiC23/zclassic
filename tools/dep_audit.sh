#!/usr/bin/env bash
# tools/dep_audit.sh — Dependency vulnerability audit for vendored libraries.
# Checks vendored OpenSSL, SQLite, libevent, leveldb, zlib, libsecp256k1,
# and the pinned Zcash Sapling prover. Exit 0 = all clear, exit 1 = findings.
#
# Usage:  ./tools/dep_audit.sh [--json]
# Or:     make audit

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
VENDOR_LIB="$REPO_ROOT/vendor/lib"
VENDOR_INC="$REPO_ROOT/vendor/include"
BUILD_VENDOR="$REPO_ROOT/tools/scripts/build_vendor.sh"

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
        status="FAIL"
        detail="version could not be detected (audit fails closed)"
        ((FAIL++)) || true
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

provenance_ok() {
    VENDOR_PROVENANCE_QUIET=1 "$BUILD_VENDOR" --check-provenance "$@" \
        >/dev/null 2>&1
}

stamp_version() {
    local archive="$1" stamp
    stamp="$VENDOR_LIB/.provenance/$archive.stamp"
    [[ -f "$stamp" ]] || { printf 'unknown'; return; }
    sed -n 's/^version=//p' "$stamp" | head -1
}

# Installed bytes must first match the deterministic build provenance contract.
# Version strings and ABI symbols are useful defense-in-depth, but cannot bless
# an arbitrary archive on their own.
PROVENANCE_STATUS="current"
if ! provenance_ok; then
    PROVENANCE_STATUS="STALE_OR_UNPROVEN"
fi

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

# LevelDB's tracked headers intentionally expose an older compatible C API, so
# they cannot identify the installed archive. The verified stamp can.
LEVELDB_VER="$(stamp_version libleveldb.a)"

# zlib: embedded copyright string in libz.a
ZLIB_VER=$(extract_version "$VENDOR_LIB/libz.a" 'deflate \K[0-9]+\.[0-9]+(\.[0-9]+)?' || echo "unknown")

# libsecp256k1: the legacy committed bytes have no recoverable upstream commit.
# The checked manifest locks their exact SHA256/size and explicitly forbids a
# replacement until source URL/revision/hash + reproducible recipe are proven.
SECP_STATUS="HASH_LOCKED"
provenance_ok libsecp256k1.a || SECP_STATUS="MANIFEST_MISMATCH"

# librustzcash: the canonical ZClassic revision is source-pinned rather than
# semver-versioned. Audit both provenance and the exact proving ABI the C
# adapter consumes; an arbitrary or verifier-only archive must not pass.
RUSTZCASH_COMMIT="06da3b9ac8f278e5d4ae13088cf0a4c03d2c13f5"
RUSTZCASH_STATUS="present"
if [[ ! -f "$VENDOR_LIB/librustzcash.a" ]]; then
    RUSTZCASH_STATUS="MISSING"
elif ! provenance_ok librustzcash.a; then
    RUSTZCASH_STATUS="INVALID_PROVENANCE"
elif ! nm -g --defined-only "$VENDOR_LIB/librustzcash.a" 2>/dev/null |
       awk '/librustzcash_sapling_spend_proof/ { spend=1 }
            /librustzcash_sapling_output_proof/ { output=1 }
            END { exit (spend && output) ? 0 : 1 }'; then
    RUSTZCASH_STATUS="INVALID_ABI"
fi

[[ "$SECP_STATUS" == "HASH_LOCKED" ]] || ((FAIL++)) || true
[[ "$RUSTZCASH_STATUS" == "present" ]] || ((FAIL++)) || true

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

if [[ "$PROVENANCE_STATUS" != "current" ]]; then
    ((FAIL++)) || true
fi
if [[ $JSON -eq 1 ]]; then
    RESULTS+=("{\"name\":\"vendor-provenance\",\"version\":\"v1\",\"minimum\":\"exact\",\"status\":\"$( [[ $PROVENANCE_STATUS == current ]] && echo PASS || echo FAIL )\",\"detail\":\"$PROVENANCE_STATUS — archive SHA256 must match source/recipe/toolchain/dependency stamp\"}")
else
    if [[ "$PROVENANCE_STATUS" == "current" ]]; then
        printf "  %-20s %-14s %-10s \033[32m%-6s\033[0m %s\n" \
            "vendor-provenance" "v1" "exact" "PASS" \
            "all installed archives match deterministic stamps"
    else
        printf "  %-20s %-14s %-10s \033[31m%-6s\033[0m %s\n" \
            "vendor-provenance" "v1" "exact" "FAIL" \
            "$PROVENANCE_STATUS — run make vendor"
    fi
fi

check "OpenSSL"    "$OPENSSL_VER"  "$OPENSSL_MIN"  "$OPENSSL_NOTE"
check "SQLite"     "$SQLITE_VER"   "$SQLITE_MIN"   "$SQLITE_NOTE"
check "libevent"   "$LIBEVENT_VER" "$LIBEVENT_MIN" "$LIBEVENT_NOTE"
check "leveldb"    "$LEVELDB_VER"  "$LEVELDB_MIN"  "$LEVELDB_NOTE"
check "zlib"       "$ZLIB_VER"     "$ZLIB_MIN"     "$ZLIB_NOTE"

# secp256k1: presence check only (Bitcoin Core fork, no versioned CVEs)
if [[ $JSON -eq 1 ]]; then
    RESULTS+=("{\"name\":\"libsecp256k1\",\"version\":\"legacy\",\"minimum\":\"hash-lock\",\"status\":\"$( [[ $SECP_STATUS == HASH_LOCKED ]] && echo PASS || echo FAIL )\",\"detail\":\"$SECP_STATUS — exact committed hash; replacement requires proven source\"}")
else
    if [[ "$SECP_STATUS" == "HASH_LOCKED" ]]; then
        printf "  %-20s %-14s %-10s \033[32m%-6s\033[0m %s\n" \
            "libsecp256k1" "legacy" "hash-lock" "PASS" \
            "exact manifest hash; source unresolved, replacement prohibited"
    else
        printf "  %-20s %-14s %-10s \033[31m%-6s\033[0m %s\n" \
            "libsecp256k1" "legacy" "hash-lock" "FAIL" "$SECP_STATUS"
    fi
fi

# librustzcash: exact source revision + required proving ABI.
if [[ $JSON -eq 1 ]]; then
    RESULTS+=("{\"name\":\"librustzcash\",\"version\":\"${RUSTZCASH_COMMIT:0:12}\",\"minimum\":\"canonical-pin\",\"status\":\"$( [[ $RUSTZCASH_STATUS == present ]] && echo PASS || echo FAIL )\",\"detail\":\"$RUSTZCASH_STATUS — source SHA256 and Cargo.lock enforced by build_vendor.sh; proving-only ABI\"}")
else
    if [[ "$RUSTZCASH_STATUS" == "present" ]]; then
        printf "  %-20s %-14s %-10s \033[32m%-6s\033[0m %s\n" \
            "librustzcash" "${RUSTZCASH_COMMIT:0:12}" "pinned" "PASS" \
            "source pin + Spend/Output proving ABI present"
    else
        printf "  %-20s %-14s %-10s \033[31m%-6s\033[0m %s\n" \
            "librustzcash" "${RUSTZCASH_COMMIT:0:12}" "pinned" "FAIL" \
            "$RUSTZCASH_STATUS"
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
    [[ $FAIL -eq 0 ]] || exit 1
else
    echo ""
    if [[ $FAIL -gt 0 ]]; then
        echo -e "\033[31m  ✗ $FAIL dependency provenance/version finding(s).\033[0m"
        echo "    Action: run make vendor, inspect pins, and re-run make audit."
        exit 1
    elif [[ $WARN -gt 0 ]]; then
        echo -e "\033[33m  ⚠ $WARN dependency version(s) could not be detected.\033[0m"
        exit 0
    else
        echo -e "\033[32m  ✓ All dependencies at or above minimum safe versions.\033[0m"
        exit 0
    fi
fi
