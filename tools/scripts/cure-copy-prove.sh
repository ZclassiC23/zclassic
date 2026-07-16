#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# cure-copy-prove.sh — the W4-1 sovereign-cure copy-prove driver. Wraps the
# same discipline as tools/repro_on_copy.sh's --install-consensus-bundle mode
# (docs/work/sovereign-cutover-runbook.md), plus the one step that harness
# does not know how to do: placing the independent replay receipt
# (config/consensus_state_replay_receipt.h) inside the COPY's own datadir
# BEFORE the terminal -install-consensus-bundle call, since
# consensus_state_replay_receipt_authority_available() reads it through the
# install target's OWN datadir_fd (see docs/work/cure-runbook-2026-07-16.md
# section 0). Without this step every copy-prove attempt at the bundle
# install would silently stay VERIFIED_CONTAINED and never actually cure
# anything.
#
# This script NEVER targets a live datadir: --copy-dir (or its generated
# default) must carry the "-COPY-" marker and must not match a known live
# datadir name, mirroring the invariant enforced by
# app/controllers/src/agent_copy_prove_controller.c's cp_path_safety_ok().
#
# Usage:
#   tools/scripts/cure-copy-prove.sh [--dry-run] \
#     --bundle=PATH_TO_consensus-state-bundle-<anchor>.sqlite \
#     --receipt=PATH_TO_consensus_state_replay_receipt.v1 \
#     [--src=DIR]                 (default: $HOME/.zclassic-c23)
#     [--copy-dir=DIR]            (default: $HOME/.zclassic-c23-COPY-<ts>-cure)
#     [--expect-climb-past=H]     (required unless --dry-run)
#     [--deadline=SECS]           (default 3600)
#     [--rpcport=N] [--p2p-port=N] [--fs-port=N] [--https-port=N]
#
# Exit codes: 0 PASS, 1 FAIL (G-SOV not cleared), 2 usage/precondition error.
#
# What it does NOT do: touch --src beyond a read-only `cp -a` source, set
# ZCL_DEPLOY_ALLOW_CANONICAL, restart/stop any systemd unit, or delete
# anything outside its own --copy-dir.
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
NODE_BIN="${ZCL_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
RPC_BIN="${ZCL_RPC_BIN:-$REPO_ROOT/build/bin/zcl-rpc}"

BUNDLE=""
RECEIPT=""
SRC="$HOME/.zclassic-c23"
COPY_DIR=""
EXPECT_CLIMB_PAST=""
DEADLINE=3600
RPCPORT=18299
P2PPORT=18933
FSPORT=18934
HTTPSPORT=18935
DRY_RUN=0

usage() {
    sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)              DRY_RUN=1 ;;
        --bundle=*)              BUNDLE="${1#--bundle=}" ;;
        --receipt=*)             RECEIPT="${1#--receipt=}" ;;
        --src=*)                 SRC="${1#--src=}" ;;
        --copy-dir=*)            COPY_DIR="${1#--copy-dir=}" ;;
        --expect-climb-past=*)   EXPECT_CLIMB_PAST="${1#--expect-climb-past=}" ;;
        --deadline=*)            DEADLINE="${1#--deadline=}" ;;
        --rpcport=*)             RPCPORT="${1#--rpcport=}" ;;
        --p2p-port=*)            P2PPORT="${1#--p2p-port=}" ;;
        --fs-port=*)             FSPORT="${1#--fs-port=}" ;;
        --https-port=*)          HTTPSPORT="${1#--https-port=}" ;;
        -h|--help)               usage; exit 0 ;;
        *) echo "cure-copy-prove: unknown option $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# ── validation ──────────────────────────────────────────────────────────

[ -n "$BUNDLE" ]  || { echo "cure-copy-prove: --bundle=PATH is required" >&2; exit 2; }
[ -n "$RECEIPT" ] || { echo "cure-copy-prove: --receipt=PATH is required" >&2; exit 2; }
[ -s "$BUNDLE" ]  || { echo "cure-copy-prove: bundle not found or empty: $BUNDLE" >&2; exit 2; }
[ -s "$RECEIPT" ] || { echo "cure-copy-prove: receipt not found or empty: $RECEIPT" >&2; exit 2; }
[ -d "$SRC" ]     || { echo "cure-copy-prove: source datadir not found: $SRC" >&2; exit 2; }

if [ -z "$COPY_DIR" ]; then
    COPY_DIR="$HOME/.zclassic-c23-COPY-$(date +%Y%m%d%H%M%S)-cure"
fi

# Safety invariant: never a live datadir, always the throwaway marker. Same
# rule as app/controllers/src/agent_copy_prove_controller.c cp_path_safety_ok().
case "$COPY_DIR" in
    *"/.zclassic-c23-COPY-"*) : ;;
    *) echo "cure-copy-prove: REFUSED — --copy-dir must contain the '/.zclassic-c23-COPY-' marker: $COPY_DIR" >&2
       exit 2 ;;
esac
for live in "$HOME/.zclassic-c23" "$HOME/.zclassic-c23-dev" "$HOME/.zclassic-c23-soak" \
            "$HOME/.zclassic-c23-mint" "$HOME/.zclassic-c23-mint-receipt" "$HOME/.zclassic"; do
    if [ "$COPY_DIR" = "$live" ]; then
        echo "cure-copy-prove: REFUSED — --copy-dir aliases a known live/protected datadir: $COPY_DIR" >&2
        exit 2
    fi
done
if [ -e "$COPY_DIR" ]; then
    echo "cure-copy-prove: REFUSED — --copy-dir already exists, refusing to overwrite: $COPY_DIR" >&2
    exit 2
fi

if [ "$DRY_RUN" = "0" ]; then
    case "$EXPECT_CLIMB_PAST" in
        ""|*[!0-9]*)
            echo "cure-copy-prove: --expect-climb-past=H (non-negative integer) is required unless --dry-run" >&2
            exit 2 ;;
    esac
    [ -x "$NODE_BIN" ] || { echo "cure-copy-prove: $NODE_BIN not built (run make build-only)" >&2; exit 2; }
    [ -x "$RPC_BIN" ]  || { echo "cure-copy-prove: $RPC_BIN not built (run make zcl-rpc)" >&2; exit 2; }
fi

# ── plan (always printed) ──────────────────────────────────────────────

echo "======================================================================"
echo "  cure-copy-prove plan"
echo "  bundle:            $BUNDLE"
echo "  receipt:           $RECEIPT"
echo "  src (cp -a from):  $SRC"
echo "  copy_dir (cp -a to, must not pre-exist): $COPY_DIR"
echo "  expect_climb_past: ${EXPECT_CLIMB_PAST:-<none, dry-run only>}"
echo "  deadline_secs:     $DEADLINE"
echo "  node_bin:          $NODE_BIN"
echo "  ports:              rpc=$RPCPORT p2p=$P2PPORT fs=$FSPORT https=$HTTPSPORT"
echo "  steps:"
echo "    1. cp -a \"$SRC\" \"$COPY_DIR\""
echo "    2. cp \"$RECEIPT\" \"$COPY_DIR/consensus_state_replay_receipt.v1\""
echo "    3. \"$NODE_BIN\" -datadir=\"$COPY_DIR\" -install-consensus-bundle=\"$BUNDLE\""
echo "       (terminal; require literal 'INSTALLED:' banner + exit 0)"
echo "    4. \"$NODE_BIN\" -datadir=\"$COPY_DIR\" -rpcport=$RPCPORT -port=$P2PPORT"
echo "       -connect=127.0.0.1:39999 -nolegacyimport -nofilesync (isolated boot)"
echo "    5. poll getblockcount + dumpstate reducer_frontier until"
echo "       height > $EXPECT_CLIMB_PAST or deadline; check G-SOV parts 2-3"
echo "    6. print PASS/FAIL with full evidence; kill the copy's node"
echo "======================================================================"

if [ "$DRY_RUN" = "1" ]; then
    echo "[cure-copy-prove] --dry-run: no filesystem or process action taken."
    exit 0
fi

RUN_START_EPOCH=$(date +%s)

# ── step 1: copy ────────────────────────────────────────────────────────

echo "[cure-copy-prove] cp -a $SRC -> $COPY_DIR"
cp -a "$SRC" "$COPY_DIR"
rm -f "$COPY_DIR"/*.pid "$COPY_DIR"/.lock 2>/dev/null || true

# ── step 2: inject the receipt ─────────────────────────────────────────

echo "[cure-copy-prove] installing replay receipt into the copy's own datadir_fd path"
cp "$RECEIPT" "$COPY_DIR/consensus_state_replay_receipt.v1"

cleanup() {
    if [ -n "${NODE_PID:-}" ] && kill -0 "$NODE_PID" 2>/dev/null; then
        kill -TERM "$NODE_PID" 2>/dev/null || true
        sleep 2
        kill -KILL "$NODE_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

ISO_HOME="$COPY_DIR/.isolated-home"
mkdir -p "$ISO_HOME"
[ -e "$HOME/.zcash-params" ] && ln -sfn "$HOME/.zcash-params" "$ISO_HOME/.zcash-params"

NODE_ISO_ARGS="-fsport=$FSPORT -httpsport=$HTTPSPORT -connect=127.0.0.1:39999 -nolegacyimport -nofilesync"

# ── step 3: terminal install call (phase 1) ────────────────────────────

INSTALL_LOG="$COPY_DIR/cure_install_bundle.log"
echo "[cure-copy-prove] phase 1: terminal -install-consensus-bundle=$BUNDLE (timeout ${DEADLINE}s)"
install_rc=0
if command -v timeout >/dev/null 2>&1; then
    HOME="$ISO_HOME" timeout "${DEADLINE}s" \
        "$NODE_BIN" -datadir="$COPY_DIR" -rpcport="$RPCPORT" -port="$P2PPORT" \
        $NODE_ISO_ARGS -install-consensus-bundle="$BUNDLE" \
        > "$INSTALL_LOG" 2>&1 || install_rc=$?
else
    HOME="$ISO_HOME" \
        "$NODE_BIN" -datadir="$COPY_DIR" -rpcport="$RPCPORT" -port="$P2PPORT" \
        $NODE_ISO_ARGS -install-consensus-bundle="$BUNDLE" \
        > "$INSTALL_LOG" 2>&1 || install_rc=$?
fi

if [ "$install_rc" != "0" ] || ! grep -q '^INSTALLED: -install-consensus-bundle:' "$INSTALL_LOG"; then
    echo "======================================================================"
    echo "  cure-copy-prove VERDICT: FAIL"
    echo "  copy:    $COPY_DIR"
    echo "  reason:  terminal -install-consensus-bundle did not report INSTALLED"
    echo "           (exit=$install_rc). Inspect $INSTALL_LOG. Nothing further"
    echo "           was booted."
    echo "======================================================================"
    exit 1
fi
echo "[cure-copy-prove] install reported INSTALLED — booting normally to prove H* CLIMB"

# ── step 4: normal boot (phase 2) ──────────────────────────────────────

HOME="$ISO_HOME" ZCL_MIRROR_SYNC=0 \
"$NODE_BIN" -datadir="$COPY_DIR" -rpcport="$RPCPORT" -port="$P2PPORT" \
    $NODE_ISO_ARGS \
    > "$COPY_DIR/cure_node.log" 2>&1 &
NODE_PID=$!

rpc() { HOME="$ISO_HOME" ZCL_DATADIR="$COPY_DIR" ZCL_RPCPORT="$RPCPORT" "$RPC_BIN" "$@" 2>/dev/null || true; }
tip() {
    resp="$(rpc getblockcount)"
    printf '%s\n' "$resp" |
        sed -n 's/.*"result"[[:space:]]*:[[:space:]]*\(-\{0,1\}[0-9][0-9]*\).*/\1/p' | head -1
}

# ── step 5: poll for H* CLIMB (G-SOV part 1) ───────────────────────────

deadline_epoch=$(( $(date +%s) + DEADLINE ))
max_tip=-1
first_tip=-1
climbed_past=0
cookie="$COPY_DIR/.cookie"
while [ "$(date +%s)" -lt "$deadline_epoch" ]; do
    if ! kill -0 "$NODE_PID" 2>/dev/null; then
        echo "[cure-copy-prove] node exited early (see $COPY_DIR/cure_node.log)"
        break
    fi
    if [ -f "$cookie" ]; then
        t="$(tip)"; t="${t:--1}"
        if [ "$t" -ge 0 ] 2>/dev/null; then
            [ "$first_tip" -lt 0 ] && first_tip="$t" && echo "[cure-copy-prove] first tip: $t"
            [ "$t" -gt "$max_tip" ] && max_tip="$t"
            if [ "$t" -gt "$EXPECT_CLIMB_PAST" ] 2>/dev/null; then
                climbed_past=1
                echo "[cure-copy-prove] H* CLIMBED to $t (> $EXPECT_CLIMB_PAST)"
                break
            fi
        fi
    fi
    sleep 5
done

# ── G-SOV parts 2-3: coins_applied continuity + provenance markers ─────

frontier_json="$(rpc dumpstate '"reducer_frontier"')"
coins_applied="$(printf '%s' "$frontier_json" | sed -n 's/.*"coins_applied_height":\([0-9-]*\).*/\1/p' | head -1)"
hstar_now="$(printf '%s' "$frontier_json" | sed -n 's/.*"hstar":\([0-9-]*\).*/\1/p' | head -1)"
sov_continuity_ok=0
if [ -n "${coins_applied:-}" ] && [ -n "${hstar_now:-}" ] && \
   [ "$coins_applied" = "$((hstar_now + 1))" ] 2>/dev/null; then
    sov_continuity_ok=1
fi

# dbquery is SELECT-only, auto-LIMIT'd; avoid a WHERE ... IN (...) literal to
# sidestep triple-quoting (shell -> JSON param -> SQL string) — just read the
# small progress_meta table and grep for both provenance markers.
provenance_json="$(rpc dbquery '"SELECT key FROM progress_meta"' 50 2>/dev/null || true)"
provenance_rows="$(printf '%s' "$provenance_json" | grep -o 'coins_kv_migration_complete\|coins_kv_self_folded' | sort -u | wc -l | tr -d ' ')"
sov_provenance_ok=0
[ "${provenance_rows:-0}" = "2" ] && sov_provenance_ok=1

DURATION_SECS=$(( $(date +%s) - RUN_START_EPOCH ))

echo "======================================================================"
if [ "$climbed_past" = "1" ] && [ "$sov_continuity_ok" = "1" ] && [ "$sov_provenance_ok" = "1" ]; then
    verdict="PASS"
    rc=0
else
    verdict="FAIL"
    rc=1
fi
echo "  cure-copy-prove VERDICT: $verdict"
echo "  copy:                 $COPY_DIR"
echo "  first_tip:             $first_tip"
echo "  max_tip:                $max_tip"
echo "  expect_climb_past:      $EXPECT_CLIMB_PAST"
echo "  G-SOV.1 climbed_past:   $climbed_past"
echo "  G-SOV.2 coins_applied:  $coins_applied (hstar=$hstar_now, want coins_applied==hstar+1) ok=$sov_continuity_ok"
echo "  G-SOV.3 provenance:     coins_kv_migration_complete + coins_kv_self_folded present: $sov_provenance_ok"
echo "  duration_secs:          $DURATION_SECS"
echo "  install_log:            $INSTALL_LOG"
echo "  node_log:               $COPY_DIR/cure_node.log"
if [ "$verdict" = "FAIL" ]; then
    echo "  NOTE: this is a false-green trap if you only look at climbed_past —"
    echo "        G-SOV requires ALL THREE items. Cross-check hash-agreement"
    echo "        against zclassicd/mirrors by hand (item 4 of the runbook's"
    echo "        acceptance bar; not automated here) before any live step."
fi
echo "======================================================================"
exit "$rc"
