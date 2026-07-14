#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# node_slo_probe.sh — the EXTERNAL uptime prober (lane E3, Instant-Sync/
# Strength program). This is the scoreboard for "staying synced" that does
# NOT trust node self-reports: it is a separate client process that dials
# each local instance's RPC port the same way any outside caller would, and
# records what it actually got back — including nothing at all.
#
# Probes THREE local instances by CLIENT-VIEWPOINT RPC (never in-process
# introspection):
#   canonical  rpcport 18232  datadir ~/.zclassic-c23       (deploy/zclassic23.service)
#   soak       rpcport 18242  datadir ~/.zclassic-c23-soak  (deploy/examples/zclassic23-soak-node.service)
#   dev        rpcport 18252  datadir ~/.zclassic-c23-dev   (deploy/zcl23-dev.service)
# Ports/datadirs are hardcoded defaults (each also overridable via env, see
# below) rather than parsed from the unit files: the three ports are a
# stable, documented contract (see CLAUDE.md "Running" + the unit files
# themselves), and a probe that could silently start reading a DIFFERENT
# port because a unit file comment changed is a worse failure mode than one
# that is explicit and greppable here.
#
# Also reads the legacy zclassicd ORACLE (rpcport 8232, datadir ~/.zclassic)
# as an external freshness reference — same oracle soak_evidence.sh uses.
#
# Query mechanism: zcl-rpc getblockchaininfo (the same lightweight raw-RPC
# CLI soak_evidence.sh uses — NOT the native `zclassic23 status` command,
# which returns the full ~15 KB diagnostic envelope and can take seconds to
# assemble on a loaded/wedged node; getblockchaininfo answers in single-digit
# milliseconds and carries both "blocks" (served height) and "headers"
# (validated header tip) in one call).
#
# Appends ONE JSON line per probe PER INSTANCE (3 lines per collect run) to
# ~/.local/state/zclassic23-slo/uptime-ledger.jsonl:
#   ts               epoch the sample was taken
#   instance         "canonical" | "soak" | "dev"
#   rpcport          the port probed
#   datadir          the datadir probed (client-side identity, not proof)
#   reachable        true iff the RPC answered with a parseable height
#   served_height    this instance's getblockchaininfo "blocks", or null
#   header_height    this instance's getblockchaininfo "headers"
#                    (validated header/target tip), or null
#   latency_ms       wall-clock round trip for the getblockchaininfo call,
#                    measured by THIS prober, or null when the call never
#                    returned (timeout) — measured even on failure so a
#                    slow-then-refused probe is distinguishable from an
#                    instant refusal
#   oracle_height    zclassicd getblockcount this cycle, or null
#   max_height       max(served_height) over all instances + oracle THIS
#                    cycle, or null if nothing answered
#   gap_vs_max       max_height - served_height, or null
#   gap_vs_oracle    oracle_height - served_height, or null (either side
#                    unreachable => null, never a fabricated 0)
#   error_detail     truncated raw RPC error/timeout text when unreachable,
#                    "" otherwise
#
# An unreachable instance is NOT a probe failure — it IS the data point
# (same doctrine as soak_evidence.sh: a hole in the evidence is itself
# evidence). This script never exits non-zero because a NODE didn't answer;
# it exits non-zero only if it could not LOCK or APPEND to its own ledger.
#
# Bounded ledger: rotates at 50 MB, keeping 2 rotated generations
# (uptime-ledger.jsonl.1, uptime-ledger.jsonl.2) plus the live file.
#
# Usage:
#   node_slo_probe.sh [collect]     # default action: one probe-and-append cycle
#   node_slo_probe.sh --selftest    # hermetic; fixture RPC commands, no nodes
#
# Env (test/operator injection seams):
#   ZCL_SLO_LEDGER_DIR      ledger dir (default ~/.local/state/zclassic23-slo)
#   ZCL_SLO_RPC_TIMEOUT_SEC per-instance RPC timeout (default 8)
#   ZCL_SLO_ROTATE_BYTES    rotation threshold (default 52428800 = 50 MiB)
#   ZCL_SLO_CANON_CMD / ZCL_SLO_SOAK_CMD / ZCL_SLO_DEV_CMD / ZCL_SLO_ORACLE_CMD
#                           override the exact command run per instance
#                           (selftest injection seam — same pattern as
#                           soak_evidence.sh's ZCL_SOAK_RPC_CMD)
#
# No python (banned), no jq (installed but unused by repo convention) —
# bash + sed + flock only, same rule as soak_evidence.sh / replay_canary.sh.

set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELF="$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}")"

LEDGER_DIR="${ZCL_SLO_LEDGER_DIR:-${HOME:-/root}/.local/state/zclassic23-slo}"
LEDGER_FILE="$LEDGER_DIR/uptime-ledger.jsonl"
RPC_TIMEOUT_SEC="${ZCL_SLO_RPC_TIMEOUT_SEC:-8}"
ROTATE_BYTES="${ZCL_SLO_ROTATE_BYTES:-52428800}"   # 50 MiB

# ── instance table ────────────────────────────────────────────────────
# name:rpcport:datadir:command-override-env-var
CANON_DATADIR="${HOME:-/root}/.zclassic-c23"
CANON_RPCPORT=18232
SOAK_DATADIR="${HOME:-/root}/.zclassic-c23-soak"
SOAK_RPCPORT=18242
DEV_DATADIR="${HOME:-/root}/.zclassic-c23-dev"
DEV_RPCPORT=18252
ORACLE_DATADIR="${HOME:-/root}/.zclassic"
ORACLE_RPCPORT=8232

# systemd user services run with a minimal PATH that does not include
# ~/bin, so a bare `zcl-rpc` in the default probe commands would silently
# fail every probe under the installed timer even though it works fine
# from an interactive shell. Resolve an explicit path once: the operator's
# ~/bin/zcl-rpc symlink first (matches every interactive invocation on this
# box), then this checkout's own build output, then whatever PATH provides.
resolve_zcl_rpc_bin() {
    if [ -n "${ZCL_SLO_RPC_BIN:-}" ]; then printf '%s' "$ZCL_SLO_RPC_BIN"; return 0; fi
    local candidates=(
        "${HOME:-/root}/bin/zcl-rpc"
        "$SCRIPT_DIR/../../build/bin/zcl-rpc"
    )
    local c
    for c in "${candidates[@]}"; do
        [ -x "$c" ] && { printf '%s' "$c"; return 0; }
    done
    command -v zcl-rpc 2>/dev/null || printf 'zcl-rpc'
}
ZCL_RPC_BIN="$(resolve_zcl_rpc_bin)"

# ── helpers ────────────────────────────────────────────────────────────

# jnum <value>: print the value, or JSON null when empty.
jnum() { if [ -n "${1:-}" ]; then printf '%s' "$1"; else printf 'null'; fi; }

# jstr <value>: print a JSON string literal (escaped), "" on empty.
json_escape() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
jstr() { printf '"%s"' "$(json_escape "${1:-}")"; }

# rpc_probe <default-cmd> <override-var-name>: run the (possibly overridden)
# command, print "<served>\x1f<header>\x1f<latency_ms>\x1f<raw-tail>". Never
# raises — a failing/timing-out command still yields a line with empty
# served/header, matching the soak_evidence.sh "|| true" doctrine so set -e
# cannot turn an unreachable node into a script abort.
rpc_probe() {
    local default_cmd="$1" override_var="$2" cmd
    cmd="${!override_var:-$default_cmd}"
    local t0 t1 out served header latency_ms
    t0="$(date +%s%N)"
    out="$(bash -c "$cmd" 2>&1 || true)"
    t1="$(date +%s%N)"
    latency_ms=$(( (t1 - t0) / 1000000 ))
    served="$(printf '%s' "$out" | sed -n 's/.*"blocks":\([0-9][0-9]*\).*/\1/p' | head -n1)"
    header="$(printf '%s' "$out" | sed -n 's/.*"headers":\([0-9][0-9]*\).*/\1/p' | head -n1)"
    local raw_tail=""
    if [ -z "$served" ]; then
        raw_tail="$(printf '%s' "$out" | tr '\n' ' ' | cut -c1-200)"
        [ -n "$raw_tail" ] || raw_tail="empty_response"
    fi
    printf '%s\x1f%s\x1f%s\x1f%s' "$served" "$header" "$latency_ms" "$raw_tail"
}

field() { printf '%s' "$1" | cut -d $'\x1f' -f"$2"; }

# rotate_ledger_if_needed: logrotate-style, 2 kept generations, run BEFORE
# this cycle's lines are appended so a rotation never splits one run's
# 3 lines across two files.
rotate_ledger_if_needed() {
    [ -f "$LEDGER_FILE" ] || return 0
    local size
    size="$(stat -c %s "$LEDGER_FILE" 2>/dev/null || echo 0)"
    case "$size" in ''|*[!0-9]*) size=0 ;; esac
    if [ "$size" -ge "$ROTATE_BYTES" ]; then
        [ -f "$LEDGER_FILE.2" ] && rm -f "$LEDGER_FILE.2"
        [ -f "$LEDGER_FILE.1" ] && mv "$LEDGER_FILE.1" "$LEDGER_FILE.2"
        mv "$LEDGER_FILE" "$LEDGER_FILE.1"
        echo "node-slo-probe: rotated ledger (size=$size bytes >= $ROTATE_BYTES)" >&2
    fi
}

# append_line <json-line>: flock-serialized append (bounded -w 30, explicit
# failure) — same pattern as soak_evidence.sh so a timer run and an ad-hoc
# operator run can never interleave a torn line.
append_line() {
    local line="$1" append_rc=0
    (
        flock -x -w 30 9 || exit 9
        printf '%s\n' "$line" >&9
    ) 9>>"$LEDGER_FILE" || append_rc=$?
    if [ "$append_rc" -ne 0 ]; then
        if [ "$append_rc" -eq 9 ]; then
            echo "node-slo-probe: FAIL could not acquire append lock on $LEDGER_FILE within 30s" >&2
        else
            echo "node-slo-probe: FAIL could not append to $LEDGER_FILE (rc=$append_rc)" >&2
        fi
        return 1
    fi
    return 0
}

# max_of <a> <b> ...: print the max of the non-empty numeric args, or "".
max_of() {
    local best="" v
    for v in "$@"; do
        [ -n "$v" ] || continue
        if [ -z "$best" ] || [ "$v" -gt "$best" ]; then best="$v"; fi
    done
    printf '%s' "$best"
}

# ── collect ────────────────────────────────────────────────────────────

cmd_collect() {
    mkdir -p "$LEDGER_DIR"
    rotate_ledger_if_needed

    local ts; ts="$(date +%s)"

    local canon_default="ZCL_DATADIR=\"$CANON_DATADIR\" ZCL_RPCPORT=$CANON_RPCPORT timeout $RPC_TIMEOUT_SEC \"$ZCL_RPC_BIN\" getblockchaininfo"
    local soak_default="ZCL_DATADIR=\"$SOAK_DATADIR\" ZCL_RPCPORT=$SOAK_RPCPORT timeout $RPC_TIMEOUT_SEC \"$ZCL_RPC_BIN\" getblockchaininfo"
    local dev_default="ZCL_DATADIR=\"$DEV_DATADIR\" ZCL_RPCPORT=$DEV_RPCPORT timeout $RPC_TIMEOUT_SEC \"$ZCL_RPC_BIN\" getblockchaininfo"
    local oracle_default="ZCL_DATADIR=\"$ORACLE_DATADIR\" ZCL_RPCPORT=$ORACLE_RPCPORT timeout $RPC_TIMEOUT_SEC \"$ZCL_RPC_BIN\" getblockchaininfo"

    local canon soak dev oracle
    canon="$(rpc_probe "$canon_default" ZCL_SLO_CANON_CMD)"
    soak="$(rpc_probe "$soak_default" ZCL_SLO_SOAK_CMD)"
    dev="$(rpc_probe "$dev_default" ZCL_SLO_DEV_CMD)"
    oracle="$(rpc_probe "$oracle_default" ZCL_SLO_ORACLE_CMD)"

    local canon_served; canon_served="$(field "$canon" 1)"
    local soak_served;  soak_served="$(field "$soak" 1)"
    local dev_served;   dev_served="$(field "$dev" 1)"
    local oracle_served; oracle_served="$(field "$oracle" 1)"

    local max_height
    max_height="$(max_of "$canon_served" "$soak_served" "$dev_served" "$oracle_served")"

    local any_unreachable=0
    emit_instance() {
        local name="$1" rpcport="$2" datadir="$3" probe="$4"
        local served header latency_ms detail reachable gap_max gap_oracle
        served="$(field "$probe" 1)"
        header="$(field "$probe" 2)"
        latency_ms="$(field "$probe" 3)"
        detail="$(field "$probe" 4)"
        if [ -n "$served" ]; then reachable="true"; else reachable="false"; any_unreachable=1; fi
        gap_max=""
        [ -n "$served" ] && [ -n "$max_height" ] && gap_max=$((max_height - served))
        gap_oracle=""
        [ -n "$served" ] && [ -n "$oracle_served" ] && gap_oracle=$((oracle_served - served))

        local line
        line="$(printf '{"ts":%s,"instance":%s,"rpcport":%s,"datadir":%s,"reachable":%s,"served_height":%s,"header_height":%s,"latency_ms":%s,"oracle_height":%s,"max_height":%s,"gap_vs_max":%s,"gap_vs_oracle":%s,"error_detail":%s}' \
            "$ts" "$(jstr "$name")" "$rpcport" "$(jstr "$datadir")" "$reachable" \
            "$(jnum "$served")" "$(jnum "$header")" "$(jnum "$latency_ms")" \
            "$(jnum "$oracle_served")" "$(jnum "$max_height")" \
            "$(jnum "$gap_max")" "$(jnum "$gap_oracle")" "$(jstr "$detail")")"
        append_line "$line" || return 1
        echo "$line"
        if [ "$reachable" != "true" ]; then
            echo "node-slo-probe: WARN instance=$name unreachable detail=${detail:-none}" >&2
        fi
    }

    local rc=0
    emit_instance "canonical" "$CANON_RPCPORT" "$CANON_DATADIR" "$canon" || rc=1
    emit_instance "soak"      "$SOAK_RPCPORT"  "$SOAK_DATADIR"  "$soak"  || rc=1
    emit_instance "dev"       "$DEV_RPCPORT"   "$DEV_DATADIR"   "$dev"   || rc=1

    echo "node-slo-probe: collect done file=$LEDGER_FILE oracle_height=$(jnum "$oracle_served") max_height=$(jnum "$max_height") any_unreachable=$any_unreachable"
    return "$rc"
}

# ── selftest (hermetic; injected commands, no live nodes) ──────────────

st_fail() { echo "selftest: FAIL $*" >&2; exit 1; }

cmd_selftest() {
    ST_TMP="$(mktemp -d /tmp/zcl-node-slo-probe-selftest.XXXXXX)"
    trap 'rm -rf "$ST_TMP"' EXIT

    # A) all four reachable, dev lags behind the others.
    (
        export ZCL_SLO_LEDGER_DIR="$ST_TMP/a"
        export ZCL_SLO_CANON_CMD="echo '{\"result\":{\"blocks\":100,\"headers\":100}}'"
        export ZCL_SLO_SOAK_CMD="echo '{\"result\":{\"blocks\":101,\"headers\":101}}'"
        export ZCL_SLO_DEV_CMD="echo '{\"result\":{\"blocks\":90,\"headers\":101}}'"
        export ZCL_SLO_ORACLE_CMD="echo '{\"result\":{\"blocks\":101,\"headers\":101}}'"
        bash "$SELF" collect >/dev/null
    )
    local f="$ST_TMP/a/uptime-ledger.jsonl"
    [ -s "$f" ] || st_fail "case=all-reachable ledger file missing/empty"
    [ "$(wc -l < "$f")" -eq 3 ] || st_fail "case=all-reachable expected 3 lines, got $(wc -l < "$f")"
    grep -q '"instance":"dev".*"served_height":90.*"gap_vs_max":11.*"gap_vs_oracle":11' "$f" \
        || { cat "$f" >&2; st_fail "case=all-reachable dev gap math wrong"; }
    grep -q '"instance":"soak".*"served_height":101.*"gap_vs_max":0.*"gap_vs_oracle":0' "$f" \
        || { cat "$f" >&2; st_fail "case=all-reachable soak (at max) gap math wrong"; }
    echo "selftest: ok case=all-reachable"

    # B) soak unreachable (command fails) — still ONE line, ok:false-shaped
    # (reachable:false), other two instances unaffected, exit 0 (a hole is
    # not a script failure).
    (
        export ZCL_SLO_LEDGER_DIR="$ST_TMP/b"
        export ZCL_SLO_CANON_CMD="echo '{\"result\":{\"blocks\":200,\"headers\":200}}'"
        export ZCL_SLO_SOAK_CMD="false"
        export ZCL_SLO_DEV_CMD="echo '{\"result\":{\"blocks\":200,\"headers\":200}}'"
        export ZCL_SLO_ORACLE_CMD="echo '{\"result\":{\"blocks\":200,\"headers\":200}}'"
        bash "$SELF" collect >/dev/null 2>&1
    ) || st_fail "case=soak-down collect must exit 0 on an unreachable node"
    f="$ST_TMP/b/uptime-ledger.jsonl"
    grep -q '"instance":"soak","rpcport":18242,"datadir":"[^"]*","reachable":false,"served_height":null,"header_height":null,"latency_ms":[0-9]*,"oracle_height":200,"max_height":200,"gap_vs_max":null,"gap_vs_oracle":null' "$f" \
        || { cat "$f" >&2; st_fail "case=soak-down wrong null-shaped line"; }
    grep -q '"instance":"canonical".*"reachable":true.*"served_height":200' "$f" \
        || { cat "$f" >&2; st_fail "case=soak-down canonical line should still be reachable"; }
    echo "selftest: ok case=soak-down"

    # C) ALL four unreachable — three null-shaped lines, ledger still
    # created, still exit 0 (the hole IS the evidence).
    (
        export ZCL_SLO_LEDGER_DIR="$ST_TMP/c"
        export ZCL_SLO_CANON_CMD="false"
        export ZCL_SLO_SOAK_CMD="false"
        export ZCL_SLO_DEV_CMD="false"
        export ZCL_SLO_ORACLE_CMD="false"
        bash "$SELF" collect >/dev/null 2>&1
    ) || st_fail "case=all-down collect must exit 0"
    f="$ST_TMP/c/uptime-ledger.jsonl"
    [ "$(wc -l < "$f")" -eq 3 ] || st_fail "case=all-down expected 3 lines"
    grep -c '"reachable":false' "$f" | grep -q '^3$' \
        || { cat "$f" >&2; st_fail "case=all-down expected all 3 lines reachable:false"; }
    echo "selftest: ok case=all-down"

    # D) rotation: pre-seed a ledger already past the (tiny, test-only)
    # rotation threshold; after collect, .1 exists and the live file holds
    # only this run's fresh lines.
    (
        export ZCL_SLO_LEDGER_DIR="$ST_TMP/d"
        mkdir -p "$ST_TMP/d"
        printf 'x%.0s' $(seq 1 200) > "$ST_TMP/d/uptime-ledger.jsonl"
        echo >> "$ST_TMP/d/uptime-ledger.jsonl"
        export ZCL_SLO_ROTATE_BYTES=100
        export ZCL_SLO_CANON_CMD="echo '{\"result\":{\"blocks\":5,\"headers\":5}}'"
        export ZCL_SLO_SOAK_CMD="echo '{\"result\":{\"blocks\":5,\"headers\":5}}'"
        export ZCL_SLO_DEV_CMD="echo '{\"result\":{\"blocks\":5,\"headers\":5}}'"
        export ZCL_SLO_ORACLE_CMD="echo '{\"result\":{\"blocks\":5,\"headers\":5}}'"
        bash "$SELF" collect >/dev/null 2>&1
    ) || st_fail "case=rotation collect must exit 0"
    [ -f "$ST_TMP/d/uptime-ledger.jsonl.1" ] || st_fail "case=rotation expected .1 rotated file"
    [ "$(wc -l < "$ST_TMP/d/uptime-ledger.jsonl")" -eq 3 ] \
        || st_fail "case=rotation expected fresh live file with 3 lines"
    echo "selftest: ok case=rotation"

    echo "selftest: PASS"
}

# ── dispatch ─────────────────────────────────────────────────────────

case "${1:-collect}" in
    collect)    shift || true; cmd_collect "$@" ;;
    --selftest) shift; cmd_selftest "$@" ;;
    *)
        echo "usage: node_slo_probe.sh [collect] | --selftest" >&2
        exit 2
        ;;
esac
