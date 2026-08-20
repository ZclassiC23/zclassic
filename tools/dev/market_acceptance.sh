#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# B3 acceptance: two isolated regtest daemons run the full P2P file-market
# trade end-to-end. A (seller, -externalip + file service) plans and commits a
# signed paid offer; the offer gossips to B (buyer) over the live peer link;
# B plans, commits a real Sapling payment (z_sendmany t->z + exact memo), is
# REFUSED delivery before confirmation (authorize-before-read), then after one
# mined block the seller wallet trial-decrypts its exact payment note and B
# retrieves the file chunk-by-chunk — each paid-chunk request makes the seller
# reconcile the claim live (that, not block arrival, flips the row to
# CONFIRMED) — into an atomically published destination.
# Idempotent replays (offer re-commit, plan re-plan, purchase re-commit) and
# the seller-side confirmed claim row close the proof.
#
# Modelled on tools/dev/zcode_dht_acceptance.sh: same setsid isolation, port
# refuse-set discipline, wallet-custody recipe, mining cadence, pgid cleanup.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NODE_BIN="${ZCL_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
RPC_BIN="${ZCL_RPC_BIN:-$REPO_ROOT/build/bin/zcl-rpc}"
MKT_HELPER="${ZCL_MARKET_HELPER:-$REPO_ROOT/build/bin/zclassic23-market-acceptance-helper}"

MKT_LIVE_PORTS="8023 8033 8034 8035 8043 8044 8045 8046 8232 8443 \
18034 18232 18234 18243 18244 18245 18246"
# Fresh block vs the siblings (dht: 29211-29273, science: 39111-39123,
# p2p 20022-20027 + 18033) and vs this host's zclassic23-live instance,
# which already owns 39311/39312 — the refuse-set check caught it.
# P2P reconnects pass the production reachable-port policy only for
# explicit test-safe ports, so the one post-restart link is an
# operator-directed onetry, never a redial.
A_PORT=20030; A_RPC=39511; A_FS=39512; A_HTTPS=39513
B_PORT=20031; B_RPC=39521; B_FS=39522; B_HTTPS=39523
DEAD_SINK=39999
MKT_WAIT="${MKT_WAIT:-90}"
MKT_WORK=""; MKT_DD_A=""; MKT_DD_B=""; MKT_PGID_A=""; MKT_PGID_B=""
MKT_EXTRA_FLAGS=()
MKT_CLEANED=0
MKT_KEEP="${MKT_KEEP:-0}"
# Throwaway passphrases for the wallet-custody recipe (never argv: they
# ride the wallet-passphrase credential file and --input=- stdin only).
MKT_WALLET_PASS="market-acceptance-wallet-pass"
MKT_BACKUP_PASS="market-acceptance-backup-pass"

# Trade terms: a three-chunk fixture (two full 50 MiB chunks + a tail).
# Retrieve requires the full-file purchase, so chunks_paid is always
# num_chunks below. The price keeps the whole trade inside the dev wallet
# scope's lifetime lab cap (DEV_LAB_CAP_ZAT = 0.05 ZCL,
# app/services/src/wallet_money_service.c) plus the wallet default fee:
# total is 100 * 30000 + ceil(12345 * 30000 / 2**20) = 3000354 zat.
PRICE_PER_MB_ZAT=30000
FIXTURE_TAIL_BYTES=12345
IDEMPOTENCY_KEY="market-acceptance-purchase-1"

mkt_die() {
    echo "market-acceptance: FATAL: $*" >&2
    if [ -n "$MKT_WORK" ] && [ -d "$MKT_WORK" ]; then
        printf '%s\n' "$*" >"$MKT_WORK/FAILURE"
    fi
    exit 2
}
mkt_note() { echo "market-acceptance: $*"; }

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
            "$REPO_ROOT"/test-tmp/zcl23-mktacc-*) rm -rf "$MKT_WORK" ;;
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
    "$MKT_HELPER" rpc-result
}
mkt_jget() {
    "$MKT_HELPER" get "$1"
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
    # Overwinter+Sapling from genesis on BOTH nodes (the zcashd -nuparams
    # equivalent; regtest otherwise pins them NO_ACTIVATION and no shielded
    # payment can ever be mined or relayed).
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
# The money freshness classifier fails closed on finding_peers; the sync
# FSM only leaves it behind a peer it can sync FROM (outbound).
mkt_wait_sync_live() {
    local dd="$1" rpc="$2" deadline state
    deadline=$(( $(date +%s) + MKT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        state="$(mkt_rpc "$dd" "$rpc" downloadstats 2>/dev/null \
            | mkt_jget 'result.sync_state' 2>/dev/null || true)"
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
            | mkt_jget 'result.sync_state' 2>/dev/null || true)"
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
        coins="$(printf '%s' "$dump" | mkt_jget 'state.coins_best_height' 2>/dev/null || true)"
        hstar="$(printf '%s' "$dump" | mkt_jget 'state.hstar' 2>/dev/null || true)"
        [ "$coins" = "$tip" ] && [ "$hstar" = "$tip" ] && return 0
        sleep 1
    done
    echo "market-acceptance: reducer_frontier at stall: $dump" >&2
    return 1
}
# RPC-ready != chain-loaded: the money gate needs the active chain index,
# which loads after the RPC starts serving.
mkt_wait_chain_loaded() {
    local dd="$1" rpc="$2" tip="$3" deadline chain blocks ibd
    deadline=$(( $(date +%s) + MKT_WAIT ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        chain="$(mkt_rpc "$dd" "$rpc" getblockchaininfo 2>/dev/null || true)"
        blocks="$(printf '%s' "$chain" | mkt_jget 'result.blocks' 2>/dev/null || true)"
        ibd="$(printf '%s' "$chain" | mkt_jget 'result.initialblockdownload' 2>/dev/null || true)"
        [ "$blocks" = "$tip" ] && [ "$ibd" != "True" ] && return 0
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
            | mkt_jget 'state.zcl.spendable' 2>/dev/null || true)"
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
    [ "$(printf '%s' "$status" | mkt_jget 'ok' 2>/dev/null || true)" = "True" ] || {
        printf '%s\n' "$status" >&2; return 1; }
    if [ "$(printf '%s' "$status" | mkt_jget 'data.unlocked' 2>/dev/null || true)" != "True" ]; then
        unlock="$(printf '%s' "{\"passphrase\":\"$MKT_WALLET_PASS\",\"timeout_seconds\":3600}" \
            | mkt_native "$dd" "$rpc" core wallet security unlock --input=- || true)"
        [ "$(printf '%s' "$unlock" | mkt_jget 'data.unlocked' 2>/dev/null || true)" = "True" ] || {
            printf '%s\n' "$unlock" >&2; return 1; }
    fi
    return 0
}
mkt_backup_wallet() {
    local dd="$1" rpc="$2" out
    out="$(printf '%s' "{\"confirm\":true,\"password\":\"$MKT_BACKUP_PASS\"}" \
        | mkt_native "$dd" "$rpc" core wallet backup now --input=- || true)"
    [ "$(printf '%s' "$out" | mkt_jget 'ok' 2>/dev/null || true)" = "True" ] || {
        printf '%s\n' "$out" >&2; return 1; }
}

for port in $A_PORT $A_RPC $A_FS $A_HTTPS $B_PORT $B_RPC $B_FS $B_HTTPS; do
    mkt_assert_port "$port"
done
[ -x "$NODE_BIN" ] && [ -x "$RPC_BIN" ] && [ -x "$MKT_HELPER" ] ||
    mkt_die "build node, RPC, and market acceptance helper binaries first"
mkdir -p "$REPO_ROOT/test-tmp"
MKT_WORK="$(mktemp -d "$REPO_ROOT/test-tmp/zcl23-mktacc-XXXXXX")"
MKT_DD_A="$MKT_WORK/a"; MKT_DD_B="$MKT_WORK/b"
MKT_CONTENT="$MKT_WORK/content"
MKT_DOWNLOADS="$MKT_WORK/downloads"
mkdir -p "$MKT_DD_A" "$MKT_DD_B" "$MKT_CONTENT" "$MKT_DOWNLOADS"
FIXTURE="$MKT_CONTENT/seller-fixture.bin"
DESTINATION="$MKT_DOWNLOADS/bought-copy.bin"

# Deterministic three-chunk fixture (2 x 50 MiB + tail), plus the exact
# manifest root the offer must commit (sha3-256 over the concatenated
# per-chunk sha3-256 digests) and the exact total price.
read -r FIXTURE_SIZE EXPECT_ROOT EXPECT_TOTAL_ZAT \
    <<<"$("$MKT_HELPER" fixture-create "$FIXTURE" "$PRICE_PER_MB_ZAT" \
        "$FIXTURE_TAIL_BYTES")" || mkt_die "fixture build failed"
EXPECTED_CHUNKS=3

# Wallet custody: boot both nodes with a passphrase credential so key writes
# encrypt at rest (WKS1). The seller-key envelope (metadata DEK) and the
# buyer's money gate both refuse a locked or plaintext wallet.
MKT_CRED_DIR="$MKT_WORK/cred"
install -d -m 700 "$MKT_CRED_DIR"
install -m 600 /dev/null "$MKT_CRED_DIR/wallet-passphrase"
printf '%s\n' "$MKT_WALLET_PASS" >"$MKT_CRED_DIR/wallet-passphrase"
export CREDENTIALS_DIRECTORY="$MKT_CRED_DIR"

mkt_note "booting seller A (-externalip + file service) and buyer B"
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
mkt_wait_fold "$MKT_DD_B" "$B_RPC" 101 || mkt_die "B reducer fold did not reach the funding tip"

# The buyer's purchase money gate reads the forward-folded coins set, whose
# authority stamps land only at boot: restart B (the funded node) so they
# stamp, then re-link with an operator-directed onetry so B owns the
# OUTBOUND peer the money-freshness classifier demands (it fails closed on
# finding_peers). A stays up on the dead sink the whole time, so no
# already-connected skip can delay the link.
mkt_note "restarting B so the forward-folded coins set stamps its authority"
mkt_kill_group "$MKT_PGID_B"; MKT_PGID_B=""
MKT_PGID_B="$(mkt_spawn "$MKT_DD_B" "$B_PORT" "$B_RPC" "$B_FS" "$B_HTTPS" "127.0.0.1:$DEAD_SINK")"
mkt_wait_rpc "$MKT_DD_B" "$B_RPC" "$MKT_PGID_B" || mkt_die "B custody restart failed"
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
printf '%s' "$OFFER_PLAN" | "$MKT_HELPER" offer-plan "$EXPECT_ROOT" \
    "$FIXTURE_SIZE" "$EXPECTED_CHUNKS" "$EXPECT_TOTAL_ZAT" ||
    mkt_die "offer plan preview mismatch: $OFFER_PLAN"
OFFER_COUNT="$(mkt_native "$MKT_DD_A" "$A_RPC" core storage query \
    --input='{"sql":"SELECT COUNT(*) AS n FROM file_offers"}' || true)"
[ "$(printf '%s' "$OFFER_COUNT" | mkt_jget 'data.rows.0.0' 2>/dev/null || true)" = "0" ] ||
    mkt_die "offer plan mutated seller storage: $OFFER_COUNT"
SELLER_LIST_RAW="$(a_rpc zmarket_list 2>&1 || true)"
SELLER_LIST="$(printf '%s' "$SELLER_LIST_RAW" | mkt_result 2>&1 || true)"
printf '%s' "$SELLER_LIST" | "$MKT_HELPER" market-empty ||
    mkt_die "offer plan changed or malformed the seller market view: raw=$SELLER_LIST_RAW projected=$SELLER_LIST"

mkt_note "seller commits the offer (seal, persist, bind, flood)"
OFFER_COMMIT="$(printf '%s' "{\"filepath\":\"$FIXTURE\",\"price_per_mb_zat\":$PRICE_PER_MB_ZAT,\"confirm\":true}" \
    | mkt_native "$MKT_DD_A" "$A_RPC" app market offer --input=- || true)"
OFFER_ID="$(printf '%s' "$OFFER_COMMIT" |
    "$MKT_HELPER" offer-commit "$EXPECT_ROOT" ||
    mkt_die "offer commit refused: $OFFER_COMMIT")"
mkt_note "seller offer committed: offer_id=$OFFER_ID"

# ── Phase 2: the offer gossips to the buyer ──────────────────────────
mkt_note "waiting for the signed offer to gossip to the buyer"
LIST_DEADLINE=$(( $(date +%s) + MKT_WAIT ))
while :; do
    BUYER_LIST="$(printf '%s' '{"profile":"open"}' |
        mkt_native "$MKT_DD_B" "$B_RPC" app market list --input=- 2>/dev/null || true)"
    case "$BUYER_LIST" in
        *"$OFFER_ID"*) break ;;
    esac
    [ "$(date +%s)" -lt "$LIST_DEADLINE" ] ||
        mkt_die "offer never gossiped to the buyer: $BUYER_LIST"
    sleep 1
done
DEFAULT_BUYER_LIST="$(mkt_native "$MKT_DD_B" "$B_RPC" app market list 2>/dev/null || true)"
printf '%s' "$DEFAULT_BUYER_LIST" | "$MKT_HELPER" market-hidden "$OFFER_ID" ||
    mkt_die "default moderation view did not honestly hide the unreviewed offer: $DEFAULT_BUYER_LIST"
BUYER_ENTRY="$BUYER_LIST"
printf '%s' "$BUYER_ENTRY" | "$MKT_HELPER" buyer-entry "$OFFER_ID" \
    "$EXPECT_ROOT" "$PRICE_PER_MB_ZAT" "$EXPECTED_CHUNKS" \
    "$EXPECT_TOTAL_ZAT" || mkt_die "buyer market list entry mismatch: $BUYER_ENTRY"

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
PLAN_ID="$(printf '%s' "$PLAN" |
    "$MKT_HELPER" purchase-plan "$OFFER_ID" "$EXPECT_TOTAL_ZAT" ||
    mkt_die "purchase plan refused: $PLAN")"
mkt_note "buyer purchase planned: plan_id=$PLAN_ID"

mkt_note "buyer commits the purchase (broadcasts the Sapling payment)"
COMMIT="$(printf '%s' "{\"wallet_scope\":\"dev\",\"plan_id\":\"$PLAN_ID\",\"confirm\":true}" \
    | mkt_native "$MKT_DD_B" "$B_RPC" app market purchase commit --input=- || true)"
TXID="$(printf '%s' "$COMMIT" | "$MKT_HELPER" purchase-commit ||
    mkt_die "purchase commit refused: $COMMIT")"
mkt_note "purchase payment broadcast: txid=$TXID"

# ── Phase 4: authorize-before-read — delivery refused pre-confirmation ──
mkt_note "buyer retrieves before confirmation: the seller must refuse"
EARLY_RETRIEVE="$(printf '%s' "{\"plan_id\":\"$PLAN_ID\",\"destination_path\":\"$DESTINATION\"}" \
    | mkt_native "$MKT_DD_B" "$B_RPC" app market purchase retrieve --input=- || true)"
printf '%s' "$EARLY_RETRIEVE" | "$MKT_HELPER" early-refusal ||
    mkt_die "pre-confirmation retrieve was not refused: $EARLY_RETRIEVE"
[ ! -e "$DESTINATION" ] ||
    mkt_die "destination published before payment confirmation"

# Mempool relay is trickle, not instant: mining the confirmation before A
# has the payment produces a coinbase-only block and the purchase never
# confirms. Wait until A's mempool names the exact txid (either hex order).
mkt_note "waiting for the seller mempool to hold the payment"
TXID_REV="$("$MKT_HELPER" reverse-hex "$TXID")"
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
    state="$(printf '%s' "$STATUS" | mkt_jget 'data.state' 2>/dev/null || true)"
    [ "$state" = "confirmed" ] && break
    [ "$(date +%s)" -lt "$STATUS_DEADLINE" ] ||
        mkt_die "purchase never confirmed: $STATUS"
    sleep 1
done
printf '%s' "$STATUS" | "$MKT_HELPER" purchase-status "$TXID" ||
    mkt_die "confirmed purchase status mismatch: $STATUS"

# The seller claim row is a rebuildable projection: nothing reconciles it on
# block arrival. The seller re-derives authority live inside
# market_payment_authorize_chunk on each paid-chunk request (the buyer's
# retrieve), so the row only flips to CONFIRMED during authorized delivery.
# First wait for the seller wallet to have trial-decrypted its exact payment
# note at the confirmation height (tip_finalize scans every connected block);
# that note is the receipt authority the chunk gate binds against.
mkt_note "waiting for the seller wallet to decrypt its exact payment note"
NOTE_DEADLINE=$(( $(date +%s) + MKT_WAIT ))
while :; do
    NOTE="$(mkt_native "$MKT_DD_A" "$A_RPC" core storage query \
        --input="{\"sql\":\"SELECT COUNT(*) FROM wallet_sapling_notes WHERE value=$EXPECT_TOTAL_ZAT AND block_height=102\"}" || true)"
    ncount="$(printf '%s' "$NOTE" | mkt_jget 'data.rows.0.0' 2>/dev/null || true)"
    [ "$ncount" = "1" ] && break
    [ "$(date +%s)" -lt "$NOTE_DEADLINE" ] ||
        mkt_die "seller never decrypted its payment note: $NOTE"
    sleep 1
done

# ── Phase 6: authorized retrieval + verified publication ─────────────
# Retry on DELIVERY_NOT_READY: the seller's per-chunk authorization
# reconciles the claim live, so a still-lagging seller projection is a
# transient refusal, not a verdict.
mkt_note "buyer retrieves the file; each chunk authorizes against the chain"
RETRIEVE_DEADLINE=$(( $(date +%s) + MKT_WAIT ))
while :; do
    RETRIEVE="$(printf '%s' "{\"plan_id\":\"$PLAN_ID\",\"destination_path\":\"$DESTINATION\"}" \
        | mkt_native "$MKT_DD_B" "$B_RPC" app market purchase retrieve --input=- || true)"
    rok="$(printf '%s' "$RETRIEVE" | mkt_jget 'ok' 2>/dev/null || true)"
    [ "$rok" = "True" ] && break
    case "$RETRIEVE" in
        *DELIVERY_NOT_READY*) ;;
        *) mkt_die "retrieve failed with a non-delivery error: $RETRIEVE" ;;
    esac
    [ "$(date +%s)" -lt "$RETRIEVE_DEADLINE" ] ||
        mkt_die "retrieve never authorized: $RETRIEVE"
    sleep 2
done
printf '%s' "$RETRIEVE" | "$MKT_HELPER" retrieve "$FIXTURE_SIZE" \
    "$EXPECTED_CHUNKS" || mkt_die "retrieve failed: $RETRIEVE"
cmp -s "$FIXTURE" "$DESTINATION" ||
    mkt_die "delivered bytes differ from the seller fixture"
DELIVERED_ROOT="$("$MKT_HELPER" file-root "$DESTINATION")"
[ "$DELIVERED_ROOT" = "$EXPECT_ROOT" ] ||
    mkt_die "delivered bytes re-derive a different content root"

# The authorized delivery above made the seller reconcile the claim against
# its exact canonical note; the durable row must now read CONFIRMED.
mkt_note "verifying the seller-side payment claim is confirmed"
CLAIM="$(mkt_native "$MKT_DD_A" "$A_RPC" core storage query \
    --input='{"sql":"SELECT status, status_reason, confirmations, block_height FROM market_payment_claims"}' || true)"
printf '%s' "$CLAIM" | "$MKT_HELPER" claim ||
    mkt_die "seller claim row mismatch: $CLAIM"
FINAL_STATUS="$(printf '%s' "{\"plan_id\":\"$PLAN_ID\"}" \
    | mkt_native "$MKT_DD_B" "$B_RPC" app market purchase status --input=- || true)"
[ "$(printf '%s' "$FINAL_STATUS" | mkt_jget 'data.destination_published' 2>/dev/null || true)" = "True" ] ||
    mkt_die "purchase status does not show the completed download: $FINAL_STATUS"

# ── Phase 7: idempotent replays ──────────────────────────────────────
mkt_note "re-committing the same purchase plan (idempotent replay, no double-spend)"
RECOMMIT="$(printf '%s' "{\"wallet_scope\":\"dev\",\"plan_id\":\"$PLAN_ID\",\"confirm\":true}" \
    | mkt_native "$MKT_DD_B" "$B_RPC" app market purchase commit --input=- || true)"
printf '%s' "$RECOMMIT" | "$MKT_HELPER" recommit "$TXID" ||
    mkt_die "purchase re-commit was not an exact replay: $RECOMMIT"
REPLAN="$(printf '%s' "{\"wallet_scope\":\"dev\",\"offer_id\":\"$OFFER_ID\",\"source_address\":\"$BUYER_ADDR\",\"chunk_start\":0,\"chunks_paid\":$EXPECTED_CHUNKS,\"idempotency_key\":\"$IDEMPOTENCY_KEY\"}" \
    | mkt_native "$MKT_DD_B" "$B_RPC" app market purchase plan --input=- || true)"
printf '%s' "$REPLAN" | "$MKT_HELPER" replan "$PLAN_ID" ||
    mkt_die "purchase re-plan was not an exact replay: $REPLAN"

mkt_note "seller re-commits the same offer (content-addressed idempotent)"
REOFFER="$(printf '%s' "{\"filepath\":\"$FIXTURE\",\"price_per_mb_zat\":$PRICE_PER_MB_ZAT,\"confirm\":true}" \
    | mkt_native "$MKT_DD_A" "$A_RPC" app market offer --input=- || true)"
printf '%s' "$REOFFER" | "$MKT_HELPER" reoffer "$OFFER_ID" ||
    mkt_die "offer re-commit was not an exact replay: $REOFFER"

mkt_note "PASS: two-daemon market trade — signed offer gossip, Sapling payment, authorize-before-read refusal, confirmed delivery byte-identical to the offer root, idempotent replays"
