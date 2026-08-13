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
BETA_SECOND_ROOT_EXPECTED="263a53e35626a3e4b0a25c8a3f6f70b478872484f037380d4e8cd4b977935d26"
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
    local role="$1" pos=-1 i connect_role
    shift
    local connect_roles=("$@")
    local connects=()
    for i in 0 1 2 3 4 5 6; do
        [ "${ORDER[$i]}" = "$role" ] && pos="$i"
    done
    [ "$pos" -ge 0 ] || beta_die "role $role is absent from sparse order"
    dht_kill_group "${PIDS[$role]:-}"
    PIDS[$role]=""
    if [ "${#connect_roles[@]}" -gt 0 ]; then
        for connect_role in "${connect_roles[@]}"; do
            connects+=("127.0.0.1:${PORTS[$connect_role]}")
        done
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
    dht_wait_auth "${DDS[$role]}" "${RPCS[$role]}" "${#connects[@]}" ||
        beta_die "role $role did not reauthenticate"
    beta_assert_installed_process "$role"
}

beta_reset_package_store() {
    local role="$1" backup_name="$2" name
    shift 2
    local backup="$DHT_WORK/$backup_name"
    dht_kill_group "${PIDS[$role]:-}"
    PIDS[$role]=""
    mkdir -p "$backup"
    for name in active addplans attestations badges cas downloads generations \
            installed manifests pins receipts recipes releases staging; do
        [ ! -e "${DDS[$role]}/zcode/$name" ] ||
            mv "${DDS[$role]}/zcode/$name" "$backup/$name"
    done
    beta_restart "$role" "$@"
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
    local second_root="$DHT_WORK/c23-beta-second-author"
    local second_package="$second_root/package"
    mkdir -p "$root/dependencies" "$package/include/stranger" \
        "$package/src" "$package/tests" "$package/app" \
        "$second_package/include/visitor" "$second_package/src" \
        "$second_package/tests" "$second_package/app"
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
    cat >"$second_package/LICENSE" <<'EOF'
MIT License

Copyright (c) 2026 C23 Commons Second Stranger Beta fixture author

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
    cat >"$second_package/include/visitor/hex_frame.h" <<'EOF'
/* Copyright 2026 C23 Commons Second Stranger Beta fixture author - MIT License
 * purpose: Render a bounded byte string as framed lowercase hexadecimal. */

#ifndef VISITOR_HEX_FRAME_H
#define VISITOR_HEX_FRAME_H

#include <stdbool.h>
#include <stddef.h>

#define VISITOR_HEX_FRAME_MAX_INPUT 32u
#define VISITOR_HEX_FRAME_OUTPUT_SIZE \
    (VISITOR_HEX_FRAME_MAX_INPUT * 2u + 3u)

bool visitor_hex_frame(const unsigned char *bytes, size_t bytes_len,
                       char *out, size_t out_size);

#endif
EOF
    cat >"$second_package/src/hex_frame.c" <<'EOF'
/* Copyright 2026 C23 Commons Second Stranger Beta fixture author - MIT License
 * purpose: Compose the Commons base codec into a small portable formatter. */

#include "visitor/hex_frame.h"

#include "base/hex.h"

bool visitor_hex_frame(const unsigned char *bytes, size_t bytes_len,
                       char *out, size_t out_size)
{
    if ((!bytes && bytes_len != 0u) || !out ||
        bytes_len > VISITOR_HEX_FRAME_MAX_INPUT ||
        out_size < bytes_len * 2u + 3u)
        return false;
    out[0] = '[';
    zcl_hex_encode(bytes, bytes_len, out + 1);
    out[bytes_len * 2u + 1u] = ']';
    out[bytes_len * 2u + 2u] = '\0';
    return true;
}
EOF
    cat >"$second_package/tests/test_hex_frame.c" <<'EOF'
/* Copyright 2026 C23 Commons Second Stranger Beta fixture author - MIT License */

#include "visitor/hex_frame.h"

#include <string.h>

int main(void)
{
    static const unsigned char bytes[] = { 0x00, 0xff, 0x23 };
    char out[VISITOR_HEX_FRAME_OUTPUT_SIZE];
    if (!visitor_hex_frame(bytes, sizeof(bytes), out, sizeof(out)) ||
        strcmp(out, "[00ff23]") != 0)
        return 1;
    if (visitor_hex_frame(NULL, 1u, out, sizeof(out)) ||
        visitor_hex_frame(bytes, sizeof(bytes), out, 4u))
        return 2;
    if (!visitor_hex_frame(NULL, 0u, out, sizeof(out)) ||
        strcmp(out, "[]") != 0)
        return 3;
    return 0;
}
EOF
    cat >"$second_package/app/main.c" <<'EOF'
/* Copyright 2026 C23 Commons Second Stranger Beta fixture author - MIT License */

#include "visitor/hex_frame.h"

#include <stdio.h>

int main(void)
{
    static const unsigned char bytes[] = { 0xc0, 0x23 };
    char out[VISITOR_HEX_FRAME_OUTPUT_SIZE];
    if (!visitor_hex_frame(bytes, sizeof(bytes), out, sizeof(out)))
        return 1;
    puts(out);
    return 0;
}
EOF
    cat >"$second_package/zcode-package.json" <<EOF
{
  "schema": 1,
  "name": "visitor/hex-frame",
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
    }
  ],
  "files": [
    "LICENSE",
    "app/main.c",
    "include/visitor/hex_frame.h",
    "src/hex_frame.c",
    "tests/test_hex_frame.c",
    "zcode-package.json"
  ]
}
EOF
    BETA_AUTHOR_ROOT="$root"
    BETA_PACKAGE_DIR="$package"
    BETA_SECOND_AUTHOR_ROOT="$second_root"
    BETA_SECOND_PACKAGE_DIR="$second_package"
}

beta_prepare() {
    local dir="$1" pubkey="$2" sequence="$3"
    "$NODE_BIN" -regtest zcode package dev prepare \
        --input="{\"dir\":\"$dir\",\"publisher_pubkey\":\"$pubkey\",\"publisher_sequence\":$sequence,\"chain_id\":\"zclassic-regtest\"}" \
        2>/dev/null | tail -1
}

beta_seal_publish() {
    local role="$1" label="$2" dir="$3" pubkey="$4" sequence="$5" signature="$6" day="$7"
    local prep body manifest recipe seal release release_id plan commit root transport
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
    release_id="$(printf '%s' "$seal" | beta_jget 'd["data"]["release_id"]')"
    plan="$(beta_native "$role" zcode create \
        --input="{\"mode\":\"plan\",\"release_hex\":\"$release\",\"manifest_hex\":\"$manifest\",\"recipe_hex\":\"$recipe\",\"dir\":\"$dir\",\"day\":$day}")"
    beta_ok "$label create plan" "$plan"
    commit="$(beta_native "$role" zcode create \
        --input="{\"mode\":\"commit\",\"release_hex\":\"$release\",\"manifest_hex\":\"$manifest\",\"recipe_hex\":\"$recipe\",\"dir\":\"$dir\",\"day\":$day}")"
    beta_ok "$label create commit" "$commit"
    root="$(printf '%s' "$commit" | beta_jget 'd["data"]["package_root"]')"
    transport="$(printf '%s' "$commit" | beta_jget 'd["data"]["transport_root"]')"
    printf '%s %s %s' "$root" "$transport" "$release_id"
}

beta_publish_version() {
    local role="$1" label="$2" dir="$3" sequence="$4" day="$5"
    beta_publish_signed_dir "$role" "$label" "$dir" "$AUTHOR_PUB" \
        "$AUTHOR_KEY" "$sequence" "$day"
}

beta_publish_signed_dir() {
    local role="$1" label="$2" dir="$3" pubkey="$4" key="$5"
    local sequence="$6" day="$7"
    local prep signature
    prep="$(beta_prepare "$dir" "$pubkey" "$sequence")"
    beta_ok "$label prepare" "$prep"
    signature="$(beta_sign_package "$prep" "$key")"
    read -r BETA_VERSION_ROOT BETA_VERSION_TRANSPORT BETA_VERSION_RELEASE_ID \
        <<<"$(beta_seal_publish "$role" "$label" "$dir" "$pubkey" \
            "$sequence" "$signature" "$day")"
    BETA_VERSION_RECIPE_ROOT="$(printf '%s' "$prep" | beta_jget \
        'd["data"]["recipe_root"]')"
    BETA_VERSION_LOCK_ROOT="$(printf '%s' "$prep" | beta_jget \
        'd["data"]["dependency_lock_root"]')"
    BETA_VERSION_API_ROOT="$(printf '%s' "$prep" | beta_jget \
        'd["data"]["api_capsule_root"]')"
}

beta_sign_package() {
    local prep="$1" key="$2"
    local digest signature
    digest="$(printf '%s' "$prep" | beta_jget 'd["data"]["release_signing_digest"]')"
    # The digest and signature are public release material. Keep only the
    # private key off argv via a mode-0600 descriptor; the installed signer
    # validates canonical lowercase hex and prints the public signature.
    exec 7<"$key"
    signature="$("$C23_BETA_INSTALL_BIN/zclassic23-package-sign" \
        --sign-digest "$digest" --key-fd 7)" ||
        beta_die "offline author signature failed"
    exec 7<&-
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
    [ "$(printf '%s' "$imported" | beta_jget 'd["data"]["download"]["state"]')" = complete ] ||
        beta_die "role $role omitted the completed download receipt"
    BETA_FETCH_REQUESTED_OBJECTS="$(printf '%s' "$imported" | beta_jget 'd["data"]["download"]["requested_objects"]')"
    BETA_FETCH_TRANSFERRED_OBJECTS="$(printf '%s' "$imported" | beta_jget 'd["data"]["download"]["transferred_objects"]')"
    BETA_FETCH_REUSED_OBJECTS="$(printf '%s' "$imported" | beta_jget 'd["data"]["download"]["reused_objects"]')"
    BETA_FETCH_REQUESTED_BYTES="$(printf '%s' "$imported" | beta_jget 'd["data"]["download"]["requested_bytes"]')"
    BETA_FETCH_TRANSFERRED_BYTES="$(printf '%s' "$imported" | beta_jget 'd["data"]["download"]["transferred_bytes"]')"
    BETA_FETCH_REUSED_BYTES="$(printf '%s' "$imported" | beta_jget 'd["data"]["download"]["reused_bytes"]')"
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
    local role="$1" root="$2" now plan plan_id commit actual expected
    shift 2
    [ "$#" -gt 0 ] || beta_die "role $role build graph has no expected roots"
    now="$(date +%s)"
    plan="$(beta_native "$role" zcode use \
        --input="{\"name_or_root\":\"$root\",\"now_unix\":$now}")"
    beta_ok "role $role use plan" "$plan"
    [ "$(printf '%s' "$plan" | beta_jget 'd["data"]["step_count"]')" -eq "$#" ] ||
        beta_die "role $role did not resolve the expected $#-package DAG"
    actual="$(printf '%s' "$plan" | beta_jget \
        '" ".join(sorted(x["root"] for x in d["data"]["steps"]))')"
    expected="$(printf '%s\n' "$@" | sort | paste -sd' ' -)"
    [ "$actual" = "$expected" ] ||
        beta_die "role $role dependency plan substituted an unpinned root"
    plan_id="$(printf '%s' "$plan" | beta_jget 'd["data"]["plan_id"]')"
    commit="$(beta_native "$role" zcode use \
        --input="{\"plan_id\":\"$plan_id\",\"now_unix\":$((now + 1))}")"
    beta_ok "role $role use commit" "$commit"
    BETA_BUILD_RECEIPT="$(printf '%s' "$commit" | beta_jget \
        '[x["build_receipt_id"] for x in d["data"]["steps"] if x["root"]=="'"$root"'"][0]')"
    [ "${#BETA_BUILD_RECEIPT}" -eq 64 ] ||
        beta_die "role $role omitted target receipt"
    BETA_BUILD_REBUILT="$(printf '%s' "$commit" | beta_jget \
        'sum(1 for x in d["data"]["steps"] if not x["already_installed"])')"
    BETA_BUILD_REUSED="$(printf '%s' "$commit" | beta_jget \
        'sum(1 for x in d["data"]["steps"] if x["already_installed"])')"
    BETA_BUILD_EVIDENCE_REUSED="$(printf '%s' "$commit" | beta_jget \
        'sum(1 for x in d["data"]["steps"] if x["receipt_reused"])')"
    BETA_BUILD_ACTIVE_ROOT="$(printf '%s' "$commit" | beta_jget \
        'd["data"]["active_root"]')"
    BETA_BUILD_RESULT="$commit"
    [ "$BETA_BUILD_ACTIVE_ROOT" = "$root" ] ||
        beta_die "role $role activated a root other than the exact request"
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
BASE_SIGNATURE="$(beta_sign_package "$BASE_PREP" "$AUTHOR_KEY")"
read -r BASE_PUBLISHED BETA_BASE_TRANSPORT BASE_RELEASE_ID <<<"$(beta_seal_publish "$BETA_A" base "$BETA_AUTHOR_ROOT/dependencies/base" \
    "$AUTHOR_PUB" 1 "$BASE_SIGNATURE" 1)"
[ "$BASE_PUBLISHED" = "$BETA_BASE_ROOT" ] || beta_die "base fixture root drifted"

SHA3_PREP="$(beta_prepare "$BETA_AUTHOR_ROOT/dependencies/sha3" "$AUTHOR_PUB" 2)"
beta_ok "sha3 prepare for signature" "$SHA3_PREP"
SHA3_SIGNATURE="$(beta_sign_package "$SHA3_PREP" "$AUTHOR_KEY")"
read -r SHA3_PUBLISHED BETA_SHA3_TRANSPORT SHA3_RELEASE_ID <<<"$(beta_seal_publish "$BETA_A" sha3 "$BETA_AUTHOR_ROOT/dependencies/sha3" \
    "$AUTHOR_PUB" 2 "$SHA3_SIGNATURE" 8)"
[ "$SHA3_PUBLISHED" = "$BETA_SHA3_ROOT" ] || beta_die "sha3 fixture root drifted"

PACKAGE_PREP="$(beta_prepare "$BETA_PACKAGE_DIR" "$AUTHOR_PUB" 3)"
beta_ok "outside package prepare" "$PACKAGE_PREP"
BETA_V1_RECIPE_ROOT="$(printf '%s' "$PACKAGE_PREP" | beta_jget \
    'd["data"]["recipe_root"]')"
BETA_V1_LOCK_ROOT="$(printf '%s' "$PACKAGE_PREP" | beta_jget \
    'd["data"]["dependency_lock_root"]')"
BETA_V1_API_ROOT="$(printf '%s' "$PACKAGE_PREP" | beta_jget \
    'd["data"]["api_capsule_root"]')"
PACKAGE_SIGNATURE="$(beta_sign_package "$PACKAGE_PREP" "$AUTHOR_KEY")"
read -r PACKAGE_PUBLISHED BETA_PACKAGE_TRANSPORT PACKAGE_RELEASE_ID <<<"$(beta_seal_publish "$BETA_A" outside "$BETA_PACKAGE_DIR" \
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

# The same ordinary node is now an unrelated author. Its private key and
# signed release identity are independent of A, while the package graph pins
# the already-published base root exactly. Nothing enters the compiled
# registry and no package-specific production handler is involved.
beta_note "B independently authors a second outside-tree package"
SECOND_AUTHOR_KEY="$BETA_SECOND_AUTHOR_ROOT/author.key"
SECOND_AUTHOR_PUB="$($C23_BETA_INSTALL_BIN/zclassic23-package-sign \
    --generate "$SECOND_AUTHOR_KEY")"
[ "$(stat -c %a "$SECOND_AUTHOR_KEY")" = 600 ] &&
[ "$SECOND_AUTHOR_PUB" != "$AUTHOR_PUB" ] ||
    beta_die "second package did not use an independent mode-600 author key"
beta_publish_signed_dir "$BETA_B" second-outside "$BETA_SECOND_PACKAGE_DIR" \
    "$SECOND_AUTHOR_PUB" "$SECOND_AUTHOR_KEY" 1 18
BETA_SECOND_ROOT="$BETA_VERSION_ROOT"
BETA_SECOND_TRANSPORT="$BETA_VERSION_TRANSPORT"
BETA_SECOND_RELEASE_ID="$BETA_VERSION_RELEASE_ID"
BETA_SECOND_RECIPE_ROOT="$BETA_VERSION_RECIPE_ROOT"
BETA_SECOND_LOCK_ROOT="$BETA_VERSION_LOCK_ROOT"
BETA_SECOND_API_ROOT="$BETA_VERSION_API_ROOT"
[ "$BETA_SECOND_ROOT" = "$BETA_SECOND_ROOT_EXPECTED" ] &&
[ "$BETA_SECOND_ROOT" != "$BETA_PACKAGE_ROOT" ] &&
[ "$BETA_SECOND_ROOT" != "$BETA_BASE_ROOT" ] &&
[ "$BETA_SECOND_ROOT" != "$BETA_SHA3_ROOT" ] ||
    beta_die "second package exact root drifted: $BETA_SECOND_ROOT"
! grep -q "$BETA_SECOND_ROOT" \
    "$C23_BETA_FIXTURE_SOURCE/config/zcode_package_registry.def" ||
    beta_die "second outside package leaked into the compiled registry"
beta_restart "$BETA_B" "$BETA_A"
beta_publish_package "$BETA_B" "$BETA_SECOND_ROOT" \
    "$BETA_SECOND_TRANSPORT" 1

beta_note "C and D explicitly build/test the same exact graph"
beta_fetch_graph "$BETA_C"
C_V1_TRANSFERRED_OBJECTS="$BETA_FETCH_TRANSFERRED_OBJECTS"
C_V1_TRANSFERRED_BYTES="$BETA_FETCH_TRANSFERRED_BYTES"
beta_fetch_graph "$BETA_D"
beta_build_graph "$BETA_C" "$BETA_PACKAGE_ROOT" "$BETA_BASE_ROOT" \
    "$BETA_SHA3_ROOT" "$BETA_PACKAGE_ROOT"
C_RECEIPT="$BETA_BUILD_RECEIPT"
C_BASE_RECEIPT="$(printf '%s' "$BETA_BUILD_RESULT" | beta_jget \
    '[x["build_receipt_id"] for x in d["data"]["steps"] if x["root"]=="'"$BETA_BASE_ROOT"'"][0]')"
[ "$BETA_BUILD_REBUILT" -eq 3 ] && [ "$BETA_BUILD_REUSED" -eq 0 ] &&
[ "$BETA_BUILD_EVIDENCE_REUSED" -eq 0 ] ||
    beta_die "C's cold graph build did not execute exactly three packages"
beta_build_graph "$BETA_D" "$BETA_PACKAGE_ROOT" "$BETA_BASE_ROOT" \
    "$BETA_SHA3_ROOT" "$BETA_PACKAGE_ROOT"
D_RECEIPT="$BETA_BUILD_RECEIPT"
[ "$BETA_BUILD_REBUILT" -eq 3 ] && [ "$BETA_BUILD_REUSED" -eq 0 ] &&
[ "$BETA_BUILD_EVIDENCE_REUSED" -eq 0 ] ||
    beta_die "D's cold graph build did not execute exactly three packages"
[ "$C_RECEIPT" = "$D_RECEIPT" ] ||
    beta_die "independent deterministic build receipts disagree"
C_ARCHIVE="${DDS[$BETA_C]}/zcode/installed/$BETA_PACKAGE_ROOT/lib/libsha3-note.a"
D_ARCHIVE="${DDS[$BETA_D]}/zcode/installed/$BETA_PACKAGE_ROOT/lib/libsha3-note.a"
[ -f "$C_ARCHIVE" ] && [ -f "$D_ARCHIVE" ] || beta_die "target archive missing"
C_ARTIFACT="$(openssl dgst -sha3-256 "$C_ARCHIVE" | awk '{print $NF}')"
D_ARTIFACT="$(openssl dgst -sha3-256 "$D_ARCHIVE" | awk '{print $NF}')"
[ "$C_ARTIFACT" = "$D_ARTIFACT" ] || beta_die "C/D archive roots disagree"

beta_note "C reuses one exact base build for two downstream packages"
beta_fetch_pin "$BETA_C" "$BETA_SECOND_ROOT" "$BETA_SECOND_TRANSPORT"
SECOND_TRANSFERRED_OBJECTS="$BETA_FETCH_TRANSFERRED_OBJECTS"
SECOND_TRANSFERRED_BYTES="$BETA_FETCH_TRANSFERRED_BYTES"
beta_build_graph "$BETA_C" "$BETA_SECOND_ROOT" "$BETA_BASE_ROOT" \
    "$BETA_SECOND_ROOT"
SECOND_RECEIPT="$BETA_BUILD_RECEIPT"
SECOND_BASE_RECEIPT="$(printf '%s' "$BETA_BUILD_RESULT" | beta_jget \
    '[x["build_receipt_id"] for x in d["data"]["steps"] if x["root"]=="'"$BETA_BASE_ROOT"'"][0]')"
[ "$BETA_BUILD_REBUILT" -eq 1 ] && [ "$BETA_BUILD_REUSED" -eq 1 ] &&
[ "$BETA_BUILD_EVIDENCE_REUSED" -eq 1 ] &&
[ "$C_BASE_RECEIPT" = "$SECOND_BASE_RECEIPT" ] ||
    beta_die "second package did not reuse C's exact base artifact evidence"
SECOND_ARCHIVE="${DDS[$BETA_C]}/zcode/installed/$BETA_SECOND_ROOT/lib/libhex-frame.a"
C_BASE_ARCHIVE="${DDS[$BETA_C]}/zcode/installed/$BETA_BASE_ROOT/lib/libbase.a"
[ -f "$SECOND_ARCHIVE" ] && [ -f "$C_BASE_ARCHIVE" ] ||
    beta_die "second package or shared base archive missing"
SECOND_ARTIFACT="$(openssl dgst -sha3-256 "$SECOND_ARCHIVE" | awk '{print $NF}')"
C_BASE_ARTIFACT="$(openssl dgst -sha3-256 "$C_BASE_ARCHIVE" | awk '{print $NF}')"
FIRST_SHARED_SOURCE="$DHT_WORK/first-shared-standalone-source"
FIRST_SHARED_CHECKOUT="$(beta_native "$BETA_C" zcode package checkout \
    --input="{\"root\":\"$BETA_PACKAGE_ROOT\",\"destination\":\"$FIRST_SHARED_SOURCE\"}")"
beta_ok "C inert first-package checkout" "$FIRST_SHARED_CHECKOUT"
FIRST_SHARED_APP_BIN="$DHT_WORK/sha3-note-shared-base"
cc -std=c23 -O2 -march=x86-64 -mtune=generic -Wall -Wextra -Werror \
    -I"${DDS[$BETA_C]}/zcode/installed/$BETA_PACKAGE_ROOT/include" \
    -I"${DDS[$BETA_C]}/zcode/installed/$BETA_BASE_ROOT/include" \
    -I"${DDS[$BETA_C]}/zcode/installed/$BETA_SHA3_ROOT/include" \
    "$FIRST_SHARED_SOURCE/app/main.c" \
    "${DDS[$BETA_C]}/zcode/installed/$BETA_PACKAGE_ROOT/lib/libsha3-note.a" \
    "${DDS[$BETA_C]}/zcode/installed/$BETA_SHA3_ROOT/lib/libsha3.a" \
    "$C_BASE_ARCHIVE" -o "$FIRST_SHARED_APP_BIN"
FIRST_SHARED_APP_OUTPUT="$("$FIRST_SHARED_APP_BIN" hello)"
[ "$FIRST_SHARED_APP_OUTPUT" = "$BETA_EXPECTED_NOTE" ] ||
    beta_die "first shared-dependency application output drifted"
SECOND_SOURCE="$DHT_WORK/second-standalone-source"
SECOND_CHECKOUT="$(beta_native "$BETA_C" zcode package checkout \
    --input="{\"root\":\"$BETA_SECOND_ROOT\",\"destination\":\"$SECOND_SOURCE\"}")"
beta_ok "C inert second-package checkout" "$SECOND_CHECKOUT"
[ ! -e "$SECOND_SOURCE/.git" ] ||
    beta_die "second checkout materialized Git metadata"
SECOND_APP_BIN="$DHT_WORK/hex-frame"
cc -std=c23 -O2 -march=x86-64 -mtune=generic -Wall -Wextra -Werror \
    -I"${DDS[$BETA_C]}/zcode/installed/$BETA_SECOND_ROOT/include" \
    -I"${DDS[$BETA_C]}/zcode/installed/$BETA_BASE_ROOT/include" \
    "$SECOND_SOURCE/app/main.c" "$SECOND_ARCHIVE" "$C_BASE_ARCHIVE" \
    -o "$SECOND_APP_BIN"
SECOND_APP_OUTPUT="$("$SECOND_APP_BIN")"
[ "$SECOND_APP_OUTPUT" = '[c023]' ] ||
    beta_die "second standalone application output drifted"
PACKAGE_CPU_RUNTIME_PROOF="compiler-baseline"
if command -v qemu-x86_64 >/dev/null 2>&1; then
    [ "$(qemu-x86_64 -cpu qemu64 "$FIRST_SHARED_APP_BIN" hello)" = \
        "$BETA_EXPECTED_NOTE" ] &&
    [ "$(qemu-x86_64 -cpu qemu64 "$SECOND_APP_BIN")" = '[c023]' ] ||
        beta_die "installed package failed the qemu64 CPU-floor proof"
    PACKAGE_CPU_RUNTIME_PROOF="qemu64"
fi

# C holds the independently verified second carrier, while D already holds the
# original three-root graph. Restarting both ordinary package hosts gives B a
# mixed-provider graph through authenticated C and D peers. Keeping each
# exact carrier with the node that proves it also leaves publication headroom
# for the existing v2/v3 update story.
beta_restart "$BETA_C" "$BETA_D"
beta_publish_package "$BETA_C" "$BETA_SECOND_ROOT" \
    "$BETA_SECOND_TRANSPORT" 1
beta_publish_package "$BETA_D" "$BETA_BASE_ROOT" "$BETA_BASE_TRANSPORT" 1
beta_publish_package "$BETA_D" "$BETA_SHA3_ROOT" "$BETA_SHA3_TRANSPORT" 1
beta_publish_package "$BETA_D" "$BETA_PACKAGE_ROOT" "$BETA_PACKAGE_TRANSPORT" 1
dht_kill_group "${PIDS[$BETA_A]}"; PIDS[$BETA_A]=""

# Remove only B's temporary package-store projections, preserving its DHT
# identity and common chain. The move is recoverable inside this run and makes
# the next fetch prove C and D serve their exact carriers onward after A has
# disappeared.
beta_reset_package_store "$BETA_B" \
    b-pre-disappearance-package-store "$BETA_C" "$BETA_D"
beta_fetch_graph "$BETA_B"
beta_fetch_pin "$BETA_B" "$BETA_SECOND_ROOT" "$BETA_SECOND_TRANSPORT"
B_AFTER="$(beta_native "$BETA_B" zcode package search \
    --input='{"name_prefix":"stranger/sha3-note","limit":4}')"
B_SECOND_AFTER="$(beta_native "$BETA_B" zcode package search \
    --input='{"name_prefix":"visitor/hex-frame","limit":4}')"
beta_ok "B first package after publisher removal" "$B_AFTER"
beta_ok "B second package after publisher removal" "$B_SECOND_AFTER"
[ "$(printf '%s' "$B_AFTER" | beta_jget \
    'set(x["package_root"] for x in d["data"]["results"])==set(["'"$BETA_PACKAGE_ROOT"'"])')" = True ] &&
[ "$(printf '%s' "$B_SECOND_AFTER" | beta_jget \
    'set(x["package_root"] for x in d["data"]["results"])==set(["'"$BETA_SECOND_ROOT"'"])')" = True ] ||
    beta_die "onward providers did not preserve both graphs after publisher disappearance"

# Reconstruct the package source inertly, then link the standalone application
# only after D's explicit build/test acceptance produced installed artifacts.
APP_SOURCE="$DHT_WORK/standalone-source"
CHECKOUT="$(beta_native "$BETA_D" zcode package checkout \
    --input="{\"root\":\"$BETA_PACKAGE_ROOT\",\"destination\":\"$APP_SOURCE\"}")"
beta_ok "D inert package checkout" "$CHECKOUT"
[ ! -e "$APP_SOURCE/.git" ] || beta_die "checkout materialized Git metadata"
APP_BIN="$DHT_WORK/sha3-note"
cc -std=c23 -O2 -march=x86-64 -mtune=generic -Wall -Wextra -Werror \
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

beta_note "v2 source edit transfers and rebuilds only the changed package"
V1_NOTE_BACKUP="$DHT_WORK/v1-note.c"
V1_HEADER_BACKUP="$DHT_WORK/v1-note.h"
cp "$APP_SOURCE/src/note.c" "$V1_NOTE_BACKUP"
cp "$APP_SOURCE/include/stranger/note.h" "$V1_HEADER_BACKUP"
python3 - "$APP_SOURCE/src/note.c" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
old = "    if (!text || !out)\n        return false;"
new = "    if (!text || !out || text[0] == '\\0')\n        return false;"
if text.count(old) != 1:
    raise SystemExit("v2 edit target is not exact")
path.write_text(text.replace(old, new))
PY
beta_publish_version "$BETA_D" v2 "$APP_SOURCE" 4 22
BETA_V2_ROOT="$BETA_VERSION_ROOT"
BETA_V2_TRANSPORT="$BETA_VERSION_TRANSPORT"
BETA_V2_RELEASE_ID="$BETA_VERSION_RELEASE_ID"
BETA_V2_RECIPE_ROOT="$BETA_VERSION_RECIPE_ROOT"
BETA_V2_LOCK_ROOT="$BETA_VERSION_LOCK_ROOT"
BETA_V2_API_ROOT="$BETA_VERSION_API_ROOT"
[ "$BETA_V2_ROOT" != "$BETA_PACKAGE_ROOT" ] &&
[ "$BETA_V2_RELEASE_ID" != "$PACKAGE_RELEASE_ID" ] ||
    beta_die "v2 source bytes did not produce distinct exact identities"
[ "$BETA_V2_RECIPE_ROOT" = "$BETA_V1_RECIPE_ROOT" ] &&
[ "$BETA_V2_LOCK_ROOT" != "$BETA_V1_LOCK_ROOT" ] &&
[ "$BETA_V2_API_ROOT" = "$BETA_V1_API_ROOT" ] ||
    beta_die "v2 did not isolate source identity from recipe and public API"
beta_restart "$BETA_D" "$BETA_C"
beta_publish_package "$BETA_D" "$BETA_V2_ROOT" "$BETA_V2_TRANSPORT" 2
beta_fetch_pin "$BETA_C" "$BETA_V2_ROOT" "$BETA_V2_TRANSPORT"
V2_REQUESTED_OBJECTS="$BETA_FETCH_REQUESTED_OBJECTS"
V2_TRANSFERRED_OBJECTS="$BETA_FETCH_TRANSFERRED_OBJECTS"
V2_REUSED_OBJECTS="$BETA_FETCH_REUSED_OBJECTS"
V2_REQUESTED_BYTES="$BETA_FETCH_REQUESTED_BYTES"
V2_TRANSFERRED_BYTES="$BETA_FETCH_TRANSFERRED_BYTES"
V2_REUSED_BYTES="$BETA_FETCH_REUSED_BYTES"
[ "$V2_REQUESTED_OBJECTS" -eq "$V2_TRANSFERRED_OBJECTS" ] &&
[ "$V2_TRANSFERRED_OBJECTS" -gt 0 ] && [ "$V2_REUSED_OBJECTS" -gt 0 ] &&
[ "$V2_REQUESTED_BYTES" -eq "$V2_TRANSFERRED_BYTES" ] &&
[ "$V2_TRANSFERRED_BYTES" -gt 0 ] && [ "$V2_REUSED_BYTES" -gt 0 ] &&
[ "$V2_TRANSFERRED_BYTES" -lt "$C_V1_TRANSFERRED_BYTES" ] ||
    beta_die "v2 did not reuse unchanged v1 carrier objects"
beta_build_graph "$BETA_C" "$BETA_V2_ROOT" "$BETA_BASE_ROOT" \
    "$BETA_SHA3_ROOT" "$BETA_V2_ROOT"
V2_RECEIPT="$BETA_BUILD_RECEIPT"
[ "$BETA_BUILD_REBUILT" -eq 1 ] && [ "$BETA_BUILD_REUSED" -eq 2 ] &&
[ "$BETA_BUILD_EVIDENCE_REUSED" -eq 2 ] &&
[ "$V2_RECEIPT" != "$C_RECEIPT" ] ||
    beta_die "v2 did not rebuild only its exact changed package"
V2_ARCHIVE="${DDS[$BETA_C]}/zcode/installed/$BETA_V2_ROOT/lib/libsha3-note.a"
[ -f "$V2_ARCHIVE" ] || beta_die "v2 archive missing"
V2_ARTIFACT="$(openssl dgst -sha3-256 "$V2_ARCHIVE" | awk '{print $NF}')"
[ "$V2_ARTIFACT" != "$C_ARTIFACT" ] ||
    beta_die "v2 semantic implementation edit did not change its archive"

beta_note "v3 moves the same offline author identity to interchangeable C"
python3 - "$APP_SOURCE/include/stranger/note.h" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
old = "\n#endif\n"
new = "\n#define STRANGER_NOTE_API_VERSION 3u\n\n#endif\n"
if text.count(old) != 1:
    raise SystemExit("v3 edit target is not exact")
path.write_text(text.replace(old, new))
PY
beta_publish_version "$BETA_C" v3 "$APP_SOURCE" 5 29
BETA_V3_ROOT="$BETA_VERSION_ROOT"
BETA_V3_TRANSPORT="$BETA_VERSION_TRANSPORT"
BETA_V3_RELEASE_ID="$BETA_VERSION_RELEASE_ID"
BETA_V3_RECIPE_ROOT="$BETA_VERSION_RECIPE_ROOT"
BETA_V3_LOCK_ROOT="$BETA_VERSION_LOCK_ROOT"
BETA_V3_API_ROOT="$BETA_VERSION_API_ROOT"
[ "$BETA_V3_ROOT" != "$BETA_PACKAGE_ROOT" ] &&
[ "$BETA_V3_ROOT" != "$BETA_V2_ROOT" ] &&
[ "$BETA_V3_RELEASE_ID" != "$BETA_V2_RELEASE_ID" ] ||
    beta_die "v3 header bytes did not produce a third exact identity"
[ "$BETA_V3_RECIPE_ROOT" = "$BETA_V1_RECIPE_ROOT" ] &&
[ "$BETA_V3_LOCK_ROOT" != "$BETA_V1_LOCK_ROOT" ] &&
[ "$BETA_V3_LOCK_ROOT" != "$BETA_V2_LOCK_ROOT" ] &&
[ "$BETA_V3_API_ROOT" != "$BETA_V2_API_ROOT" ] ||
    beta_die "v3 did not isolate its public API capsule change"
beta_restart "$BETA_C" "$BETA_D"
beta_publish_package "$BETA_C" "$BETA_V3_ROOT" "$BETA_V3_TRANSPORT" 1
beta_fetch_pin "$BETA_D" "$BETA_V3_ROOT" "$BETA_V3_TRANSPORT"
V3_REQUESTED_OBJECTS="$BETA_FETCH_REQUESTED_OBJECTS"
V3_TRANSFERRED_OBJECTS="$BETA_FETCH_TRANSFERRED_OBJECTS"
V3_REUSED_OBJECTS="$BETA_FETCH_REUSED_OBJECTS"
V3_REQUESTED_BYTES="$BETA_FETCH_REQUESTED_BYTES"
V3_TRANSFERRED_BYTES="$BETA_FETCH_TRANSFERRED_BYTES"
V3_REUSED_BYTES="$BETA_FETCH_REUSED_BYTES"
[ "$V3_REQUESTED_OBJECTS" -eq "$V3_TRANSFERRED_OBJECTS" ] &&
[ "$V3_TRANSFERRED_OBJECTS" -gt 0 ] && [ "$V3_REUSED_OBJECTS" -gt 0 ] &&
[ "$V3_REQUESTED_BYTES" -eq "$V3_TRANSFERRED_BYTES" ] &&
[ "$V3_TRANSFERRED_BYTES" -gt 0 ] && [ "$V3_REUSED_BYTES" -gt 0 ] &&
[ "$V3_TRANSFERRED_BYTES" -lt "$C_V1_TRANSFERRED_BYTES" ] ||
    beta_die "v3 did not reuse unchanged package and dependency objects"
beta_build_graph "$BETA_D" "$BETA_V3_ROOT" "$BETA_BASE_ROOT" \
    "$BETA_SHA3_ROOT" "$BETA_V3_ROOT"
V3_RECEIPT="$BETA_BUILD_RECEIPT"
[ "$BETA_BUILD_REBUILT" -eq 1 ] && [ "$BETA_BUILD_REUSED" -eq 2 ] &&
[ "$BETA_BUILD_EVIDENCE_REUSED" -eq 2 ] &&
[ "$V3_RECEIPT" != "$V2_RECEIPT" ] ||
    beta_die "v3 did not rebuild only the exact header-change closure"
V3_ARCHIVE="${DDS[$BETA_D]}/zcode/installed/$BETA_V3_ROOT/lib/libsha3-note.a"
[ -f "$V3_ARCHIVE" ] || beta_die "v3 archive missing"
V3_ARTIFACT="$(openssl dgst -sha3-256 "$V3_ARCHIVE" | awk '{print $NF}')"

# All three signed author statements coexist. A higher sequence is evidence
# of what this author said later, never permission to replace an exact root.
VERSION_SEARCH="$(beta_native "$BETA_C" zcode package search \
    --input='{"name_prefix":"stranger/sha3-note","limit":8}')"
beta_ok "C version search" "$VERSION_SEARCH"
[ "$(printf '%s' "$VERSION_SEARCH" | beta_jget \
    'set(x["package_root"] for x in d["data"]["results"])==set(["'"$BETA_PACKAGE_ROOT"'","'"$BETA_V2_ROOT"'","'"$BETA_V3_ROOT"'"])')" = True ] &&
[ "$(printf '%s' "$VERSION_SEARCH" | beta_jget \
    'set(x["publisher_sequence"] for x in d["data"]["results"])==set([3,4,5])')" = True ] &&
[ "$(printf '%s' "$VERSION_SEARCH" | beta_jget \
    'len(set(x["publisher"] for x in d["data"]["results"]))')" -eq 1 ] ||
    beta_die "signed package history did not preserve v1/v2/v3 distinctly"

# A fresh B store asks for v1 by exact root after v2 and v3 exist. D must
# serve v1 rather than silently substituting the highest author sequence.
beta_reset_package_store "$BETA_B" b-after-v3-package-store "$BETA_D"
beta_fetch_graph "$BETA_B"
B_V1_SHOW="$(beta_native "$BETA_B" zcode package show \
    --input="{\"root\":\"$BETA_PACKAGE_ROOT\"}")"
beta_ok "B exact v1 after v3" "$B_V1_SHOW"
[ "$(printf '%s' "$B_V1_SHOW" | beta_jget 'd["data"]["package_root"]')" = "$BETA_PACKAGE_ROOT" ] ||
    beta_die "B silently substituted a successor for pinned v1"

beta_note "exact byte revert recreates v1 identities and reuses all evidence"
cp "$V1_NOTE_BACKUP" "$APP_SOURCE/src/note.c"
cp "$V1_HEADER_BACKUP" "$APP_SOURCE/include/stranger/note.h"
cmp -s "$APP_SOURCE/src/note.c" "$BETA_PACKAGE_DIR/src/note.c" &&
cmp -s "$APP_SOURCE/include/stranger/note.h" \
    "$BETA_PACKAGE_DIR/include/stranger/note.h" ||
    beta_die "revert did not restore exact v1 source bytes"
REVERT_PREP="$(beta_prepare "$APP_SOURCE" "$AUTHOR_PUB" 3)"
beta_ok "exact revert prepare" "$REVERT_PREP"
[ "$(printf '%s' "$REVERT_PREP" | beta_jget 'd["data"]["package_root"]')" = "$BETA_PACKAGE_ROOT" ] &&
[ "$(printf '%s' "$REVERT_PREP" | beta_jget 'd["data"]["recipe_root"]')" = "$BETA_V1_RECIPE_ROOT" ] &&
[ "$(printf '%s' "$REVERT_PREP" | beta_jget 'd["data"]["dependency_lock_root"]')" = "$BETA_V1_LOCK_ROOT" ] &&
[ "$(printf '%s' "$REVERT_PREP" | beta_jget 'd["data"]["api_capsule_root"]')" = "$BETA_V1_API_ROOT" ] ||
    beta_die "exact revert did not recreate all v1 semantic roots"
REVERT_SIGNATURE="$(beta_sign_package "$REVERT_PREP" "$AUTHOR_KEY")"
REVERT_SEAL="$("$NODE_BIN" zcode package dev seal \
    --input="{\"release_body_hex\":\"$(printf '%s' "$REVERT_PREP" | beta_jget 'd["data"]["release_body_hex"]')\",\"signature_hex\":\"$REVERT_SIGNATURE\"}" \
    2>/dev/null | tail -1)"
beta_ok "exact revert seal" "$REVERT_SEAL"
[ "$(printf '%s' "$REVERT_SEAL" | beta_jget 'd["data"]["release_id"]')" = "$PACKAGE_RELEASE_ID" ] ||
    beta_die "exact revert did not recreate the original signed release"
REVERT_STORE="$DHT_WORK/revert-identity-store"
mkdir -p "$REVERT_STORE"
REVERT_RELEASE="$(printf '%s' "$REVERT_SEAL" | beta_jget \
    'd["data"]["release_hex"]')"
REVERT_MANIFEST="$(printf '%s' "$REVERT_PREP" | beta_jget \
    'd["data"]["manifest_hex"]')"
REVERT_RECIPE="$(printf '%s' "$REVERT_PREP" | beta_jget \
    'd["data"]["recipe_hex"]')"
REVERT_PLAN="$("$NODE_BIN" -regtest zcode create \
    --input="{\"mode\":\"plan\",\"release_hex\":\"$REVERT_RELEASE\",\"manifest_hex\":\"$REVERT_MANIFEST\",\"recipe_hex\":\"$REVERT_RECIPE\",\"dir\":\"$APP_SOURCE\",\"day\":15,\"datadir\":\"$REVERT_STORE\"}" \
    2>/dev/null | tail -1)"
beta_ok "exact revert carrier plan" "$REVERT_PLAN"
REVERT_COMMIT="$("$NODE_BIN" -regtest zcode create \
    --input="{\"mode\":\"commit\",\"release_hex\":\"$REVERT_RELEASE\",\"manifest_hex\":\"$REVERT_MANIFEST\",\"recipe_hex\":\"$REVERT_RECIPE\",\"dir\":\"$APP_SOURCE\",\"day\":15,\"datadir\":\"$REVERT_STORE\"}" \
    2>/dev/null | tail -1)"
beta_ok "exact revert carrier commit" "$REVERT_COMMIT"
[ "$(printf '%s' "$REVERT_COMMIT" | beta_jget 'd["data"]["transport_root"]')" = "$BETA_PACKAGE_TRANSPORT" ] ||
    beta_die "exact revert did not recreate the original carrier root"
REVERT_FETCH="$(beta_native "$BETA_C" zcode package fetch \
    --input="{\"root\":\"$BETA_PACKAGE_TRANSPORT\",\"namespace\":\"$BETA_NAMESPACE\",\"maximum_bytes\":268435456}")"
beta_ok "C exact revert fetch" "$REVERT_FETCH"
[ "$(printf '%s' "$REVERT_FETCH" | beta_jget 'd["data"]["fetch_result"]')" = already-complete ] ||
    beta_die "exact revert transferred bytes instead of using local v1"
beta_build_graph "$BETA_C" "$BETA_PACKAGE_ROOT" "$BETA_BASE_ROOT" \
    "$BETA_SHA3_ROOT" "$BETA_PACKAGE_ROOT"
REVERT_RECEIPT="$BETA_BUILD_RECEIPT"
[ "$BETA_BUILD_REBUILT" -eq 0 ] && [ "$BETA_BUILD_REUSED" -eq 3 ] &&
[ "$BETA_BUILD_EVIDENCE_REUSED" -eq 3 ] &&
[ "$REVERT_RECEIPT" = "$C_RECEIPT" ] ||
    beta_die "exact revert did not reuse the original bound build evidence"

# Continue in the SAME physical-node harness with the shared core lane's
# signer-owned async proof machinery. This adds two independent reproduction
# statements without introducing a product-local worker, scheduler, transport,
# signer, or process topology. Its standard profile also proves an inert
# importer cannot acquire execution/evidence authority merely by fetching.
beta_note "installed nodes obtain two signer-owned reproduction receipts"
source "$SCRIPT_DIR/zcode_async_proof_acceptance_hook.sh"

# C remains live at the end of the shared hook. D is an interchangeable full
# node and may have been stopped by later lease scenarios; restart it only to
# inspect its own durable receipt projection through the typed SQL surface.
[ -n "${PIDS[$ZAP_D]:-}" ] || zap_start_node "$ZAP_D"
C_SIGNER="$(zap_sql_value "$ZAP_C" "SELECT w.signer_pubkey FROM build_actions a JOIN build_workers w ON w.worker_id=a.worker_id WHERE a.action_id='$C_STANDARD_ACTION'")"
D_SIGNER="$(zap_sql_value "$ZAP_D" "SELECT w.signer_pubkey FROM build_actions a JOIN build_workers w ON w.worker_id=a.worker_id WHERE a.action_id='$D_STANDARD_ACTION'")"
[ "${#C_SIGNER}" -eq 64 ] && [ "${#D_SIGNER}" -eq 64 ] &&
[ "$C_SIGNER" != "$D_SIGNER" ] ||
    beta_die "installed reproduction report lost its two distinct signers"

printf '%s\n' "{\"schema\":\"zcl.c23_commons_beta_stretch.v1\",\"verdict\":\"PASS\",\"second_package_root\":\"$BETA_SECOND_ROOT\",\"second_transport_root\":\"$BETA_SECOND_TRANSPORT\",\"second_release_id\":\"$BETA_SECOND_RELEASE_ID\",\"second_recipe_root\":\"$BETA_SECOND_RECIPE_ROOT\",\"second_dependency_lock_root\":\"$BETA_SECOND_LOCK_ROOT\",\"second_api_capsule_root\":\"$BETA_SECOND_API_ROOT\",\"second_author_pubkey\":\"$SECOND_AUTHOR_PUB\",\"authors_distinct\":true,\"shared_dependency_root\":\"$BETA_BASE_ROOT\",\"shared_dependency_receipt\":\"$C_BASE_RECEIPT\",\"shared_dependency_artifact_root\":\"$C_BASE_ARTIFACT\",\"shared_dependency_physical_builds_on_consumer\":1,\"shared_dependency_receipt_reused\":true,\"downstream_applications\":2,\"package_build_target\":\"linux-x86_64\",\"package_cpu_runtime_proof\":\"$PACKAGE_CPU_RUNTIME_PROOF\",\"first_standalone_output\":\"$FIRST_SHARED_APP_OUTPUT\",\"second_build_receipt_id\":\"$SECOND_RECEIPT\",\"second_artifact_root\":\"$SECOND_ARTIFACT\",\"second_objects_transferred\":$SECOND_TRANSFERRED_OBJECTS,\"second_bytes_transferred\":$SECOND_TRANSFERRED_BYTES,\"second_standalone_output\":\"$SECOND_APP_OUTPUT\",\"compiled_registry_admission\":false,\"second_publisher_store_removed\":true,\"alternate_provider_refetch\":true}"

printf '%s\n' "{\"schema\":\"zcl.c23_commons_beta_installed.v1\",\"verdict\":\"PASS\",\"installed_binary\":\"$C23_BETA_INSTALL_BIN/zclassic23\",\"repository_source_used_by_consumers\":false,\"package_root\":\"$BETA_PACKAGE_ROOT\",\"dependency_roots\":[\"$BETA_BASE_ROOT\",\"$BETA_SHA3_ROOT\"],\"author_pubkey\":\"$AUTHOR_PUB\",\"build_receipt_id\":\"$C_RECEIPT\",\"artifact_root\":\"$C_ARTIFACT\",\"fetch_inert\":true,\"explicit_builds\":2,\"publisher_disappearance_survived\":true,\"standalone_output\":\"$APP_OUTPUT\",\"updates\":{\"v1\":{\"package_root\":\"$BETA_PACKAGE_ROOT\",\"transport_root\":\"$BETA_PACKAGE_TRANSPORT\",\"release_id\":\"$PACKAGE_RELEASE_ID\",\"recipe_root\":\"$BETA_V1_RECIPE_ROOT\",\"dependency_lock_root\":\"$BETA_V1_LOCK_ROOT\",\"api_capsule_root\":\"$BETA_V1_API_ROOT\",\"artifact_root\":\"$C_ARTIFACT\"},\"v2\":{\"package_root\":\"$BETA_V2_ROOT\",\"transport_root\":\"$BETA_V2_TRANSPORT\",\"release_id\":\"$BETA_V2_RELEASE_ID\",\"recipe_root\":\"$BETA_V2_RECIPE_ROOT\",\"dependency_lock_root\":\"$BETA_V2_LOCK_ROOT\",\"api_capsule_root\":\"$BETA_V2_API_ROOT\",\"artifact_root\":\"$V2_ARTIFACT\",\"objects_requested\":$V2_REQUESTED_OBJECTS,\"objects_transferred\":$V2_TRANSFERRED_OBJECTS,\"objects_reused\":$V2_REUSED_OBJECTS,\"bytes_requested\":$V2_REQUESTED_BYTES,\"bytes_transferred\":$V2_TRANSFERRED_BYTES,\"bytes_reused\":$V2_REUSED_BYTES,\"packages_rebuilt\":1,\"packages_reused\":2,\"prior_evidence_reused\":2,\"prior_evidence_invalidated\":1},\"v3\":{\"package_root\":\"$BETA_V3_ROOT\",\"transport_root\":\"$BETA_V3_TRANSPORT\",\"release_id\":\"$BETA_V3_RELEASE_ID\",\"recipe_root\":\"$BETA_V3_RECIPE_ROOT\",\"dependency_lock_root\":\"$BETA_V3_LOCK_ROOT\",\"api_capsule_root\":\"$BETA_V3_API_ROOT\",\"artifact_root\":\"$V3_ARTIFACT\",\"objects_requested\":$V3_REQUESTED_OBJECTS,\"objects_transferred\":$V3_TRANSFERRED_OBJECTS,\"objects_reused\":$V3_REUSED_OBJECTS,\"bytes_requested\":$V3_REQUESTED_BYTES,\"bytes_transferred\":$V3_TRANSFERRED_BYTES,\"bytes_reused\":$V3_REUSED_BYTES,\"packages_rebuilt\":1,\"packages_reused\":2,\"prior_evidence_reused\":2,\"prior_evidence_invalidated\":1},\"revert\":{\"package_root\":\"$BETA_PACKAGE_ROOT\",\"transport_root\":\"$BETA_PACKAGE_TRANSPORT\",\"release_id\":\"$PACKAGE_RELEASE_ID\",\"bytes_transferred\":0,\"packages_rebuilt\":0,\"packages_reused\":3,\"prior_evidence_reused\":3,\"build_receipt_id\":\"$REVERT_RECEIPT\"},\"v1_fetchable_after_v3\":true,\"author_sequence_is_advisory\":true,\"exact_root_local_policy\":true},\"signed_reproduction\":{\"actions\":[\"$C_STANDARD_ACTION\",\"$D_STANDARD_ACTION\"],\"output_root\":\"$C_OUTPUT\",\"signers\":[\"$C_SIGNER\",\"$D_SIGNER\"],\"distinct_signers\":2,\"requester_executed\":false}}"
