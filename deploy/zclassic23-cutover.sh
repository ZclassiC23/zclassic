#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# zclassic23-cutover.sh — promote a warm-standby (or any healthy candidate
# datadir) to the canonical serving identity (ports 8033/18232, datadir
# ~/.zclassic-c23), with a hard preflight and an AUTO-ROLLBACK that can never
# strand you with no running canonical.
#
# Invoked by `make cutover CANDIDATE_DATADIR=<path>`. Canonical cutover is an
# OWNER action: this script refuses to touch the canonical unit until you pass
# --yes, and it prints a side-by-side height comparison first.
#
# ── Why a directory-rename swap and not a symlink ──────────────────────────
# The node opens its datadir with open(O_DIRECTORY|O_NOFOLLOW)
# (config/src/boot_datadir_lock.c) and mint/anchor preflight does the same.
# O_NOFOLLOW makes a SYMLINKED datadir root fail at boot with ELOOP, so a
# `~/.zclassic-c23 -> real-dir` convention is UNSAFE — the promoted node would
# refuse to start. systemd ALSO cannot expand $ENV in ReadWritePaths /
# StandardOutput, so an env-indirected datadir would break the canonical
# unit's sandbox + log paths. Both rule out symlink/env swaps.
#
# Instead we keep the canonical unit COMPLETELY UNTOUCHED (it always points at
# the literal ~/.zclassic-c23) and swap the DATADIRS underneath it with two
# renames while both units are stopped:
#     mv ~/.zclassic-c23        -> ~/.zclassic-c23.pre-cutover-<ts>   (demote)
#     mv <candidate>            -> ~/.zclassic-c23                    (promote)
# Rollback is exactly those two renames reversed. Ports, sandbox paths, log
# path and core dir all stay correct because the promoted data literally IS
# ~/.zclassic-c23 afterward. The candidate and ~/.zclassic-c23's parent must be
# on the same filesystem (checked) so each rename is atomic and instant.
#
# ── What it does NOT do ────────────────────────────────────────────────────
# No file inside any datadir is modified; it only RENAMES whole directories.
# It never deletes a datadir. The demoted old-canonical dir is preserved
# (~/.zclassic-c23.pre-cutover-<ts>) for manual inspection/rollback.
#
# Exit codes: 0 PROMOTED, 1 ROLLED-BACK, 2 REFUSED (preflight / usage).
set -eu

# ── knobs (all overridable; the test suite injects mocks through these) ─────
SYSTEMCTL="${SYSTEMCTL:-systemctl --user}"
CANONICAL_UNIT="${CANONICAL_UNIT:-zclassic23}"
STANDBY_UNIT="${STANDBY_UNIT:-zclassic23-standby}"
CANONICAL_DATADIR="${CANONICAL_DATADIR:-$HOME/.zclassic-c23}"
CANONICAL_RPCPORT="${CANONICAL_RPCPORT:-18232}"
CANDIDATE_DATADIR="${CANDIDATE_DATADIR:-}"
CANDIDATE_RPCPORT="${CANDIDATE_RPCPORT:-18272}"   # standby default RPC port
READY_TIMEOUT="${READY_TIMEOUT:-300}"             # seconds to reach the bar
POLL_INTERVAL="${POLL_INTERVAL:-5}"
STOP_GRACE="${STOP_GRACE:-10}"                    # wait for a stopped node to release its datadir
ALLOW_CROSS_FS="${ALLOW_CROSS_FS:-0}"
ASSUME_YES=0

# Seams the fixture selftest overrides. The real readers just talk to zcl-rpc.
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
RPC_BIN="${ZCL_RPC_BIN:-$SCRIPT_DIR/build/bin/zcl-rpc}"
HSTAR_READER="${CUTOVER_HSTAR_READER:-_hstar_read_rpc}"

log()  { printf '[cutover] %s\n' "$*" >&2; }
die()  { printf '[cutover] ERROR: %s\n' "$*" >&2; exit 2; }

usage() {
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
}

# ── H* reader ───────────────────────────────────────────────────────────────
# _hstar_read_rpc <role> <rpcport> <datadir> -> prints an integer H* on stdout,
# or -1 if the node is unreachable / the answer is not a non-negative integer.
# <role> lets a mock distinguish call sites (pre/post/poststop); the real
# reader ignores it. getblockcount serves H* (the provable frontier tip).
_hstar_read_rpc() {
    _role="$1"; _port="$2"; _dd="$3"
    _resp="$(ZCL_DATADIR="$_dd" ZCL_RPCPORT="$_port" "$RPC_BIN" getblockcount 2>/dev/null || true)"
    _val="$(printf '%s\n' "$_resp" | \
        sed -n 's/.*"result"[[:space:]]*:[[:space:]]*\(-\{0,1\}[0-9][0-9]*\).*/\1/p' | head -1)"
    case "${_val:-}" in
        ''|*[!0-9]*) printf '%s\n' "-1" ;;
        *)           printf '%s\n' "$_val" ;;
    esac
}

hstar() { "$HSTAR_READER" "$1" "$2" "$3"; }

# ── unit control (SYSTEMCTL=echo in tests makes these visible no-ops) ────────
unit_stop()  { log "stopping $1"; $SYSTEMCTL stop "$1" >/dev/null 2>&1 || $SYSTEMCTL stop "$1" || true; }
unit_start() { log "starting $1"; $SYSTEMCTL start "$1"; }

# ── argument parsing ────────────────────────────────────────────────────────
while [ $# -gt 0 ]; do
    case "$1" in
        --yes)                ASSUME_YES=1 ;;
        --candidate=*)        CANDIDATE_DATADIR="${1#--candidate=}" ;;
        --candidate-rpcport=*) CANDIDATE_RPCPORT="${1#--candidate-rpcport=}" ;;
        --timeout=*)          READY_TIMEOUT="${1#--timeout=}" ;;
        --allow-cross-fs)     ALLOW_CROSS_FS=1 ;;
        -h|--help)            usage; exit 0 ;;
        --*)                  die "unknown option $1 (see --help)" ;;
        *) [ -z "$CANDIDATE_DATADIR" ] && CANDIDATE_DATADIR="$1" || die "unexpected arg $1" ;;
    esac
    shift
done

# ── validation ──────────────────────────────────────────────────────────────
[ -n "$CANDIDATE_DATADIR" ] || die "CANDIDATE_DATADIR is required (make cutover CANDIDATE_DATADIR=<path>)"
[ -d "$CANDIDATE_DATADIR" ] || die "candidate datadir not found: $CANDIDATE_DATADIR"
# Canonicalize to catch a candidate that IS the canonical datadir.
_cand_abs="$(CDPATH= cd -- "$CANDIDATE_DATADIR" && pwd -P)" || die "cannot resolve $CANDIDATE_DATADIR"
if [ -d "$CANONICAL_DATADIR" ]; then
    _canon_abs="$(CDPATH= cd -- "$CANONICAL_DATADIR" && pwd -P)" || die "cannot resolve $CANONICAL_DATADIR"
    [ "$_cand_abs" != "$_canon_abs" ] || die "candidate == canonical datadir; nothing to promote"
fi

# Same-filesystem is required for atomic instant renames (a cross-fs mv is a
# slow, non-atomic copy that can half-fail mid-incident).
if [ "$ALLOW_CROSS_FS" != "1" ]; then
    _cand_dev="$(stat -c %d "$_cand_abs" 2>/dev/null || echo x)"
    _canon_parent_dev="$(stat -c %d "$(dirname "$CANONICAL_DATADIR")" 2>/dev/null || echo y)"
    [ "$_cand_dev" = "$_canon_parent_dev" ] || \
        die "candidate and $(dirname "$CANONICAL_DATADIR") are on different filesystems; a rename swap would not be atomic. Move the candidate onto the same fs, or pass --allow-cross-fs to accept a slow copy."
fi

# ══ PRE-FLIGHT ══════════════════════════════════════════════════════════════
log "PRE-FLIGHT: reading heights"
CANON_H="$(hstar canonical-pre  "$CANONICAL_RPCPORT" "$CANONICAL_DATADIR")"
CAND_H="$(hstar  candidate-pre  "$CANDIDATE_RPCPORT" "$CANDIDATE_DATADIR")"

printf '\n'
printf '  ┌─ cutover preflight ───────────────────────────────────────────\n'
printf '  │ canonical  unit=%-20s rpc=%-6s  H*=%s\n' "$CANONICAL_UNIT" "$CANONICAL_RPCPORT" "$CANON_H"
printf '  │ candidate  dir =%-20s rpc=%-6s  H*=%s\n' "$_cand_abs" "$CANDIDATE_RPCPORT" "$CAND_H"
printf '  └───────────────────────────────────────────────────────────────\n\n'

# The candidate MUST be readable + healthy; you never promote what you cannot
# verify. (A negative H* means unreachable rpc or a non-integer answer.)
[ "$CAND_H" -ge 0 ] 2>/dev/null || \
    { log "REFUSED: candidate is not healthy/readable on rpc $CANDIDATE_RPCPORT (H*=$CAND_H). Start the standby unit first."; exit 2; }

# If canonical is readable, the candidate must not be BEHIND it — promoting a
# node that serves a lower tip would regress the network's view. If canonical
# is DOWN (H*=-1) this is exactly the failover case: allow it, but the operator
# still has to pass --yes with eyes open.
if [ "$CANON_H" -ge 0 ] 2>/dev/null; then
    BAR="$CANON_H"
    if [ "$CAND_H" -lt "$CANON_H" ] 2>/dev/null; then
        log "REFUSED: candidate H*=$CAND_H is BEHIND canonical H*=$CANON_H. Let the standby catch up, then retry."
        exit 2
    fi
    log "preflight OK: candidate H*=$CAND_H >= canonical H*=$CANON_H"
else
    BAR="$CAND_H"
    log "WARNING: canonical is unreachable on rpc $CANONICAL_RPCPORT (H*=$CANON_H) — treating this as a FAILOVER. The promoted node must reach candidate H*=$CAND_H."
fi

if [ "$ASSUME_YES" != "1" ]; then
    log "REFUSED: canonical cutover is an owner action. Re-run with --yes to promote (heights above)."
    exit 2
fi

# ══ EXECUTE ═════════════════════════════════════════════════════════════════
TS="$(date +%Y%m%d%H%M%S)"
DEMOTED_DIR="$CANONICAL_DATADIR.pre-cutover-$TS"
DEMOTED_DONE=0
PROMOTED_DONE=0

# Rollback is defined BEFORE we mutate anything so it is always reachable. It
# reverses exactly the renames that actually happened and ALWAYS leaves a
# running canonical on the OLD datadir.
rollback() {
    _reason="$1"
    log "AUTO-ROLLBACK: $_reason"
    unit_stop "$CANONICAL_UNIT"
    if [ "$PROMOTED_DONE" = "1" ]; then
        # undo promote: the candidate's data is currently at CANONICAL_DATADIR
        if [ -e "$CANDIDATE_DATADIR" ]; then
            log "FATAL: cannot restore candidate — $CANDIDATE_DATADIR reappeared; leaving promoted data at $CANONICAL_DATADIR for manual repair"
        else
            mv "$CANONICAL_DATADIR" "$CANDIDATE_DATADIR"
            log "restored candidate datadir -> $CANDIDATE_DATADIR"
            PROMOTED_DONE=0
        fi
    fi
    if [ "$DEMOTED_DONE" = "1" ]; then
        # undo demote: move the old canonical back into place
        if [ -e "$CANONICAL_DATADIR" ]; then
            log "FATAL: cannot restore old canonical — $CANONICAL_DATADIR is occupied; old data preserved at $DEMOTED_DIR"
        else
            mv "$DEMOTED_DIR" "$CANONICAL_DATADIR"
            log "restored old canonical datadir <- $DEMOTED_DIR"
            DEMOTED_DONE=0
        fi
    fi
    # Best-effort restarts — a failure here must NOT abort rollback before the
    # verdict prints (the old datadir is already restored above, which is the
    # part that matters). Loudly flag a genuinely un-startable canonical.
    unit_start "$CANONICAL_UNIT" || log "FATAL: old canonical did NOT restart — data is safe at $CANONICAL_DATADIR but no canonical is running; investigate NOW"
    unit_start "$STANDBY_UNIT" || log "note: could not restart $STANDBY_UNIT (candidate datadir may have been the standby's)"
    printf 'CUTOVER: ROLLED-BACK  canonical_H*=%s candidate_H*=%s bar=%s\n' "$CANON_H" "$CAND_H" "$BAR"
    exit 1
}

log "EXECUTE: stopping both units before the datadir swap"
unit_stop "$STANDBY_UNIT"
unit_stop "$CANONICAL_UNIT"

# The candidate datadir must be RELEASED (its node fully stopped) before we can
# safely rename it — moving a datadir with a live writer corrupts it. Poll its
# rpc until it stops answering.
_waited=0
while [ "$_waited" -lt "$STOP_GRACE" ]; do
    _live="$(hstar candidate-poststop "$CANDIDATE_RPCPORT" "$CANDIDATE_DATADIR")"
    [ "$_live" -lt 0 ] 2>/dev/null && break
    sleep 1
    _waited=$((_waited + 1))
done
_live="$(hstar candidate-poststop "$CANDIDATE_RPCPORT" "$CANDIDATE_DATADIR")"
if [ "$_live" -ge 0 ] 2>/dev/null; then
    log "REFUSED (pre-swap): candidate still answering rpc $CANDIDATE_RPCPORT after stop+${STOP_GRACE}s — it is not stopped; refusing to rename a live datadir. Both units left stopped? Restarting canonical."
    unit_start "$CANONICAL_UNIT"
    exit 2
fi

# demote: preserve the old canonical (only if it exists — a failover from a
# canonical that never had a datadir is valid).
if [ -e "$CANONICAL_DATADIR" ]; then
    mv "$CANONICAL_DATADIR" "$DEMOTED_DIR"
    DEMOTED_DONE=1
    log "demoted old canonical -> $DEMOTED_DIR"
fi
# promote: candidate becomes the canonical datadir.
mv "$CANDIDATE_DATADIR" "$CANONICAL_DATADIR"
PROMOTED_DONE=1
log "promoted candidate -> $CANONICAL_DATADIR"

unit_start "$CANONICAL_UNIT" || rollback "canonical unit failed to start on the promoted datadir"

# ── verify: new canonical becomes ready AND reaches the pre-cutover bar ──────
log "verifying promoted canonical reaches H* >= $BAR within ${READY_TIMEOUT}s"
_deadline=$(( $(date +%s) + READY_TIMEOUT ))
NEW_H=-1
while [ "$(date +%s)" -lt "$_deadline" ]; do
    NEW_H="$(hstar canonical-post "$CANONICAL_RPCPORT" "$CANONICAL_DATADIR")"
    if [ "$NEW_H" -ge "$BAR" ] 2>/dev/null; then
        printf 'CUTOVER: PROMOTED  old_canonical_H*=%s candidate_H*=%s new_canonical_H*=%s (bar=%s, demoted=%s)\n' \
            "$CANON_H" "$CAND_H" "$NEW_H" "$BAR" "$DEMOTED_DIR"
        exit 0
    fi
    sleep "$POLL_INTERVAL"
done

rollback "promoted canonical did not reach H* >= $BAR within ${READY_TIMEOUT}s (last H*=$NEW_H)"
