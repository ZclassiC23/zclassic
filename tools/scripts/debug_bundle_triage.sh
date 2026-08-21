#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# debug_bundle_triage.sh — one-screen triage for a zcl.debug_bundle.v1 JSON.
#
# `ops debug bundle` (or the supervisor-stall auto-capture) writes a JSON with
# every registered state dumper's body; this script extracts the handful of
# fields that answer "why is the node wedged?" in seconds.
#
# argv[1] is a bundle file, or a directory containing debug-bundle-*.json
# (newest by mtime wins). Read-only; never touches the datadir otherwise.
# Exit 0 on success, 1 on a malformed/unreadable bundle, 2 on usage.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
JSONQ="${JSONQ:-$REPO_ROOT/build/bin/jsonq}"
BUNDLE_FORMAT="zcl.debug_bundle.v1"
MAX_BLOCKERS=5

die() { printf 'REFUSE: %s\n' "$1" >&2; exit 1; }

if [ "$#" -ne 1 ]; then
    printf 'usage: %s <bundle.json | datadir>\n' "$0" >&2
    exit 2
fi
[ -x "$JSONQ" ] || die "build/bin/jsonq is missing — run make jsonq"

pick_bundle() {
    local p="$1"
    if [ -d "$p" ]; then
        find "$p" -maxdepth 1 -type f -name 'debug-bundle-*.json' -printf '%T@ %p\n' \
            2>/dev/null | sort -n | tail -1 | awk '{ $1=""; sub(/^ /,""); print }'
        return 0
    fi
    [ -f "$p" ] && printf '%s\n' "$p"
}

jqg() { printf '%s' "$DOC" | "$JSONQ" get "$1" 2>/dev/null || true; }
jqh() { printf '%s' "$DOC" | "$JSONQ" has "$1" >/dev/null 2>&1; }
jqc() { printf '%s' "$DOC" | "$JSONQ" count "$1" 2>/dev/null || echo 0; }

PATH_IN="$1"
BUNDLE="$(pick_bundle "$PATH_IN")"
[ -n "$BUNDLE" ] || die "no debug-bundle-*.json under $PATH_IN"

DOC="$(cat "$BUNDLE")"
fmt="$(jqg format)"
[ "$fmt" = "$BUNDLE_FORMAT" ] || die "$BUNDLE: format is ${fmt:-missing}, expected $BUNDLE_FORMAT"
jqh subsystems || die "$BUNDLE: missing object key 'subsystems'"

short() {
    local s="${1:-}"
    [ -n "$s" ] || { printf '<missing>'; return; }
    printf '%s' "${s:0:12}"
}

age_s() {
    local us="${1:-}"
    if [ -z "$us" ] || [ "$us" -lt 0 ] 2>/dev/null; then
        printf '?'
        return
    fi
    awk -v us="$us" 'BEGIN {
        s = us / 1000000.0
        if (s < 90) printf "%.1fs", s
        else if (s < 5400) printf "%.1fm", s / 60
        else printf "%.1fh", s / 3600
    }'
}

h() {
    local v="${1:-}"
    if [ -n "$v" ] && [ "$v" -ge 0 ] 2>/dev/null; then
        printf '%s' "$v"
    else
        printf '-'
    fi
}

echo "bundle $(basename "$BUNDLE")"
trig="$(jqg trigger)"; trig="${trig:-manual}"
trig_extra=""
child="$(jqg trigger_child)"
if [ -n "$child" ]; then
    trig_extra=" child=$child reason=$(jqg trigger_stall_reason)"
    trig_extra="${trig_extra% }"; [ -n "$(jqg trigger_stall_reason)" ] || trig_extra=" child=$child reason=?"
fi
echo "  captured $(jqg captured_at_utc)  trigger=${trig}${trig_extra}"
echo "  build v$(jqg build.version) commit=$(short "$(jqg build.build_commit)") src=$(short "$(jqg build.source_id_sha256)")"

echo "== frontier =="
if jqh subsystems.reducer_frontier.error; then
    echo "  reducer_frontier dump failed: $(jqg subsystems.reducer_frontier.error)"
elif ! jqh subsystems.reducer_frontier; then
    echo "  reducer_frontier: absent"
else
    rf="subsystems.reducer_frontier"
    hstar="$(jqg $rf.hstar)"
    floor="$(jqg $rf.served_floor)"
    gap="$(jqg $rf.served_gap)"
    if [ "$(jqg $rf.network_tip_read_ok)" = "true" ]; then
        tip="network_tip=$(h "$(jqg $rf.network_tip)") tail_gap=$(jqg $rf.hstar_to_network_tip_gap)"
    else
        tip="network_tip=- (no peer height)"
    fi
    echo "  H*=$(h "$hstar") served_floor=$(h "$floor") gap=${gap:-0} provable_tip=$(h "$(jqg $rf.cached_provable_tip)")  $tip"
    if [ "$(jqg $rf.hstar_next_blocked)" = "true" ]; then
        echo "  H*+1 BLOCKED stage=$(jqg $rf.hstar_next_primary_stage) kind=$(jqg $rf.hstar_next_primary_kind) detail=$(jqg $rf.hstar_next_primary_detail)"
        owner="$(jqg $rf.hstar_next_primary_repair_owner)"
        [ -n "$owner" ] && echo "    repair_owner: $owner"
    elif [ "$(jqg $rf.hstar_next_pending_edge)" = "true" ]; then
        echo "  H*+1 pending edge stage=$(jqg $rf.hstar_next_pending_stage) ($(jqg $rf.hstar_next_pending_detail))"
    else
        echo "  H*+1: no blocker"
    fi
    coins_best="$(jqg $rf.coins_best_height)"
    if [ -z "$coins_best" ] || [ "$coins_best" -lt 0 ] 2>/dev/null; then
        cover="coins_applied_height absent (fresh datadir)"
    elif [ "$(jqg $rf.coins_best_above_hstar)" = "true" ]; then
        cover="coins AHEAD of H*"
    elif [ -n "$hstar" ] && [ "$coins_best" -ge $((hstar - 1)) ] 2>/dev/null; then
        cover="coins cover H*"
    else
        cover="coins BEHIND H*"
    fi
    echo "  coins_best=$(h "$coins_best") vs H*=$(h "$hstar")  ($cover)"
fi

echo "== blockers =="
if jqh subsystems.blocker.error; then
    echo "  blocker dump failed: $(jqg subsystems.blocker.error)"
else
    blk="subsystems.blocker"
    active="$(jqg $blk.active_count)"
    n="$(jqc $blk.blockers)"
    [ -n "$active" ] || active="$n"
    if [ "${active:-0}" = "0" ] && [ "$n" = "0" ]; then
        echo "  none active"
    else
        echo "  active=${active} permanent=$(jqg $blk.permanent_count) dependency=$(jqg $blk.dependency_count) transient=$(jqg $blk.transient_count) resource=$(jqg $blk.resource_count)"
        i=0
        while [ "$i" -lt "$n" ] && [ "$i" -lt "$MAX_BLOCKERS" ]; do
            echo "  - $(jqg $blk.blockers[$i].id) owner=$(jqg $blk.blockers[$i].owner) class=$(jqg $blk.blockers[$i].class) age=$(age_s "$(jqg $blk.blockers[$i].age_us)") fires=$(jqg $blk.blockers[$i].fire_count)"
            reason="$(jqg $blk.blockers[$i].reason)"
            [ "${#reason}" -gt 72 ] && reason="${reason:0:69}..."
            echo "      reason: $reason"
            caused="$(jqg $blk.blockers[$i].caused_by)"
            if [ -n "$caused" ]; then
                detail="$(jqg $blk.blockers[$i].cause_detail)"
                suffix=""
                [ -n "$detail" ] && suffix=" ($detail)"
                echo "      caused_by: ${caused}${suffix}"
            fi
            i=$((i + 1))
        done
        if [ "$n" -gt "$MAX_BLOCKERS" ]; then
            echo "  ... and $((n - MAX_BLOCKERS)) more"
        fi
    fi
fi

echo "== supervisor =="
if ! jqh supervisor_stalls; then
    echo "  supervisor_stalls section absent"
else
    sup="supervisor_stalls"
    echo "  children=$(jqg $sup.child_count) stalled_or_fired=$(jqg $sup.stalled_or_fired_count)"
    n="$(jqc $sup.children)"
    i=0
    while [ "$i" -lt "$n" ]; do
        echo "  - $(jqg $sup.children[$i].name) reason=$(jqg $sup.children[$i].stall_reason) fires=$(jqg $sup.children[$i].stall_fires) last_tick_age=$(age_s "$(jqg $sup.children[$i].last_tick_age_us)") progress=$(jqg $sup.children[$i].progress_marker)"
        i=$((i + 1))
    done
fi

if jqh subsystems.sovereignty && ! jqh subsystems.sovereignty.error; then
    sov="subsystems.sovereignty"
    echo "== sovereignty =="
    echo "  trust_mode=$(jqg $sov.trust_mode) self_folded=$(jqg $sov.self_folded_marker) proven_authority=$(jqg $sov.coins_kv_proven_authority) self_derived=$(jqg $sov.self_derived_tip_static_checks) ($(jqg $sov.self_derived_reason))"
fi
