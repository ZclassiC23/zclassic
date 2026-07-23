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
#       [--file-peer=HOST:PORT] [--budget=SECS] [--sample=SECS]
#   ZCL_CS_NODE_BIN=/path/to/zclassic23 ZCL_CS_PEER=127.0.0.1:8033 \
#       ZCL_CS_FILE_PEER=127.0.0.1:18034 \
#       ZCL_CS_HEADER_SOURCE=/path/to/zclassicd-datadir-copy \
#       ZCL_CS_BUNDLE_PATH=/path/to/consensus-state-bundle.sqlite \
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
#   5  FRONTIER-BUSY-TIMEOUT — `dumpstate reducer_frontier` kept returning a
#                        partial `{"snapshot_status":"progress_store_busy",
#                        "retryable":true}` doc (no "hstar" field) for the
#                        entire busy-timeout window (--busy-timeout=SECS /
#                        ZCL_CS_FRONTIER_BUSY_TIMEOUT_SECS, default 120s) —
#                        this harness never observed a real frontier sample.
#                        Distinct from FAIL: this is an instrument failure
#                        ("we could not read the node's state"), not a claim
#                        about the node's actual progress.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

NODE_BIN="${ZCL_CS_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
PEER="${ZCL_CS_PEER:-127.0.0.1:8033}"
FILE_PEER="${ZCL_CS_FILE_PEER:-}"
HEADER_SOURCE="${ZCL_CS_HEADER_SOURCE:-}"
BUNDLE_PATH="${ZCL_CS_BUNDLE_PATH:-}"
BUDGET="${ZCL_CS_BUDGET_SECS:-600}"     # 10-minute MVP C3 target
SAMPLE_SECS="${ZCL_CS_SAMPLE_SECS:-10}"
ARTIFACT_ROOT="${ZCL_CS_ARTIFACT_ROOT:-$REPO_ROOT/build/c3-stopwatch}"
# Bounded window a persistently-busy progress_store may occupy before this
# harness gives up observing and reports FRONTIER-BUSY-TIMEOUT instead of
# silently folding busy reads into "no forward progress" (see D6 / the
# is_busy_response()/rpc_frontier() comment below).
FRONTIER_BUSY_TIMEOUT_SECS="${ZCL_CS_FRONTIER_BUSY_TIMEOUT_SECS:-120}"
# Bounded number of supervised self-respawns this harness will FOLLOW before
# calling it a runaway (see the respawn-seam handling in the main loop). A
# clean self-exit carrying a self_respawn_* exit-reason breadcrumb is the node
# asking its supervisor (systemd Restart=always in production; THIS harness in
# the drill) to relaunch it on the SAME datadir — e.g. to consume an
# install-on-next-boot request. The node's own progress.kv restart budget
# bounds this too; the harness cap is the belt-and-suspenders runaway stop.
MAX_BOOTS="${ZCL_CS_MAX_BOOTS:-12}"

# ── argv: --bin=PATH / --peer=H:P / --file-peer=H:P / --budget=N / --sample=N /
#    --busy-timeout=N / --selftest, or bare positionals (bin, peer) for quick
#    manual use. Flags win over env vars; env vars win over the defaults
#    above. --selftest runs the hermetic busy-JSON classification self-check
#    below (is_busy_response()) and exits — no binary, network, or mktemp
#    datadir touched.
SELFTEST=0
for arg in "$@"; do
    case "$arg" in
        --bin=*)    NODE_BIN="${arg#--bin=}" ;;
        --peer=*)   PEER="${arg#--peer=}" ;;
        --file-peer=*) FILE_PEER="${arg#--file-peer=}" ;;
        --budget=*) BUDGET="${arg#--budget=}" ;;
        --sample=*) SAMPLE_SECS="${arg#--sample=}" ;;
        --busy-timeout=*) FRONTIER_BUSY_TIMEOUT_SECS="${arg#--busy-timeout=}" ;;
        --selftest) SELFTEST=1 ;;
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
busy_streak_start=0
boots=1
last_respawn_reason=""

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

# is_busy_response — true iff a `dumpstate reducer_frontier` body is the
# PARTIAL progress_store-busy doc — {"snapshot_status":"progress_store_busy",
# "retryable":true} — rather than a genuine empty/absent response. A naive
# `grep -q '"hstar"'` miss cannot tell "the store is busy, retry" from "the
# node isn't answering at all" apart; this lets both rpc_frontier() and the
# failure-bundle capture below tell the two apart and label busy honestly
# instead of reading it as hstar=-1 or a silent empty.
is_busy_response() {
    printf '%s' "$1" | grep -qE '"retryable"[[:space:]]*:[[:space:]]*true'
}

# is_self_respawn_reason — true iff the given boot-exit-reason.v1 `reason` value
# is a supervised self-respawn request (self_respawn_tip_watchdog /
# self_respawn_supervisor_backstop / self_respawn_both — see
# lib/util/include/util/shutdown_stagewatch.h). The node writes this breadcrumb
# EARLY in its clean shutdown (fsync + atomic rename, before any teardown
# stage) when the chain-tip watchdog, the supervisor backstop, or the
# checkpoint-bundle install-ready condition asked to be relaunched. A clean
# exit carrying it means "bring me back on the SAME datadir" — exactly what
# systemd Restart=always does in production; here THIS harness is the
# supervisor. Anything else (operator_or_external, empty, or no breadcrumb at
# all after a crash) is NOT a respawn request and is a real death.
is_self_respawn_reason() {
    case "${1:-}" in
        self_respawn_*) return 0 ;;
        *)              return 1 ;;
    esac
}

# read_exit_reason — extract the `reason=` value from the node's
# <datadir>/boot-exit-reason.v1 breadcrumb, or print nothing if absent. See the
# writer shutdown_stagewatch_write_exit_reason() (magic=ZCLEXITRSN, version=1,
# reason=<name>, ts=<unix>).
read_exit_reason() {
    local f="$DATADIR/boot-exit-reason.v1"
    [ -f "$f" ] || return 0
    sed -n 's/^reason=\(.*\)$/\1/p' "$f" 2>/dev/null | tail -1
}

# --selftest: hermetic classification self-check for is_busy_response() /
# the "hstar" field detector rpc_frontier() uses — canned JSON fixtures,
# no binary/network/mktemp touched. Exits before any real infra setup.
if [ "$SELFTEST" = "1" ]; then
    st_fail=0
    st_check() {  # desc, expect_rc, actual_rc
        if [ "$3" = "$2" ]; then
            echo "  ok: $1"
        else
            echo "  FAIL: $1 (expected rc=$2 got rc=$3)"
            st_fail=1
        fi
    }
    st_busy_json='{"snapshot_status":"progress_store_busy","retryable":true}'
    st_good_json='{"hstar":123,"network_tip":456,"network_tip_read_ok":true}'
    st_other_json='{"error":"method not found"}'

    echo "cold-start-wipe-stopwatch: --selftest running canned-JSON checks"
    is_busy_response "$st_busy_json";  st_check "busy fixture IS recognized as busy" 0 $?
    is_busy_response "$st_good_json";  st_check "good hstar fixture NOT recognized as busy" 1 $?
    is_busy_response "$st_other_json"; st_check "unrelated-error fixture NOT recognized as busy" 1 $?
    is_busy_response "";               st_check "empty response NOT recognized as busy" 1 $?
    printf '%s' "$st_good_json" | grep -q '"hstar"'; st_check "good fixture has hstar field" 0 $?
    printf '%s' "$st_busy_json" | grep -q '"hstar"'; st_check "busy fixture has NO hstar field (would retry, not misread as -1)" 1 $?

    # Exit-reason classification: a self_respawn_* breadcrumb means "relaunch
    # me" (the harness follows it); everything else is a real death.
    is_self_respawn_reason "self_respawn_tip_watchdog";        st_check "tip-watchdog respawn IS a respawn request" 0 $?
    is_self_respawn_reason "self_respawn_supervisor_backstop"; st_check "backstop respawn IS a respawn request" 0 $?
    is_self_respawn_reason "self_respawn_both";                st_check "both-respawn IS a respawn request" 0 $?
    is_self_respawn_reason "operator_or_external";             st_check "operator/external exit is NOT a respawn request" 1 $?
    is_self_respawn_reason "";                                 st_check "empty/absent breadcrumb is NOT a respawn request (crash class)" 1 $?
    is_self_respawn_reason "self_respawn";                     st_check "bare 'self_respawn' (no suffix) is NOT a known respawn reason" 1 $?

    if [ "$st_fail" = 0 ]; then
        echo "cold-start-wipe-stopwatch: --selftest PASS"
        exit 0
    fi
    echo "cold-start-wipe-stopwatch: --selftest FAIL" >&2
    exit 1
fi

# capture_failure_bundle — on any non-pass verdict, snapshot the live
# diagnostic state a human/agent needs to root-cause the run WITHOUT
# re-running the harness: frontier.json (dumpstate reducer_frontier),
# reducer_drive.json (the synchronous drain/lock owner and its last exit),
# reducer_stage_profile.json (per-stage RPF_* sub-phase timing: disk read,
# event encode/append, created-index, stage-log cursor — the fold-cost split),
# stage-*.json (each reducer stage's cursor/counters/last blocker),
# blocker.json (dumpstate blocker), ops.log.tail.txt (the typed `ops logs`
# command if the node is still alive/RPC-reachable, else a plain tail of
# node.log). Sets BUNDLE_CAPTURE_FAILED=true if ANY piece could not be
# captured — a dropped bundle piece is RECORDED, never silently missing.
# Sets FRONTIER_BUSY_AT_CAPTURE=true when frontier.json WAS captured but its
# content is a progress_store-busy partial doc — the file is still written
# (never dropped just because it's busy), only LABELED, per D6. Safe to call
# before NODE_BIN/DATADIR/PID are ever set (an early binary-absent/peer-
# unreachable skip has nothing to capture from).
capture_failure_bundle() {
    BUNDLE_CAPTURE_FAILED="false"
    FRONTIER_BUSY_AT_CAPTURE="false"
    local got_frontier=0 got_drive=0 got_profile=0 got_stages=0 got_blocker=0 got_logs=0
    if [ -n "${PID:-}" ] && kill -0 "$PID" 2>/dev/null && [ -x "${NODE_BIN:-}" ] && [ -n "${DATADIR:-}" ]; then
        "$NODE_BIN" -rpcport="$RPC" -datadir="$DATADIR" dumpstate reducer_frontier \
            >"$ARTIFACT_DIR/frontier.json" 2>/dev/null && [ -s "$ARTIFACT_DIR/frontier.json" ] && got_frontier=1
        if [ "$got_frontier" = 1 ] && is_busy_response "$(cat "$ARTIFACT_DIR/frontier.json" 2>/dev/null)"; then
            FRONTIER_BUSY_AT_CAPTURE="true"
        fi
        "$NODE_BIN" -rpcport="$RPC" -datadir="$DATADIR" dumpstate reducer_drive \
            >"$ARTIFACT_DIR/reducer_drive.json" 2>/dev/null && [ -s "$ARTIFACT_DIR/reducer_drive.json" ] && got_drive=1
        "$NODE_BIN" -rpcport="$RPC" -datadir="$DATADIR" dumpstate reducer_stage_profile \
            >"$ARTIFACT_DIR/reducer_stage_profile.json" 2>/dev/null && [ -s "$ARTIFACT_DIR/reducer_stage_profile.json" ] && got_profile=1
        got_stages=1
        for stage_name in header_admit validate_headers body_fetch body_persist \
                          script_validate proof_validate utxo_apply tip_finalize; do
            "$NODE_BIN" -rpcport="$RPC" -datadir="$DATADIR" dumpstate "$stage_name" \
                >"$ARTIFACT_DIR/stage-$stage_name.json" 2>/dev/null &&
                [ -s "$ARTIFACT_DIR/stage-$stage_name.json" ] || got_stages=0
        done
        "$NODE_BIN" -rpcport="$RPC" -datadir="$DATADIR" dumpstate blocker \
            >"$ARTIFACT_DIR/blocker.json" 2>/dev/null && [ -s "$ARTIFACT_DIR/blocker.json" ] && got_blocker=1
        "$NODE_BIN" -rpcport="$RPC" -datadir="$DATADIR" ops logs \
            --pattern='.' --since_secs=3600 --max_lines=500 --level=all \
            >"$ARTIFACT_DIR/ops.log.tail.txt" 2>/dev/null && [ -s "$ARTIFACT_DIR/ops.log.tail.txt" ] && got_logs=1
    fi
    if [ "$got_logs" = 0 ] && [ -n "${DATADIR:-}" ] && [ -f "$DATADIR/node.log" ]; then
        tail -200 "$DATADIR/node.log" >"$ARTIFACT_DIR/ops.log.tail.txt" 2>/dev/null && got_logs=1
    fi
    [ "$got_frontier" = 1 ] && [ "$got_drive" = 1 ] && [ "$got_profile" = 1 ] &&
        [ "$got_stages" = 1 ] && [ "$got_blocker" = 1 ] && [ "$got_logs" = 1 ] ||
        BUNDLE_CAPTURE_FAILED="true"
}

write_artifact() {
    verdict="$1"; rc="$2"; reason="${3:-}"
    captured_at="$(date +%s)"
    elapsed=0
    [ "${start:-0}" -gt 0 ] && elapsed=$((captured_at - start))
    mkdir -p "$ARTIFACT_DIR" 2>/dev/null || return 0
    BUNDLE_CAPTURE_FAILED="false"
    FRONTIER_BUSY_AT_CAPTURE="false"
    [ "$verdict" != "pass" ] && capture_failure_bundle
    NODE_LOG_CAPTURED="false"
    if [ -n "${DATADIR:-}" ] && [ -f "$DATADIR/node.log" ] &&
       cp -p -- "$DATADIR/node.log" "$ARTIFACT_DIR/node.log" 2>/dev/null; then
        NODE_LOG_CAPTURED="true"
    fi
    {
        printf '{\n'
        printf '  "schema": "zcl.c3_stopwatch_artifact.v1",\n'
        printf '  "verdict": %s,\n' "$(json_string "$verdict")"
        printf '  "exit_code": %s,\n' "$rc"
        printf '  "reason": %s,\n' "$(json_string "$reason")"
        printf '  "wall_clock_seconds": %s,\n' "$(json_number_or_null "$elapsed")"
        printf '  "budget_seconds": %s,\n' "$(json_number_or_null "$BUDGET")"
        printf '  "boots": %s,\n' "$(json_number_or_null "$boots")"
        printf '  "last_respawn_reason": %s,\n' "$(json_string "$last_respawn_reason")"
        printf '  "peer": %s,\n' "$(json_string "$PEER")"
        printf '  "file_peer": %s,\n' "$(json_string "$FILE_PEER")"
        printf '  "header_source": %s,\n' "$(json_string "$HEADER_SOURCE")"
        printf '  "staged_bundle": %s,\n' "$(json_string "$BUNDLE_PATH")"
        printf '  "node_bin": %s,\n' "$(json_string "$NODE_BIN")"
        printf '  "first_hstar": %s,\n' "$(json_number_or_null "${first_hstar:-}")"
        printf '  "max_hstar": %s,\n' "$(json_number_or_null "$max_hstar")"
        printf '  "final_hstar": %s,\n' "$(json_number_or_null "$last_hstar")"
        printf '  "final_network_tip": %s,\n' "$(json_number_or_null "$last_network_tip")"
        printf '  "reached_network_tip": %s,\n' "$([ "$verdict" = "pass" ] && printf true || printf false)"
        printf '  "scratch_datadir": %s,\n' "$(json_string "${DATADIR:-}")"
        printf '  "scratch_datadir_removed": true,\n'
        printf '  "node_log_captured": %s,\n' "$NODE_LOG_CAPTURED"
        printf '  "bundle_capture_failed": %s,\n' "$BUNDLE_CAPTURE_FAILED"
        printf '  "frontier_busy_at_capture": %s\n' "$FRONTIER_BUSY_AT_CAPTURE"
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
if [ -n "$FILE_PEER" ]; then
    file_peer_host="${FILE_PEER%:*}"
    file_peer_port="${FILE_PEER##*:}"
    [ -n "$file_peer_host" ] && [ -n "$file_peer_port" ] && \
        [ "$file_peer_host" != "$file_peer_port" ] \
        || skip "invalid file-service peer address: $FILE_PEER"
    if ! timeout 3 bash -c \
        "exec 3<>/dev/tcp/$file_peer_host/$file_peer_port" 2>/dev/null; then
        skip "file-service peer not reachable: $FILE_PEER"
    fi
fi
[ -z "$HEADER_SOURCE" ] || [ -d "$HEADER_SOURCE" ] \
    || skip "header-source copy absent: $HEADER_SOURCE"
[ -z "$BUNDLE_PATH" ] || [ -f "$BUNDLE_PATH" ] \
    || skip "bundle fixture absent: $BUNDLE_PATH"

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
echo "cold-start-wipe-stopwatch: file_peer=${FILE_PEER:-<none>}"
echo "cold-start-wipe-stopwatch: header_source=${HEADER_SOURCE:-<autonomous>} staged_bundle=${BUNDLE_PATH:-<autonomous>}"
echo "cold-start-wipe-stopwatch: datadir=$DATADIR (freshly wiped)"
echo "cold-start-wipe-stopwatch: iso-home=$ISO_HOME (no .zclassic legacy dir — genuinely fresh machine)"

start=$(date +%s)
if [ -n "$BUNDLE_PATH" ]; then
    mkdir -p "$DATADIR/bundles" || die "mkdir bundle staging dir failed"
    bundle_name="$(basename -- "$BUNDLE_PATH")"
    cp --reflink=auto -p -- "$BUNDLE_PATH" \
        "$DATADIR/bundles/$bundle_name" \
        || die "staging checkpoint bundle failed"
    echo "cold-start-wipe-stopwatch: staged bundle=$DATADIR/bundles/$bundle_name"
fi
if [ -n "$HEADER_SOURCE" ]; then
    echo "cold-start-wipe-stopwatch: importing frozen-validated headers from datadir COPY"
    if ! env HOME="$ISO_HOME" "$NODE_BIN" --importblockindex \
        "$HEADER_SOURCE" "$DATADIR/node.db" >>"$DATADIR/node.log" 2>&1; then
        die "header import from datadir COPY failed"
    fi
    echo "cold-start-wipe-stopwatch: header import complete"
fi
node_args=(
    -datadir="$DATADIR" \
    -port=$P2P \
    -rpcport=$RPC \
    -fsport=$FS \
    -httpsport=$HTTPS \
    -listen=0 \
    -connect="$PEER" \
    -nolegacyimport \
    -nobgvalidation \
    -showmetrics=0
)
[ -n "$FILE_PEER" ] && node_args+=( -fileservice="$FILE_PEER" )

# launch_node — (re)start the node against the SAME fresh datadir with the SAME
# args, appending to the SAME node.log, and set the watched PID. Called once for
# the initial boot and once per followed self-respawn (boot 2..N): a supervised
# respawn is defined as "relaunch me on the same datadir", so staging (bundle /
# header import / install-on-next-boot request) is NEVER re-done here — it
# persists in the datadir for the next boot to consume. Mirrors what systemd
# Restart=always does to the live unit.
launch_node() {
    setsid env HOME="$ISO_HOME" "$NODE_BIN" \
        "${node_args[@]}" \
        >>"$DATADIR/node.log" 2>&1 &
    PID=$!
}

launch_node
echo "cold-start-wipe-stopwatch: launched pid=$PID (boot $boots)"

rpc() { "$NODE_BIN" -rpcport=$RPC -datadir="$DATADIR" "$@" 2>/dev/null; }

# dumpstate reads can transiently miss while the reducer drive holds the
# progress_store lock — `dumpstate reducer_frontier` then returns a PARTIAL
# doc, {"snapshot_status":"progress_store_busy","retryable":true}, with NO
# "hstar" field (see is_busy_response() above). Retry with bounded,
# growing backoff (1,1,2,3,5s — ~12s worst case per call) so a lock-
# contention blip never reads as a regression or an empty sample. Sets the
# global FRONTIER_LAST_BUSY=1 when every retry within this call still came
# back busy (0 otherwise) — the caller tracks a busy STREAK across sample
# ticks so a progress_store that never clears gets its own named verdict
# (see FRONTIER_BUSY_TIMEOUT_SECS in the main loop) instead of silently
# degrading into "no forward progress, no blocker" (silent-stall FAIL) —
# those are different claims: "we could not observe" is not "we observed
# nothing happening".
FRONTIER_LAST_BUSY=0
rpc_frontier() {
    local out="" backoff
    for backoff in 1 1 2 3 5 0; do
        out="$(rpc dumpstate reducer_frontier)"
        if printf '%s' "$out" | grep -q '"hstar"'; then
            FRONTIER_LAST_BUSY=0
            printf '%s' "$out"
            return 0
        fi
        [ "$backoff" = 0 ] && break
        sleep "$backoff"
    done
    if is_busy_response "$out"; then
        FRONTIER_LAST_BUSY=1
    else
        FRONTIER_LAST_BUSY=0
    fi
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
        # The watched PID is gone. Distinguish a SUPERVISED SELF-RESPAWN (a
        # clean exit that wrote a self_respawn_* exit-reason breadcrumb — the
        # node asking to be relaunched on the same datadir, e.g. to consume an
        # install-on-next-boot request) from a REAL death (crash / no
        # breadcrumb / an unexpected operator_or_external exit nobody asked
        # for). Reading the breadcrumb — not the exit code — is authoritative:
        # it is written+fsync'd EARLY in the node's clean shutdown, so it is
        # already durable by the time the process actually exits ~seconds
        # later. (If the node's own in-process execv re-exec HELD, the PID
        # would never have died and we would not be here — this follows the
        # early-_exit path that bypasses that re-exec off-systemd.)
        wait "$PID" 2>/dev/null; node_ec=$?
        reason="$(read_exit_reason)"
        if is_self_respawn_reason "$reason"; then
            last_respawn_reason="$reason"
            if [ "$boots" -ge "$MAX_BOOTS" ] 2>/dev/null; then
                echo "cold-start-wipe-stopwatch: node EXITED early (t=${elapsed}s) — log tail:"
                tail -20 "$DATADIR/node.log" 2>/dev/null | sed 's/^/  /'
                die "self-respawn budget exhausted: followed $boots boots (>= max $MAX_BOOTS), last reason=$reason — the node keeps asking to respawn without reaching tip (runaway)"
            fi
            # Consume the breadcrumb so a subsequent crash (which writes NO new
            # breadcrumb) can never be mis-read as another respawn request via
            # this now-stale one. Then relaunch on the SAME datadir, keeping the
            # wall clock (start) running across boots.
            rm -f "$DATADIR/boot-exit-reason.v1" 2>/dev/null || true
            boots=$((boots + 1))
            echo "cold-start-wipe-stopwatch: FOLLOWED self-respawn (reason=$reason ec=$node_ec) — relaunching boot $boots on same datadir (t=${elapsed}s, wall clock continues)"
            launch_node
            echo "cold-start-wipe-stopwatch: launched pid=$PID (boot $boots)"
            sleep "$SAMPLE_SECS"
            continue
        fi
        echo "cold-start-wipe-stopwatch: node EXITED early (t=${elapsed}s, ec=$node_ec, boots=$boots) — log tail:"
        tail -20 "$DATADIR/node.log" 2>/dev/null | sed 's/^/  /'
        die "node process died before reaching network_tip (exit_reason=${reason:-<none: crash or no breadcrumb>} ec=$node_ec boots=$boots)"
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

    # Bounded busy-streak check: a progress_store that stays busy
    # (retryable:true, no "hstar") across the ENTIRE FRONTIER_BUSY_TIMEOUT_SECS
    # window means this harness never observed a real frontier sample in
    # that time — an instrument failure, not evidence the node stalled.
    # Reported as its own named verdict, never folded into hstar=-1 / "no
    # forward progress, no blocker" (see D6).
    if [ "$hs" = "-1" ] && [ "$FRONTIER_LAST_BUSY" = "1" ]; then
        [ "$busy_streak_start" = 0 ] && busy_streak_start="$now"
        busy_elapsed=$((now - busy_streak_start))
        if [ "$busy_elapsed" -ge "$FRONTIER_BUSY_TIMEOUT_SECS" ]; then
            echo "=== cold-start-wipe-stopwatch: FRONTIER-BUSY-TIMEOUT — progress_store_busy persisted ${busy_elapsed}s (>= ${FRONTIER_BUSY_TIMEOUT_SECS}s) with no hstar sample ever observed in that window ==="
            write_artifact "frontier_busy_timeout" 5 \
                "progress_store_busy persisted >= ${FRONTIER_BUSY_TIMEOUT_SECS}s (--busy-timeout); last raw frontier response: $fj"
            exit 5
        fi
    else
        busy_streak_start=0
    fi

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

# BOOTS=<n> is the total number of node launches this run spanned (1 = no
# respawn; >1 = the harness FOLLOWED that many supervised self-respawns across
# the single wiped datadir, total wall clock counted across all of them). The
# recorder folds it into the durable ledger line.
echo "BOOTS=$boots"

if [ "$reached" = 1 ]; then
    echo "WALL_CLOCK_SECONDS=$elapsed"
    echo "=== cold-start-wipe-stopwatch: PASS — H* reached network_tip=$last_network_tip in ${elapsed}s across $boots boot(s) (budget ${BUDGET}s) ==="
    write_artifact "pass" 0 "wiped datadir reached network_tip within budget across $boots boot(s)"
    exit 0
fi

echo "WALL_CLOCK_SECONDS=$elapsed"
climbed=0
if [ -n "$first_hstar" ] && [ "$max_hstar" -gt "$first_hstar" ] 2>/dev/null; then
    climbed=1
fi

if [ "$climbed" = 1 ]; then
    echo "=== cold-start-wipe-stopwatch: SEAM — H* climbed ($first_hstar -> $max_hstar) across $boots boot(s) but did not reach network_tip=$last_network_tip within ${BUDGET}s ==="
    write_artifact "seam" 3 "H* made forward progress across $boots boot(s) but did not catch network_tip within budget"
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
