#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# node_slo_probe.sh — the EXTERNAL uptime prober (lane E3, Instant-Sync/
# Strength program). This is the scoreboard for "staying synced" that does
# NOT trust node self-reports: it is a separate client process that dials
# each local instance's RPC port the same way any outside caller would, and
# records what it actually got back — including nothing at all.
#
# Probes the DEPLOYED local instances by CLIENT-VIEWPOINT RPC (never
# in-process introspection):
#   canonical  rpcport 18232  datadir ~/.zclassic-c23      (deploy/zclassic23.service)
#   dev        rpcport 18252  datadir ~/.zclassic-c23-dev  (deploy/zcl23-dev.service)
# Ports/datadirs are hardcoded defaults (each also overridable via env, see
# below) rather than parsed from the unit files: the ports are a stable,
# documented contract (see CLAUDE.md "Running" + the unit files themselves),
# and a probe that could silently start reading a DIFFERENT port because a
# unit file comment changed is a worse failure mode than one that is
# explicit and greppable here.
#
# The instance table carries only instances that are actually DEPLOYED on
# this host. A row for an instance nobody runs answers reachable:false on
# every single poll forever, which (a) drags every uptime percentage derived
# from this ledger toward zero for a reason that has nothing to do with node
# health, and (b) trains every reader to ignore any_unreachable — the exact
# way a monitor goes blind. The soak lane (rpcport 18242, datadir
# ~/.zclassic-c23-soak) is such a case: its unit ships as an EXAMPLE
# (deploy/examples/zclassic23-soak-node.service) and is not installed here,
# so it is not probed. Installing that unit means adding its row back to
# INSTANCES below — one line, and the example unit says so.
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
# Appends ONE JSON line per probe PER INSTANCE (one line per row of the
# instance table per collect run) to
# ~/.local/state/zclassic23-slo/uptime-ledger.jsonl:
#   ts               epoch the sample was taken
#   instance         "canonical" | "dev"
#   rpcport          the port probed
#   datadir          the datadir probed (client-side identity, not proof)
#   reachable        true iff the RPC answered with a parseable height
#   unreachable_streak
#                    how many CONSECUTIVE polls (this one included) this
#                    instance has failed to answer; 0 on a reachable
#                    sample. This is what separates "went down a minute
#                    ago" (streak 1) from "has been dark for a week"
#                    (streak 10000) without re-reading the whole ledger,
#                    and it is what makes a NEW outage legible next to an
#                    old one instead of both looking like one flat
#                    reachable:false wall
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
# It does, however, get LOUDER rather than quieter as an outage ages: below
# ZCL_SLO_BLIND_STREAK consecutive misses an instance logs `WARN ...
# streak=N`, at or above it the line becomes `BLIND instance=... unreachable
# for N consecutive polls`, and the closing summary carries any_blind=1.
# A blind instance is either a node that needs fixing or a table row that
# needs deleting; both are actionable, and neither is background noise.
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
#   ZCL_SLO_BLIND_STREAK    consecutive misses at which an instance is
#                           reported BLIND rather than merely unreachable
#                           (default 10 polls = 10 minutes at the standing
#                           60 s cadence)
#   ZCL_SLO_CANON_CMD / ZCL_SLO_DEV_CMD / ZCL_SLO_ORACLE_CMD
#                           override the exact command run per instance
#                           (selftest injection seam — same pattern as
#                           soak_evidence.sh's ZCL_SOAK_RPC_CMD); the
#                           per-instance variable name is the 4th field of
#                           that instance's INSTANCES row
#
# No python (banned), no jq (installed but unused by repo convention) —
# bash + sed + flock only, same rule as soak_evidence.sh / replay_canary.sh.

set -euo pipefail
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELF="$SCRIPT_DIR/$(basename "${BASH_SOURCE[0]}")"

LEDGER_DIR="${ZCL_SLO_LEDGER_DIR:-${HOME:-/root}/.local/state/zclassic23-slo}"
LEDGER_FILE="$LEDGER_DIR/uptime-ledger.jsonl"
STREAK_DIR="$LEDGER_DIR/streak"
RPC_TIMEOUT_SEC="${ZCL_SLO_RPC_TIMEOUT_SEC:-8}"
ROTATE_BYTES="${ZCL_SLO_ROTATE_BYTES:-52428800}"   # 50 MiB
BLIND_STREAK="${ZCL_SLO_BLIND_STREAK:-10}"

# ── instance table ────────────────────────────────────────────────────
# ONE list, probed in order. Row format:
#     name|rpcport|datadir|command-override-env-var
# Membership rule: an instance belongs here only while it is DEPLOYED on
# this host. Deleting a lane means deleting its row (and the ledger stops
# carrying a permanently-null sample for it); installing a lane means adding
# one. Everything downstream — the ledger schema, the summary reader, the
# pager — is driven off whatever rows are here.
INSTANCES=(
    "canonical|18232|${HOME:-/root}/.zclassic-c23|ZCL_SLO_CANON_CMD"
    "dev|18252|${HOME:-/root}/.zclassic-c23-dev|ZCL_SLO_DEV_CMD"
)
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

# streak_read <instance>: consecutive-miss count carried over from previous
# runs, 0 when absent/garbage. Kept in a tiny per-instance state file rather
# than derived from the ledger tail so a ledger ROTATION cannot reset a long
# outage back to "just went down" — the one moment the distinction matters
# most is exactly when the file rolls.
streak_read() {
    local f="$STREAK_DIR/$1" v=""
    [ -f "$f" ] && v="$(cat "$f" 2>/dev/null || true)"
    case "$v" in ''|*[!0-9]*) v=0 ;; esac
    printf '%s' "$v"
}

# streak_write <instance> <n>: atomic replace; a failure here degrades the
# streak counter, never the sample, so it can not fail a collect.
streak_write() {
    mkdir -p "$STREAK_DIR" 2>/dev/null || return 0
    local f="$STREAK_DIR/$1"
    printf '%s\n' "$2" > "$f.tmp" 2>/dev/null && mv -f "$f.tmp" "$f" 2>/dev/null
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

    local oracle_default="ZCL_DATADIR=\"$ORACLE_DATADIR\" ZCL_RPCPORT=$ORACLE_RPCPORT timeout $RPC_TIMEOUT_SEC \"$ZCL_RPC_BIN\" getblockchaininfo"
    local oracle; oracle="$(rpc_probe "$oracle_default" ZCL_SLO_ORACLE_CMD)"
    local oracle_served; oracle_served="$(field "$oracle" 1)"

    # Probe every row FIRST, then emit: max_height must be the max over the
    # whole cycle, so no line can be written before the last node answered.
    local -a names=() ports=() dirs=() probes=()
    local row name rpcport datadir var default_cmd
    for row in "${INSTANCES[@]}"; do
        IFS='|' read -r name rpcport datadir var <<<"$row"
        default_cmd="ZCL_DATADIR=\"$datadir\" ZCL_RPCPORT=$rpcport timeout $RPC_TIMEOUT_SEC \"$ZCL_RPC_BIN\" getblockchaininfo"
        names+=("$name"); ports+=("$rpcport"); dirs+=("$datadir")
        probes+=("$(rpc_probe "$default_cmd" "$var")")
    done

    local max_height serveds=("$oracle_served") i
    for ((i = 0; i < ${#names[@]}; i++)); do serveds+=("$(field "${probes[$i]}" 1)"); done
    max_height="$(max_of "${serveds[@]}")"

    local any_unreachable=0 any_blind=0
    emit_instance() {
        local name="$1" rpcport="$2" datadir="$3" probe="$4"
        local served header latency_ms detail reachable gap_max gap_oracle streak
        served="$(field "$probe" 1)"
        header="$(field "$probe" 2)"
        latency_ms="$(field "$probe" 3)"
        detail="$(field "$probe" 4)"
        streak="$(streak_read "$name")"
        if [ -n "$served" ]; then
            reachable="true"; streak=0
        else
            reachable="false"; any_unreachable=1; streak=$((streak + 1))
            if [ "$streak" -ge "$BLIND_STREAK" ]; then any_blind=1; fi
        fi
        streak_write "$name" "$streak"
        gap_max=""
        [ -n "$served" ] && [ -n "$max_height" ] && gap_max=$((max_height - served))
        gap_oracle=""
        [ -n "$served" ] && [ -n "$oracle_served" ] && gap_oracle=$((oracle_served - served))

        local line
        line="$(printf '{"ts":%s,"instance":%s,"rpcport":%s,"datadir":%s,"reachable":%s,"unreachable_streak":%s,"served_height":%s,"header_height":%s,"latency_ms":%s,"oracle_height":%s,"max_height":%s,"gap_vs_max":%s,"gap_vs_oracle":%s,"error_detail":%s}' \
            "$ts" "$(jstr "$name")" "$rpcport" "$(jstr "$datadir")" "$reachable" "$streak" \
            "$(jnum "$served")" "$(jnum "$header")" "$(jnum "$latency_ms")" \
            "$(jnum "$oracle_served")" "$(jnum "$max_height")" \
            "$(jnum "$gap_max")" "$(jnum "$gap_oracle")" "$(jstr "$detail")")"
        append_line "$line" || return 1
        echo "$line"
        if [ "$reachable" != "true" ]; then
            if [ "$streak" -ge "$BLIND_STREAK" ]; then
                echo "node-slo-probe: BLIND instance=$name unreachable for $streak consecutive polls (threshold $BLIND_STREAK) — fix the node or delete its row from the instance table detail=${detail:-none}" >&2
            else
                echo "node-slo-probe: WARN instance=$name unreachable streak=$streak detail=${detail:-none}" >&2
            fi
        fi
    }

    local rc=0
    for ((i = 0; i < ${#names[@]}; i++)); do
        emit_instance "${names[$i]}" "${ports[$i]}" "${dirs[$i]}" "${probes[$i]}" || rc=1
    done

    echo "node-slo-probe: collect done file=$LEDGER_FILE instances=${#names[@]} oracle_height=$(jnum "$oracle_served") max_height=$(jnum "$max_height") any_unreachable=$any_unreachable any_blind=$any_blind"
    return "$rc"
}

# ── selftest (hermetic; injected commands, no live nodes) ──────────────

st_fail() { echo "selftest: FAIL $*" >&2; exit 1; }

# The instance table is the ONE place that says which nodes exist on this
# host. Downstream readers must ASK for it rather than infer it from ledger
# history: a retired lane's rows stay in the retained ledger forever, so an
# inferring reader treats a deleted node as a node that stopped answering.
# That is what the pager used to do, and retiring a lane would have made it
# page "prober may be dead" — falsely, forever.
cmd_list_instances() {
    local row
    for row in "${INSTANCES[@]}"; do
        printf '%s\n' "${row%%|*}"
    done
}

cmd_selftest() {
    ST_TMP="$(mktemp -d /tmp/zcl-node-slo-probe-selftest.XXXXXX)"
    trap 'rm -rf "$ST_TMP"' EXIT

    # A) every instance + the oracle reachable, dev lagging.
    (
        export ZCL_SLO_LEDGER_DIR="$ST_TMP/a"
        export ZCL_SLO_CANON_CMD="echo '{\"result\":{\"blocks\":100,\"headers\":100}}'"
        export ZCL_SLO_DEV_CMD="echo '{\"result\":{\"blocks\":90,\"headers\":101}}'"
        export ZCL_SLO_ORACLE_CMD="echo '{\"result\":{\"blocks\":101,\"headers\":101}}'"
        bash "$SELF" collect >/dev/null
    )
    local f="$ST_TMP/a/uptime-ledger.jsonl"
    [ -s "$f" ] || st_fail "case=all-reachable ledger file missing/empty"
    [ "$(wc -l < "$f")" -eq 2 ] || st_fail "case=all-reachable expected 2 lines, got $(wc -l < "$f")"
    grep -q '"instance":"dev".*"served_height":90.*"gap_vs_max":11.*"gap_vs_oracle":11' "$f" \
        || { cat "$f" >&2; st_fail "case=all-reachable dev gap math wrong"; }
    grep -q '"instance":"canonical".*"served_height":100.*"gap_vs_max":1.*"gap_vs_oracle":1' "$f" \
        || { cat "$f" >&2; st_fail "case=all-reachable canonical gap math wrong"; }
    grep -q '"reachable":true,"unreachable_streak":0' "$f" \
        || { cat "$f" >&2; st_fail "case=all-reachable streak must be 0 on a reachable sample"; }
    echo "selftest: ok case=all-reachable"

    # B) one instance unreachable (command fails) — still ONE line,
    # null-shaped (reachable:false, streak 1), the other instance
    # unaffected, exit 0 (a hole is not a script failure).
    (
        export ZCL_SLO_LEDGER_DIR="$ST_TMP/b"
        export ZCL_SLO_CANON_CMD="echo '{\"result\":{\"blocks\":200,\"headers\":200}}'"
        export ZCL_SLO_DEV_CMD="false"
        export ZCL_SLO_ORACLE_CMD="echo '{\"result\":{\"blocks\":200,\"headers\":200}}'"
        bash "$SELF" collect >/dev/null 2>&1
    ) || st_fail "case=dev-down collect must exit 0 on an unreachable node"
    f="$ST_TMP/b/uptime-ledger.jsonl"
    grep -q '"instance":"dev","rpcport":18252,"datadir":"[^"]*","reachable":false,"unreachable_streak":1,"served_height":null,"header_height":null,"latency_ms":[0-9]*,"oracle_height":200,"max_height":200,"gap_vs_max":null,"gap_vs_oracle":null' "$f" \
        || { cat "$f" >&2; st_fail "case=dev-down wrong null-shaped line"; }
    grep -q '"instance":"canonical".*"reachable":true.*"served_height":200' "$f" \
        || { cat "$f" >&2; st_fail "case=dev-down canonical line should still be reachable"; }
    echo "selftest: ok case=dev-down"

    # C) EVERYTHING unreachable — one null-shaped line per instance, ledger
    # still created, still exit 0 (the hole IS the evidence).
    (
        export ZCL_SLO_LEDGER_DIR="$ST_TMP/c"
        export ZCL_SLO_CANON_CMD="false"
        export ZCL_SLO_DEV_CMD="false"
        export ZCL_SLO_ORACLE_CMD="false"
        bash "$SELF" collect >/dev/null 2>&1
    ) || st_fail "case=all-down collect must exit 0"
    f="$ST_TMP/c/uptime-ledger.jsonl"
    [ "$(wc -l < "$f")" -eq 2 ] || st_fail "case=all-down expected 2 lines"
    grep -c '"reachable":false' "$f" | grep -q '^2$' \
        || { cat "$f" >&2; st_fail "case=all-down expected all lines reachable:false"; }
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
        export ZCL_SLO_DEV_CMD="echo '{\"result\":{\"blocks\":5,\"headers\":5}}'"
        export ZCL_SLO_ORACLE_CMD="echo '{\"result\":{\"blocks\":5,\"headers\":5}}'"
        bash "$SELF" collect >/dev/null 2>&1
    ) || st_fail "case=rotation collect must exit 0"
    [ -f "$ST_TMP/d/uptime-ledger.jsonl.1" ] || st_fail "case=rotation expected .1 rotated file"
    [ "$(wc -l < "$ST_TMP/d/uptime-ledger.jsonl")" -eq 2 ] \
        || st_fail "case=rotation expected fresh live file with 2 lines"
    echo "selftest: ok case=rotation"

    # E) the retired soak lane is GONE from the ledger, not merely quiet: no
    # row means no sample, ever. This is the regression guard for the
    # permanently-unreachable instance that made any_unreachable meaningless.
    if grep -q '"instance":"soak"' "$ST_TMP"/*/uptime-ledger.jsonl; then
        st_fail "case=no-retired-rows an undeployed instance is still being probed"
    fi
    local healthy_out
    healthy_out="$(env "ZCL_SLO_LEDGER_DIR=$ST_TMP/e" \
        "ZCL_SLO_CANON_CMD=echo '{\"result\":{\"blocks\":7,\"headers\":7}}'" \
        "ZCL_SLO_DEV_CMD=echo '{\"result\":{\"blocks\":7,\"headers\":7}}'" \
        "ZCL_SLO_ORACLE_CMD=echo '{\"result\":{\"blocks\":7,\"headers\":7}}'" \
        bash "$SELF" collect 2>&1)"
    printf '%s' "$healthy_out" | grep -q 'any_unreachable=0 any_blind=0' \
        || { printf '%s\n' "$healthy_out" >&2; st_fail "case=no-retired-rows a healthy box must report any_unreachable=0"; }
    echo "selftest: ok case=no-retired-rows"

    # F) a long outage is distinguishable from a fresh one: the streak
    # climbs across polls, crosses ZCL_SLO_BLIND_STREAK into a BLIND line,
    # and snaps back to 0 the moment the node answers again.
    local streak_env=(
        "ZCL_SLO_LEDGER_DIR=$ST_TMP/f"
        "ZCL_SLO_BLIND_STREAK=3"
        "ZCL_SLO_CANON_CMD=echo '{\"result\":{\"blocks\":9,\"headers\":9}}'"
        "ZCL_SLO_ORACLE_CMD=echo '{\"result\":{\"blocks\":9,\"headers\":9}}'"
    )
    local blind_out="" i
    for i in 1 2 3; do
        blind_out="$(env "${streak_env[@]}" ZCL_SLO_DEV_CMD=false bash "$SELF" collect 2>&1)"
    done
    f="$ST_TMP/f/uptime-ledger.jsonl"
    grep -q '"instance":"dev".*"unreachable_streak":1' "$f" \
        || { cat "$f" >&2; st_fail "case=blind-streak first miss must record streak 1"; }
    grep -q '"instance":"dev".*"unreachable_streak":3' "$f" \
        || { cat "$f" >&2; st_fail "case=blind-streak third miss must record streak 3"; }
    printf '%s' "$blind_out" | grep -q 'BLIND instance=dev unreachable for 3 consecutive polls' \
        || { printf '%s\n' "$blind_out" >&2; st_fail "case=blind-streak must escalate to BLIND at the threshold"; }
    printf '%s' "$blind_out" | grep -q 'any_blind=1' \
        || { printf '%s\n' "$blind_out" >&2; st_fail "case=blind-streak summary must carry any_blind=1"; }
    local recover_out
    recover_out="$(env "${streak_env[@]}" "ZCL_SLO_DEV_CMD=echo '{\"result\":{\"blocks\":9,\"headers\":9}}'" \
        bash "$SELF" collect 2>&1)"
    printf '%s' "$recover_out" | grep -q '"instance":"dev".*"reachable":true,"unreachable_streak":0' \
        || { printf '%s\n' "$recover_out" >&2; st_fail "case=blind-streak recovery must reset the streak to 0"; }
    printf '%s' "$recover_out" | grep -q 'any_blind=0' \
        || { printf '%s\n' "$recover_out" >&2; st_fail "case=blind-streak recovery must clear any_blind"; }
    echo "selftest: ok case=blind-streak"

    echo "selftest: PASS"
}

# ── dispatch ─────────────────────────────────────────────────────────

case "${1:-collect}" in
    collect)    shift || true; cmd_collect "$@" ;;
    --selftest) shift; cmd_selftest "$@" ;;
    --list-instances) shift; cmd_list_instances "$@" ;;
    *)
        echo "usage: node_slo_probe.sh [collect] | --selftest | --list-instances" >&2
        exit 2
        ;;
esac
