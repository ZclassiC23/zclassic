#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Moderation acceptance: two isolated regtest daemons apply DIFFERENT
# moderation profiles to the SAME signed market offer. A (seller,
# -externalip + file service) commits one signed paid offer; it gossips
# to B (buyer) over the loopback P2P link. The proof then walks the
# per-node moderation surface (schema v65 review_state):
#   1. B's boot-default general-audience.v1 profile HIDES the offer from
#      `app market list` (it ingested as unreviewed) with an honest
#      hidden_count >= 1, and `app market moderation status` names the
#      active profile and the unreviewed count.
#   2. The explicit per-request opt-in (`profile:"open"`) shows the same
#      offer annotated review_state=unreviewed.
#   3. Switching B's node default to open-view (plan/commit with the
#      leaf's plan_token) shows it on the default list.
#   4. Back on general-audience, B's OWN review marks drive visibility:
#      reviewed_ok shows, sensitive hides; the open view always shows
#      the current annotation.
#   5. A (never touched by moderation) keeps its own view of the same
#      offer_id: the two nodes legitimately disagree — B shows the
#      reviewed_ok offer while A's default list hides it. Hidden is not
#      rejected: file_offers holds exactly one row on BOTH nodes.
#   6. Protocol-validity separation: the signed wire columns
#      (auth_version, endpoint, ids, pubkeys, signature, expiry) are
#      byte-identical on A and B and unchanged on B across every
#      moderation action — review_state never enters the wire and never
#      leaves the node (the review-set reply says local_only=true,
#      gossiped=false).
# No purchase is planned or paid: this is a VISIBILITY acceptance, not
# a trade.
#
# Modelled on tools/dev/market_acceptance.sh: same setsid isolation,
# port refuse-set discipline, wallet-custody recipe, mining cadence,
# pgid cleanup, phase banners. Tor is deliberately absent — moderation
# is independent of transport, so this uses the clearnet sibling's
# cheaper boot.
#
# Knobs: MKT_WAIT (chain/sync/gossip gate budget, s), MKT_KEEP=1
# preserves the scratch tree.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NODE_BIN="${ZCL_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
RPC_BIN="${ZCL_RPC_BIN:-$REPO_ROOT/build/bin/zcl-rpc}"

MKT_LIVE_PORTS="8023 8033 8034 8035 8043 8044 8045 8046 8232 8443 \
18034 18232 18234 18243 18244 18245 18246"
# Fresh block vs the siblings (market: 395xx quads + 20030/20031 + 39999,
# onion: 396xx quads + 20040/20041 + 39998, dht: 39211-39273, science:
# 39111-39123, p2p 20022-20027 + 18033) and vs this host's
# zclassic23-live instance (39311/39312).
A_PORT=20050; A_RPC=39711; A_FS=39712; A_HTTPS=39713
B_PORT=20051; B_RPC=39721; B_FS=39722; B_HTTPS=39723
DEAD_SINK=39997
MKT_WAIT="${MKT_WAIT:-90}"
MKT_WORK=""; MKT_DD_A=""; MKT_DD_B=""; MKT_PGID_A=""; MKT_PGID_B=""
MKT_EXTRA_FLAGS=()
MKT_CLEANED=0
MKT_KEEP="${MKT_KEEP:-0}"
# Throwaway passphrases for the wallet-custody recipe (never argv: they
# ride the wallet-passphrase credential file and --input=- stdin only).
MKT_WALLET_PASS="market-moderation-acceptance-wallet-pass"
MKT_BACKUP_PASS="market-moderation-acceptance-backup-pass"

# One small one-chunk fixture: the trade itself is out of scope, so the
# fixture only has to give the offer a real committed root and price.
PRICE_PER_MB_ZAT=30000
FIXTURE_BYTES=$((150 * 1024))
EXPECTED_CHUNKS=1

# The signed-wire columns moderation must NEVER touch (last_seen/ttl are
# gossip-refresh metadata, not wire; review_state is the local-only v65
# column under test). Snapshot-compared across every moderation action.
WIRE_SQL="SELECT auth_version,endpoint_type,peer_port,nonce,issued_unix,expires_unix,hex(root_hash),hex(offer_id),hex(seller_pubkey),hex(seller_signature),filename,size_bytes,num_chunks,price_per_mb FROM file_offers"

mkt_die() {
    echo "market-moderation-acceptance: FATAL: $*" >&2
    if [ -n "$MKT_WORK" ] && [ -d "$MKT_WORK" ]; then
        printf '%s\n' "$*" >"$MKT_WORK/FAILURE"
    fi
    exit 2
}
mkt_note() { echo "market-moderation-acceptance: $*"; }

mkt_assert_port() {
    local p="$1" live
    for live in $MKT_LIVE_PORTS; do
        [ "$p" = "$live" ] && mkt_die "port $p is in the live refuse-set"
    done
    ss -tlnH "sport = :$p" 2>/dev/null | grep -q . &&
        mkt_die "port $p is already listening"
    return 0
}

mkt_kill_group() {
    local pgid="$1" sig="${2:-TERM}" i
    [ -n "$pgid" ] || return 0
    kill -"$sig" "-$pgid" 2>/dev/null || true
    for i in $(seq 1 50); do
        kill -0 "-$pgid" 2>/dev/null || return 0
        sleep 0.2
    done
    kill -KILL "-$pgid" 2>/dev/null || true
}

mkt_cleanup() {
    [ "$MKT_CLEANED" = 1 ] && return 0
    MKT_CLEANED=1
    mkt_kill_group "$MKT_PGID_A"
    mkt_kill_group "$MKT_PGID_B"
    if [ "$MKT_KEEP" = 1 ] && [ -n "$MKT_WORK" ]; then
        mkt_note "preserved acceptance artifacts: $MKT_WORK"
    elif [ -n "$MKT_WORK" ] && [ -d "$MKT_WORK" ]; then
        case "$MKT_WORK" in
            "$REPO_ROOT"/test-tmp/zcl23-modacc-*) rm -rf "$MKT_WORK" ;;
            *) mkt_note "WARN refusing to remove non-scratch $MKT_WORK" ;;
        esac
    fi
}
trap mkt_cleanup EXIT INT TERM

mkt_rpc() {
    local dd="$1" port="$2"; shift 2
    ZCL_DATADIR="$dd" ZCL_RPCPORT="$port" "$RPC_BIN" "$@" 2>/dev/null
}
a_rpc() { mkt_rpc "$MKT_DD_A" "$A_RPC" "$@"; }
b_rpc() { mkt_rpc "$MKT_DD_B" "$B_RPC" "$@"; }
mkt_result() {
    python3 -c 'import json,sys
d=json.load(sys.stdin)
if d.get("error") is not None: raise SystemExit(2)
v=d.get("result")
print(json.dumps(v,separators=(",",":")) if isinstance(v,(dict,list)) else v)'
}
mkt_jget() {
    local expr="$1"
    python3 -c "import json,sys; d=json.load(sys.stdin); print($expr)"
}
mkt_native() {
    local dd="$1" rpc="$2"; shift 2
    "$NODE_BIN" -datadir="$dd" -rpcport="$rpc" "$@" 2>/dev/null | tail -1
}

mkt_spawn() {
    local dd="$1" p2p="$2" rpc="$3" fs="$4" https="$5"; shift 5
    local args=() connect
    for connect in "$@"; do args+=("-connect=$connect"); done
    [ "${#args[@]}" -gt 0 ] || args+=("-connect=127.0.0.1:$DEAD_SINK")
    # No -allow-plaintext-wallet: the wallet-passphrase credential
    # (CREDENTIALS_DIRECTORY, exported below) encrypts key writes at rest
    # (WKS1); -operator-lane=dev arms the dev wallet scope. -regtestshielded
    # activates Overwinter+Sapling from genesis on BOTH nodes (the zcashd
    # -nuparams equivalent; regtest otherwise pins them NO_ACTIVATION).
    # No -tor: moderation is transport-independent, so the seller carries
    # the clearnet -externalip endpoint opt-in instead.
    setsid "$NODE_BIN" -datadir="$dd" -regtest -port="$p2p" \
        -rpcport="$rpc" -fsport="$fs" -httpsport="$https" \
        "${args[@]}" -packagehost=0 -regtestshielded \
        -operator-lane=dev -wallet-no-phrase-backup \
        -nobgvalidation -nolegacyimport -showmetrics=0 \
        "${MKT_EXTRA_FLAGS[@]}" \
        >>"$dd/node.log" 2>&1 &
    echo "$!"
}

mkt_height() {
    mkt_rpc "$1" "$2" getblockcount | mkt_result
}
mkt_wait_rpc() {
    local dd="$1" rpc="$2" pid="$3" deadline
    deadline=$(( $(date +%s) + MKT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        kill -0 "$pid" 2>/dev/null || return 1
        [ -f "$dd/.cookie" ] && mkt_height "$dd" "$rpc" >/dev/null 2>&1 && return 0
        sleep 0.5
    done
    return 1
}
mkt_wait_height() {
    local dd="$1" rpc="$2" target="$3" deadline h
    deadline=$(( $(date +%s) + MKT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        h="$(mkt_height "$dd" "$rpc" 2>/dev/null || true)"
        [ "$h" = "$target" ] && return 0
        sleep 0.5
    done
    return 1
}

# The regtest miner stamps blocks from whole-second wall time.  More than six
# consecutive blocks with one timestamp becomes <= the peer's 11-block MTP
# even though the local submission path accepted the batch.  Mine in groups
# of five with a wall-clock step so the second node validates the same chain.
mkt_mine_to_address() {
    local rpc_fn="$1" count="$2" address="$3" chunk
    while [ "$count" -gt 0 ]; do
        chunk=5
        [ "$count" -lt "$chunk" ] && chunk="$count"
        "$rpc_fn" generatetoaddress "$chunk" "\"$address\"" | mkt_result >/dev/null
        count=$((count - chunk))
        [ "$count" -eq 0 ] || sleep 1
    done
}

mkt_wait_connected() {
    local dd="$1" rpc="$2" deadline n
    deadline=$(( $(date +%s) + MKT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        n="$(mkt_rpc "$dd" "$rpc" getconnectioncount 2>/dev/null | mkt_result 2>/dev/null || true)"
        [ "${n:-0}" -ge 1 ] 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}
# A listen-only node never leaves finding_peers: each node needs its own
# outbound link before its sync FSM can walk to at_tip.
mkt_wait_at_tip() {
    local dd="$1" rpc="$2" deadline state
    deadline=$(( $(date +%s) + MKT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        state="$(mkt_rpc "$dd" "$rpc" downloadstats 2>/dev/null \
            | mkt_jget 'd["result"]["sync_state"]' 2>/dev/null || true)"
        [ "$state" = "at_tip" ] && return 0
        sleep 0.5
    done
    return 1
}
mkt_unlock_wallet() {
    local dd="$1" rpc="$2" status unlock
    status="$(mkt_native "$dd" "$rpc" core wallet security status || true)"
    [ "$(printf '%s' "$status" | mkt_jget 'd.get("ok",False)' 2>/dev/null || true)" = "True" ] || {
        printf '%s\n' "$status" >&2; return 1; }
    if [ "$(printf '%s' "$status" | mkt_jget 'd["data"]["unlocked"]' 2>/dev/null || true)" != "True" ]; then
        unlock="$(printf '%s' "{\"passphrase\":\"$MKT_WALLET_PASS\",\"timeout_seconds\":3600}" \
            | mkt_native "$dd" "$rpc" core wallet security unlock --input=- || true)"
        [ "$(printf '%s' "$unlock" | mkt_jget 'd["data"]["unlocked"]' 2>/dev/null || true)" = "True" ] || {
            printf '%s\n' "$unlock" >&2; return 1; }
    fi
    return 0
}
mkt_backup_wallet() {
    local dd="$1" rpc="$2" out
    out="$(printf '%s' "{\"confirm\":true,\"password\":\"$MKT_BACKUP_PASS\"}" \
        | mkt_native "$dd" "$rpc" core wallet backup now --input=- || true)"
    [ "$(printf '%s' "$out" | mkt_jget 'd.get("ok",False)' 2>/dev/null || true)" = "True" ] || {
        printf '%s\n' "$out" >&2; return 1; }
}

# One node's signed-wire row set for the moderated offer (WIRE_SQL
# excludes last_seen/ttl/review_state by construction), printed as a
# canonical JSON string for byte comparison.
mod_wire_rows() {
    local dd="$1" rpc="$2" out
    out="$(mkt_native "$dd" "$rpc" core storage query \
        --input="{\"sql\":\"$WIRE_SQL\"}" || true)"
    printf '%s' "$out" | mkt_jget 'json.dumps(d["data"]["rows"],separators=(",",":"))' 2>/dev/null
}
# The node's local-only review_state column for the one stored offer.
mod_review_col() {
    local dd="$1" rpc="$2" out
    out="$(mkt_native "$dd" "$rpc" core storage query \
        --input='{"sql":"SELECT review_state FROM file_offers"}' || true)"
    printf '%s' "$out" | mkt_jget 'd["data"]["rows"][0][0]' 2>/dev/null
}
# The `app market list` body, optionally with a per-request profile
# override ({"profile":"open"} is the explicit opt-in view).
mod_list() {
    local dd="$1" rpc="$2" profile="${3:-}"
    if [ -n "$profile" ]; then
        printf '%s' "{\"profile\":\"$profile\"}" \
            | mkt_native "$dd" "$rpc" app market list --input=- 2>/dev/null
    else
        mkt_native "$dd" "$rpc" app market list 2>/dev/null
    fi
}
# Assert one default (no per-request override) list body hides the offer:
# absent from offers, an honest hidden_count, and the expected profile
# name. $3 = expected profile.
mod_assert_hidden() {
    python3 - "$1" "$OFFER_ID" "$3" <<'PY' || mkt_die "default list did not hide the $3 offer: $1"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
data=d["data"]
assert data["profile"]==sys.argv[3],data
assert data["profile_override"] is False,data
ids=[o.get("offer_id") for o in data["offers"]]
assert sys.argv[2] not in ids,data
assert data["hidden_count"]>=1,data
assert data["offer_count"]==len(data["offers"]),data
PY
}
# Assert one list body shows the offer with an exact review_state. $3 =
# expected review_state, $4 = expected profile name.
mod_assert_shown() {
    python3 - "$1" "$OFFER_ID" "$3" "$4" <<'PY' || mkt_die "list did not show the offer as $3 ($4): $1"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
data=d["data"]
assert data["profile"]==sys.argv[4],data
match=[o for o in data["offers"] if o.get("offer_id")==sys.argv[2]]
assert len(match)==1,data
assert match[0]["review_state"]==sys.argv[3],match[0]
assert data["hidden_count"]==0,data
PY
}
# Switch one node's active profile through the leaf's plan/commit idiom:
# mode=plan mints a plan_token bound to the current active profile and
# the target; mode=commit requires that exact token.
mod_profile_set() {
    local dd="$1" rpc="$2" target="$3" expect_prev="$4" plan token commit
    plan="$(printf '%s' "{\"profile\":\"$target\",\"mode\":\"plan\"}" \
        | mkt_native "$dd" "$rpc" app market moderation profile set --input=- || true)"
    token="$(python3 - "$plan" "$target" <<'PY' || mkt_die "profile set plan refused: $plan"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
data=d["data"]
assert data["mode"]=="plan" and data["committed"] is False,data
assert data["profile"]==sys.argv[2],data
tok=data["plan_token"]
assert isinstance(tok,str) and tok,data
print(tok)
PY
)"
    commit="$(printf '%s' "{\"profile\":\"$target\",\"mode\":\"commit\",\"plan_token\":\"$token\"}" \
        | mkt_native "$dd" "$rpc" app market moderation profile set --input=- || true)"
    python3 - "$commit" "$target" "$expect_prev" <<'PY' || mkt_die "profile set commit refused: $commit"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
data=d["data"]
assert data["mode"]=="commit" and data["committed"] is True,data
assert data["profile"]==sys.argv[2],data
assert data["previous_profile"]==sys.argv[3],data
PY
}
# Mark the offer's local review_state on one node and assert the reply
# proves the mark is local-only and never gossiped.
mod_review_set() {
    local dd="$1" rpc="$2" state="$3" expect_prev="$4" out
    out="$(printf '%s' "{\"offer_id\":\"$OFFER_ID\",\"review_state\":\"$state\"}" \
        | mkt_native "$dd" "$rpc" app market moderation review set --input=- || true)"
    python3 - "$out" "$OFFER_ID" "$state" "$expect_prev" <<'PY' || mkt_die "review set $state refused: $out"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
data=d["data"]
assert data["offer_id"]==sys.argv[2],data
assert data["review_state"]==sys.argv[3],data
assert data["previous_review_state"]==sys.argv[4],data
assert data["local_only"] is True and data["gossiped"] is False,data
PY
}

for port in $A_PORT $A_RPC $A_FS $A_HTTPS $B_PORT $B_RPC $B_FS $B_HTTPS; do
    mkt_assert_port "$port"
done
[ -x "$NODE_BIN" ] && [ -x "$RPC_BIN" ] || mkt_die "build node and RPC binaries first"
mkdir -p "$REPO_ROOT/test-tmp"
MKT_WORK="$(mktemp -d "$REPO_ROOT/test-tmp/zcl23-modacc-XXXXXX")"
MKT_DD_A="$MKT_WORK/a"; MKT_DD_B="$MKT_WORK/b"
MKT_CONTENT="$MKT_WORK/content"
mkdir -p "$MKT_DD_A" "$MKT_DD_B" "$MKT_CONTENT"
FIXTURE="$MKT_CONTENT/seller-fixture.bin"

# Deterministic one-chunk fixture, plus the exact manifest root the offer
# must commit (sha3-256 over the concatenated per-chunk sha3-256 digests)
# and the exact total price.
read -r FIXTURE_SIZE EXPECT_ROOT EXPECT_TOTAL_ZAT <<<"$(python3 - "$FIXTURE" "$PRICE_PER_MB_ZAT" "$FIXTURE_BYTES" <<'PY'
import hashlib,sys
path, price, size = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
chunk = 50 * 1024 * 1024
digests = []
written = 0
with open(path, "wb") as f:
    while written < size:
        n = min(chunk, size - written)
        block = bytes(((written + i * 11) & 0xFF) for i in range(n))
        f.write(block)
        digests.append(hashlib.sha3_256(block).digest())
        written += n
root = hashlib.sha3_256(b"".join(digests)).hexdigest()
mb = 1024 * 1024
whole, rem = divmod(written, mb)
pw, pr = divmod(price, mb)
total = whole * price + rem * pw + (rem * pr + mb - 1) // mb
print(written, root, total)
PY
)" || mkt_die "fixture build failed"

# Wallet custody: boot both nodes with a passphrase credential so key writes
# encrypt at rest (WKS1). The seller-key envelope (metadata DEK) refuses a
# locked or plaintext wallet.
MKT_CRED_DIR="$MKT_WORK/cred"
install -d -m 700 "$MKT_CRED_DIR"
install -m 600 /dev/null "$MKT_CRED_DIR/wallet-passphrase"
printf '%s\n' "$MKT_WALLET_PASS" >"$MKT_CRED_DIR/wallet-passphrase"
export CREDENTIALS_DIRECTORY="$MKT_CRED_DIR"

mkt_note "booting seller A (-externalip + file service) and buyer B (no Tor: moderation is transport-independent)"
MKT_EXTRA_FLAGS=("-externalip=127.0.0.1")
MKT_PGID_A="$(mkt_spawn "$MKT_DD_A" "$A_PORT" "$A_RPC" "$A_FS" "$A_HTTPS" "127.0.0.1:$DEAD_SINK")"
mkt_wait_rpc "$MKT_DD_A" "$A_RPC" "$MKT_PGID_A" || mkt_die "seller A RPC warmup failed"
MKT_EXTRA_FLAGS=()
MKT_PGID_B="$(mkt_spawn "$MKT_DD_B" "$B_PORT" "$B_RPC" "$B_FS" "$B_HTTPS" "127.0.0.1:$A_PORT")"
mkt_wait_rpc "$MKT_DD_B" "$B_RPC" "$MKT_PGID_B" || mkt_die "buyer B RPC warmup failed"
! rg -q "unrecognized flag" "$MKT_DD_A/node.log" "$MKT_DD_B/node.log" ||
    mkt_die "a boot flag was not recognized"

mkt_note "mining 101 spendable regtest blocks to the buyer"
BUYER_ADDR="$(b_rpc getnewaddress | mkt_result)"
mkt_mine_to_address b_rpc 101 "$BUYER_ADDR"
mkt_wait_height "$MKT_DD_A" "$A_RPC" 101 || mkt_die "A did not sync the funding chain"

# Symmetric one-shot link so BOTH nodes own an outbound peer and walk to
# at_tip (B dialed A at boot; A is on the dead sink until this onetry).
a_rpc addnode "\"127.0.0.1:$B_PORT\"" "\"onetry\"" >/dev/null || true
mkt_wait_connected "$MKT_DD_A" "$A_RPC" || mkt_die "A never connected outbound to B"
mkt_wait_at_tip "$MKT_DD_A" "$A_RPC" || mkt_die "A sync never reached at_tip"
mkt_wait_at_tip "$MKT_DD_B" "$B_RPC" || mkt_die "B sync never reached at_tip"

# The offer's payee gate refuses an unseeded Sapling keystore and a locked
# wallet: unlock both, re-top the RAM-only keypool bookkeeping, mint the
# seller's first z-address, take the current-key encrypted backups.
mkt_note "unlocking both wallets and taking current-key encrypted backups"
mkt_unlock_wallet "$MKT_DD_A" "$A_RPC" || mkt_die "A wallet unlock failed"
mkt_unlock_wallet "$MKT_DD_B" "$B_RPC" || mkt_die "B wallet unlock failed"
a_rpc getnewaddress | mkt_result >/dev/null || mkt_die "A keypool top-up failed"
b_rpc getnewaddress | mkt_result >/dev/null || mkt_die "B keypool top-up failed"
a_rpc z_getnewaddress | mkt_result >/dev/null || mkt_die "A sapling keystore seeding failed"
mkt_backup_wallet "$MKT_DD_A" "$A_RPC" || mkt_die "A custody backup failed"
mkt_backup_wallet "$MKT_DD_B" "$B_RPC" || mkt_die "B custody backup failed"

# ── Phase 1: seller offer plan (non-mutating) then commit ────────────
mkt_note "seller plans the offer (non-mutating preview)"
OFFER_PLAN="$(printf '%s' "{\"filepath\":\"$FIXTURE\",\"price_per_mb_zat\":$PRICE_PER_MB_ZAT}" \
    | mkt_native "$MKT_DD_A" "$A_RPC" app market offer --input=- || true)"
python3 - "$OFFER_PLAN" "$EXPECT_ROOT" "$FIXTURE_SIZE" "$EXPECTED_CHUNKS" "$EXPECT_TOTAL_ZAT" <<'PY' || mkt_die "offer plan preview mismatch: $OFFER_PLAN"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
data=d["data"]
assert data["stage"]=="plan" and data["committed"] is False and data["spends_funds"] is False,data
assert data["root_hash"]==sys.argv[2],data
assert data["size_bytes"]==int(sys.argv[3]) and data["num_chunks"]==int(sys.argv[4]),data
assert data["total_zat"]==int(sys.argv[5]),data
assert data["price_per_mb_zat"]>0 and "commit_input" in data,data
assert "offer_id" not in data and "seller_pubkey" not in data,data
PY
OFFER_COUNT="$(mkt_native "$MKT_DD_A" "$A_RPC" core storage query \
    --input='{"sql":"SELECT COUNT(*) AS n FROM file_offers"}' || true)"
[ "$(printf '%s' "$OFFER_COUNT" | mkt_jget 'd["data"]["rows"][0][0]' 2>/dev/null || true)" = "0" ] ||
    mkt_die "offer plan mutated seller storage: $OFFER_COUNT"

mkt_note "seller commits the offer (seal, persist, bind, flood)"
OFFER_COMMIT="$(printf '%s' "{\"filepath\":\"$FIXTURE\",\"price_per_mb_zat\":$PRICE_PER_MB_ZAT,\"confirm\":true}" \
    | mkt_native "$MKT_DD_A" "$A_RPC" app market offer --input=- || true)"
OFFER_ID="$(python3 - "$OFFER_COMMIT" "$EXPECT_ROOT" <<'PY' || mkt_die "offer commit refused: $OFFER_COMMIT"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
data=d["data"]
assert data["stage"]=="committed" and data["committed"] is True,data
assert data["idempotent_replay"] is False and data["announced"] is True,data
assert data["root_hash"]==sys.argv[2],data
oid=data["offer_id"]
assert len(oid)==64 and int(oid,16)>0,data
assert len(data["seller_pubkey"])==64,data
print(oid)
PY
)"
mkt_note "seller offer committed: offer_id=$OFFER_ID"

# The seller's pre-gossip wire row: the reference every later comparison
# must reproduce byte-for-byte.
A_WIRE_PRE="$(mod_wire_rows "$MKT_DD_A" "$A_RPC")"
[ -n "$A_WIRE_PRE" ] || mkt_die "seller wire row snapshot failed"
[ "$(mod_review_col "$MKT_DD_A" "$A_RPC")" = "unreviewed" ] ||
    mkt_die "seller's own offer did not ingest as unreviewed"

# ── Phase 2: the offer gossips to the buyer ──────────────────────────
# The default profile HIDES unreviewed offers, so the gossip wait polls
# the explicit open view — the offer being listed there is itself the
# first ingest proof (hidden != absent).
mkt_note "waiting for the signed offer to gossip to the buyer"
LIST_DEADLINE=$(( $(date +%s) + MKT_WAIT ))
while :; do
    BUYER_OPEN="$(mod_list "$MKT_DD_B" "$B_RPC" open || true)"
    case "$BUYER_OPEN" in
        *"$OFFER_ID"*) break ;;
    esac
    [ "$(date +%s)" -lt "$LIST_DEADLINE" ] ||
        mkt_die "offer never gossiped to the buyer: $BUYER_OPEN"
    sleep 1
done
B_WIRE_PRE="$(mod_wire_rows "$MKT_DD_B" "$B_RPC")"
[ -n "$B_WIRE_PRE" ] || mkt_die "buyer wire row snapshot failed"
[ "$B_WIRE_PRE" = "$A_WIRE_PRE" ] ||
    mkt_die "gossiped wire row differs from the seller's: A=$A_WIRE_PRE B=$B_WIRE_PRE"
[ "$(mod_review_col "$MKT_DD_B" "$B_RPC")" = "unreviewed" ] ||
    mkt_die "buyer did not ingest the offer as unreviewed"

# ── Phase 3: the boot-default profile HIDES the unreviewed offer ─────
mkt_note "buyer default profile (general-audience.v1) must hide the unreviewed offer"
B_DEFAULT="$(mod_list "$MKT_DD_B" "$B_RPC" || true)"
mod_assert_hidden "$B_DEFAULT" x "general-audience.v1"

mkt_note "buyer moderation status names the active profile and the unreviewed count"
B_STATUS="$(mkt_native "$MKT_DD_B" "$B_RPC" app market moderation status || true)"
python3 - "$B_STATUS" <<'PY' || mkt_die "moderation status mismatch: $B_STATUS"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
data=d["data"]
assert data["active_profile"]=="general-audience.v1",data
assert "open-view" in data["available_profiles"],data
counts=data["review_counts"]
assert counts.get("unreviewed",0)>=1,counts
assert data["view_filter_only"] is True,data
assert data["review_counts_live"] is True,data
PY

# ── Phase 4: the explicit per-request opt-in shows it, annotated ─────
mkt_note "buyer open override must show the same offer annotated unreviewed"
B_OPEN="$(mod_list "$MKT_DD_B" "$B_RPC" open || true)"
mod_assert_shown "$B_OPEN" x unreviewed open-view

# ── Phase 5: switch B's node default to open-view ────────────────────
mkt_note "switching the buyer node default to open-view (plan/commit)"
mod_profile_set "$MKT_DD_B" "$B_RPC" open-view general-audience.v1
B_DEFAULT_OPEN="$(mod_list "$MKT_DD_B" "$B_RPC" || true)"
mod_assert_shown "$B_DEFAULT_OPEN" x unreviewed open-view
[ -f "$MKT_DD_B/market/moderation.v1" ] ||
    mkt_die "profile commit did not persist market/moderation.v1"
POLICY_BODY="$(cat "$MKT_DD_B/market/moderation.v1")"
case "$POLICY_BODY" in
    *"profile=open-view"*) : ;;
    *) mkt_die "persisted policy does not name open-view: $POLICY_BODY" ;;
esac

# ── Phase 6: back to general-audience — hidden again ─────────────────
mkt_note "switching the buyer node default back to general-audience.v1"
mod_profile_set "$MKT_DD_B" "$B_RPC" general-audience.v1 open-view
B_DEFAULT_GA="$(mod_list "$MKT_DD_B" "$B_RPC" || true)"
mod_assert_hidden "$B_DEFAULT_GA" x "general-audience.v1"

# ── Phase 7: B's own review mark drives visibility; A disagrees ──────
mkt_note "buyer marks the offer reviewed_ok: B shows it, A's default still hides the SAME offer_id"
mod_review_set "$MKT_DD_B" "$B_RPC" reviewed_ok unreviewed
B_REVIEWED="$(mod_list "$MKT_DD_B" "$B_RPC" || true)"
mod_assert_shown "$B_REVIEWED" x reviewed_ok general-audience.v1
# The two-node disagreement proof: A never marked anything, so A's own
# default list hides the same offer_id while B's shows it. Moderation is
# per-node view filtering, not a network-wide verdict.
A_DEFAULT="$(mod_list "$MKT_DD_A" "$A_RPC" || true)"
mod_assert_hidden "$A_DEFAULT" x "general-audience.v1"
A_OPEN="$(mod_list "$MKT_DD_A" "$A_RPC" open || true)"
mod_assert_shown "$A_OPEN" x unreviewed open-view
mkt_note "disagreement proven: B lists $OFFER_ID as reviewed_ok, A hides it as unreviewed"

# ── Phase 8: sensitive hides again; the open view keeps showing ──────
mkt_note "buyer marks the offer sensitive: default hides, open view shows it annotated"
mod_review_set "$MKT_DD_B" "$B_RPC" sensitive reviewed_ok
B_SENSITIVE="$(mod_list "$MKT_DD_B" "$B_RPC" || true)"
mod_assert_hidden "$B_SENSITIVE" x "general-audience.v1"
B_SENSITIVE_OPEN="$(mod_list "$MKT_DD_B" "$B_RPC" open || true)"
mod_assert_shown "$B_SENSITIVE_OPEN" x sensitive open-view

# ── Phase 9: hidden != rejected; the wire round-tripped unchanged ────
mkt_note "verifying gossip storage and protocol-validity separation"
A_COUNT="$(mkt_native "$MKT_DD_A" "$A_RPC" core storage query \
    --input='{"sql":"SELECT COUNT(*) AS n FROM file_offers"}' || true)"
[ "$(printf '%s' "$A_COUNT" | mkt_jget 'd["data"]["rows"][0][0]' 2>/dev/null || true)" = "1" ] ||
    mkt_die "seller file_offers row count is not 1: $A_COUNT"
B_COUNT="$(mkt_native "$MKT_DD_B" "$B_RPC" core storage query \
    --input='{"sql":"SELECT COUNT(*) AS n FROM file_offers"}' || true)"
[ "$(printf '%s' "$B_COUNT" | mkt_jget 'd["data"]["rows"][0][0]' 2>/dev/null || true)" = "1" ] ||
    mkt_die "buyer file_offers row count is not 1 (hidden != rejected): $B_COUNT"
# review_state is the ONLY column the two nodes may disagree on.
[ "$(mod_review_col "$MKT_DD_A" "$A_RPC")" = "unreviewed" ] ||
    mkt_die "seller review_state moved — moderation must be buyer-local here"
[ "$(mod_review_col "$MKT_DD_B" "$B_RPC")" = "sensitive" ] ||
    mkt_die "buyer review_state is not the sensitive mark just set"
# The signed wire columns are byte-identical on both nodes and unchanged
# on each across every profile switch and review mark.
A_WIRE_POST="$(mod_wire_rows "$MKT_DD_A" "$A_RPC")"
B_WIRE_POST="$(mod_wire_rows "$MKT_DD_B" "$B_RPC")"
[ "$A_WIRE_POST" = "$A_WIRE_PRE" ] ||
    mkt_die "seller wire columns changed across the acceptance: pre=$A_WIRE_PRE post=$A_WIRE_POST"
[ "$B_WIRE_POST" = "$B_WIRE_PRE" ] ||
    mkt_die "moderation altered the buyer's signed wire columns: pre=$B_WIRE_PRE post=$B_WIRE_POST"
[ "$A_WIRE_POST" = "$B_WIRE_POST" ] ||
    mkt_die "signed wire diverged between nodes: A=$A_WIRE_POST B=$B_WIRE_POST"

mkt_note "PASS: two-daemon moderation acceptance — one signed offer, two profiles: general-audience hides the unreviewed offer with an honest hidden_count, the open opt-in and the open-view node default show it annotated, reviewed_ok/sensitive marks drive per-node visibility, A and B legitimately disagree about the same offer_id, the offer stays gossip-stored on both (hidden != rejected), and the signed wire columns are byte-identical and untouched by every moderation action (review_state never leaves the node)"
