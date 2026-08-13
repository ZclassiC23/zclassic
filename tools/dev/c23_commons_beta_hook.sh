#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# purpose: Compose installed C23 package author/fetch/build/use over the shared DHT harness.

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    echo "c23-commons-beta: run make c23-commons-installed-acceptance" >&2
    exit 2
fi

beta_die() { dht_die "c23-commons-beta: $*"; }
beta_note() { dht_note "c23-commons-beta: $*"; }
beta_jget() {
    local expression="$1"
    python3 -c "import json,sys; d=json.load(sys.stdin); print($expression)"
}
beta_ok() {
    local label="$1" document="$2"
    [ "$(printf '%s' "$document" | beta_jget 'd.get("ok",False)' 2>/dev/null || true)" = True ] ||
        beta_die "$label failed: $document"
}
beta_native() {
    local role="$1"; shift
    dht_native "${DDS[$role]}" "${RPCS[$role]}" -regtest "$@"
}

BETA_NAMESPACE="zclassic23.package"
BETA_BASE_ROOT="7f15ba590a82de200152b1c02b5b1dc11b4932a9b690fbe332e7c2fa60d764fe"
BETA_SHA3_ROOT="ea54d7038792764c059a697792d46ee92fe75e29aa302d3c8db3a208a580876e"
BETA_PACKAGE_ROOT="401b0377f494937690ca0b2c240fbbf18b34ad519f132f0ed425882ec7bf9390"
BETA_EXPECTED_NOTE="3338be694f50c5f338814986cdf0686453a888b84f424d792af4b9202398f392"

BETA_A="${ORDER[0]}"
BETA_B="${ORDER[1]}"
BETA_C="${ORDER[2]}"
BETA_D="${ORDER[3]}"

beta_assert_installed_process() {
    local role="$1" exe cwd
    exe="$(readlink -f "/proc/${PIDS[$role]}/exe" 2>/dev/null || true)"
    cwd="$(readlink -f "/proc/${PIDS[$role]}/cwd" 2>/dev/null || true)"
    [ "$exe" = "$C23_BETA_INSTALL_BIN/zclassic23" ] ||
        beta_die "role $role is not running the installed node: $exe"
    case "$cwd" in
        "$C23_BETA_FIXTURE_SOURCE"|"$C23_BETA_FIXTURE_SOURCE"/*)
            beta_die "role $role inherited a repository working directory" ;;
    esac
}

beta_restart() {
    local role="$1" connect_role="${2:-}" pos=-1 i
    local connects=()
    for i in 0 1 2 3 4 5 6; do
        [ "${ORDER[$i]}" = "$role" ] && pos="$i"
    done
    [ "$pos" -ge 0 ] || beta_die "role $role is absent from sparse order"
    dht_kill_group "${PIDS[$role]:-}"
    PIDS[$role]=""
    if [ -n "$connect_role" ]; then
        connects=("127.0.0.1:${PORTS[$connect_role]}")
    elif [ "$pos" -eq 0 ]; then
        # The shared sparse proof deliberately leaves ORDER[0] on a dead
        # sink and relies on a later cold-discovery action.  This composed
        # journey needs an authenticated package peer immediately after each
        # policy/host restart, so reconnect the origin to its already-live
        # next neighbour before asserting authentication.
        connects=("127.0.0.1:${PORTS[${ORDER[1]}]}")
    else
        connects=("127.0.0.1:${PORTS[${ORDER[$((pos - 1))]}]}")
    fi
    dht_spawn "PIDS[$role]" "${DDS[$role]}" "${PORTS[$role]}" \
        "${RPCS[$role]}" "${FSPORTS[$role]}" \
        "${HTTPSPORTS[$role]}" "${connects[@]}"
    dht_wait_rpc "${DDS[$role]}" "${RPCS[$role]}" "${PIDS[$role]}" ||
        beta_die "role $role did not restart"
    dht_wait_auth "${DDS[$role]}" "${RPCS[$role]}" 1 ||
        beta_die "role $role did not reauthenticate"
    beta_assert_installed_process "$role"
}

beta_allow_package_policy() {
    local role="$1" common plan token commit
    common='"operation":"add","source":"local","effect":"allow","scope":"service_type","action_mask":63,"value":"'"$BETA_NAMESPACE"'"'
    plan="$(beta_native "$role" zcode network policy mutate \
        --input="{\"mode\":\"plan\",$common}")"
    beta_ok "role $role package-policy plan" "$plan"
    token="$(printf '%s' "$plan" | beta_jget 'd["data"]["plan_token"]')"
    commit="$(beta_native "$role" zcode network policy mutate \
        --input="{\"mode\":\"commit\",$common,\"plan_token\":\"$token\"}")"
    beta_ok "role $role package-policy commit" "$commit"
}

beta_prepare_fixture() {
    local root="$DHT_WORK/c23-beta-author" package="$DHT_WORK/c23-beta-author/package"
    mkdir -p "$root/dependencies" "$package/include/stranger" \
        "$package/src" "$package/tests" "$package/app"
    cp -a "$C23_BETA_FIXTURE_SOURCE/lib/base" "$root/dependencies/base"
    cp -a "$C23_BETA_FIXTURE_SOURCE/lib/sha3" "$root/dependencies/sha3"
    cat >"$package/LICENSE" <<'EOF'
MIT License

Copyright (c) 2026 C23 Commons Stranger Beta fixture author

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
EOF
    cat >"$package/include/stranger/note.h" <<'EOF'
/* Copyright 2026 C23 Commons Stranger Beta fixture author - MIT License
 * purpose: Render a SHA3-256 note identity as canonical lowercase hex. */

#ifndef STRANGER_NOTE_H
#define STRANGER_NOTE_H

#include <stdbool.h>
#include <stddef.h>

#define STRANGER_NOTE_HEX_SIZE 65u

bool stranger_note_digest(const char *text,
                          char out[static STRANGER_NOTE_HEX_SIZE]);

#endif
EOF
    cat >"$package/src/note.c" <<'EOF'
/* Copyright 2026 C23 Commons Stranger Beta fixture author - MIT License
 * purpose: Compose the Commons base codec and SHA3 package into note IDs. */

#include "stranger/note.h"

#include "base/hex.h"
#include "sha3/sha3.h"

#include <stdint.h>
#include <string.h>

bool stranger_note_digest(const char *text,
                          char out[static STRANGER_NOTE_HEX_SIZE])
{
    if (!text || !out)
        return false;
    uint8_t digest[SHA3_256_OUTPUT_SIZE];
    zcl_sha3_256((const unsigned char *)text, strlen(text), digest);
    zcl_hex_encode(digest, sizeof(digest), out);
    return true;
}
EOF
    cat >"$package/tests/test_note.c" <<'EOF'
/* Copyright 2026 C23 Commons Stranger Beta fixture author - MIT License */

#include "stranger/note.h"

#include <string.h>

int main(void)
{
    char digest[STRANGER_NOTE_HEX_SIZE];
    if (!stranger_note_digest("hello", digest) ||
        strcmp(digest,
               "3338be694f50c5f338814986cdf0686453a888b84f424d792af4b9202398f392") != 0)
        return 1;
    if (stranger_note_digest(NULL, digest))
        return 2;
    return 0;
}
EOF
    cat >"$package/app/main.c" <<'EOF'
/* Copyright 2026 C23 Commons Stranger Beta fixture author - MIT License */

#include "stranger/note.h"

#include <stdio.h>

int main(int argc, char **argv)
{
    const char *note = argc > 1 ? argv[1] : "hello";
    char digest[STRANGER_NOTE_HEX_SIZE];
    if (!stranger_note_digest(note, digest)) {
        fputs("could not derive note identity\n", stderr);
        return 1;
    }
    puts(digest);
    return 0;
}
EOF
    cat >"$package/zcode-package.json" <<EOF
{
  "schema": 1,
  "name": "stranger/sha3-note",
  "semver": "1.0.0",
  "language": "c23",
  "license": "MIT",
  "include_dir": "include",
  "source_dir": "src",
  "dependencies": [
    {
      "root": "$BETA_BASE_ROOT",
      "name": "zclassic23/base",
      "semver": "0.1.0-dev.1"
    },
    {
      "root": "$BETA_SHA3_ROOT",
      "name": "zclassic23/sha3",
      "semver": "0.1.0-dev.1"
    }
  ],
  "files": [
    "LICENSE",
    "app/main.c",
    "include/stranger/note.h",
    "src/note.c",
    "tests/test_note.c",
    "zcode-package.json"
  ]
}
EOF
    BETA_AUTHOR_ROOT="$root"
    BETA_PACKAGE_DIR="$package"
}

beta_prepare() {
    local dir="$1" pubkey="$2" sequence="$3"
    "$NODE_BIN" -regtest zcode package dev prepare \
        --input="{\"dir\":\"$dir\",\"publisher_pubkey\":\"$pubkey\",\"publisher_sequence\":$sequence,\"chain_id\":\"zclassic-regtest\"}" \
        2>/dev/null | tail -1
}

beta_seal_publish() {
    local label="$1" dir="$2" pubkey="$3" sequence="$4" signature="$5" day="$6"
    local prep body manifest recipe seal release plan commit root transport
    prep="$(beta_prepare "$dir" "$pubkey" "$sequence")"
    beta_ok "$label prepare" "$prep"
    body="$(printf '%s' "$prep" | beta_jget 'd["data"]["release_body_hex"]')"
    manifest="$(printf '%s' "$prep" | beta_jget 'd["data"]["manifest_hex"]')"
    recipe="$(printf '%s' "$prep" | beta_jget 'd["data"]["recipe_hex"]')"
    seal="$("$NODE_BIN" zcode package dev seal \
        --input="{\"release_body_hex\":\"$body\",\"signature_hex\":\"$signature\"}" \
        2>/dev/null | tail -1)"
    beta_ok "$label seal" "$seal"
    release="$(printf '%s' "$seal" | beta_jget 'd["data"]["release_hex"]')"
    plan="$(beta_native "$BETA_A" zcode create \
        --input="{\"mode\":\"plan\",\"release_hex\":\"$release\",\"manifest_hex\":\"$manifest\",\"recipe_hex\":\"$recipe\",\"dir\":\"$dir\",\"day\":$day}")"
    beta_ok "$label create plan" "$plan"
    commit="$(beta_native "$BETA_A" zcode create \
        --input="{\"mode\":\"commit\",\"release_hex\":\"$release\",\"manifest_hex\":\"$manifest\",\"recipe_hex\":\"$recipe\",\"dir\":\"$dir\",\"day\":$day}")"
    beta_ok "$label create commit" "$commit"
    root="$(printf '%s' "$commit" | beta_jget 'd["data"]["package_root"]')"
    transport="$(printf '%s' "$commit" | beta_jget 'd["data"]["transport_root"]')"
    printf '%s %s' "$root" "$transport"
}

beta_sign_package() {
    local prep="$1" key="$2" digest_file="$3" signature_file="$4"
    local digest signature
    digest="$(printf '%s' "$prep" | beta_jget 'd["data"]["release_signing_digest"]')"
    python3 - "$digest" "$digest_file" <<'PY'
import pathlib,sys
pathlib.Path(sys.argv[2]).write_bytes(bytes.fromhex(sys.argv[1]))
PY
    : >"$signature_file"
    chmod 0600 "$digest_file" "$signature_file"
    exec 7<"$key" 8<"$digest_file" 9>"$signature_file"
    "$C23_BETA_INSTALL_BIN/zclassic23-package-sign" --sign \
        --key-fd 7 --digest-fd 8 --signature-fd 9 ||
        beta_die "offline author signature failed"
    exec 7<&- 8<&- 9>&-
    signature="$(xxd -p -c 128 "$signature_file")"
    [ "${#signature}" -eq 128 ] || beta_die "offline signature is not compact"
    printf '%s' "$signature"
}

beta_publish_record() {
    local role="$1" kind="$2" root="$3" transport="$4" sequence="$5"
    local now expiry common plan token commit
    now="$(date +%s)"; expiry=$((now + 3600))
    common="\"kind\":\"$kind\",\"namespace\":\"$BETA_NAMESPACE\",\"transport_root\":\"$transport\",\"sequence\":$sequence,\"not_before\":$((now - 5)),\"expiry\":$expiry"
    [ "$kind" != pointer ] ||
        common="$common,\"semantic_root\":\"$root\""
    plan="$(beta_native "$role" zcode network publish \
        --input="{\"mode\":\"plan\",$common}" || true)"
    beta_ok "role $role $kind plan $root" "$plan"
    token="$(printf '%s' "$plan" | beta_jget 'd["data"]["plan_token"]')"
    commit="$(beta_native "$role" zcode network publish \
        --input="{\"mode\":\"commit\",$common,\"plan_token\":\"$token\"}" || true)"
    beta_ok "role $role $kind commit $root" "$commit"
}

beta_publish_package() {
    local role="$1" root="$2" transport="$3" sequence="$4"
    # The frozen DHT grammar keeps semantic selection and byte custody
    # separate: POINTER binds package root -> carrier root; PROVIDER names
    # an authenticated peer serving that exact carrier.
    beta_publish_record "$role" pointer "$root" "$transport" "$sequence"
    beta_publish_record "$role" provider "$root" "$transport" "$sequence"
}

beta_wait_complete() {
    local role="$1" root="$2" deadline out complete=False
    deadline=$(( $(date +%s) + ${C23_BETA_WAIT:-180} ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        out="$(beta_native "$role" zcode package pin \
            --input="{\"root\":\"$root\",\"mode\":\"plan\"}" || true)"
        complete="$(printf '%s' "$out" | beta_jget 'd.get("data",{}).get("package",{}).get("complete",False)' 2>/dev/null || true)"
        [ "$complete" = True ] && return 0
        sleep 1
    done
    return 1
}

beta_pin() {
    local role="$1" root="$2" plan token commit
    plan="$(beta_native "$role" zcode package pin \
        --input="{\"root\":\"$root\",\"mode\":\"plan\"}")"
    beta_ok "role $role pin plan $root" "$plan"
    token="$(printf '%s' "$plan" | beta_jget 'd["data"]["plan_token"]')"
    commit="$(beta_native "$role" zcode package pin \
        --input="{\"root\":\"$root\",\"mode\":\"commit\",\"plan_token\":\"$token\"}")"
    beta_ok "role $role pin commit $root" "$commit"
}

beta_fetch_pin() {
    local role="$1" root="$2" transport="$3" fetched imported
    fetched="$(beta_native "$role" zcode package fetch \
        --input="{\"root\":\"$transport\",\"namespace\":\"$BETA_NAMESPACE\",\"maximum_bytes\":268435456}")"
    beta_ok "role $role fetch $transport" "$fetched"
    [ "$(printf '%s' "$fetched" | beta_jget 'd["data"].get("live",False)')" = True ] ||
        beta_die "role $role did not route fetch through its live daemon"
    beta_wait_complete "$role" "$transport" ||
        beta_die "role $role did not complete $transport"
    imported="$(beta_native "$role" zcode package fetch \
        --input="{\"root\":\"$transport\",\"namespace\":\"$BETA_NAMESPACE\",\"maximum_bytes\":268435456}")"
    beta_ok "role $role import $transport" "$imported"
    [ "$(printf '%s' "$imported" | beta_jget 'd["data"].get("reconstructed",False)')" = True ] ||
        beta_die "role $role did not reconstruct signed carrier $transport"
    [ "$(printf '%s' "$imported" | beta_jget 'd["data"]["package_root"]')" = "$root" ] ||
        beta_die "role $role carrier mapped to the wrong package root"
    beta_pin "$role" "$transport"
    beta_pin "$role" "$root"
}

beta_fetch_graph() {
    local role="$1"
    beta_fetch_pin "$role" "$BETA_BASE_ROOT" "$BETA_BASE_TRANSPORT"
    beta_fetch_pin "$role" "$BETA_SHA3_ROOT" "$BETA_SHA3_TRANSPORT"
    beta_fetch_pin "$role" "$BETA_PACKAGE_ROOT" "$BETA_PACKAGE_TRANSPORT"
}

beta_build_graph() {
    local role="$1" now plan plan_id commit receipt
    now="$(date +%s)"
    plan="$(beta_native "$role" zcode use \
        --input="{\"name_or_root\":\"$BETA_PACKAGE_ROOT\",\"now_unix\":$now}")"
    beta_ok "role $role use plan" "$plan"
    [ "$(printf '%s' "$plan" | beta_jget 'd["data"]["step_count"]')" -eq 3 ] ||
        beta_die "role $role did not resolve the exact three-package DAG"
    plan_id="$(printf '%s' "$plan" | beta_jget 'd["data"]["plan_id"]')"
    commit="$(beta_native "$role" zcode use \
        --input="{\"plan_id\":\"$plan_id\",\"now_unix\":$((now + 1))}")"
    beta_ok "role $role use commit" "$commit"
    receipt="$(printf '%s' "$commit" | beta_jget \
        '[x["build_receipt_id"] for x in d["data"]["steps"] if x["root"]=="'"$BETA_PACKAGE_ROOT"'"][0]')"
    [ "${#receipt}" -eq 64 ] || beta_die "role $role omitted target receipt"
    printf '%s' "$receipt"
}

beta_note "installed roles: A=$BETA_A B=$BETA_B C=$BETA_C D=$BETA_D"
for role in "$BETA_A" "$BETA_B" "$BETA_C" "$BETA_D"; do
    beta_assert_installed_process "$role"
    beta_allow_package_policy "$role"
done
for role in "$BETA_A" "$BETA_B" "$BETA_C" "$BETA_D"; do
    beta_restart "$role"
done

beta_prepare_fixture
AUTHOR_KEY="$BETA_AUTHOR_ROOT/author.key"
AUTHOR_PUB="$($C23_BETA_INSTALL_BIN/zclassic23-package-sign --generate "$AUTHOR_KEY")"
[ "$(stat -c %a "$AUTHOR_KEY")" = 600 ] || beta_die "author key is not mode 600"

BASE_PREP="$(beta_prepare "$BETA_AUTHOR_ROOT/dependencies/base" "$AUTHOR_PUB" 1)"
beta_ok "base prepare for signature" "$BASE_PREP"
BASE_SIGNATURE="$(beta_sign_package "$BASE_PREP" "$AUTHOR_KEY" \
    "$BETA_AUTHOR_ROOT/base.digest" "$BETA_AUTHOR_ROOT/base.signature")"
read -r BASE_PUBLISHED BETA_BASE_TRANSPORT <<<"$(beta_seal_publish base "$BETA_AUTHOR_ROOT/dependencies/base" \
    "$AUTHOR_PUB" 1 "$BASE_SIGNATURE" 1)"
[ "$BASE_PUBLISHED" = "$BETA_BASE_ROOT" ] || beta_die "base fixture root drifted"

SHA3_PREP="$(beta_prepare "$BETA_AUTHOR_ROOT/dependencies/sha3" "$AUTHOR_PUB" 2)"
beta_ok "sha3 prepare for signature" "$SHA3_PREP"
SHA3_SIGNATURE="$(beta_sign_package "$SHA3_PREP" "$AUTHOR_KEY" \
    "$BETA_AUTHOR_ROOT/sha3.digest" "$BETA_AUTHOR_ROOT/sha3.signature")"
read -r SHA3_PUBLISHED BETA_SHA3_TRANSPORT <<<"$(beta_seal_publish sha3 "$BETA_AUTHOR_ROOT/dependencies/sha3" \
    "$AUTHOR_PUB" 2 "$SHA3_SIGNATURE" 8)"
[ "$SHA3_PUBLISHED" = "$BETA_SHA3_ROOT" ] || beta_die "sha3 fixture root drifted"

PACKAGE_PREP="$(beta_prepare "$BETA_PACKAGE_DIR" "$AUTHOR_PUB" 3)"
beta_ok "outside package prepare" "$PACKAGE_PREP"
PACKAGE_SIGNATURE="$(beta_sign_package "$PACKAGE_PREP" "$AUTHOR_KEY" \
    "$BETA_AUTHOR_ROOT/release.digest" "$BETA_AUTHOR_ROOT/release.signature")"
read -r PACKAGE_PUBLISHED BETA_PACKAGE_TRANSPORT <<<"$(beta_seal_publish outside "$BETA_PACKAGE_DIR" \
    "$AUTHOR_PUB" 3 "$PACKAGE_SIGNATURE" 15)"
[ "$PACKAGE_PUBLISHED" = "$BETA_PACKAGE_ROOT" ] ||
    beta_die "outside package root drifted: $PACKAGE_PUBLISHED"

# Restart A so its ordinary package-host engine loads the dynamically
# published stores. The package is absent from the compiled registry.
! grep -q "$BETA_PACKAGE_ROOT" "$C23_BETA_FIXTURE_SOURCE/config/zcode_package_registry.def" ||
    beta_die "outside package leaked into the compiled registry"
beta_restart "$BETA_A"
beta_publish_package "$BETA_A" "$BETA_BASE_ROOT" "$BETA_BASE_TRANSPORT" 1
beta_publish_package "$BETA_A" "$BETA_SHA3_ROOT" "$BETA_SHA3_TRANSPORT" 1
beta_publish_package "$BETA_A" "$BETA_PACKAGE_ROOT" "$BETA_PACKAGE_TRANSPORT" 1

beta_note "B fetches exact graph without execution"
beta_fetch_graph "$BETA_B"
[ ! -e "${DDS[$BETA_B]}/zcode/installed" ] ||
    beta_die "B built or installed package bytes during fetch"
B_SEARCH="$(beta_native "$BETA_B" zcode package search \
    --input='{"name_prefix":"stranger/sha3-note","limit":4}')"
beta_ok "B local verified-package search" "$B_SEARCH"
[ "$(printf '%s' "$B_SEARCH" | beta_jget 'd["data"]["total_matches"]')" -eq 1 ] ||
    beta_die "B could not inspect the fetched signed release"

beta_note "C and D explicitly build/test the same exact graph"
beta_fetch_graph "$BETA_C"
beta_fetch_graph "$BETA_D"
C_RECEIPT="$(beta_build_graph "$BETA_C")"
D_RECEIPT="$(beta_build_graph "$BETA_D")"
[ "$C_RECEIPT" = "$D_RECEIPT" ] ||
    beta_die "independent deterministic build receipts disagree"
C_ARCHIVE="${DDS[$BETA_C]}/zcode/installed/$BETA_PACKAGE_ROOT/lib/libsha3-note.a"
D_ARCHIVE="${DDS[$BETA_D]}/zcode/installed/$BETA_PACKAGE_ROOT/lib/libsha3-note.a"
[ -f "$C_ARCHIVE" ] && [ -f "$D_ARCHIVE" ] || beta_die "target archive missing"
C_ARTIFACT="$(openssl dgst -sha3-256 "$C_ARCHIVE" | awk '{print $NF}')"
D_ARTIFACT="$(openssl dgst -sha3-256 "$D_ARCHIVE" | awk '{print $NF}')"
[ "$C_ARTIFACT" = "$D_ARTIFACT" ] || beta_die "C/D archive roots disagree"

beta_publish_package "$BETA_D" "$BETA_BASE_ROOT" "$BETA_BASE_TRANSPORT" 1
beta_publish_package "$BETA_D" "$BETA_SHA3_ROOT" "$BETA_SHA3_TRANSPORT" 1
beta_publish_package "$BETA_D" "$BETA_PACKAGE_ROOT" "$BETA_PACKAGE_TRANSPORT" 1
dht_kill_group "${PIDS[$BETA_A]}"; PIDS[$BETA_A]=""

# Remove only B's temporary package-store projections, preserving its DHT
# identity and common chain. The move is recoverable inside this run and makes
# the next fetch prove D serves onward after A has disappeared.
dht_kill_group "${PIDS[$BETA_B]}"; PIDS[$BETA_B]=""
B_BACKUP="$DHT_WORK/b-pre-disappearance-package-store"
mkdir -p "$B_BACKUP"
for name in active addplans attestations badges cas downloads generations \
        installed manifests pins receipts recipes releases staging; do
    [ ! -e "${DDS[$BETA_B]}/zcode/$name" ] ||
        mv "${DDS[$BETA_B]}/zcode/$name" "$B_BACKUP/$name"
done
beta_restart "$BETA_B" "$BETA_D"
beta_fetch_graph "$BETA_B"
B_AFTER="$(beta_native "$BETA_B" zcode package search \
    --input='{"name_prefix":"stranger/sha3-note","limit":4}')"
beta_ok "B post-publisher search" "$B_AFTER"
[ "$(printf '%s' "$B_AFTER" | beta_jget 'd["data"]["total_matches"]')" -eq 1 ] ||
    beta_die "D did not preserve the graph after publisher disappearance"

# Reconstruct the package source inertly, then link the standalone application
# only after D's explicit build/test acceptance produced installed artifacts.
APP_SOURCE="$DHT_WORK/standalone-source"
CHECKOUT="$(beta_native "$BETA_D" zcode package checkout \
    --input="{\"root\":\"$BETA_PACKAGE_ROOT\",\"destination\":\"$APP_SOURCE\"}")"
beta_ok "D inert package checkout" "$CHECKOUT"
[ ! -e "$APP_SOURCE/.git" ] || beta_die "checkout materialized Git metadata"
APP_BIN="$DHT_WORK/sha3-note"
cc -std=c23 -O2 -Wall -Wextra -Werror \
    -I"${DDS[$BETA_D]}/zcode/installed/$BETA_PACKAGE_ROOT/include" \
    -I"${DDS[$BETA_D]}/zcode/installed/$BETA_BASE_ROOT/include" \
    -I"${DDS[$BETA_D]}/zcode/installed/$BETA_SHA3_ROOT/include" \
    "$APP_SOURCE/app/main.c" \
    "${DDS[$BETA_D]}/zcode/installed/$BETA_PACKAGE_ROOT/lib/libsha3-note.a" \
    "${DDS[$BETA_D]}/zcode/installed/$BETA_SHA3_ROOT/lib/libsha3.a" \
    "${DDS[$BETA_D]}/zcode/installed/$BETA_BASE_ROOT/lib/libbase.a" \
    -o "$APP_BIN"
APP_OUTPUT="$("$APP_BIN" hello)"
[ "$APP_OUTPUT" = "$BETA_EXPECTED_NOTE" ] || beta_die "standalone app output drifted"

printf '%s\n' "{\"schema\":\"zcl.c23_commons_beta_installed.v1\",\"verdict\":\"PASS\",\"installed_binary\":\"$C23_BETA_INSTALL_BIN/zclassic23\",\"repository_source_used_by_consumers\":false,\"package_root\":\"$BETA_PACKAGE_ROOT\",\"dependency_roots\":[\"$BETA_BASE_ROOT\",\"$BETA_SHA3_ROOT\"],\"author_pubkey\":\"$AUTHOR_PUB\",\"build_receipt_id\":\"$C_RECEIPT\",\"artifact_root\":\"$C_ARTIFACT\",\"fetch_inert\":true,\"explicit_builds\":2,\"publisher_disappearance_survived\":true,\"standalone_output\":\"$APP_OUTPUT\"}"
