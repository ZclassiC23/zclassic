#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# S6 acceptance: two isolated regtest nodes anchor independent ZID masters,
# provision chain-bound DHT delegations, upgrade their existing P2P link to
# Noise XX, exchange FIND_NODE/NODES, persist authenticated contacts, reload
# them cold, and recover the same incumbent after a short disconnect.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NODE_BIN="${ZCL_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
RPC_BIN="${ZCL_RPC_BIN:-$REPO_ROOT/build/bin/zcl-rpc}"

DHT_LIVE_PORTS="8023 8033 8034 8035 8043 8044 8045 8046 8232 8443 \
18034 18232 18234 18243 18244 18245 18246"
# P2P reconnects pass the production reachable-port policy; use two of its
# explicit test-safe ports rather than arbitrary high ports that only the
# initial operator-directed -connect dial may bypass.
A_PORT=20022; A_RPC=39211; A_FS=39212; A_HTTPS=39213
B_PORT=18033; B_RPC=39221; B_FS=39222; B_HTTPS=39223
DEAD_SINK=39999
DHT_WAIT="${DHT_WAIT:-90}"
DHT_WORK=""; DHT_DD_A=""; DHT_DD_B=""; DHT_PGID_A=""; DHT_PGID_B=""
DHT_CLEANED=0
DHT_KEEP="${DHT_KEEP:-0}"

dht_die() {
    echo "zcode-dht-acceptance: FATAL: $*" >&2
    if [ -n "$DHT_WORK" ] && [ -d "$DHT_WORK" ]; then
        printf '%s\n' "$*" >"$DHT_WORK/FAILURE"
    fi
    exit 2
}
dht_note() { echo "zcode-dht-acceptance: $*"; }

dht_assert_port() {
    local p="$1" live
    for live in $DHT_LIVE_PORTS; do
        [ "$p" = "$live" ] && dht_die "port $p is in the live refuse-set"
    done
    ss -tlnH "sport = :$p" 2>/dev/null | grep -q . &&
        dht_die "port $p is already listening"
    return 0
}

dht_kill_group() {
    local pgid="$1" sig="${2:-TERM}" i
    [ -n "$pgid" ] || return 0
    kill -"$sig" "-$pgid" 2>/dev/null || true
    for i in $(seq 1 50); do
        kill -0 "-$pgid" 2>/dev/null || return 0
        sleep 0.2
    done
    kill -KILL "-$pgid" 2>/dev/null || true
}

dht_cleanup() {
    [ "$DHT_CLEANED" = 1 ] && return 0
    DHT_CLEANED=1
    dht_kill_group "$DHT_PGID_A"
    dht_kill_group "$DHT_PGID_B"
    if [ "$DHT_KEEP" = 1 ] && [ -n "$DHT_WORK" ]; then
        dht_note "preserved acceptance artifacts: $DHT_WORK"
    elif [ -n "$DHT_WORK" ] && [ -d "$DHT_WORK" ]; then
        case "$DHT_WORK" in
            "$REPO_ROOT"/test-tmp/zcl23-dhtacc-*) rm -rf "$DHT_WORK" ;;
            *) dht_note "WARN refusing to remove non-scratch $DHT_WORK" ;;
        esac
    fi
}
trap dht_cleanup EXIT INT TERM

dht_rpc() {
    local dd="$1" port="$2"; shift 2
    ZCL_DATADIR="$dd" ZCL_RPCPORT="$port" "$RPC_BIN" "$@" 2>/dev/null
}
a_rpc() { dht_rpc "$DHT_DD_A" "$A_RPC" "$@"; }
b_rpc() { dht_rpc "$DHT_DD_B" "$B_RPC" "$@"; }
dht_result() {
    python3 -c 'import json,sys
d=json.load(sys.stdin)
if d.get("error") is not None: raise SystemExit(2)
v=d.get("result")
print(json.dumps(v,separators=(",",":")) if isinstance(v,(dict,list)) else v)'
}
dht_jget() {
    local expr="$1"
    python3 -c "import json,sys; d=json.load(sys.stdin); print($expr)"
}
dht_native() {
    local dd="$1" rpc="$2"; shift 2
    "$NODE_BIN" -datadir="$dd" -rpcport="$rpc" "$@" 2>/dev/null | tail -1
}
dht_status() { dht_native "$1" "$2" zcode network status; }

dht_spawn() {
    local dd="$1" p2p="$2" rpc="$3" fs="$4" https="$5" connect="$6"
    setsid "$NODE_BIN" -datadir="$dd" -regtest -port="$p2p" \
        -rpcport="$rpc" -fsport="$fs" -httpsport="$https" \
        -connect="$connect" -packagehost=0 -v2transport \
        -allow-plaintext-wallet -wallet-no-phrase-backup \
        -nobgvalidation -nolegacyimport -showmetrics=0 \
        >>"$dd/node.log" 2>&1 &
    echo "$!"
}

dht_height() {
    dht_rpc "$1" "$2" getblockcount | dht_result
}
dht_wait_rpc() {
    local dd="$1" rpc="$2" pid="$3" deadline
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        kill -0 "$pid" 2>/dev/null || return 1
        [ -f "$dd/.cookie" ] && dht_height "$dd" "$rpc" >/dev/null 2>&1 && return 0
        sleep 0.5
    done
    return 1
}
dht_wait_height() {
    local dd="$1" rpc="$2" target="$3" deadline h
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        h="$(dht_height "$dd" "$rpc" 2>/dev/null || true)"
        [ "$h" = "$target" ] && return 0
        sleep 0.5
    done
    return 1
}

# The regtest miner stamps blocks from whole-second wall time.  More than six
# consecutive blocks with one timestamp becomes <= the peer's 11-block MTP
# even though the local submission path accepted the batch.  Mine in groups
# of five with a wall-clock step so the second node validates the same chain.
dht_mine_to_address() {
    local count="$1" address="$2" chunk
    while [ "$count" -gt 0 ]; do
        chunk=5
        [ "$count" -lt "$chunk" ] && chunk="$count"
        a_rpc generatetoaddress "$chunk" "\"$address\"" | dht_result >/dev/null
        count=$((count - chunk))
        [ "$count" -eq 0 ] || sleep 1
    done
}
dht_mine_empty() {
    local count="$1" chunk
    while [ "$count" -gt 0 ]; do
        chunk=5
        [ "$count" -lt "$chunk" ] && chunk="$count"
        a_rpc generate "$chunk" | dht_result >/dev/null
        count=$((count - chunk))
        [ "$count" -eq 0 ] || sleep 1
    done
}
dht_wait_auth() {
    local dd="$1" rpc="$2" deadline out enabled auth accepted
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        out="$(dht_status "$dd" "$rpc" 2>/dev/null || true)"
        enabled="$(printf '%s' "$out" | dht_jget 'd.get("data",{}).get("enabled",False)' 2>/dev/null || true)"
        auth="$(printf '%s' "$out" | dht_jget 'd.get("data",{}).get("connected_authenticated",0)' 2>/dev/null || true)"
        accepted="$(printf '%s' "$out" | dht_jget 'd.get("data",{}).get("frames_accepted",0)' 2>/dev/null || true)"
        [ "$enabled" = True ] && [ "${auth:-0}" -ge 1 ] && [ "${accepted:-0}" -ge 1 ] && return 0
        sleep 0.5
    done
    return 1
}
dht_wait_cold_load() {
    local dd="$1" rpc="$2" deadline out loaded cold
    deadline=$(( $(date +%s) + DHT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        out="$(dht_status "$dd" "$rpc" 2>/dev/null || true)"
        loaded="$(printf '%s' "$out" | dht_jget 'd.get("data",{}).get("persistence_loaded",False)' 2>/dev/null || true)"
        cold="$(printf '%s' "$out" | dht_jget 'd.get("data",{}).get("cold_contacts",0)' 2>/dev/null || true)"
        [ "$loaded" = True ] && [ "${cold:-0}" -eq 1 ] && return 0
        sleep 0.5
    done
    return 1
}

dht_build_helper() {
    cc -std=c23 -O1 -w -D_GNU_SOURCE -ffunction-sections -fdata-sections \
        -Wl,--gc-sections -I"$REPO_ROOT/lib/base/include" \
        -I"$REPO_ROOT/lib/crypto/include" -I"$REPO_ROOT/lib/support/include" \
        -I"$REPO_ROOT/lib/util/include" -I"$REPO_ROOT/lib/platform/include" \
        -I"$REPO_ROOT/lib/json/include" -I"$REPO_ROOT/lib/core/include" \
        -I"$REPO_ROOT/lib/net/include" -I"$REPO_ROOT/lib/noise/include" \
        -I"$REPO_ROOT/lib/vcs/include" -I"$REPO_ROOT/lib/zid/include" \
        -I"$REPO_ROOT/core/math/include" -o "$DHT_WORK/dht-peer" \
        "$REPO_ROOT/tools/zcode_dht_acceptance_peer.c" \
        "$REPO_ROOT/lib/net/src/v2_transport.c" \
        "$REPO_ROOT/lib/noise/src/noise_handshake.c" \
        "$REPO_ROOT/lib/noise/src/session_transport.c" \
        "$REPO_ROOT/lib/vcs/src/zcode_dht.c" \
        "$REPO_ROOT/lib/vcs/src/zcode_dht_delegation.c" \
        "$REPO_ROOT/lib/vcs/src/zcode_dht_identity.c" \
        "$REPO_ROOT/lib/vcs/src/zcode_dht_msgs.c" \
        "$REPO_ROOT/lib/zid/src/zid.c" \
        "$REPO_ROOT/lib/zid/src/zendp.c" \
        "$REPO_ROOT/lib/crypto/src/ed25519.c" \
        "$REPO_ROOT/lib/crypto/src/sha512.c" \
        "$REPO_ROOT/lib/crypto/src/sha256.c" \
        "$REPO_ROOT/lib/crypto/src/sha3.c" \
        "$REPO_ROOT/lib/crypto/src/hmac_sha256.c" \
        "$REPO_ROOT/lib/crypto/src/hkdf_sha256.c" \
        "$REPO_ROOT/lib/crypto/src/chacha20poly1305.c" \
        "$REPO_ROOT/lib/crypto/src/curve25519.c" \
        "$REPO_ROOT/lib/crypto/src/x25519_safe.c" \
        "$REPO_ROOT/lib/crypto/src/random_secret.c" \
        "$REPO_ROOT/core/math/src/hash.c" \
        "$REPO_ROOT/lib/core/src/utiltime.c" \
        "$REPO_ROOT/lib/core/src/random.c" \
        "$REPO_ROOT/lib/base/src/safe_alloc.c" \
        "$REPO_ROOT/lib/base/src/log_level.c" \
        "$REPO_ROOT/lib/base/src/result.c" \
        "$REPO_ROOT/lib/support/src/cleanse.c" \
        "$REPO_ROOT/lib/platform/src/clock.c" \
        "$REPO_ROOT/lib/platform/src/rng.c" \
        "$REPO_ROOT/lib/util/src/write_all.c" \
        "$REPO_ROOT/lib/json/src/json.c" \
        "$REPO_ROOT/lib/util/src/hw_profile.c" \
        "$REPO_ROOT/lib/util/src/cpu_topology.c" 2>/dev/null ||
        dht_die "acceptance helper compile failed"
}

dht_check_find() {
    local reply="$1" target="$2" a_id="$3" b_id="$4"
    printf '%s' "$reply" | python3 -c '
import json,sys
d=json.load(sys.stdin)
target=int(sys.argv[1],16); expected={sys.argv[2],sys.argv[3]}
assert d["ok"] is True, d
rows=d["data"]["node_ids"]
assert len(rows) == 2 and set(rows) == expected, rows
assert rows == sorted(rows,key=lambda x:(int(x,16)^target,x)), rows
' "$target" "$a_id" "$b_id" || dht_die "FIND_NODE result/order mismatch"
}

dht_check_contacts_file() {
    local path="$1" self_id="$2"
    python3 -c '
import pathlib,struct,sys
p=pathlib.Path(sys.argv[1]); b=p.read_bytes(); self_id=bytes.fromhex(sys.argv[2])
assert b[:8] == b"ZCDHTC\r\n"
assert struct.unpack_from("<H",b,8)[0] == 2
assert b[42:74] == self_id
n=struct.unpack_from("<I",b,74)[0]
entry_bytes=303 # node id + canonical delegation + local time/failures
assert n == 1 and len(b) == 78 + n*entry_bytes
ids=[b[78+i*entry_bytes:110+i*entry_bytes] for i in range(n)]
assert ids == sorted(ids) and len(ids) == len(set(ids))
' "$path" "$self_id" || dht_die "non-canonical contacts file: $path"
}

dht_check_attack_deltas() {
    local before="$1" after="$2"
    python3 -c '
import json,sys
b=json.loads(sys.argv[1])["data"]; a=json.loads(sys.argv[2])["data"]
br=b["frames_rejected"]; ar=a["frames_rejected"]
want={"malformed":2,"identity":1,"replay":1,"unsolicited":1,
      "expired":1,"poisoned-contacts":1}
for key,delta in want.items():
    assert ar[key]-br[key] == delta,(key,br[key],ar[key])
for key in set(ar)-set(want):
    assert ar[key]-br[key] == 0,(key,br[key],ar[key])
assert a["frames_accepted"]-b["frames_accepted"] >= 1
' "$before" "$after" || dht_die "hostile Noise-frame counter deltas differ"
}

for port in $A_PORT $A_RPC $A_FS $A_HTTPS $B_PORT $B_RPC $B_FS $B_HTTPS; do
    dht_assert_port "$port"
done
[ -x "$NODE_BIN" ] && [ -x "$RPC_BIN" ] || dht_die "build node and RPC binaries first"
mkdir -p "$REPO_ROOT/test-tmp"
DHT_WORK="$(mktemp -d "$REPO_ROOT/test-tmp/zcl23-dhtacc-XXXXXX")"
DHT_DD_A="$DHT_WORK/a"; DHT_DD_B="$DHT_WORK/b"
mkdir -p "$DHT_DD_A" "$DHT_DD_B"
dht_build_helper

SEED_A=1111111111111111111111111111111111111111111111111111111111111111
SEED_B=2222222222222222222222222222222222222222222222222222222222222222
install -m 600 /dev/null "$DHT_WORK/master-a.hex"
install -m 600 /dev/null "$DHT_WORK/master-b.hex"
printf '%s\n' "$SEED_A" >"$DHT_WORK/master-a.hex"
printf '%s\n' "$SEED_B" >"$DHT_WORK/master-b.hex"
PUB_A="$("$DHT_WORK/dht-peer" pubkey "$SEED_A")"
PUB_B="$("$DHT_WORK/dht-peer" pubkey "$SEED_B")"

dht_note "booting two clean packagehost-off regtest nodes"
DHT_PGID_A="$(dht_spawn "$DHT_DD_A" "$A_PORT" "$A_RPC" "$A_FS" "$A_HTTPS" "127.0.0.1:$DEAD_SINK")"
dht_wait_rpc "$DHT_DD_A" "$A_RPC" "$DHT_PGID_A" || dht_die "node A RPC warmup failed"
DHT_PGID_B="$(dht_spawn "$DHT_DD_B" "$B_PORT" "$B_RPC" "$B_FS" "$B_HTTPS" "127.0.0.1:$A_PORT")"
dht_wait_rpc "$DHT_DD_B" "$B_RPC" "$DHT_PGID_B" || dht_die "node B RPC warmup failed"
! rg -q "unrecognized flag '-v2transport'" "$DHT_DD_A/node.log" "$DHT_DD_B/node.log" ||
    dht_die "v2transport was not recognized"

dht_note "mining spendable regtest funds and anchoring both masters"
ADDR="$(a_rpc getnewaddress | dht_result)"
dht_mine_to_address 101 "$ADDR"
dht_wait_height "$DHT_DD_B" "$B_RPC" 101 || dht_die "B did not sync funding chain"
ANCHOR_A="$(dht_native "$DHT_DD_A" "$A_RPC" core identity anchor --input="{\"pubkey\":\"$PUB_A\"}")"
ANCHOR_B="$(dht_native "$DHT_DD_A" "$A_RPC" core identity anchor --input="{\"pubkey\":\"$PUB_B\"}")"
[ "$(printf '%s' "$ANCHOR_A" | dht_jget 'd["ok"]')" = True ] || dht_die "A anchor failed: $ANCHOR_A"
[ "$(printf '%s' "$ANCHOR_B" | dht_jget 'd["ok"]')" = True ] || dht_die "B anchor failed: $ANCHOR_B"
dht_mine_empty 21
dht_wait_height "$DHT_DD_B" "$B_RPC" 122 || dht_die "B did not sync final beacon chain"

dht_note "provisioning independent delegations through the operator leaf"
DELEGATE_A="$(dht_native "$DHT_DD_A" "$A_RPC" zcode network delegate --input="{\"seed_file\":\"$DHT_WORK/master-a.hex\"}")"
DELEGATE_B="$(dht_native "$DHT_DD_B" "$B_RPC" zcode network delegate --input="{\"seed_file\":\"$DHT_WORK/master-b.hex\"}")"
[ "$(printf '%s' "$DELEGATE_A" | dht_jget 'd["ok"]')" = True ] || dht_die "A delegation failed: $DELEGATE_A"
[ "$(printf '%s' "$DELEGATE_B" | dht_jget 'd["ok"]')" = True ] || dht_die "B delegation failed: $DELEGATE_B"
NODE_A="$(printf '%s' "$DELEGATE_A" | dht_jget 'd["data"]["node_id"]')"
NODE_B="$(printf '%s' "$DELEGATE_B" | dht_jget 'd["data"]["node_id"]')"
[ "$NODE_A" != "$NODE_B" ] || dht_die "independent masters derived one node ID"

dht_note "restarting to prove capability learning, Noise, and DHT bootstrap"
dht_kill_group "$DHT_PGID_B"; DHT_PGID_B=""
dht_kill_group "$DHT_PGID_A"; DHT_PGID_A=""
DHT_PGID_A="$(dht_spawn "$DHT_DD_A" "$A_PORT" "$A_RPC" "$A_FS" "$A_HTTPS" "127.0.0.1:$DEAD_SINK")"
dht_wait_rpc "$DHT_DD_A" "$A_RPC" "$DHT_PGID_A" || dht_die "A restart failed"
DHT_PGID_B="$(dht_spawn "$DHT_DD_B" "$B_PORT" "$B_RPC" "$B_FS" "$B_HTTPS" "127.0.0.1:$A_PORT")"
dht_wait_rpc "$DHT_DD_B" "$B_RPC" "$DHT_PGID_B" || dht_die "B restart failed"
dht_wait_auth "$DHT_DD_A" "$A_RPC" || dht_die "A never authenticated B over DHT"
dht_wait_auth "$DHT_DD_B" "$B_RPC" || dht_die "B never authenticated A over DHT"
rg -q 'controlled Noise reconnect requested' "$DHT_DD_B/node.log" ||
    dht_die "plaintext capability-learning reconnect was not observed"

TARGET_A=0101010101010101010101010101010101010101010101010101010101010101
TARGET_B=fefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefe
FIND_A="$(dht_native "$DHT_DD_A" "$A_RPC" zcode network find --input="{\"node_id\":\"$TARGET_A\"}")"
FIND_B="$(dht_native "$DHT_DD_B" "$B_RPC" zcode network find --input="{\"node_id\":\"$TARGET_B\"}")"
dht_check_find "$FIND_A" "$TARGET_A" "$NODE_A" "$NODE_B"
dht_check_find "$FIND_B" "$TARGET_B" "$NODE_A" "$NODE_B"

dht_note "rejecting hostile frames inside an authenticated Noise session"
ATTACK_BEFORE="$(dht_status "$DHT_DD_A" "$A_RPC")"
"$DHT_WORK/dht-peer" attack 127.0.0.1 "$A_PORT" "$DHT_DD_B" |
    grep -qx 'attack-sequence-sent' || dht_die "hostile Noise peer failed"
ATTACK_AFTER="$(dht_status "$DHT_DD_A" "$A_RPC")"
dht_check_attack_deltas "$ATTACK_BEFORE" "$ATTACK_AFTER"

dht_note "clean shutdown, canonical-file check, and cold reload"
dht_kill_group "$DHT_PGID_B"; DHT_PGID_B=""
dht_kill_group "$DHT_PGID_A"; DHT_PGID_A=""
dht_check_contacts_file "$DHT_DD_A/zcode/dht/contacts.v2" "$NODE_A"
dht_check_contacts_file "$DHT_DD_B/zcode/dht/contacts.v2" "$NODE_B"

# Start each node without its peer: the loaded contact must be visible as
# cold before any network refresh can authenticate it.
DHT_PGID_A="$(dht_spawn "$DHT_DD_A" "$A_PORT" "$A_RPC" "$A_FS" "$A_HTTPS" "127.0.0.1:$DEAD_SINK")"
dht_wait_rpc "$DHT_DD_A" "$A_RPC" "$DHT_PGID_A" || dht_die "A cold-load boot failed"
dht_wait_cold_load "$DHT_DD_A" "$A_RPC" ||
    dht_die "A persisted contact did not publish cold"
DHT_PGID_B="$(dht_spawn "$DHT_DD_B" "$B_PORT" "$B_RPC" "$B_FS" "$B_HTTPS" "127.0.0.1:$A_PORT")"
dht_wait_rpc "$DHT_DD_B" "$B_RPC" "$DHT_PGID_B" || dht_die "B reload boot failed"
dht_wait_auth "$DHT_DD_A" "$A_RPC" || dht_die "A reload did not refresh B"
dht_wait_auth "$DHT_DD_B" "$B_RPC" || dht_die "B reload did not refresh A"

dht_note "short disconnect retains incumbent, then reconnect resets it"
dht_kill_group "$DHT_PGID_A"; DHT_PGID_A=""
sleep 2
PEERS="$(dht_native "$DHT_DD_B" "$B_RPC" zcode network peers --input='{"limit":64}')"
[ "$(printf '%s' "$PEERS" | dht_jget 'd["data"]["count"]')" -eq 1 ] || dht_die "B evicted A during a short disconnect"
DHT_PGID_A="$(dht_spawn "$DHT_DD_A" "$A_PORT" "$A_RPC" "$A_FS" "$A_HTTPS" "127.0.0.1:$DEAD_SINK")"
dht_wait_rpc "$DHT_DD_A" "$A_RPC" "$DHT_PGID_A" || dht_die "A recovery boot failed"
dht_wait_auth "$DHT_DD_B" "$B_RPC" || dht_die "B did not reauthenticate A"
FINAL_FIND="$(dht_native "$DHT_DD_B" "$B_RPC" zcode network find --input="{\"node_id\":\"$TARGET_A\"}")"
dht_check_find "$FINAL_FIND" "$TARGET_A" "$NODE_A" "$NODE_B"

dht_note "PASS: chain-bound Noise DHT discovery/find/persistence/recovery"
