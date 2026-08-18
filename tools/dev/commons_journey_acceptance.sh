#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# commons-journey-acceptance — one complete two-node C23 Commons journey.
#
# A person describes useful software behavior. Z23 reuses existing C23 first,
# creates only what is missing, shows the result, reproduces the exact bytes
# on another node, and lets the person accept and use that exact version.
# This proves that sentence, end to end, on two fresh isolated datadirs:
#
#   zcode guide -> work start -> work run -> work show
#     -> publish -> discover -> fetch -> source reproduce
#     -> work accept -> zcode use
#
# After checkout and build it contacts nothing: no GitHub, no registry, no
# package server. Node B learns the package from node A over the node's own
# authenticated DHT and gets every byte from node A's package swarm.
#
# WHAT IT PROVES, in the order the script asserts it:
#   1. Reusable code is selected before new code is written.
#   2. Code that is not available locally is never reported as reused.
#   3. Only the behavior still missing enters candidate work.
#   4. Fetched source stays inert; nothing builds or runs it on arrival.
#   5. Building and testing it requires an explicit local admission.
#   6. Source, dependency, recipe, toolchain, action, artifact and receipt
#      identities stay bound to each other.
#   7. Node B reconstructs the inputs and reproduces byte-identical output.
#   8. Altered source, dependency, receipt or artifact is refused BY NAME.
#   9. Acceptance is explicit, and `zcode use` then runs the application.
#
# STATUS: steps 1-7 are asserted today. Steps 8 and 9 (work accept, the
# accepted app's own publication, byte-identical remote source reproduction,
# the tamper refusals, and zcode use) are still being built; the verdict this
# script prints at the end names exactly what it did and did not prove.
#
# The fixture is deliberately small and real: tools/dev/fixtures/commons_journey
# holds z23/textstat (a finished, dependency-free counter package) and
# z23/wordcount (an application that reuses it and needs one behavior nothing
# in the commons provides — the longest line). Nothing here is a mock.
#
# DELIBERATELY opt-in (NOT in `make ci`): it spawns two real regtest daemons,
# mines a regtest chain, and runs confined package builds. It touches no
# production datadir, no wallet key of yours, and no live port.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# tools/dev/node_lifecycle.sh is the single owner of process-group ownership,
# port claims, work-dir creation and cleanup. Reusing it is the point: a
# second lifecycle state machine is a second thing that can lie about whether
# the last run really went away.
. "$SCRIPT_DIR/node_lifecycle.sh"

CJ_FIXTURES="$SCRIPT_DIR/fixtures/commons_journey"
CJ_SIGNER="${ZCL_PACKAGE_SIGN_BIN:-$REPO_ROOT/build/bin/zclassic23-package-sign}"

# Two nodes, both on the production reachable-port policy's test-safe ports.
# Both P2P ports must be in the production reachable-port allowlist
# (lib/net/include/net/port_policy.h). The initial operator-directed dial
# bypasses that policy, but the controlled Noise reconnect does not, so an
# arbitrary high port connects once and then silently drops to zero peers.
A_PORT=20028; A_RPC=29281; A_FS=29282; A_HTTPS=29283
B_PORT=20027; B_RPC=29291; B_FS=29292; B_HTTPS=29293

CJ_WALLET_PASS="commons-journey-wallet-pass"
CJ_BACKUP_PASS="commons-journey-backup-pass"
CJ_SEED_A=1212121212121212121212121212121212121212121212121212121212121212
CJ_SEED_B=3434343434343434343434343434343434343434343434343434343434343434

# What the person asks for, in plain words. "use <package>" is the deliberately
# narrow grammar that may prove reuse; the rest is the behavior that is missing.
CJ_GOAL="use z23/textstat and report the longest line of a text file"

# The proof profile. `quick` proves the candidate on this node: it compiles it,
# runs its acceptance tests, and writes receipts here. `standard` additionally
# demands receipts from a SECOND independent build node — the zero-wait
# development protocol that zcode-async-proof-acceptance already owns. This
# journey keeps node B for what the mission asks of it: independently
# reconstructing the source and reproducing the exact bytes.
CJ_PROFILE="quick"

# The application manifest is written at run time because a dependency is named
# by its exact root, and that root is not known until the package it names has
# been prepared. Passing an empty root writes the no-dependency form.
cj_write_package_json() {
    local ws="$1" dep_root="$2" deps="[]"
    if [ -n "$dep_root" ]; then
        deps="[{\"root\": \"$dep_root\", \"name\": \"z23/textstat\", \"semver\": \"0.1.0\"}]"
    fi
    cat >"$ws/zcode-package.json" <<JSON
{
  "schema": 1,
  "name": "z23/wordcount",
  "semver": "0.1.0",
  "language": "c23",
  "license": "Apache-2.0",
  "include_dir": "include",
  "source_dir": "src",
  "dependencies": $deps,
  "files": [
    "LICENSE",
    "app/main.c",
    "include/wordcount/wordcount.h",
    "src/wordcount.c",
    "tests/test_wordcount.c",
    "zcode-package.json"
  ]
}
JSON
}

cj_die()  { dht_die "commons-journey: $*"; }
cj_note() { echo "commons-journey: $*"; }
cj_step() { echo; echo "commons-journey: ── $* ──"; }

# Every leaf answers with one JSON line, and that line is the contract: a
# refusal is a named `ok:false` document, not a shell status. `pipefail` would
# otherwise turn an ordinary refusal into a silent `set -e` abort inside a
# `x="$(cj_a ...)"` assignment, losing the very name the refusal carries. So
# these never fail — cj_require_ok / cj_require_refusal read the answer.
# `-regtest` is not decoration. A one-shot CLI derives its chain params from
# its own flags, not from the datadir it is pointed at, so without it these
# leaves run under MAINNET rules against a regtest node: `zcode package dev
# prepare` stamps chain_id "zclassic-main" into the signed release, the
# publishing node accepts it (its CLI is equally mainnet), and the fetching
# node's daemon — which really is regtest — refuses the carrier with
# acceptance: wrong-chain-id. Every other node hook in this tree wraps
# dht_native the same way.
cj_a() { dht_native "$DHT_DD_A" "$A_RPC" -regtest "$@" || true; }
cj_b() { dht_native "$DHT_DD_B" "$B_RPC" -regtest "$@" || true; }
cj_jget() { "$DHT_ACCEPTANCE_C23" json-get "$@"; }
cj_field() { printf '%s' "$2" | cj_jget "$1" "${3:-}"; }

cj_require_ok() {
    local label="$1" doc="$2"
    [ "$(cj_field ok "$doc" False)" = True ] ||
        cj_die "$label failed: $doc"
}

# A refusal is only useful if it says which rule failed. Assert the exact
# name, never just "it returned false".
cj_require_refusal() {
    local label="$1" doc="$2" want="$3" got
    [ "$(cj_field ok "$doc" True)" = False ] ||
        cj_die "$label was ACCEPTED but must be refused: $doc"
    got="$(cj_field error.code "$doc" '')$(cj_field error.rule "$doc" '')"
    got="$got $(cj_field error.message "$doc" '')"
    case "$got" in
        *"$want"*) cj_note "refused by name: $label -> $want" ;;
        *) cj_die "$label was refused, but not by name '$want': $doc" ;;
    esac
}

# ── bring-up ─────────────────────────────────────────────────────────────
# The custody/anchor/delegation ordering below is the recipe proven by
# zcode-dht-acceptance. It is a sequence of ordinary product commands, not a
# private test path: the wallet must be encrypted at rest, unlocked, and
# backed up before the identity anchor's custody gate will pass, and the
# money-freshness gate needs a live outbound peer.
cj_wait_rpc_or_die() {
    dht_wait_rpc "$1" "$2" "$3" || cj_die "$4 RPC warmup failed"
}

cj_boot() {
    local dd rpc
    for port in $A_PORT $A_RPC $A_FS $A_HTTPS $B_PORT $B_RPC $B_FS $B_HTTPS; do
        dht_assert_port "$port"
    done
    [ -x "$NODE_BIN" ] && [ -x "$RPC_BIN" ] && [ -x "$DHT_ACCEPTANCE_C23" ] &&
    [ -x "$CJ_SIGNER" ] ||
        cj_die "build the node, RPC, C23 acceptance helper and package signer first"
    [ -d "$CJ_FIXTURES/textstat" ] && [ -d "$CJ_FIXTURES/wordcount" ] ||
        cj_die "missing fixture packages under $CJ_FIXTURES"

    dht_make_work zcl23-journey
    # This journey never proves a shielded transaction; keep its boot cost and
    # outcome independent of operator-installed proving parameters.
    if [ -z "$DHT_PARAMS_DIR" ]; then
        DHT_PARAMS_DIR="$DHT_WORK/no-zk-params"
        mkdir -p "$DHT_PARAMS_DIR"
    fi
    DHT_DD_A="$DHT_WORK/node-a"; DHT_DD_B="$DHT_WORK/node-b"
    DHT_MINE_DD="$DHT_DD_A"; DHT_MINE_RPC="$A_RPC"
    mkdir -p "$DHT_DD_A" "$DHT_DD_B"

    install -d -m 700 "$DHT_WORK/cred"
    install -m 600 /dev/null "$DHT_WORK/cred/wallet-passphrase"
    printf '%s\n' "$CJ_WALLET_PASS" >"$DHT_WORK/cred/wallet-passphrase"
    export CREDENTIALS_DIRECTORY="$DHT_WORK/cred"

    for dd in a b; do
        install -m 600 /dev/null "$DHT_WORK/master-$dd.hex"
    done
    printf '%s\n' "$CJ_SEED_A" >"$DHT_WORK/master-a.hex"
    printf '%s\n' "$CJ_SEED_B" >"$DHT_WORK/master-b.hex"
    cj_note "booting two clean regtest nodes (A asks, B proves)"
    cj_note "booting two clean regtest nodes (A hosts packages and builds)"
    DHT_PACKAGEHOST=1
    DHT_BUILDWORKERS=0
    dht_spawn DHT_PGID_A "$DHT_DD_A" "$A_PORT" "$A_RPC" "$A_FS" \
        "$A_HTTPS" "127.0.0.1:$DEAD_SINK"
    cj_wait_rpc_or_die "$DHT_DD_A" "$A_RPC" "$DHT_PGID_A" "node A"
    DHT_BUILDWORKERS=1
    dht_spawn DHT_PGID_B "$DHT_DD_B" "$B_PORT" "$B_RPC" "$B_FS" \
        "$B_HTTPS" "127.0.0.1:$A_PORT"
    cj_wait_rpc_or_die "$DHT_DD_B" "$B_RPC" "$DHT_PGID_B" "node B"
    rpc="$(a_rpc getnewaddress | dht_result)"
    [ -n "$rpc" ] || cj_die "node A produced no funding address"
    CJ_ADDR="$rpc"
}

a_rpc() { dht_rpc "$DHT_DD_A" "$A_RPC" "$@"; }
b_rpc() { dht_rpc "$DHT_DD_B" "$B_RPC" "$@"; }
# ── the node identities the package swarm authenticates with ─────────────
cj_wait_height() {
    dht_wait_height "$1" "$2" "$3" || cj_die "$4 did not reach height $3"
}

cj_identities() {
    local anchor
    cj_note "mining spendable regtest funds"
    dht_mine_to_address 101 "$CJ_ADDR"
    cj_wait_height "$DHT_DD_B" "$B_RPC" 101 "node B"
    dht_wait_fold "$DHT_DD_A" "$A_RPC" 101 ||
        cj_die "node A reducer fold did not reach the funding tip"

    # A must own the only link during the custody phase, and its coins-set
    # authority stamp lands at boot, so B goes to the dead sink and A restarts.
    dht_kill_group "$DHT_PGID_B"; DHT_PGID_B=""
    DHT_BUILDWORKERS=1
    dht_spawn DHT_PGID_B "$DHT_DD_B" "$B_PORT" "$B_RPC" "$B_FS" \
        "$B_HTTPS" "127.0.0.1:$DEAD_SINK"
    cj_wait_rpc_or_die "$DHT_DD_B" "$B_RPC" "$DHT_PGID_B" "node B dead-sink bounce"
    dht_kill_group "$DHT_PGID_A"; DHT_PGID_A=""
    DHT_BUILDWORKERS=0
    dht_spawn DHT_PGID_A "$DHT_DD_A" "$A_PORT" "$A_RPC" "$A_FS" \
        "$A_HTTPS" "127.0.0.1:$DEAD_SINK"
    cj_wait_rpc_or_die "$DHT_DD_A" "$A_RPC" "$DHT_PGID_A" "node A custody restart"
    dht_wait_fold "$DHT_DD_A" "$A_RPC" 101 ||
        cj_die "node A reducer fold did not survive the restart"
    a_rpc addnode "\"127.0.0.1:$B_PORT\"" "\"onetry\"" >/dev/null || true
    dht_wait_connected "$DHT_DD_A" "$A_RPC" || cj_die "node A never connected outbound"
    dht_wait_sync_live "$DHT_DD_A" "$A_RPC" || cj_die "node A sync never left finding_peers"
    dht_wait_chain_loaded "$DHT_DD_A" "$A_RPC" 101 ||
        cj_die "node A active chain index did not load"

    cj_note "unlocking the wallet and taking the current-key encrypted backup"
    DHT_WALLET_PASS="$CJ_WALLET_PASS" dht_unlock_wallet "$DHT_DD_A" "$A_RPC" ||
        cj_die "node A wallet unlock failed"
    a_rpc getnewaddress | dht_result >/dev/null ||
        cj_die "post-restart keypool top-up failed"
    DHT_BACKUP_PASS="$CJ_BACKUP_PASS" dht_backup_wallet "$DHT_DD_A" "$A_RPC" ||
        cj_die "node A custody backup failed"
    dht_wait_spendable "$DHT_DD_A" "$A_RPC" ||
        cj_die "node A vault spendable never became positive"

    cj_note "anchoring both masters, then provisioning independent delegations"
    CJ_PUB_A="$("$DHT_WORK/journey-peer" pubkey "$CJ_SEED_A")"
    CJ_PUB_B="$("$DHT_WORK/journey-peer" pubkey "$CJ_SEED_B")"
    [ "$CJ_PUB_A" != "$CJ_PUB_B" ] || cj_die "both masters derived one pubkey"
    anchor="$(dht_anchor "$DHT_DD_A" "$A_RPC" "$CJ_PUB_A" "journey-anchor-a")" ||
        cj_die "node A anchor failed"
    dht_mine_empty 1; sleep 1
    anchor="$(dht_anchor "$DHT_DD_A" "$A_RPC" "$CJ_PUB_B" "journey-anchor-b")" ||
        cj_die "node B anchor failed"
    dht_mine_empty 22
    # Ask the chain how tall it is rather than hardcoding a number that drifts
    # the moment an anchor costs a block more or less.
    local tip
    tip="$(dht_height "$DHT_DD_A" "$A_RPC")"
    [ -n "$tip" ] || cj_die "node A reported no chain height after anchoring"
    cj_wait_height "$DHT_DD_B" "$B_RPC" "$tip" "node B"

    local del_a del_b
    del_a="$(cj_a zcode network delegate \
        --input="{\"seed_file\":\"$DHT_WORK/master-a.hex\"}")"
    cj_require_ok "node A delegation" "$del_a"
    del_b="$(cj_b zcode network delegate \
        --input="{\"seed_file\":\"$DHT_WORK/master-b.hex\"}")"
    cj_require_ok "node B delegation" "$del_b"
    CJ_NODE_A="$(cj_field data.node_id "$del_a")"
    CJ_NODE_B="$(cj_field data.node_id "$del_b")"
    [ -n "$CJ_NODE_A" ] && [ "$CJ_NODE_A" != "$CJ_NODE_B" ] ||
        cj_die "the two masters derived one node identity"
    cj_note "independent node identities: A=${CJ_NODE_A:0:16}… B=${CJ_NODE_B:0:16}…"
}

# The seed-to-pubkey derivation is the node's own ZID code; compile the same
# helper the DHT acceptance uses rather than inventing a second derivation.
cj_build_peer_helper() {
    cc -std=c23 -O1 -w -D_GNU_SOURCE -ffunction-sections -fdata-sections \
        -Wl,--gc-sections \
        -I"$REPO_ROOT/lib/base/include" -I"$REPO_ROOT/lib/sha3/include" \
        -I"$REPO_ROOT/lib/crypto/include" -I"$REPO_ROOT/lib/support/include" \
        -I"$REPO_ROOT/lib/util/include" -I"$REPO_ROOT/lib/platform/include" \
        -I"$REPO_ROOT/lib/json/include" -I"$REPO_ROOT/lib/core/include" \
        -I"$REPO_ROOT/lib/net/include" -I"$REPO_ROOT/lib/noise/include" \
        -I"$REPO_ROOT/lib/vcs/include" -I"$REPO_ROOT/lib/zid/include" \
        -I"$REPO_ROOT/core/math/include" -o "$DHT_WORK/journey-peer" \
        "$REPO_ROOT/tools/zcode_dht_acceptance_peer.c" \
        "$REPO_ROOT/lib/net/src/v2_transport.c" \
        "$REPO_ROOT/lib/noise/src/noise_handshake.c" \
        "$REPO_ROOT/lib/noise/src/session_transport.c" \
        "$REPO_ROOT/lib/vcs/src/zcode_dht.c" \
        "$REPO_ROOT/lib/vcs/src/zcode_dht_delegation.c" \
        "$REPO_ROOT/lib/vcs/src/zcode_dht_identity.c" \
        "$REPO_ROOT/lib/vcs/src/zcode_dht_msgs.c" \
        "$REPO_ROOT/lib/zid/src/zid.c" "$REPO_ROOT/lib/zid/src/zendp.c" \
        "$REPO_ROOT/lib/crypto/src/ed25519.c" \
        "$REPO_ROOT/lib/crypto/src/sha512.c" \
        "$REPO_ROOT/lib/crypto/src/sha256.c" \
        "$REPO_ROOT/lib/sha3/src/sha3.c" \
        "$REPO_ROOT/lib/crypto/src/hmac_sha256.c" \
        "$REPO_ROOT/lib/crypto/src/hkdf_sha256.c" \
        "$REPO_ROOT/lib/crypto/src/chacha20poly1305.c" \
        "$REPO_ROOT/lib/support/src/log_throttle.c" \
        "$REPO_ROOT/lib/crypto/src/curve25519.c" \
        "$REPO_ROOT/lib/crypto/src/x25519_safe.c" \
        "$REPO_ROOT/lib/crypto/src/random_secret.c" \
        "$REPO_ROOT/core/math/src/hash.c" \
        "$REPO_ROOT/lib/core/src/utiltime.c" \
        "$REPO_ROOT/lib/core/src/random.c" \
        "$REPO_ROOT/lib/base/src/safe_alloc.c" \
        "$REPO_ROOT/lib/base/src/log_level.c" \
        "$REPO_ROOT/lib/base/src/result.c" \
        "$REPO_ROOT/lib/base/src/cleanse.c" \
        "$REPO_ROOT/lib/platform/src/clock.c" \
        "$REPO_ROOT/lib/platform/src/rng.c" \
        "$REPO_ROOT/lib/util/src/write_all.c" \
        "$REPO_ROOT/lib/json/src/json.c" \
        "$REPO_ROOT/lib/util/src/hw_profile.c" \
        "$REPO_ROOT/lib/util/src/cpu_topology.c" ||
        cj_die "node identity helper compile failed"
}


# ── human-first assertions ───────────────────────────────────────────────
# Item 4 of the mission is a product property, not a comment: every terminal
# step must show current state, the important result, and exactly ONE safe
# next command, with roots and proof internals hidden unless details=true.
cj_has_root() {
    # Not `grep -q`: under pipefail a MATCH can surface printf's SIGPIPE 141
    # instead of grep's 0, so the decision would invert exactly when a root
    # IS present — the case this predicate exists to catch.
    local hit
    hit="$(printf '%s' "$1" | grep -oE '[0-9a-f]{64}' || true)"
    [ -n "$hit" ]
}

cj_human_first() {
    local label="$1" doc="$2" next
    next="$(cj_field data.next_safe_command "$doc" '')"
    [ -n "$next" ] ||
        cj_die "$label showed no next safe command: $doc"
    case "$next" in
        *" and "*|*";"*|*" or "*)
            cj_die "$label offered more than one next command: $next" ;;
    esac
    [ "$(cj_field data.stage "$doc" '')" != "" ] ||
        cj_die "$label did not say what stage the work is in: $doc"
    cj_note "$label -> next safe command: $next"
}

cj_roots_hidden() {
    local label="$1" plain="$2" detailed="$3"
    ! cj_has_root "$(cj_field data.expert "$plain" '')" ||
        cj_die "$label exposed proof internals without details=true: $plain"
    cj_has_root "$(cj_field data.expert "$detailed" '')" ||
        cj_die "$label hid its proof internals even with details=true: $detailed"
    [ "$(cj_field data.details_available "$plain" False)" = True ] ||
        cj_die "$label never told the reader details=true exists: $plain"
    cj_note "$label -> roots hidden by default, present with details=true"
}

# ── the offline publisher identity ───────────────────────────────────────
# A 32-byte mode-0600 secret used only through inherited descriptors. It
# never reaches a command JSON body or a daemon datadir.
cj_sign_digest() {
    local digest="$1" out
    printf '%s' "$digest" | xxd -r -p >"$DHT_WORK/release.digest"
    : >"$DHT_WORK/release.signature"
    chmod 0600 "$DHT_WORK/release.digest" "$DHT_WORK/release.signature"
    exec 7<"$DHT_WORK/publisher.key" 8<"$DHT_WORK/release.digest" \
         9>"$DHT_WORK/release.signature"
    "$CJ_SIGNER" --sign --key-fd 7 --digest-fd 8 --signature-fd 9 ||
        cj_die "offline release signing failed"
    exec 7<&- 8<&- 9>&-
    out="$(xxd -p -c 128 "$DHT_WORK/release.signature")"
    [ -n "$out" ] || cj_die "offline signing produced no signature"
    printf '%s' "$out"
}

# Publish one package tree into a node's own store. prepare -> sign -> seal
# -> plan -> commit, all through the ordinary leaves.
# Sets CJ_PKG_ROOT, CJ_PKG_RELEASE, CJ_PKG_RELEASE_HEX.
cj_publish_package() {
    local node="$1" dir="$2" seq="$3"
    local prepare digest body manifest recipe signature seal plan commit
    prepare="$("cj_$node" zcode package dev prepare \
        --input="{\"dir\":\"$dir\",\"publisher_pubkey\":\"$CJ_PUBLISHER\",\"publisher_sequence\":$seq}")"
    cj_require_ok "prepare $dir" "$prepare"
    CJ_PKG_ROOT="$(cj_field data.package_root "$prepare")"
    digest="$(cj_field data.release_signing_digest "$prepare")"
    body="$(cj_field data.release_body_hex "$prepare")"
    manifest="$(cj_field data.manifest_hex "$prepare")"
    recipe="$(cj_field data.recipe_hex "$prepare")"
    signature="$(cj_sign_digest "$digest")"
    seal="$("cj_$node" zcode package dev seal \
        --input="{\"release_body_hex\":\"$body\",\"signature_hex\":\"$signature\"}")"
    cj_require_ok "seal $dir" "$seal"
    CJ_PKG_RELEASE_HEX="$(cj_field data.release_hex "$seal")"
    CJ_PKG_RELEASE="$(cj_field data.release_id "$seal")"
    plan="$("cj_$node" zcode package publish plan \
        --input="{\"release_hex\":\"$CJ_PKG_RELEASE_HEX\",\"manifest_hex\":\"$manifest\",\"recipe_hex\":\"$recipe\",\"dir\":\"$dir\"}")"
    cj_require_ok "publish plan $dir" "$plan"
    [ "$(cj_field data.valid "$plan" False)" = True ] ||
        cj_die "publish plan refused $dir: $plan"
    commit="$("cj_$node" zcode package publish commit \
        --input="{\"release_hex\":\"$CJ_PKG_RELEASE_HEX\",\"manifest_hex\":\"$manifest\",\"recipe_hex\":\"$recipe\",\"dir\":\"$dir\"}")"
    cj_require_ok "publish commit $dir" "$commit"
    CJ_PKG_TRANSPORT="$(cj_field data.transport_root "$commit")"
    [ "${#CJ_PKG_TRANSPORT}" -eq 64 ] ||
        cj_die "publish commit named no carrier for $dir: $commit"
    [ "$(cj_field data.release_id "$commit")" = "$CJ_PKG_RELEASE" ] ||
        cj_die "publish commit changed the release id for $dir: $commit"
}

# Admit one package for local build and install: plan, then commit the plan.
cj_use_package() {
    local node="$1" ref="$2" plan commit plan_id
    plan="$("cj_$node" zcode use --input="{\"name_or_root\":\"$ref\"}")"
    cj_require_ok "zcode use plan $ref" "$plan"
    plan_id="$(cj_field data.plan_id "$plan")"
    [ -n "$plan_id" ] || cj_die "zcode use produced no plan for $ref: $plan"
    [ "$(cj_field data.ready "$plan" False)" = True ] ||
        cj_die "zcode use plan is not ready for $ref: $plan"
    commit="$("cj_$node" zcode use --input="{\"plan_id\":\"$plan_id\"}")"
    cj_require_ok "zcode use commit $ref" "$commit"
    [ "$(cj_field data.installed "$commit" False)" = True ] ||
        cj_die "zcode use did not install $ref: $commit"
    CJ_USE_COMMIT="$commit"
}

# ── peer-to-peer distribution ────────────────────────────────────────────
# The frozen DHT grammar keeps naming and custody apart. A POINTER binds the
# package root a person asks for to the carrier root that holds its bytes; a
# PROVIDER names an authenticated peer currently serving that exact carrier.
# Nothing here is a registry: a provider can vanish and the package survives,
# because the name is the content and any other holder answers for it.
cj_publish_record() {
    local node="$1" kind="$2" root="$3" transport="$4" seq="$5"
    local now expiry common plan token commit
    now="$(date +%s)"; expiry=$((now + 3600))
    common="\"kind\":\"$kind\",\"namespace\":\"zclassic23.package\",\"transport_root\":\"$transport\",\"sequence\":$seq,\"not_before\":$((now - 5)),\"expiry\":$expiry"
    [ "$kind" != pointer ] || common="$common,\"semantic_root\":\"$root\""
    plan="$("cj_$node" zcode network publish --input="{\"mode\":\"plan\",$common}")"
    cj_require_ok "node $node $kind plan $root" "$plan"
    token="$(cj_field data.plan_token "$plan")"
    commit="$("cj_$node" zcode network publish \
        --input="{\"mode\":\"commit\",$common,\"plan_token\":\"$token\"}")"
    cj_require_ok "node $node $kind commit $root" "$commit"
}

cj_announce_package() {
    local node="$1" root="$2" transport="$3" seq="$4"
    cj_publish_record "$node" pointer  "$root" "$transport" "$seq"
    cj_publish_record "$node" provider "$root" "$transport" "$seq"
}

cj_pin_root() {
    local node="$1" root="$2" plan token commit
    plan="$("cj_$node" zcode package pin --input="{\"root\":\"$root\",\"mode\":\"plan\"}")"
    cj_require_ok "node $node pin plan $root" "$plan"
    token="$(cj_field data.plan_token "$plan")"
    commit="$("cj_$node" zcode package pin \
        --input="{\"root\":\"$root\",\"mode\":\"commit\",\"plan_token\":\"$token\"}")"
    cj_require_ok "node $node pin commit $root" "$commit"
}

# Fetch one package over the overlay and reconstruct it locally. Returns only
# when this node itself reports the exact package tracked and complete.
# Sets CJ_FETCH_BYTES from the local store's own accounting.
cj_fetch_package() {
    local node="$1" root="$2" transport="$3"
    local out deadline complete=False plan next_resume
    # Admitting the exact carrier root is the whole request. The first call
    # must be accepted and routed by the live daemon — that is the fact worth
    # asserting about the network. Everything after it is answered by this
    # node about itself: `zcode package pin --mode=plan` says whether the
    # bytes are here and whole. Provider discovery is explicitly retryable,
    # so re-admitting the same root resumes the same durable download slot;
    # a refused re-admission is never allowed to stand in for completion.
    out="$("cj_$node" zcode package fetch \
        --input="{\"root\":\"$transport\",\"namespace\":\"zclassic23.package\",\"maximum_bytes\":67108864}")"
    cj_require_ok "node $node fetch $transport" "$out"
    printf '%s\n' "$out" >"$DHT_WORK/fetch-$node-${transport:0:16}.json"
    [ "$(cj_field data.live "$out" False)" = True ] ||
        cj_die "node $node did not route the fetch through its live daemon: $out"
    deadline=$(( $(date +%s) + 180 )); next_resume=$(( $(date +%s) + 15 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        plan="$("cj_$node" zcode package pin \
            --input="{\"root\":\"$root\",\"mode\":\"plan\"}" || true)"
        complete="$(cj_field data.package.complete "$plan" False)"
        [ "$complete" = True ] && break
        if [ "$(date +%s)" -ge "$next_resume" ]; then
            "cj_$node" zcode package fetch \
                --input="{\"root\":\"$transport\",\"namespace\":\"zclassic23.package\",\"maximum_bytes\":67108864}" \
                >/dev/null 2>&1 || true
            next_resume=$(( $(date +%s) + 15 ))
        fi
        sleep 1
    done
    [ "$complete" = True ] ||
        cj_die "node $node never reconstructed $root from carrier $transport: $plan"
    printf '%s\n' "$plan" >"$DHT_WORK/complete-$node-${root:0:16}.json"
    [ "$(cj_field data.package.tracked "$plan" False)" = True ] ||
        cj_die "node $node holds the bytes but does not track the package: $plan"
    CJ_FETCH_BYTES="$(cj_field data.package.total_bytes "$plan" 0)"
    # The bytes are here; the signed release still has to be admitted into
    # this node's own index before anything can name or build it. That is one
    # more routed call, once the carrier is whole. Provider discovery runs a
    # real bounded DHT lookup, so give it room between attempts instead of
    # hammering the node's discovery queue with a tight loop.
    local attempt=0 imported=False
    while [ "$attempt" -lt 6 ]; do
        out="$("cj_$node" zcode package fetch \
            --input="{\"root\":\"$transport\",\"namespace\":\"zclassic23.package\",\"maximum_bytes\":67108864}")"
        imported="$(cj_field data.reconstructed "$out" False)"
        [ "$imported" = True ] && break
        attempt=$((attempt + 1))
        sleep 5
    done
    [ "$imported" = True ] ||
        cj_die "node $node never imported the signed release for $root: $out"
    printf '%s\n' "$out" >"$DHT_WORK/import-$node-${root:0:16}.json"
    [ "$(cj_field data.package_root "$out" '')" = "$root" ] ||
        cj_die "node $node imported a carrier naming a different package: $out"
    cj_pin_root "$node" "$transport"
    cj_pin_root "$node" "$root"
}

# ── the journey ──────────────────────────────────────────────────────────
cj_journey_guide() {
    cj_step "1/9  zcode guide — the person says what they want"
    local guide
    guide="$(cj_a zcode guide)"
    cj_require_ok "zcode guide" "$guide"
    [ -n "$(cj_field data.start_command "$guide" '')" ] ||
        cj_die "zcode guide named no start command: $guide"
    case "$(cj_field data.journey "$guide" '')" in
        *reuse*) ;;
        *) cj_die "zcode guide does not describe reuse-first: $guide" ;;
    esac
    cj_note "guide -> $(cj_field data.next_action "$guide")"
}

cj_journey_publish_reusable() {
    cj_step "2/9  the commons already contains one finished, reusable package"
    CJ_PUBLISHER="$("$CJ_SIGNER" --generate "$DHT_WORK/publisher.key")"
    [ -n "$CJ_PUBLISHER" ] || cj_die "could not create the offline publisher identity"
    CJ_TEXTSTAT_SRC="$DHT_WORK/textstat"
    cp -a "$CJ_FIXTURES/textstat" "$CJ_TEXTSTAT_SRC"
    cj_publish_package a "$CJ_TEXTSTAT_SRC" 1
    CJ_TEXTSTAT_ROOT="$CJ_PKG_ROOT"
    CJ_TEXTSTAT_RELEASE="$CJ_PKG_RELEASE"
    CJ_TEXTSTAT_RELEASE_HEX="$CJ_PKG_RELEASE_HEX"
    CJ_TEXTSTAT_TRANSPORT="$CJ_PKG_TRANSPORT"
    cj_note "z23/textstat published on node A: ${CJ_TEXTSTAT_ROOT:0:16}…"

    # Discovery is a local index query over what the node actually holds.
    local search
    search="$(cj_a zcode package search --input='{"name_prefix":"z23/"}')"
    cj_require_ok "package search" "$search"
    case "$search" in
        *z23/textstat*) ;;
        *) cj_die "the published package is not discoverable: $search" ;;
    esac
}

# A second, independent machine gets the package over the network. Nothing
# central is involved: node A announces what it holds, node B asks the
# overlay for that exact content, and the bytes arrive as inert source.
cj_journey_peer_distribution() {
    cj_step "3/9  a second machine fetches it peer-to-peer — and the bytes stay inert"
    cj_announce_package a "$CJ_TEXTSTAT_ROOT" "$CJ_TEXTSTAT_TRANSPORT" 1

    # Before: node B has never seen this package.
    [ "$(cj_sql b "SELECT count(*) FROM build_receipts")" = 0 ] ||
        cj_die "node B already held build evidence before fetching anything"
    cj_fetch_package b "$CJ_TEXTSTAT_ROOT" "$CJ_TEXTSTAT_TRANSPORT"
    cj_note "node B fetched z23/textstat from the overlay: $CJ_FETCH_BYTES bytes"

    # PROOF: fetched source stays inert. Arriving on this machine executed
    # nothing and produced no evidence; only an explicit local decision can.
    [ "$(cj_sql b "SELECT count(*) FROM build_actions")" = 0 ] &&
    [ "$(cj_sql b "SELECT count(*) FROM build_receipts")" = 0 ] ||
        cj_die "fetching alone executed code or projected evidence on node B"

    # PROOF: build requires explicit local admission — and it is the person
    # on THIS machine who gives it. `zcode use` is that decision.
    # By content, not by a name anyone controls: node B admits the exact
    # root it fetched. Two machines agreeing on 64 hex characters is the
    # whole trust story.
    cj_use_package b "$CJ_TEXTSTAT_ROOT"
    cj_note "node B admitted z23/textstat explicitly: it is now installed there"
}

cj_journey_work_start_unavailable() {
    cj_step "4/9  zcode work start — reuse is searched before any code is written"
    CJ_WS="$DHT_WORK/wordcount"
    cp -a "$CJ_FIXTURES/wordcount" "$CJ_WS"
    # The application declares NO dependency yet. Whether z23/textstat may be
    # reused is a question about this node, and the honest answer before any
    # local admission is "not yet".
    cj_write_package_json "$CJ_WS" ""
    rm -f "$CJ_WS/zcode-package.json.in"

    local start plan
    start="$(cj_a zcode work start \
        --input="{\"workspace\":\"$CJ_WS\",\"goal\":\"$CJ_GOAL\",\"profile\":\"$CJ_PROFILE\"}")"
    cj_require_ok "work start (before local admission)" "$start"
    plan="$(cj_field data.reuse_plan "$start")"

    # PROOF 2: unavailable code is never claimed as reused.
    [ "$(printf '%s' "$plan" | cj_jget reused.0.name '')" = "" ] ||
        cj_die "a package that is not installed here was claimed as reused: $plan"
    [ "$(printf '%s' "$plan" | cj_jget available_after_use.0.name '')" = "z23/textstat" ] ||
        cj_die "the reusable package was not offered at all: $plan"
    [ "$(printf '%s' "$plan" | cj_jget available_after_use.0.composition '')" = \
      "explicit_use_required" ] ||
        cj_die "reuse did not require an explicit local admission: $plan"
    cj_note "before admission: reused=[] available_after_use=[z23/textstat]"

    # PROOF 3: the next step is to reuse, not to write code.
    case "$(cj_field data.next_safe_command "$start" '')" in
        "zcode use") ;;
        *) cj_die "work start told the person to write code before reusing: $start" ;;
    esac
    cj_human_first "work start (before admission)" "$start"
}

cj_journey_admit_reuse() {
    cj_step "5/9  zcode use — explicit local admission builds and installs it"
    cj_use_package a "$CJ_TEXTSTAT_ROOT"
    local installed="$DHT_DD_A/zcode/installed/$CJ_TEXTSTAT_ROOT"
    [ -f "$installed/lib/libtextstat.a" ] ||
        cj_die "admission produced no artifact for z23/textstat"
    [ -f "$installed/include/textstat/textstat.h" ] ||
        cj_die "admission produced no public header for z23/textstat"
    CJ_TEXTSTAT_ARTIFACT="$installed/lib/libtextstat.a"

    local start plan
    start="$(cj_a zcode work start \
        --input="{\"workspace\":\"$CJ_WS\",\"goal\":\"$CJ_GOAL\",\"profile\":\"$CJ_PROFILE\",\"details\":true}")"
    cj_require_ok "work start (after admission)" "$start"
    plan="$(cj_field data.reuse_plan "$start")"

    # PROOF 1: reusable code is selected before new code.
    [ "$(printf '%s' "$plan" | cj_jget reused.0.name '')" = "z23/textstat" ] ||
        cj_die "the admitted package was not selected for reuse: $plan"
    [ "$(printf '%s' "$plan" | cj_jget reused.0.installed False)" = True ] ||
        cj_die "reuse claimed a package that is not installed: $plan"
    [ "$(printf '%s' "$plan" | cj_jget available_after_use.0.name '')" = "" ] ||
        cj_die "an admitted package is still pending admission: $plan"
    # Its API is known by symbol, not guessed from the name.
    case "$(printf '%s' "$plan" | cj_jget reused.0.apis '')" in
        *textstat_words*) ;;
        *) cj_die "reuse selected a package without reading its API: $plan" ;;
    esac

    # PROOF 3: only the behavior still missing enters candidate work.
    [ "$(printf '%s' "$plan" | cj_jget new_code_required False)" = True ] ||
        cj_die "the missing behavior was not recognised as missing: $plan"
    [ "$(printf '%s' "$plan" | cj_jget missing '')" = "$CJ_GOAL" ] ||
        cj_die "the missing behavior is not the goal: $plan"
    CJ_WORK_ID="$(cj_field data.work_id "$start")"
    CJ_TASK_ROOT="$(printf '%s' "$(cj_field data.expert "$start")" | cj_jget task_root)"
    [ -n "$CJ_TASK_ROOT" ] || cj_die "work start bound no task root: $start"
    cj_note "after admission: reused=[z23/textstat installed] work=$CJ_WORK_ID"
}

# The one behavior nothing in the commons provides. It is written here, in
# the acceptance, because the acceptance is playing the part of the person
# (or the adapter) who supplies the missing code: the journey has to prove
# that only THIS enters candidate work, not that a model invented it.
cj_write_missing_behavior() {
    cat >>"$1/src/wordcount.c" <<'CREATED'

/* Created by this journey. Nothing in the commons measured a longest line,
 * so this is the only behavior that was written rather than reused. */
size_t wordcount_longest_line(const char *text, size_t len)
{
    if (!text) return 0;
    size_t longest = 0, current = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            if (current > longest) longest = current;
            current = 0;
            continue;
        }
        current++;
    }
    return current > longest ? current : longest;
}
CREATED
}

# The requester learns which identity proved its work from the receipt itself
# and approves that exact signer. There is no harness-side lifecycle bit: once
# the worker is approved the query returns no row and this is a no-op.
cj_sql() {
    local node="$1" sql="$2"
    "cj_$node" core storage query --sql="$sql" 2>/dev/null | cj_jget data.rows.0.0 ''
}

cj_approve_proving_worker() {
    local identity worker pubkey response
    identity="$(cj_sql a "SELECT r.worker_id||':'||w.signer_pubkey FROM build_receipts r JOIN build_workers w ON w.worker_id=r.worker_id WHERE r.trust_state='REMOTE_OBSERVED' AND w.approved=0 AND w.revoked=0 LIMIT 1")"
    [ -n "$identity" ] || return 0
    worker="${identity%%:*}"; pubkey="${identity#*:}"
    [ "${#worker}" -eq 64 ] && [ "${#pubkey}" -eq 64 ] ||
        cj_die "a remote receipt did not name its exact signer: $identity"
    # `datadir` again, and for the same reason as `zcode work run`: only an
    # explicitly targeted node ledger receives a live trust write. Without it
    # the leaf refuses with MISSING_DATADIR rather than quietly approving a
    # signer in a one-shot scratch lifecycle nobody will ever read.
    response="$(cj_a metaverse build worker approve \
        --input="{\"worker_id\":\"$worker\",\"signer_pubkey\":\"$pubkey\",\"capabilities\":\"p2p-approved,c23.package.recipe.v1\",\"datadir\":\"$DHT_DD_A\"}")"
    cj_require_ok "approving the proving worker" "$response"
    [ "$(cj_field data.approved "$response" False)" = True ] ||
        cj_die "the proving worker was not approved: $response"
    CJ_PROVER_WORKER="$worker"
    cj_note "node A approved the exact identity that proved its work: ${worker:0:16}…"
}

cj_wait_work_state() {
    local want="$1" budget="${2:-300}" deadline state show
    deadline=$(( $(date +%s) + budget ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        cj_approve_proving_worker
        show="$(cj_a zcode work status \
            --input="{\"workspace\":\"$CJ_WS\",\"work\":\"latest\",\"datadir\":\"$DHT_DD_A\"}")"
        state="$(cj_field data.state "$show" '')"
        [ "$state" = "$want" ] && { CJ_LAST_SHOW="$show"; return 0; }
        sleep 1
    done
    CJ_LAST_SHOW="$show"
    return 1
}

cj_journey_create_missing() {
    cj_step "6/9  zcode work run — only the missing behavior enters candidate work"
    local handoff candidate run
    handoff="$(cj_a zcode work run \
        --input="{\"workspace\":\"$CJ_WS\",\"work\":\"latest\",\"adapter\":\"manual\",\"details\":true}")"
    cj_require_ok "work run (handoff)" "$handoff"
    candidate="$(cj_field data.candidate_workspace "$handoff" '')"
    [ -d "$candidate" ] || cj_die "work run exported no candidate workspace: $handoff"
    [ "$(cj_field data.authority "$handoff" '')" = "NONE_MANUAL_HANDOFF" ] ||
        cj_die "the manual handoff claimed authority it must not have: $handoff"
    # The packet carries the goal and the exact locked dependency, so the
    # creator is told what is already reused rather than reimplementing it.
    local packet
    packet="$(cat "$candidate/.zcode-adapter-packet.json")"
    [ "$(printf '%s' "$packet" | cj_jget locked_dependencies.0.name '')" = \
      "z23/textstat" ] ||
        cj_die "the candidate packet did not carry the reused dependency"
    [ "$(printf '%s' "$packet" | cj_jget locked_dependencies.0.package_root '')" = \
      "$CJ_TEXTSTAT_ROOT" ] ||
        cj_die "the candidate packet bound a different dependency root"
    cj_write_missing_behavior "$candidate"
    # `datadir` is what turns this from a local-only projection into a real
    # submission by the live node: the requester binds the immutable action
    # and asks the overlay for an independent prover. A node never proves its
    # own work, so without this the action would simply sit SNAPSHOTTED.
    run="$(cj_a zcode work run \
        --input="{\"workspace\":\"$CJ_WS\",\"work\":\"latest\",\"adapter\":\"manual\",\"datadir\":\"$DHT_DD_A\",\"details\":true}")"
    cj_require_ok "work run (candidate)" "$run"
    [ "$(cj_field data.state "$run" '')" = "CANDIDATE_ADMITTED" ] ||
        cj_die "the candidate was not admitted: $run"
    # Exactly one source file changed: the created behavior, nothing else.
    CJ_CANDIDATE_ROOT="$(printf '%s' "$(cj_field data.expert "$run")" | cj_jget candidate_root)"
    CJ_ACTION_ID="$(printf '%s' "$(cj_field data.expert "$run")" | cj_jget action_id)"
    [ -n "$CJ_CANDIDATE_ROOT" ] && [ -n "$CJ_ACTION_ID" ] ||
        cj_die "the admitted candidate bound no candidate root or action: $run"
    # PROOF: the requester asked the commons instead of proving itself. It
    # owns the immutable action and one REQUESTED proof event, and its own
    # copy of that action stays SNAPSHOTTED with no worker and no attempt.
    CJ_PROOF_EVENT="$(cj_field data.async_proof_event_root "$run" '')"
    [ "${#CJ_PROOF_EVENT}" -eq 64 ] ||
        cj_die "the admitted candidate carried no async proof event root: $run"
    [ "$(cj_sql a "SELECT count(*) FROM build_proof_events WHERE action_id='$CJ_ACTION_ID' AND state='REQUESTED'")" = 1 ] ||
        cj_die "node A did not request independent proof for $CJ_ACTION_ID"
    [ "$(cj_sql a "SELECT count(*) FROM build_actions WHERE action_id='$CJ_ACTION_ID' AND state='SNAPSHOTTED' AND attempt_count=0 AND started_at=0 AND length(worker_id)=0")" = 1 ] ||
        cj_die "node A executed its own work instead of asking the commons"
    cj_human_first "work run (candidate)" "$run"
    cj_note "candidate admitted: ${CJ_CANDIDATE_ROOT:0:16}… action ${CJ_ACTION_ID:0:16}…"
}

# EVIDENCE_READY says the proof arrived, not that it counts. A receipt from
# another machine lands as REMOTE_OBSERVED: real evidence, no authority. The
# operator has to approve that exact signer before the ledger will let it
# stand as a build result — which is the whole point, and also why polling
# only for the state is not enough. Approve, then wait for the result the
# person actually reads.
cj_wait_proof_result() {
    local budget="${1:-180}" deadline show result
    deadline=$(( $(date +%s) + budget ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        cj_approve_proving_worker
        show="$(cj_a zcode work show \
            --input="{\"workspace\":\"$CJ_WS\",\"work\":\"latest\",\"datadir\":\"$DHT_DD_A\"}")"
        result="$(cj_field data.confirmation_ready "$show" False)"
        [ "$result" = True ] && { CJ_LAST_SHOW="$show"; return 0; }
        sleep 1
    done
    CJ_LAST_SHOW="$show"
    return 1
}

cj_journey_show() {
    cj_step "7/9  zcode work show — the person sees the real consequence"
    cj_wait_work_state EVIDENCE_READY ||
        cj_die "the candidate never reached EVIDENCE_READY: $CJ_LAST_SHOW"
    cj_wait_proof_result ||
        cj_die "node B proved it, but node A never counted the result: $CJ_LAST_SHOW"
    local plain detailed
    # The same `datadir` the proof was submitted through. Without it this leaf
    # reads no canonical proof ledger at all and answers build_result and
    # test_result "unknown" for work that is demonstrably EVIDENCE_READY —
    # the person is shown "no proof result inferred" about their own proven
    # change. `zcode.work.show` used to REJECT this key outright while its
    # twin `zcode.work.status` accepted it; check_command_input_keys now
    # fails on that divergence.
    plain="$(cj_a zcode work show \
        --input="{\"workspace\":\"$CJ_WS\",\"work\":\"latest\",\"datadir\":\"$DHT_DD_A\"}")"
    cj_require_ok "work show" "$plain"
    detailed="$(cj_a zcode work show \
        --input="{\"workspace\":\"$CJ_WS\",\"work\":\"latest\",\"datadir\":\"$DHT_DD_A\",\"details\":true}")"
    cj_require_ok "work show (details)" "$detailed"

    # The consequence is the changed files and the proof result, in words.
    [ "$(cj_field data.changed_files "$plain" 0)" -ge 1 ] ||
        cj_die "work show reported no change at all: $plain"
    case "$(cj_field data.changed_paths "$plain" '')" in
        *src/wordcount.c*) ;;
        *) cj_die "work show did not name the file that changed: $plain" ;;
    esac
    [ "$(cj_field data.build_result "$plain" '')" = passed ] ||
        cj_die "work show does not report a passing build: $plain"
    case "$(cj_field data.test_result "$plain" '')" in
        passed*) ;;
        *) cj_die "work show does not report passing tests: $plain" ;;
    esac
    # The grade is the whole point of the second machine: node A did not build
    # this, node B did, and node A counts it only because it approved that
    # exact signer.
    [ "$(cj_field data.reproduction_grade "$plain" '')" != none ] ||
        cj_die "work show claims no independent reproduction at all: $plain"
    [ "$(cj_field data.confirmation_ready "$plain" False)" = True ] ||
        cj_die "work show does not offer the person a decision: $plain"
    cj_human_first "work show" "$plain"
    cj_roots_hidden "work show" "$plain" "$detailed"
}

# ── the authenticated overlay the journey travels over ───────────────────
# Two service types cross it. `zclassic23.package` carries software bytes
# from whoever holds them; `zclassic23.work` carries one immutable action to
# whoever is willing to prove it. A node never proves its own work: `zcode
# work run` deliberately leaves its action SNAPSHOTTED so the requester's own
# worker cannot race the peer and mask missing remote evidence. Node B is
# therefore the independent build worker, and this is what makes that
# possible — one allow rule per service type on each node, and one real
# authenticated DHT session between them.
cj_allow_policy() {
    local node="$1" service="$2" common plan token commit ok code message
    common='"operation":"add","source":"local","effect":"allow","scope":"service_type","action_mask":63,"value":"'"$service"'"'
    plan="$("cj_$node" zcode network policy mutate --input="{\"mode\":\"plan\",$common}")"
    cj_require_ok "node $node $service policy plan" "$plan"
    token="$(cj_field data.plan_token "$plan")"
    commit="$("cj_$node" zcode network policy mutate \
        --input="{\"mode\":\"commit\",$common,\"plan_token\":\"$token\"}")"
    ok="$(cj_field ok "$commit" False)"
    code="$(cj_field error.code "$commit" '')"
    message="$(cj_field error.message "$commit" '')"
    [ "$ok" = True ] ||
        { [ "$code" = POLICY_REFUSED ] && [ "$message" = duplicate ]; } ||
        cj_die "node $node could not allow the $service service type: $commit"
}

cj_wait_dht_enabled() {
    local deadline a b
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        a="$(cj_field data.enabled "$(dht_status "$DHT_DD_A" "$A_RPC")" False)"
        b="$(cj_field data.enabled "$(dht_status "$DHT_DD_B" "$B_RPC")" False)"
        [ "$a" = True ] && [ "$b" = True ] && return 0
        sleep 0.5
    done
    return 1
}

# Capability learning tears down the first plaintext P2P connection and
# replaces it with Noise, and a lookup admitted in that interval belongs to
# the retired transport. Observe both ends and re-arm one fresh lookup once.
cj_connect_authenticated() {
    local deadline find lookup owner rearmed=0 auth_a auth_b started
    # Both directions. Software travels the same links the chain does, and a
    # node that only ever accepts inbound connections is not a participant in
    # the commons: it has to be able to ask a peer for bytes too.
    a_rpc addnode "\"127.0.0.1:$B_PORT\"" '"onetry"' >/dev/null || true
    b_rpc addnode "\"127.0.0.1:$A_PORT\"" '"onetry"' >/dev/null || true
    cj_wait_dht_enabled || cj_die "the two nodes' DHTs never both enabled"
    find="$(cj_a zcode network find begin --input="{\"node_id\":\"$CJ_NODE_B\"}")"
    cj_require_ok "node A lookup of node B" "$find"
    lookup="$(cj_field data.lookup_id "$find")"
    owner="$(cj_field data.owner_token "$find")"
    started="$(date +%s)"
    deadline=$((started + DHT_WAIT))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        auth_a="$(cj_field data.connected_authenticated \
            "$(dht_status "$DHT_DD_A" "$A_RPC")" 0)"
        auth_b="$(cj_field data.connected_authenticated \
            "$(dht_status "$DHT_DD_B" "$B_RPC")" 0)"
        if [ "${auth_a:-0}" -ge 1 ] 2>/dev/null &&
           [ "${auth_b:-0}" -ge 1 ] 2>/dev/null; then
            cj_a zcode network find cancel \
                --input="{\"lookup_id\":\"$lookup\",\"owner_token\":\"$owner\"}" \
                >/dev/null || true
            cj_note "authenticated overlay session established A <-> B"
            return 0
        fi
        if [ "$rearmed" -eq 0 ] && [ "$(date +%s)" -ge $((started + 20)) ]; then
            rearmed=1
            find="$(cj_a zcode network find begin \
                --input="{\"node_id\":\"$CJ_NODE_B\"}")"
            lookup="$(cj_field data.lookup_id "$find" '')"
            owner="$(cj_field data.owner_token "$find" '')"
        fi
        sleep 0.5
    done
    cj_die "the two nodes never formed an authenticated overlay session"
}

cj_overlay() {
    cj_step "the authenticated overlay the two nodes share"
    cj_allow_policy a zclassic23.package; cj_allow_policy a zclassic23.work
    cj_allow_policy b zclassic23.package; cj_allow_policy b zclassic23.work
    dht_kill_group "$DHT_PGID_B"; DHT_PGID_B=""
    dht_kill_group "$DHT_PGID_A"; DHT_PGID_A=""
    DHT_BUILDWORKERS=1
    dht_spawn DHT_PGID_B "$DHT_DD_B" "$B_PORT" "$B_RPC" "$B_FS" \
        "$B_HTTPS" "127.0.0.1:$DEAD_SINK"
    cj_wait_rpc_or_die "$DHT_DD_B" "$B_RPC" "$DHT_PGID_B" "node B (build worker)"
    DHT_BUILDWORKERS=0
    dht_spawn DHT_PGID_A "$DHT_DD_A" "$A_PORT" "$A_RPC" "$A_FS" \
        "$A_HTTPS" "127.0.0.1:$DEAD_SINK"
    cj_wait_rpc_or_die "$DHT_DD_A" "$A_RPC" "$DHT_PGID_A" "node A (requester)"
    cj_connect_authenticated
}

cj_step "bring-up: two fresh isolated datadirs"
cj_boot
cj_build_peer_helper
cj_identities

cj_step "the journey"
cj_journey_guide
cj_journey_publish_reusable
# The hosting engine that serves package bytes to peers is built at node
# start from the store on disk. Publishing is an ordinary store write, so
# the node that will serve it comes up after the package exists — the same
# ordering every real publisher has: put the software on the machine, then
# run the node that shares it.
cj_overlay
cj_journey_peer_distribution
cj_journey_work_start_unavailable
cj_journey_admit_reuse
cj_journey_create_missing
cj_journey_show

# The verdict names its own coverage. Steps 8 and 9 — zcode work accept, the
# accepted app's own publication and byte-identical remote source
# reproduction, the four tamper refusals, and zcode use running the result —
# are not asserted yet, so a green run of this script must not be read as the
# complete journey. It says so itself rather than leaving exit 0 to imply it.
cj_step "verdict"
printf '%s\n' '{"schema":"zcl.commons_journey_acceptance.v1","verdict":"PASS","steps_proven":7,"steps_total":9,"complete":false,"reuse_before_creation":true,"no_false_reuse_claim":true,"peer_to_peer_fetch":true,"fetched_source_inert":true,"explicit_local_admission":true,"independent_remote_build":true,"approved_signer_required":true,"human_first_terminal_output":true,"not_yet_proven":["work accept","accepted-app publication","remote source reproduction","tamper refused by name","zcode use runs the app"]}'
