#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# B5 acceptance (docs/work/MARKET_ONION_DELIVERY.md): two isolated regtest
# daemons, BOTH booted with -tor and NEITHER with -externalip, run the full
# P2P file-market trade with the DELIVERY leg routed over the seller's
# ephemeral onion service. A (seller) commits a signed paid offer whose v2
# wire names endpoint_type=onion; it gossips to B (buyer) over the clearnet
# loopback P2P link (the P2P link is not the leg under test); B plans and
# commits a real Sapling payment, is refused delivery before confirmation
# (authorize-before-read — served THROUGH the onion route), then, after one
# mined block, retrieves the file as 60 KiB slices over real Tor circuits
# via GET /market/chunk/<signed-request-hex>?slice=k. The proof that no
# clearnet file-service connection was used: the onion offer carries no
# usable clearnet endpoint (peer_ip zero, peer_port 0), and both tor.log
# files name the /market/chunk traffic. The negative half restarts B
# WITHOUT -tor before any successful retrieve and asserts the named
# ONION_DELIVERY_UNAVAILABLE refusal — there is never a clearnet fallback
# against a signed onion offer.
#
# Modelled on tools/dev/market_acceptance.sh: same setsid isolation, port
# refuse-set discipline, wallet-custody recipe, mining cadence, pgid
# cleanup, phase banners. Public Tor network reachability is REQUIRED: if
# neither node can bootstrap, the script FAILS with a named reason — it
# never silently passes.
#
# Knobs: MKT_WAIT (chain/sync gate budget, s), ONI_TOR_WAIT (Tor bootstrap
# + onion address budget, s), ONI_RETRIEVE_WAIT (onion retrieval budget,
# s — per-slice real Tor round trips plus seller-projection retries),
# MKT_KEEP=1 preserves the scratch tree.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NODE_BIN="${ZCL_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
RPC_BIN="${ZCL_RPC_BIN:-$REPO_ROOT/build/bin/zcl-rpc}"

MKT_LIVE_PORTS="8023 8033 8034 8035 8043 8044 8045 8046 8232 8443 \
18034 18232 18234 18243 18244 18245 18246"
# Fresh block vs the siblings (market: 395xx quads + 20030/20031 + 39999,
# dht: 39211-39273, science: 39111-39123, p2p 20022-20027 + 18033) and vs
# this host's zclassic23-live instance (39311/39312). The +11966 Tor
# bootstrap SocksPorts land at 32006/32007 and are asserted too.
A_PORT=20040; A_RPC=39611; A_FS=39612; A_HTTPS=39613
B_PORT=20041; B_RPC=39621; B_FS=39622; B_HTTPS=39623
DEAD_SINK=39998
MKT_WAIT="${MKT_WAIT:-90}"
ONI_TOR_WAIT="${ONI_TOR_WAIT:-420}"
ONI_RETRIEVE_WAIT="${ONI_RETRIEVE_WAIT:-600}"
MKT_WORK=""; MKT_DD_A=""; MKT_DD_B=""; MKT_PGID_A=""; MKT_PGID_B=""
MKT_EXTRA_FLAGS=()
MKT_CLEANED=0
MKT_KEEP="${MKT_KEEP:-0}"
# Throwaway passphrases for the wallet-custody recipe (never argv: they
# ride the wallet-passphrase credential file and --input=- stdin only).
MKT_WALLET_PASS="market-onion-acceptance-wallet-pass"
MKT_BACKUP_PASS="market-onion-acceptance-backup-pass"

# Trade terms: a ONE-chunk 150 KiB fixture. Onion delivery slices a chunk
# into 60 KiB pieces (the dynhost webserver response cap is 64 KiB), so
# 150 KiB = 3 slices over real Tor round trips — the clearnet script's
# 100 MiB fixture would be ~1700 Tor fetches. chunks_paid is num_chunks.
PRICE_PER_MB_ZAT=30000
FIXTURE_BYTES=$((150 * 1024))
EXPECTED_CHUNKS=1
EXPECTED_SLICES=3
IDEMPOTENCY_KEY="market-onion-acceptance-purchase-1"

mkt_die() {
    echo "market-onion-acceptance: FATAL: $*" >&2
    if [ -n "$MKT_WORK" ] && [ -d "$MKT_WORK" ]; then
        printf '%s\n' "$*" >"$MKT_WORK/FAILURE"
    fi
    exit 2
}
mkt_note() { echo "market-onion-acceptance: $*"; }

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
            "$REPO_ROOT"/test-tmp/zcl23-oniacc-*) rm -rf "$MKT_WORK" ;;
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
    # (WKS1); -operator-lane=dev arms the dev wallet scope the purchase
    # plan/commit leaves require. -regtestshielded activates
    # Overwinter+Sapling from genesis on BOTH nodes. -tor runs the real
    # embedded Tor (vendor/tor/libtor.a is linked in this build); its
    # DataDirectory is <datadir>/tor_data and its bootstrap SocksPort is
    # p2p_port+11966, so the two nodes never collide. -externalip is
    # DELIBERATELY ABSENT on both: it is the explicit public-endpoint
    # opt-in that would pin offer commits to the clearnet v1 wire.
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

# Tor bootstrap to the PUBLIC network takes ~10-60 s per node (warm
# tor_data is faster) and can be impossible on a host without Tor
# reachability. Poll the explorer dumpstate subsystem for the node's
# ephemeral onion address (`core status` does NOT carry it — the
# `health.checks.onion_address` surface is the in-process health
# projection; over RPC the address lives at
# `ops state --subsystem=explorer` → data.state.onion_address, fed by
# explorer_dump_state_json). On timeout, FAIL with the last bootstrap
# line — never silently skip the onion proof.
oni_onion_address() {
    mkt_native "$1" "$2" ops state --subsystem=explorer 2>/dev/null | python3 -c '
import json,sys
try:
    d = json.load(sys.stdin)
except Exception:
    raise SystemExit(0)
def walk(v):
    if isinstance(v, dict):
        for k, x in v.items():
            if k == "onion_address" and isinstance(x, str) and \
               x.endswith(".onion"):
                print(x); return True
            if walk(x): return True
    elif isinstance(v, list):
        for x in v:
            if walk(x): return True
    return False
walk(d)' 2>/dev/null
}
oni_bootstrap_tail() {
    grep "Bootstrapped" "$1/tor.log" 2>/dev/null | tail -1
}
oni_wait_onion_address() {
    local dd="$1" rpc="$2" label="$3" deadline addr
    deadline=$(( $(date +%s) + ONI_TOR_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        addr="$(oni_onion_address "$dd" "$rpc" || true)"
        case "$addr" in
            *.onion) printf '%s\n' "$addr"; return 0 ;;
        esac
        sleep 2
    done
    mkt_die "$label never published an onion address within ${ONI_TOR_WAIT}s \
— host may lack public Tor network reachability (last bootstrap line: \
$(oni_bootstrap_tail "$dd" || echo none))"
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
# The money freshness classifier fails closed on finding_peers; the sync
# FSM only leaves it behind a peer it can sync FROM (outbound).
mkt_wait_sync_live() {
    local dd="$1" rpc="$2" deadline state
    deadline=$(( $(date +%s) + MKT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        state="$(mkt_rpc "$dd" "$rpc" downloadstats 2>/dev/null \
            | mkt_jget 'd["result"]["sync_state"]' 2>/dev/null || true)"
        case "$state" in
            blocks_download|connecting_blocks|at_tip) return 0 ;;
        esac
        sleep 0.5
    done
    return 1
}
# The seller's market payment gate (market_payment_claim_ingest /
# market_payment_authorize_chunk) reads sync_get_state()==SYNC_AT_TIP as
# "chain current" and persists UNKNOWN otherwise; a node whose only links
# are inbound never leaves finding_peers, so the seller needs its own
# outbound link AND the full walk to at_tip before any payment arrives.
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
# The money gate reads the REDUCER pipeline, not the active chain: the
# authoritative coins tip AND H* must both reach the mined height.
mkt_wait_fold() {
    local dd="$1" rpc="$2" tip="$3" deadline dump coins hstar
    deadline=$(( $(date +%s) + MKT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        dump="$(mkt_native "$dd" "$rpc" dumpstate reducer_frontier || true)"
        coins="$(printf '%s' "$dump" | mkt_jget 'd["state"]["coins_best_height"]' 2>/dev/null || true)"
        hstar="$(printf '%s' "$dump" | mkt_jget 'd["state"]["hstar"]' 2>/dev/null || true)"
        [ "$coins" = "$tip" ] && [ "$hstar" = "$tip" ] && return 0
        sleep 1
    done
    echo "market-onion-acceptance: reducer_frontier at stall: $dump" >&2
    return 1
}
# RPC-ready != chain-loaded: the money gate needs the active chain index,
# which loads after the RPC starts serving.
mkt_wait_chain_loaded() {
    local dd="$1" rpc="$2" tip="$3" deadline loaded
    deadline=$(( $(date +%s) + MKT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        loaded="$(mkt_rpc "$dd" "$rpc" getblockchaininfo 2>/dev/null | python3 -c '
import json,sys
tip = int(sys.argv[1])
try:
    d = json.load(sys.stdin).get("result")
except Exception:
    d = None
print(isinstance(d, dict) and d.get("blocks") == tip and d.get("initialblockdownload") is not True)' \
            "$tip" 2>/dev/null || true)"
        [ "$loaded" = "True" ] && return 0
        sleep 1
    done
    return 1
}
# The purchase plan's reservation reads the vault read model's confirmed
# custody, which lags the reducer fold while the wallet re-derives its
# spendable coins.
mkt_wait_spendable() {
    local dd="$1" rpc="$2" deadline spend
    deadline=$(( $(date +%s) + MKT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        spend="$(mkt_native "$dd" "$rpc" dumpstate vault 2>/dev/null \
            | mkt_jget 'd["state"]["zcl"]["spendable"]' 2>/dev/null || true)"
        case "$spend" in
            ''|*[!0-9]*) ;;
            *) [ "$spend" -gt 0 ] && return 0 ;;
        esac
        sleep 1
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

for port in $A_PORT $A_RPC $A_FS $A_HTTPS $B_PORT $B_RPC $B_FS $B_HTTPS \
    $((A_PORT + 11966)) $((B_PORT + 11966)); do
    mkt_assert_port "$port"
done
[ -x "$NODE_BIN" ] && [ -x "$RPC_BIN" ] || mkt_die "build node and RPC binaries first"
mkdir -p "$REPO_ROOT/test-tmp"
MKT_WORK="$(mktemp -d "$REPO_ROOT/test-tmp/zcl23-oniacc-XXXXXX")"
MKT_DD_A="$MKT_WORK/a"; MKT_DD_B="$MKT_WORK/b"
MKT_CONTENT="$MKT_WORK/content"
MKT_DOWNLOADS="$MKT_WORK/downloads"
mkdir -p "$MKT_DD_A" "$MKT_DD_B" "$MKT_CONTENT" "$MKT_DOWNLOADS"
FIXTURE="$MKT_CONTENT/seller-fixture.bin"
DESTINATION="$MKT_DOWNLOADS/bought-copy.bin"

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
# encrypt at rest (WKS1). The seller-key envelope (metadata DEK) and the
# buyer's money gate both refuse a locked or plaintext wallet.
MKT_CRED_DIR="$MKT_WORK/cred"
install -d -m 700 "$MKT_CRED_DIR"
install -m 600 /dev/null "$MKT_CRED_DIR/wallet-passphrase"
printf '%s\n' "$MKT_WALLET_PASS" >"$MKT_CRED_DIR/wallet-passphrase"
export CREDENTIALS_DIRECTORY="$MKT_CRED_DIR"

mkt_note "booting seller A and buyer B (both -tor, neither -externalip)"
MKT_EXTRA_FLAGS=("-tor")
MKT_PGID_A="$(mkt_spawn "$MKT_DD_A" "$A_PORT" "$A_RPC" "$A_FS" "$A_HTTPS" "127.0.0.1:$DEAD_SINK")"
mkt_wait_rpc "$MKT_DD_A" "$A_RPC" "$MKT_PGID_A" || mkt_die "seller A RPC warmup failed"
MKT_PGID_B="$(mkt_spawn "$MKT_DD_B" "$B_PORT" "$B_RPC" "$B_FS" "$B_HTTPS" "127.0.0.1:$A_PORT")"
mkt_wait_rpc "$MKT_DD_B" "$B_RPC" "$MKT_PGID_B" || mkt_die "buyer B RPC warmup failed"
! rg -q "unrecognized flag" "$MKT_DD_A/node.log" "$MKT_DD_B/node.log" ||
    mkt_die "a boot flag was not recognized"

# ── Phase 0: both nodes bootstrap Tor to the public network ──────────
# No reachability is a named FAIL, never a silent pass (see
# oni_wait_onion_address).
mkt_note "waiting for both embedded Tor instances to publish onion addresses"
A_ONION="$(oni_wait_onion_address "$MKT_DD_A" "$A_RPC" "seller A")"
mkt_note "seller A onion service: $A_ONION"
B_ONION="$(oni_wait_onion_address "$MKT_DD_B" "$B_RPC" "buyer B")"
mkt_note "buyer B onion service: $B_ONION"

mkt_note "mining 101 spendable regtest blocks to the buyer"
BUYER_ADDR="$(b_rpc getnewaddress | mkt_result)"
mkt_mine_to_address b_rpc 101 "$BUYER_ADDR"
mkt_wait_height "$MKT_DD_A" "$A_RPC" 101 || mkt_die "A did not sync the funding chain"
mkt_wait_fold "$MKT_DD_B" "$B_RPC" 101 || mkt_die "B reducer fold did not reach the funding tip"

# The buyer's purchase money gate reads the forward-folded coins set, whose
# authority stamps land only at boot: restart B (the funded node) so they
# stamp, then re-link with an operator-directed onetry so B owns the
# OUTBOUND peer the money-freshness classifier demands (it fails closed on
# finding_peers). A stays up on the dead sink the whole time, so no
# already-connected skip can delay the link. B's tor_data is warm, so this
# bootstrap is the fast one.
mkt_note "restarting B so the forward-folded coins set stamps its authority"
mkt_kill_group "$MKT_PGID_B"; MKT_PGID_B=""
MKT_PGID_B="$(mkt_spawn "$MKT_DD_B" "$B_PORT" "$B_RPC" "$B_FS" "$B_HTTPS" "127.0.0.1:$DEAD_SINK")"
mkt_wait_rpc "$MKT_DD_B" "$B_RPC" "$MKT_PGID_B" || mkt_die "B custody restart failed"
B_ONION="$(oni_wait_onion_address "$MKT_DD_B" "$B_RPC" "buyer B (custody restart)")"
mkt_wait_fold "$MKT_DD_B" "$B_RPC" 101 || mkt_die "B reducer fold did not survive the restart"
b_rpc addnode "\"127.0.0.1:$A_PORT\"" "\"onetry\"" >/dev/null || true
mkt_wait_connected "$MKT_DD_B" "$B_RPC" || mkt_die "B never connected outbound to A"
mkt_wait_sync_live "$MKT_DD_B" "$B_RPC" || mkt_die "B sync never left finding_peers"
mkt_wait_chain_loaded "$MKT_DD_B" "$B_RPC" 101 || mkt_die "B active chain index did not load"

# Symmetric one-shot link: the seller's paid-chunk gate reads
# sync_get_state()==SYNC_AT_TIP, which a listen-only node never reaches.
a_rpc addnode "\"127.0.0.1:$B_PORT\"" "\"onetry\"" >/dev/null || true
mkt_wait_connected "$MKT_DD_A" "$A_RPC" || mkt_die "A never connected outbound to B"
mkt_wait_at_tip "$MKT_DD_A" "$A_RPC" || mkt_die "A sync never reached at_tip"

# The restart re-locks the encrypted-at-rest wallet (and A booted locked):
# unlock both (passphrase via --input=- only), re-top the RAM-only keypool
# bookkeeping, take the current-key encrypted backup, then wait for the
# buyer's spendable custody to turn positive.
mkt_note "unlocking both wallets and taking current-key encrypted backups"
mkt_unlock_wallet "$MKT_DD_A" "$A_RPC" || mkt_die "A wallet unlock failed"
mkt_unlock_wallet "$MKT_DD_B" "$B_RPC" || mkt_die "B wallet unlock failed"
a_rpc getnewaddress | mkt_result >/dev/null || mkt_die "A keypool top-up failed"
b_rpc getnewaddress | mkt_result >/dev/null || mkt_die "B keypool top-up failed"
# The offer's payee gate refuses an unseeded Sapling keystore; minting the
# seller's first z-address generates + persists the seed it checks for.
a_rpc z_getnewaddress | mkt_result >/dev/null || mkt_die "A sapling keystore seeding failed"
mkt_backup_wallet "$MKT_DD_A" "$A_RPC" || mkt_die "A custody backup failed"
mkt_backup_wallet "$MKT_DD_B" "$B_RPC" || mkt_die "B custody backup failed"
mkt_wait_spendable "$MKT_DD_B" "$B_RPC" || mkt_die "B vault spendable never became positive"

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
[ "$(a_rpc zmarket_list | mkt_result)" = "[]" ] ||
    mkt_die "offer plan touched the seller gossip cache"

mkt_note "seller commits the offer — Tor ready + no -externalip must select the v2 onion endpoint"
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
assert data["endpoint_source"]=="onion",data
oid=data["offer_id"]
assert len(oid)==64 and int(oid,16)>0,data
assert len(data["seller_pubkey"])==64,data
print(oid)
PY
)"
mkt_note "seller offer committed on the onion endpoint: offer_id=$OFFER_ID"

# The committed offer must name A's OWN onion service: endpoint_type=1,
# peer_port=0, and onion_pubkey must re-derive the exact onion address the
# health projection published (Tor v3: base32(pubkey || sha3-256(".onion
# checksum" || pubkey || 0x03)[:2] || 0x03) + ".onion").
A_OFFER_ROW="$(mkt_native "$MKT_DD_A" "$A_RPC" core storage query \
    --input='{"sql":"SELECT endpoint_type, peer_port, hex(onion_pubkey) FROM file_offers"}' || true)"
python3 - "$A_OFFER_ROW" "$A_ONION" <<'PY' || mkt_die "seller offer endpoint row mismatch: $A_OFFER_ROW"
import base64,hashlib,json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
rows=d["data"]["rows"]
assert len(rows)==1,rows
endpoint_type,peer_port,pubkey_hex=rows[0]
assert endpoint_type==1 and peer_port==0,(endpoint_type,peer_port)
pub=bytes.fromhex(pubkey_hex)
chk=hashlib.sha3_256(b".onion checksum"+pub+b"\x03").digest()[:2]
addr=base64.b32encode(pub+chk+b"\x03").decode().lower().rstrip("=")+".onion"
assert addr==sys.argv[2],(addr,sys.argv[2])
PY

# ── Phase 2: the offer gossips to the buyer ──────────────────────────
mkt_note "waiting for the signed v2 offer to gossip to the buyer"
LIST_DEADLINE=$(( $(date +%s) + MKT_WAIT ))
while :; do
    BUYER_LIST="$(mkt_native "$MKT_DD_B" "$B_RPC" app market list 2>/dev/null || true)"
    case "$BUYER_LIST" in
        *"$OFFER_ID"*) break ;;
    esac
    [ "$(date +%s)" -lt "$LIST_DEADLINE" ] ||
        mkt_die "offer never gossiped to the buyer: $BUYER_LIST"
    sleep 1
done
BUYER_ENTRY="$(b_rpc zmarket_list | mkt_result)"
python3 - "$BUYER_ENTRY" "$OFFER_ID" "$EXPECT_ROOT" "$PRICE_PER_MB_ZAT" "$EXPECTED_CHUNKS" "$EXPECT_TOTAL_ZAT" <<'PY' || mkt_die "buyer market list entry mismatch: $BUYER_ENTRY"
import json,sys
rows=json.loads(sys.argv[1])
match=[r for r in rows if r.get("offer_id")==sys.argv[2]]
assert len(match)==1,rows
r=match[0]
assert r["root_hash"]==sys.argv[3],r
assert r["price_per_mb_zat"]==int(sys.argv[4]) and r["num_chunks"]==int(sys.argv[5]),r
assert r["total_cost_zat"]==int(sys.argv[6]),r
# The onion endpoint carries NO usable clearnet address: the buyer
# physically cannot open a clearnet file-service connection to the seller.
assert r["authenticated"] is True and r["peer_port"]==0,r
PY
B_OFFER_ROW="$(mkt_native "$MKT_DD_B" "$B_RPC" core storage query \
    --input="{\"sql\":\"SELECT endpoint_type, peer_port FROM file_offers WHERE offer_id=x'$OFFER_ID'\"}" || true)"
[ "$(printf '%s' "$B_OFFER_ROW" | mkt_jget 'd["data"]["rows"]' 2>/dev/null || true)" = "[[1, 0]]" ] ||
    mkt_die "buyer stored the offer with a non-onion endpoint: $B_OFFER_ROW"

# ── Phase 3: buyer purchase plan + commit (real Sapling payment) ─────
mkt_note "buyer plans the full-file purchase"
PLAN=""
for try in $(seq 1 20); do
    PLAN="$(printf '%s' "{\"wallet_scope\":\"dev\",\"offer_id\":\"$OFFER_ID\",\"source_address\":\"$BUYER_ADDR\",\"chunk_start\":0,\"chunks_paid\":$EXPECTED_CHUNKS,\"idempotency_key\":\"$IDEMPOTENCY_KEY\"}" \
        | mkt_native "$MKT_DD_B" "$B_RPC" app market purchase plan --input=- || true)"
    case "$PLAN" in
        *MONEY_STATE_NOT_CURRENT*|*MONEY_SNAPSHOT_CHANGED*) sleep 1 ;;
        *) break ;;
    esac
done
PLAN_ID="$(python3 - "$PLAN" "$OFFER_ID" "$EXPECT_TOTAL_ZAT" <<'PY' || mkt_die "purchase plan refused: $PLAN"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
data=d["data"]
assert data["stage"]=="plan" and data["committed"] is False and data["spends_funds"] is False,data
assert data["offer_id"]==sys.argv[2],data
assert data["amount_zat"]==int(sys.argv[3]),data
assert data["maximum_fee_zat"]>0 and data["reserved_zat"]==data["amount_zat"]+data["maximum_fee_zat"],data
assert data["chunk_start"]==0 and data["chunks_paid"]>0 and data["state"]=="planned",data
assert data["idempotent_replay"] is False,data
pid=data["plan_id"]
assert len(pid)==64 and int(pid,16)>0,data
assert "commit_input" in data,data
print(pid)
PY
)"
mkt_note "buyer purchase planned: plan_id=$PLAN_ID"

mkt_note "buyer commits the purchase (broadcasts the Sapling payment)"
COMMIT="$(printf '%s' "{\"wallet_scope\":\"dev\",\"plan_id\":\"$PLAN_ID\",\"confirm\":true}" \
    | mkt_native "$MKT_DD_B" "$B_RPC" app market purchase commit --input=- || true)"
TXID="$(python3 - "$COMMIT" <<'PY' || mkt_die "purchase commit refused: $COMMIT"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
data=d["data"]
assert data["stage"]=="committed" and data["committed"] is True and data["spends_funds"] is True,data
assert data["idempotent_replay"] is False,data
assert data["payment_notification_queued"] is True,data
assert data["state"]=="mempool_accepted",data
txid=data["txid"]
assert len(txid)==64 and int(txid,16)>0,data
assert len(data["claim_id"])==64,data
print(txid)
PY
)"
mkt_note "purchase payment broadcast: txid=$TXID"

# ── Phase 4: authorize-before-read — refused pre-confirmation, via onion ──
# The early retrieve dials A's onion service and the /market/chunk handler
# answers with the payment-gate refusal status: the onion route serves the
# authorize-before-read boundary, not just the happy path.
mkt_note "buyer retrieves before confirmation: the onion route must refuse"
EARLY_RETRIEVE="$(printf '%s' "{\"plan_id\":\"$PLAN_ID\",\"destination_path\":\"$DESTINATION\"}" \
    | mkt_native "$MKT_DD_B" "$B_RPC" app market purchase retrieve --input=- || true)"
python3 - "$EARLY_RETRIEVE" <<'PY' || mkt_die "pre-confirmation retrieve was not refused: $EARLY_RETRIEVE"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is False,d
err=d.get("error",{})
assert err.get("code")=="DELIVERY_NOT_READY",d
msg=err.get("message","")
assert "PENDING" in msg or "UNKNOWN" in msg,d
PY
[ ! -e "$DESTINATION" ] ||
    mkt_die "destination published before payment confirmation"

# Mempool relay is trickle, not instant: mining the confirmation before A
# has the payment produces a coinbase-only block and the purchase never
# confirms. Wait until A's mempool names the exact txid (either hex order).
mkt_note "waiting for the seller mempool to hold the payment"
TXID_REV="$(python3 -c 'import sys; print(bytes.fromhex(sys.argv[1])[::-1].hex())' "$TXID")"
MEMPOOL_DEADLINE=$(( $(date +%s) + MKT_WAIT ))
while :; do
    MEMPOOL="$(a_rpc getrawmempool 2>/dev/null | mkt_result 2>/dev/null || true)"
    case "$MEMPOOL" in
        *"$TXID"*|*"$TXID_REV"*) break ;;
    esac
    [ "$(date +%s)" -lt "$MEMPOOL_DEADLINE" ] ||
        mkt_die "payment never reached the seller mempool: $MEMPOOL"
    sleep 1
done

# ── Phase 5: mine the confirmation; both sides reconcile ─────────────
mkt_note "mining the payment confirmation block"
SELLER_ADDR="$(a_rpc getnewaddress | mkt_result)"
mkt_mine_to_address a_rpc 1 "$SELLER_ADDR"
mkt_wait_height "$MKT_DD_B" "$B_RPC" 102 || mkt_die "B did not sync the confirmation block"
mkt_wait_fold "$MKT_DD_A" "$A_RPC" 102 || mkt_die "A reducer fold did not reach the confirmation tip"
mkt_wait_fold "$MKT_DD_B" "$B_RPC" 102 || mkt_die "B reducer fold did not reach the confirmation tip"

# The market purchase status leaf is a dumb durable read by design; the
# vault controller's reconcile (triggered here by vault_intent_status) is
# what advances mempool_accepted -> confirmed against the canonical chain.
mkt_note "polling the buyer purchase status until confirmed"
STATUS_DEADLINE=$(( $(date +%s) + MKT_WAIT ))
while :; do
    VI_REFRESH="$(b_rpc vault_intent_status "{\"plan_id\":\"$PLAN_ID\"}" 2>&1 || true)"
    STATUS="$(printf '%s' "{\"plan_id\":\"$PLAN_ID\"}" \
        | mkt_native "$MKT_DD_B" "$B_RPC" app market purchase status --input=- || true)"
    state="$(printf '%s' "$STATUS" | mkt_jget 'd["data"]["state"]' 2>/dev/null || true)"
    [ "$state" = "confirmed" ] && break
    [ "$(date +%s)" -lt "$STATUS_DEADLINE" ] ||
        mkt_die "purchase never confirmed: $STATUS"
    sleep 1
done
python3 - "$STATUS" "$TXID" <<'PY' || mkt_die "confirmed purchase status mismatch: $STATUS"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
data=d["data"]
assert data["state"]=="confirmed" and data["txid"]==sys.argv[2],data
assert len(data["claim_id"])==64,data
PY

# The seller wallet must trial-decrypt its exact payment note at the
# confirmation height before the chunk gate can bind against it.
mkt_note "waiting for the seller wallet to decrypt its exact payment note"
NOTE_DEADLINE=$(( $(date +%s) + MKT_WAIT ))
while :; do
    NOTE="$(mkt_native "$MKT_DD_A" "$A_RPC" core storage query \
        --input="{\"sql\":\"SELECT COUNT(*) FROM wallet_sapling_notes WHERE value=$EXPECT_TOTAL_ZAT AND block_height=102\"}" || true)"
    ncount="$(printf '%s' "$NOTE" | mkt_jget 'd["data"]["rows"][0][0]' 2>/dev/null || true)"
    [ "$ncount" = "1" ] && break
    [ "$(date +%s)" -lt "$NOTE_DEADLINE" ] ||
        mkt_die "seller never decrypted its payment note: $NOTE"
    sleep 1
done

# ── Phase 6 (negative): retrieve without Tor refuses by name ─────────
# A completed retrieve replays idempotently without touching the transport
# gate, so the no-Tor refusal must be proven BEFORE the successful
# retrieve: kill B, bring the same datadir up WITHOUT -tor, and the signed
# onion offer must refuse with ONION_DELIVERY_UNAVAILABLE — never a
# clearnet fallback.
mkt_note "restarting B WITHOUT -tor: the onion offer must refuse retrieval by name"
mkt_kill_group "$MKT_PGID_B"; MKT_PGID_B=""
MKT_EXTRA_FLAGS=()
MKT_PGID_B="$(mkt_spawn "$MKT_DD_B" "$B_PORT" "$B_RPC" "$B_FS" "$B_HTTPS" "127.0.0.1:$DEAD_SINK")"
mkt_wait_rpc "$MKT_DD_B" "$B_RPC" "$MKT_PGID_B" || mkt_die "B no-Tor restart failed"
mkt_unlock_wallet "$MKT_DD_B" "$B_RPC" || mkt_die "B no-Tor wallet unlock failed"
NOTOR_RETRIEVE="$(printf '%s' "{\"plan_id\":\"$PLAN_ID\",\"destination_path\":\"$DESTINATION\"}" \
    | mkt_native "$MKT_DD_B" "$B_RPC" app market purchase retrieve --input=- || true)"
python3 - "$NOTOR_RETRIEVE" <<'PY' || mkt_die "retrieve without Tor was not refused by name: $NOTOR_RETRIEVE"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is False,d
err=d.get("error",{})
assert err.get("code")=="ONION_DELIVERY_UNAVAILABLE",d
PY
[ ! -e "$DESTINATION" ] ||
    mkt_die "destination published by a no-Tor retrieve"
mkt_note "no-Tor retrieve refused with ONION_DELIVERY_UNAVAILABLE"

# ── Phase 7: authorized onion retrieval + verified publication ───────
# Retry on DELIVERY_NOT_READY: the seller's per-chunk authorization
# reconciles the claim live, and a real Tor circuit to a freshly published
# onion service can fail transiently — both are transient refusals, not
# verdicts. Each slice is one blocking embedded-Tor fetch, so this phase
# gets its own generous budget.
mkt_note "restarting B WITH -tor for the onion retrieval"
mkt_kill_group "$MKT_PGID_B"; MKT_PGID_B=""
MKT_EXTRA_FLAGS=("-tor")
MKT_PGID_B="$(mkt_spawn "$MKT_DD_B" "$B_PORT" "$B_RPC" "$B_FS" "$B_HTTPS" "127.0.0.1:$DEAD_SINK")"
mkt_wait_rpc "$MKT_DD_B" "$B_RPC" "$MKT_PGID_B" || mkt_die "B onion-retrieve restart failed"
B_ONION="$(oni_wait_onion_address "$MKT_DD_B" "$B_RPC" "buyer B (onion-retrieve restart)")"
mkt_unlock_wallet "$MKT_DD_B" "$B_RPC" || mkt_die "B onion-retrieve wallet unlock failed"

mkt_note "buyer retrieves the file over the seller onion service (60 KiB slices)"
RETRIEVE_DEADLINE=$(( $(date +%s) + ONI_RETRIEVE_WAIT ))
while :; do
    RETRIEVE="$(printf '%s' "{\"plan_id\":\"$PLAN_ID\",\"destination_path\":\"$DESTINATION\"}" \
        | mkt_native "$MKT_DD_B" "$B_RPC" app market purchase retrieve --input=- || true)"
    rok="$(printf '%s' "$RETRIEVE" | mkt_jget 'd["ok"]' 2>/dev/null || true)"
    [ "$rok" = "True" ] && break
    case "$RETRIEVE" in
        *DELIVERY_NOT_READY*) ;;
        *) mkt_die "retrieve failed with a non-delivery error: $RETRIEVE" ;;
    esac
    [ "$(date +%s)" -lt "$RETRIEVE_DEADLINE" ] ||
        mkt_die "retrieve never authorized over onion: $RETRIEVE"
    sleep 2
done
python3 - "$RETRIEVE" "$FIXTURE_SIZE" "$EXPECTED_CHUNKS" <<'PY' || mkt_die "retrieve failed: $RETRIEVE"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
data=d["data"]
assert data["stage"]=="retrieved",data
assert data["download_state"]=="complete" and data["destination_published"] is True,data
assert data["chunks_received"]==int(sys.argv[3]) and data["num_chunks"]==int(sys.argv[3]),data
assert data["bytes_received"]==int(sys.argv[2]) and data["size_bytes"]==int(sys.argv[2]),data
PY
cmp -s "$FIXTURE" "$DESTINATION" ||
    mkt_die "delivered bytes differ from the seller fixture"
DELIVERED_ROOT="$(python3 - "$DESTINATION" <<'PY'
import hashlib,sys
chunk = 50 * 1024 * 1024
digests = []
with open(sys.argv[1], "rb") as f:
    while True:
        b = f.read(chunk)
        if not b: break
        digests.append(hashlib.sha3_256(b).digest())
print(hashlib.sha3_256(b"".join(digests)).hexdigest())
PY
)"
[ "$DELIVERED_ROOT" = "$EXPECT_ROOT" ] ||
    mkt_die "delivered bytes re-derive a different content root"

# ── Phase 8: the onion-transport witnesses ────────────────────────────
# The delivery leg is witnessed on BOTH tor.log files (the dynhost
# webserver and client log every request at notice level, which the
# generated torrc writes to <datadir>/tor.log): the seller logged the
# /market/chunk GETs (one per slice fetch, including the pre-confirmation
# refusal), the buyer logged the matching client fetches to the .onion.
# Combined with the signed offer carrying no usable clearnet endpoint
# (peer_port=0, asserted above) and B never dialling A's file service,
# this is the proof the bytes crossed the Tor network.
mkt_note "verifying the /market/chunk traffic crossed both Tor instances"
A_CHUNK_GETS="$(grep -c "HTTP GET /market/chunk/" "$MKT_DD_A/tor.log" 2>/dev/null || true)"
[ "${A_CHUNK_GETS:-0}" -ge "$EXPECTED_SLICES" ] ||
    mkt_die "seller tor.log shows only ${A_CHUNK_GETS:-0} /market/chunk GETs (need $EXPECTED_SLICES)"
B_CHUNK_FETCHES="$(grep -c "initiated fetch to .*\.onion/market/chunk/" "$MKT_DD_B/tor.log" 2>/dev/null || true)"
[ "${B_CHUNK_FETCHES:-0}" -ge "$EXPECTED_SLICES" ] ||
    mkt_die "buyer tor.log shows only ${B_CHUNK_FETCHES:-0} onion chunk fetches (need $EXPECTED_SLICES)"
mkt_note "onion witness: seller served $A_CHUNK_GETS chunk GETs, buyer initiated $B_CHUNK_FETCHES"

# The authorized delivery above made the seller reconcile the claim against
# its exact canonical note; the durable row must now read CONFIRMED.
mkt_note "verifying the seller-side payment claim is confirmed"
CLAIM="$(mkt_native "$MKT_DD_A" "$A_RPC" core storage query \
    --input='{"sql":"SELECT status, status_reason, confirmations, block_height FROM market_payment_claims"}' || true)"
python3 - "$CLAIM" <<'PY' || mkt_die "seller claim row mismatch: $CLAIM"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
data=d["data"]
cols=data["columns"]; rows=data["rows"]
assert len(rows)==1,rows
r=dict(zip(cols,rows[0]))
assert r["status"]=="CONFIRMED" and r["confirmations"]>=1 and r["block_height"]==102,r
PY
FINAL_STATUS="$(printf '%s' "{\"plan_id\":\"$PLAN_ID\"}" \
    | mkt_native "$MKT_DD_B" "$B_RPC" app market purchase status --input=- || true)"
[ "$(printf '%s' "$FINAL_STATUS" | mkt_jget 'd["data"]["destination_published"]' 2>/dev/null || true)" = "True" ] ||
    mkt_die "purchase status does not show the completed download: $FINAL_STATUS"

# ── Phase 9: idempotent replays ──────────────────────────────────────
mkt_note "re-committing the same purchase plan (idempotent replay, no double-spend)"
RECOMMIT="$(printf '%s' "{\"wallet_scope\":\"dev\",\"plan_id\":\"$PLAN_ID\",\"confirm\":true}" \
    | mkt_native "$MKT_DD_B" "$B_RPC" app market purchase commit --input=- || true)"
python3 - "$RECOMMIT" "$TXID" <<'PY' || mkt_die "purchase re-commit was not an exact replay: $RECOMMIT"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
data=d["data"]
assert data["idempotent_replay"] is True and data["txid"]==sys.argv[2],data
PY
REPLAN="$(printf '%s' "{\"wallet_scope\":\"dev\",\"offer_id\":\"$OFFER_ID\",\"source_address\":\"$BUYER_ADDR\",\"chunk_start\":0,\"chunks_paid\":$EXPECTED_CHUNKS,\"idempotency_key\":\"$IDEMPOTENCY_KEY\"}" \
    | mkt_native "$MKT_DD_B" "$B_RPC" app market purchase plan --input=- || true)"
python3 - "$REPLAN" "$PLAN_ID" <<'PY' || mkt_die "purchase re-plan was not an exact replay: $REPLAN"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
data=d["data"]
assert data["idempotent_replay"] is True and data["plan_id"]==sys.argv[2],data
PY

mkt_note "seller re-commits the same offer (content-addressed idempotent)"
REOFFER="$(printf '%s' "{\"filepath\":\"$FIXTURE\",\"price_per_mb_zat\":$PRICE_PER_MB_ZAT,\"confirm\":true}" \
    | mkt_native "$MKT_DD_A" "$A_RPC" app market offer --input=- || true)"
python3 - "$REOFFER" "$OFFER_ID" <<'PY' || mkt_die "offer re-commit was not an exact replay: $REOFFER"
import json,sys
d=json.loads(sys.argv[1])
assert d["ok"] is True,d
data=d["data"]
assert data["stage"]=="committed" and data["idempotent_replay"] is True,data
assert data["offer_id"]==sys.argv[2],data
assert data["endpoint_source"]=="onion",data
PY

mkt_note "PASS: two-daemon onion market trade — v2 onion-endpoint offer gossip, Sapling payment, authorize-before-read refusal served through the onion route, no-Tor retrieve refused by name (ONION_DELIVERY_UNAVAILABLE), 3-slice 60 KiB onion delivery byte-identical to the offer root, /market/chunk traffic witnessed in both tor.log files, idempotent replays"
