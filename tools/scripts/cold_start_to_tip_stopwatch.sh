#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# cold_start_to_tip_stopwatch.sh — the GENUINE C3 wall-clock proof (MVP.md
# criterion 3): wipe -> boot bare -> reach network tip.
#
# This is deliberately NOT cold_start_to_tip_probe.sh. That probe pre-seeds a
# local operator bundle (block_index.bin + utxo-seed-*.snapshot) into the
# fresh datadir before boot — an assisted seed, not a wiped-empty start. This
# harness boots the target binary against a genuinely EMPTY datadir with NO
# snapshot/bundle/import flags at all: whatever the binary does on its own to
# reach a self-verified authority (the compiled-in checkpoint-ROM authority
# fold, or a full from-genesis fold once that lands — this harness does not
# care which; it only observes the result) is exactly what gets timed. Once a
# native "weld" path (checkpoint authority auto-activated at boot, no flags)
# is integrated, this is the harness that measures it with no changes needed.
#
# It gates on the real MVP claim — H* (the reducer's authoritative,
# provable tip) reaching network_tip (the best height any handshake-complete
# P2P peer advertised) — never on "the sync FSM says at_tip", which the ~7s
# in-process FSM stub (lib/test/src/test_cold_start_sync.c) asserts without
# downloading or validating a single real block. Both fields come straight off
# `dumpstate reducer_frontier` (app/jobs/src/reducer_frontier_dump.c):
# "hstar" and "network_tip"/"network_tip_read_ok".
#
# Binary-path argument: pass the binary to time via --bin=PATH (or the first
# bare positional arg), or ZCL_CS_NODE_BIN. This lets an orchestrator point
# the stopwatch at a freshly-integrated build without editing this file.
#
# FULLY ISOLATED + NON-DESTRUCTIVE:
#   - datadir is ALWAYS a fresh mktemp under /tmp — there is no flag or env
#     var to point it at any other path, so it can never collide with a
#     live datadir,
#   - isolated $HOME (no co-located ~/.zclassic legacy dir the node could
#     auto-import from — the genuinely-fresh-machine condition),
#   - dedicated non-live ports (39170-39173), -listen=0, -nolegacyimport,
#   - dials the peer via -connect as a CLIENT only (read-only P2P — never
#     writes to the peer's datadir, never touches systemd),
#   - process-group SIGKILL teardown on every exit path.
#
# Usage:
#   tools/scripts/cold_start_to_tip_stopwatch.sh [--bin=PATH] [--peer=HOST:PORT]
#       [--budget=SECS] [--sample=SECS]
#   ZCL_CS_NODE_BIN=/path/to/zclassic23 ZCL_CS_PEER=127.0.0.1:8033 \
#       tools/scripts/cold_start_to_tip_stopwatch.sh
#
# Exit codes:
#   0  PASS           — H* reached network_tip within budget. WALL_CLOCK_SECONDS
#                        printed is the real, published wipe-to-tip number.
#   3  SEAM           — H* climbed (real forward progress) but budget expired
#                        before it caught network_tip. Honest code-seam, not a
#                        fixture problem.
#   4  STALLED-NAMED  — no forward progress across the whole window, but at
#                        least one active named blocker explains why (the
#                        acceptable-stall class per docs/TENACITY.md).
#   1  FAIL           — no forward progress AND no named blocker (the silent-
#                        stall failure class), or the node process died, or a
#                        harness/setup error.
#   2  SKIP           — prerequisite absent (binary not built / peer
#                        unreachable). Not a verdict on C3 either way.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

NODE_BIN="${ZCL_CS_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
PEER="${ZCL_CS_PEER:-127.0.0.1:8033}"
BUDGET="${ZCL_CS_BUDGET_SECS:-600}"     # 10-minute MVP C3 target
SAMPLE_SECS="${ZCL_CS_SAMPLE_SECS:-10}"
ARTIFACT_ROOT="${ZCL_CS_ARTIFACT_ROOT:-$REPO_ROOT/build/c3-stopwatch}"

# ── argv: --bin=PATH / --peer=H:P / --budget=N / --sample=N, or bare
#    positionals (bin, peer) for quick manual use. Flags win over env vars;
#    env vars win over the defaults above.
for arg in "$@"; do
    case "$arg" in
        --bin=*)    NODE_BIN="${arg#--bin=}" ;;
        --peer=*)   PEER="${arg#--peer=}" ;;
        --budget=*) BUDGET="${arg#--budget=}" ;;
        --sample=*) SAMPLE_SECS="${arg#--sample=}" ;;
        --*)        echo "cold-start-wipe-stopwatch: unknown flag: $arg" >&2; exit 2 ;;
        *)
            if [ "${_POSN:-0}" = "0" ]; then NODE_BIN="$arg"; _POSN=1;
            elif [ "${_POSN:-0}" = "1" ]; then PEER="$arg"; _POSN=2;
            fi
            ;;
    esac
done

P2P=39170; RPC=39171; FS=39172; HTTPS=39173
RUN_ID="${ZCL_CS_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)-$$}"
ARTIFACT_DIR="$ARTIFACT_ROOT/$RUN_ID"
DATADIR=""
ISO_HOME=""
PID=""
start=0
first_hstar=""
max_hstar="-1"
last_hstar="-1"
last_network_tip="-1"
last_blocker_ids="-"
last_blocker_count="0"

json_escape() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/\t/\\t/g; s/\r/\\r/g' | tr '\n' ' '
}
json_string() { printf '"%s"' "$(json_escape "$1")"; }
json_number_or_null() {
    case "${1:-}" in
        ''|*[!0-9-]*) printf 'null' ;;
        *) printf '%s' "$1" ;;
    esac
}

write_artifact() {
    verdict="$1"; rc="$2"; reason="${3:-}"
    captured_at="$(date +%s)"
    elapsed=0
    [ "${start:-0}" -gt 0 ] && elapsed=$((captured_at - start))
    mkdir -p "$ARTIFACT_DIR" 2>/dev/null || return 0
    {
        printf '{\n'
        printf '  "schema": "zcl.c3_stopwatch_artifact.v1",\n'
        printf '  "verdict": %s,\n' "$(json_string "$verdict")"
        printf '  "exit_code": %s,\n' "$rc"
        printf '  "reason": %s,\n' "$(json_string "$reason")"
        printf '  "wall_clock_seconds": %s,\n' "$(json_number_or_null "$elapsed")"
        printf '  "budget_seconds": %s,\n' "$(json_number_or_null "$BUDGET")"
        printf '  "peer": %s,\n' "$(json_string "$PEER")"
        printf '  "node_bin": %s,\n' "$(json_string "$NODE_BIN")"
        printf '  "first_hstar": %s,\n' "$(json_number_or_null "${first_hstar:-}")"
        printf '  "max_hstar": %s,\n' "$(json_number_or_null "$max_hstar")"
        printf '  "final_hstar": %s,\n' "$(json_number_or_null "$last_hstar")"
        printf '  "final_network_tip": %s,\n' "$(json_number_or_null "$last_network_tip")"
        printf '  "reached_network_tip": %s,\n' "$([ "$verdict" = "pass" ] && printf true || printf false)"
        printf '  "scratch_datadir": %s,\n' "$(json_string "${DATADIR:-}")"
        printf '  "scratch_datadir_removed": true\n'
        printf '}\n'
    } >"$ARTIFACT_DIR/proof.json"
    if [ -n "${DATADIR:-}" ] && [ -f "$DATADIR/node.log" ]; then
        tail -100 "$DATADIR/node.log" >"$ARTIFACT_DIR/node.tail.log" 2>/dev/null || true
    fi
    printf '%s\n' "$ARTIFACT_DIR" >"$ARTIFACT_ROOT/latest.txt" 2>/dev/null || true
    echo "cold-start-wipe-stopwatch: artifact=$ARTIFACT_DIR"
}

skip() { echo "cold-start-wipe-stopwatch: SKIP ($*)"; write_artifact "skip" 2 "$*"; exit 2; }
die()  { echo "cold-start-wipe-stopwatch: FAIL: $*" >&2; write_artifact "fail" 1 "$*"; exit 1; }

[ -x "$NODE_BIN" ] || skip "node binary absent/not executable: $NODE_BIN"

peer_host="${PEER%:*}"
peer_port="${PEER##*:}"
[ -n "$peer_host" ] && [ -n "$peer_port" ] && [ "$peer_host" != "$peer_port" ] \
    || skip "invalid peer address: $PEER"
if ! timeout 3 bash -c "exec 3<>/dev/tcp/$peer_host/$peer_port" 2>/dev/null; then
    skip "serving peer not reachable: $PEER"
fi

DATADIR="$(mktemp -d /tmp/zcl-c3-stopwatch.XXXXXX)" || die "mktemp datadir failed"
ISO_HOME="$DATADIR-home"
mkdir -p "$ISO_HOME" || die "mkdir isolated HOME failed"
# Provision ONLY the proving-params dir into the isolated home (chain state
# stays untouched — this is not a sync shortcut, mainnet boot simply parks
# at the crypto_params_missing gate without it). Same convention as
# fresh-boot-proof.sh: a real fresh machine has params installed once and
# never re-fetches them per node, so this is not assisted seeding.
REAL_PARAMS="${ZCL_CS_PARAMS_DIR:-$HOME/.zcash-params}"
[ -d "$REAL_PARAMS" ] && ln -s "$REAL_PARAMS" "$ISO_HOME/.zcash-params" 2>/dev/null

cleanup() {
    [ -n "$PID" ] && kill -KILL -- "-$PID" 2>/dev/null || true
    case "$DATADIR" in /tmp/zcl-c3-stopwatch.*) rm -rf "$DATADIR" "$ISO_HOME" 2>/dev/null || true ;; esac
}
trap cleanup EXIT INT TERM

echo "cold-start-wipe-stopwatch: bin=$NODE_BIN peer=$PEER budget=${BUDGET}s sample=${SAMPLE_SECS}s"
echo "cold-start-wipe-stopwatch: datadir=$DATADIR (freshly wiped, empty, no bundle/snapshot)"
echo "cold-start-wipe-stopwatch: iso-home=$ISO_HOME (no .zclassic legacy dir — genuinely fresh machine)"

start=$(date +%s)
setsid env HOME="$ISO_HOME" "$NODE_BIN" \
    -datadir="$DATADIR" \
    -port=$P2P \
    -rpcport=$RPC \
    -fsport=$FS \
    -httpsport=$HTTPS \
    -listen=0 \
    -connect="$PEER" \
    -nolegacyimport \
    -nobgvalidation \
    -showmetrics=0 \
    >"$DATADIR/node.log" 2>&1 &
PID=$!
echo "cold-start-wipe-stopwatch: launched pid=$PID"

rpc() { "$NODE_BIN" -rpcport=$RPC -datadir="$DATADIR" "$@" 2>/dev/null; }

# dumpstate reads can transiently miss while the reducer drive holds the
# progress_store lock (a busy read, not a stall) — retry a few times so a
# lock-contention blip never reads as a regression.
rpc_frontier() {
    local out=""
    for _ in 1 2 3 4; do
        out="$(rpc dumpstate reducer_frontier)"
        printf '%s' "$out" | grep -q '"hstar"' && { printf '%s' "$out"; return 0; }
        sleep 1
    done
    printf '%s' "$out"
}

jget() {
    printf '%s' "$1" | grep -oE "\"$2\"[[:space:]]*:[[:space:]]*-?[0-9]+" | head -1 |
        grep -oE -- '-?[0-9]+$'
}

# Some boot-storage gates (config/src/boot.c boot_park_until_shutdown, e.g.
# crypto_params_missing) fire BEFORE the RPC server starts, so `dumpstate
# blocker` never sees them (RPC has nothing to answer with). The node still
# names the gate on stderr into node.log ("[boot] PARKED alive-degraded at
# gate '<name>' ... NOT crash-looping; waiting for a shutdown signal") — that
# is the honest named-stall signal in this window, not a silent hang. Surface
# it the same way the RPC blocker list would.
log_named_park() {
    grep -oE "PARKED alive-degraded at gate '[^']*'" "$DATADIR/node.log" 2>/dev/null |
        tail -1 | sed -E "s/.*gate '([^']*)'.*/\1/"
}

printf '%-8s %-8s %-10s %-8s %s\n' "t(s)" "hstar" "net_tip" "tip_ok" "blockers"
printf '%-8s %-8s %-10s %-8s %s\n' "----" "-----" "-------" "------" "--------"

t=0
reached=0
while :; do
    now=$(date +%s); elapsed=$((now - start))
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "cold-start-wipe-stopwatch: node EXITED early (t=${elapsed}s) — log tail:"
        tail -20 "$DATADIR/node.log" 2>/dev/null | sed 's/^/  /'
        die "node process died before reaching network_tip"
    fi

    fj="$(rpc_frontier)"
    bj="$(rpc dumpstate blocker)"

    hs="$(jget "$fj" hstar)";              [ -z "$hs" ] && hs="-1"
    nt="$(jget "$fj" network_tip)";        [ -z "$nt" ] && nt="-1"
    nt_ok="$(printf '%s' "$fj" | grep -oE '"network_tip_read_ok"[[:space:]]*:[[:space:]]*true')"
    bc="$(jget "$bj" active_count)";       [ -z "$bc" ] && bc="0"
    bids="$(printf '%s' "$bj" | tr -d '\n' |
            grep -oE '"id"[[:space:]]*:[[:space:]]*"[^"]*"' |
            sed -E 's/.*"id"[[:space:]]*:[[:space:]]*"([^"]*)"/\1/' | paste -sd, -)"
    [ -z "$bids" ] && bids="-"
    park_gate="$(log_named_park)"
    if [ -n "$park_gate" ] && [ "$bc" = "0" ]; then
        bc=1; bids="boot_park:$park_gate"
    fi

    printf '%-8s %-8s %-10s %-8s %s\n' "$elapsed" "$hs" "$nt" "${nt_ok:+yes}" "b=$bc:$bids"

    # H* must never regress (a real regression is a correctness bug, not a
    # timing seam) — read-misses (-1) are excluded, they are not regressions.
    if [ "$hs" != "-1" ] && [ "$last_hstar" != "-1" ] && [ "$hs" -lt "$last_hstar" ] 2>/dev/null; then
        die "H* REGRESSED: $last_hstar -> $hs at t=${elapsed}s (this is a correctness bug, not a budget seam)"
    fi
    if [ "$hs" != "-1" ]; then
        [ -z "$first_hstar" ] && first_hstar="$hs"
        last_hstar="$hs"
        [ "$hs" -gt "$max_hstar" ] 2>/dev/null && max_hstar="$hs"
    fi
    [ "$nt" != "-1" ] && last_network_tip="$nt"
    last_blocker_ids="$bids"; last_blocker_count="$bc"

    if [ -n "$nt_ok" ] && [ "$nt" -gt 0 ] 2>/dev/null && [ "$hs" != "-1" ] && [ "$hs" -ge "$nt" ] 2>/dev/null; then
        reached=1
        break
    fi

    [ "$elapsed" -ge "$BUDGET" ] && break
    sleep "$SAMPLE_SECS"
done

now=$(date +%s); elapsed=$((now - start))

if [ "$reached" = 1 ]; then
    echo "WALL_CLOCK_SECONDS=$elapsed"
    echo "=== cold-start-wipe-stopwatch: PASS — H* reached network_tip=$last_network_tip in ${elapsed}s (budget ${BUDGET}s) ==="
    write_artifact "pass" 0 "wiped datadir reached network_tip within budget"
    exit 0
fi

echo "WALL_CLOCK_SECONDS=$elapsed"
climbed=0
if [ -n "$first_hstar" ] && [ "$max_hstar" -gt "$first_hstar" ] 2>/dev/null; then
    climbed=1
fi

if [ "$climbed" = 1 ]; then
    echo "=== cold-start-wipe-stopwatch: SEAM — H* climbed ($first_hstar -> $max_hstar) but did not reach network_tip=$last_network_tip within ${BUDGET}s ==="
    write_artifact "seam" 3 "H* made forward progress but did not catch network_tip within budget"
    exit 3
fi

if [ "${last_blocker_count:-0}" -gt 0 ] 2>/dev/null; then
    echo "=== cold-start-wipe-stopwatch: STALLED-NAMED — no forward progress in ${BUDGET}s; active blocker(s): $last_blocker_ids ==="
    write_artifact "stalled-named" 4 "no forward progress; named blocker(s): $last_blocker_ids"
    exit 4
fi

echo "cold-start-wipe-stopwatch: last 20 log lines:"
tail -20 "$DATADIR/node.log" 2>/dev/null | sed 's/^/  /'
die "no forward progress AND no named blocker in ${BUDGET}s (silent-stall failure class)"
