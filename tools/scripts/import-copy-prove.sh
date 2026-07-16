#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# import-copy-prove.sh — copy-prove harness for the complete-shielded-history
# IMPORT path (docs/work/fast-sync-to-tip-plan-2026-07-16.md, "the operational
# fast route" — NOT the sovereign producer/bundle cure that
# tools/scripts/cure-copy-prove.sh proves). It is that script's sibling: same
# safety scaffolding (throwaway -COPY- datadir, cp -a only, ephemeral ports,
# never a live datadir), different install step and different verdict, because
# the import path is self_folded=false BY DESIGN (release_assisted trust, not
# sovereign) so it must NOT require coins_kv_self_folded the way
# cure-copy-prove.sh does.
#
# ============================================================================
# HEADER-REFRESH STEP (added 2026-07-16, before phase 1)
# ============================================================================
# A real diagnostic run against a wedged clone found the importer's tip-bind
# refusing with:
#   "tip bind FAILED: best Sapling anchor != expected header hashFinalSaplingRoot
#    at h=3151581 — refusing"
#   "Tip bind: height=3151581 hashFinalSaplingRoot=0000...0000"
# i.e. the clone's own blocks.sapling_root column was ALL-ZERO at the bind
# height because the clone's header chain was stale relative to zclassicd's
# current chainstate tip. Before phase 1 (the -import-complete-shielded call)
# this script now runs the proven read-only two-step recipe's step 1
# (docs/HANDOFF.md "legacy TWO-step recipe"):
#   "$NODE_BIN" --importblockindex "$ZD_DATADIR" "$COPY_DIR/node.db"
# against the COPY's node.db — refreshing the copy's SQLite `blocks` header
# chain (and blocks.sapling_root) from zclassicd's live LevelDB block index,
# read-only, same LOCK-avoidance as the existing recipe. Skippable with
# --skip-header-refresh; on failure the script FAILs (never silently proceeds
# with stale headers into phase 1).
# ============================================================================
#
# ============================================================================
# CONTRACT — read before running this for real
# ============================================================================
# As of 2026-07-16 the importer this script drives IS on main (merged from
# lane worktree-wf_38a8a7cc-2b5-1): the 6 chainstate_legacy_reader iterators,
# nullifier_kv_publish_full_replay_complete_in_tx, the importer service
# shielded_history_import_from_chainstate(), and the terminal boot flag below.
# (The optional `sovereignty` dumpstate subsystem, design doc section 5.5, is
# also on main via lane -2.) The importer's contract:
#   - a terminal boot flag, documented interface (this script's contract):
#       zclassic23 -datadir=TARGET-COPY -import-complete-shielded=ZCLASSICD-DATADIR
#     The importer reads ZCLASSICD-DATADIR/chainstate via its own FNV-stable
#     point-in-time snapshot (ldb_snapshot_make, manifest-changed retry —
#     mirrors --gen-utxo-snapshot), so it is handed the LIVE zclassicd datadir
#     directly and never a torn image; zclassicd keeps running untouched.
#     On success it prints, to stdout, exactly one line matching
#       ^IMPORT COMPLETE (committed=.*$
#     and exits 0. On any refusal it prints
#       ^IMPORT REFUSED .*$
#     and exits nonzero (wedge intact, both cursors stay positive). THIS
#     SCRIPT ENFORCES THAT EXACT CONTRACT (see phase 1 below): if a build
#     predates the importer, the argv loop silently ignores the unknown flag
#     and phase 1 FAILS with a message pointing back here — fail-closed, never
#     a false PASS from a plain exit-0.
#   - the `sovereignty` dumpstate check is treated as best-effort/soft (see
#     step 6) and never fails the verdict on its absence.
# ============================================================================
#
# What this proves, once the importer exists (design doc section 6):
#   (a) H* CLIMBS past the wedge height (default 3,176,325) and the
#       utxo_apply.anchor_backfill_gap / utxo_apply.nullifier_backfill_gap
#       blockers are both ABSENT from `dumpstate blocker`.
#   (b) coins_applied_height == hstar + 1 (continuity — the reducer is
#       folding forward from the wedge, not skipping/duplicating).
#   (c) EXACT same-height tip-block-hash parity against zclassicd at the
#       wedge height, wedge+1, and the copy's own final tip — the check
#       that actually catches a missing anchor/nullifier: a partial import
#       that wrongly flipped the activation cursor either re-wedges (caught
#       by (a)) or, worse, folds a DIFFERENT chain (only (c) catches that).
#
# This is an operational-readiness proof, not a sovereignty proof: it does
# NOT require (and does not check for) coins_kv_self_folded — see design doc
# section 5. Expect `trust_mode: release_assisted` after a real cutover, not
# `sovereign`. Use tools/scripts/cure-copy-prove.sh for the sovereign/bundle
# proof.
#
# Safety invariants (same as cure-copy-prove.sh):
#   - --copy-dir (or the generated default) MUST contain the literal
#     "/.zclassic-c23-COPY-" marker and MUST NOT equal a known live/protected
#     datadir path — refuses otherwise (mirrors
#     app/controllers/src/agent_copy_prove_controller.c's cp_path_safety_ok()).
#   - --copy-dir must not already exist (never silently overwrites/reuses).
#   - the live `~/.zclassic/chainstate` LevelDB is never opened directly:
#     the importer takes a FNV-stable point-in-time snapshot of it
#     (ldb_snapshot_make, manifest-changed retry) before iterating, so a
#     concurrent zclassicd write can never hand it a torn manifest.
#   - zclassicd itself is only ever queried over its existing RPC port
#     (read-only getblockhash calls) — never stopped, restarted, or written
#     to. Pass --skip-zclassicd-check to omit even that (loudly marks the
#     run as NOT gate-complete; see step 6/7 below) for e.g. a hermetic
#     fixture run with no zclassicd available.
#
# Usage:
#   tools/scripts/import-copy-prove.sh [--dry-run] \
#     [--src=DIR]                  (datadir to copy from; default: $HOME/.zclassic-c23)
#     [--chainstate-src=DIR]       (zclassicd chainstate LevelDB dir to copy from;
#                                   default: $HOME/.zclassic/chainstate)
#     [--copy-dir=DIR]             (default: $HOME/.zclassic-c23-COPY-<ts>-import)
#     [--expect-climb-past=H]      (required unless --dry-run; default 3176325 —
#                                   VERIFY against the live docs/HANDOFF.md wedge
#                                   height before trusting the default)
#     [--deadline=SECS]            (default 3600)
#     [--rpcport=N] [--p2p-port=N] [--fs-port=N] [--https-port=N]
#     [--zclassicd-datadir=DIR]    (default: $HOME/.zclassic — READ ONLY, used
#                                   solely to source RPC auth the same way
#                                   tools/zcl-rpc.c already does; never written)
#     [--zclassicd-rpcport=N]      (default 8232)
#     [--skip-zclassicd-check]     (skip gate (c); marks verdict PASS-INCOMPLETE)
#     [--skip-header-refresh]      (skip the pre-phase-1 --importblockindex header
#                                   refresh; the step is ON by default and FAILs
#                                   the run if it errors — pass this only if the
#                                   copy's headers are already known current)
#
# Exit codes: 0 PASS (or PASS-INCOMPLETE with --skip-zclassicd-check),
#             1 FAIL, 2 usage/precondition error.
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
NODE_BIN="${ZCL_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
RPC_BIN="${ZCL_RPC_BIN:-$REPO_ROOT/build/bin/zcl-rpc}"

SRC="$HOME/.zclassic-c23"
CHAINSTATE_SRC="$HOME/.zclassic/chainstate"
COPY_DIR=""
EXPECT_CLIMB_PAST=3176325
DEADLINE=3600
RPCPORT=18399
P2PPORT=19033
FSPORT=19034
HTTPSPORT=19035
ZD_DATADIR="$HOME/.zclassic"
ZD_RPCPORT=8232
SKIP_ZD_CHECK=0
SKIP_HEADER_REFRESH=0
DRY_RUN=0

usage() {
    sed -n '2,118p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)                DRY_RUN=1 ;;
        --src=*)                  SRC="${1#--src=}" ;;
        --chainstate-src=*)       CHAINSTATE_SRC="${1#--chainstate-src=}" ;;
        --copy-dir=*)             COPY_DIR="${1#--copy-dir=}" ;;
        --expect-climb-past=*)    EXPECT_CLIMB_PAST="${1#--expect-climb-past=}" ;;
        --deadline=*)             DEADLINE="${1#--deadline=}" ;;
        --rpcport=*)              RPCPORT="${1#--rpcport=}" ;;
        --p2p-port=*)             P2PPORT="${1#--p2p-port=}" ;;
        --fs-port=*)              FSPORT="${1#--fs-port=}" ;;
        --https-port=*)           HTTPSPORT="${1#--https-port=}" ;;
        --zclassicd-datadir=*)    ZD_DATADIR="${1#--zclassicd-datadir=}" ;;
        --zclassicd-rpcport=*)    ZD_RPCPORT="${1#--zclassicd-rpcport=}" ;;
        --skip-zclassicd-check)   SKIP_ZD_CHECK=1 ;;
        --skip-header-refresh)    SKIP_HEADER_REFRESH=1 ;;
        -h|--help)                usage; exit 0 ;;
        *) echo "import-copy-prove: unknown option $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# ── validation ──────────────────────────────────────────────────────────

case "$EXPECT_CLIMB_PAST" in
    ''|*[!0-9]*)
        echo "import-copy-prove: --expect-climb-past=H must be a non-negative integer" >&2
        exit 2 ;;
esac

if [ -z "$COPY_DIR" ]; then
    COPY_DIR="$HOME/.zclassic-c23-COPY-$(date +%Y%m%d%H%M%S)-import"
fi

# Safety invariant: never a live datadir, always the throwaway marker. Same
# rule as cure-copy-prove.sh / agent_copy_prove_controller.c cp_path_safety_ok().
case "$COPY_DIR" in
    *"/.zclassic-c23-COPY-"*) : ;;
    *) echo "import-copy-prove: REFUSED — --copy-dir must contain the '/.zclassic-c23-COPY-' marker: $COPY_DIR" >&2
       exit 2 ;;
esac
for live in "$HOME/.zclassic-c23" "$HOME/.zclassic-c23-dev" "$HOME/.zclassic-c23-soak" \
            "$HOME/.zclassic-c23-mint" "$HOME/.zclassic-c23-mint-receipt" "$HOME/.zclassic"; do
    if [ "$COPY_DIR" = "$live" ]; then
        echo "import-copy-prove: REFUSED — --copy-dir aliases a known live/protected datadir: $COPY_DIR" >&2
        exit 2
    fi
done
if [ -e "$COPY_DIR" ]; then
    echo "import-copy-prove: REFUSED — --copy-dir already exists, refusing to overwrite: $COPY_DIR" >&2
    exit 2
fi

if [ "$DRY_RUN" = "0" ]; then
    [ -d "$SRC" ] || { echo "import-copy-prove: source datadir not found: $SRC" >&2; exit 2; }
    [ -d "$CHAINSTATE_SRC" ] || { echo "import-copy-prove: chainstate source not found: $CHAINSTATE_SRC" >&2; exit 2; }
    [ -x "$NODE_BIN" ] || { echo "import-copy-prove: $NODE_BIN not built (run make build-only)" >&2; exit 2; }
    [ -x "$RPC_BIN" ]  || { echo "import-copy-prove: $RPC_BIN not built (run make zcl-rpc)" >&2; exit 2; }
    if [ "$SKIP_ZD_CHECK" = "0" ]; then
        [ -d "$ZD_DATADIR" ] || { echo "import-copy-prove: --zclassicd-datadir not found: $ZD_DATADIR (or pass --skip-zclassicd-check)" >&2; exit 2; }
    fi
fi

# ── plan (always printed) ──────────────────────────────────────────────

echo "======================================================================"
echo "  import-copy-prove plan  (operational IMPORT path, NOT the sovereign bundle cure)"
echo "  src (cp -a from):          $SRC"
echo "  chainstate_src (cp -a from): $CHAINSTATE_SRC"
echo "  copy_dir (must not pre-exist): $COPY_DIR"
echo "  expect_climb_past:         $EXPECT_CLIMB_PAST  (verify vs live docs/HANDOFF.md wedge height)"
echo "  deadline_secs:             $DEADLINE"
echo "  node_bin:                  $NODE_BIN"
echo "  ports:                     rpc=$RPCPORT p2p=$P2PPORT fs=$FSPORT https=$HTTPSPORT"
echo "  zclassicd cross-check:     $([ "$SKIP_ZD_CHECK" = "1" ] && echo 'SKIPPED (--skip-zclassicd-check; verdict will be PASS-INCOMPLETE at best)' || echo "datadir=$ZD_DATADIR rpcport=$ZD_RPCPORT (read-only RPC only)")"
echo "  header refresh:            $([ "$SKIP_HEADER_REFRESH" = "1" ] && echo 'SKIPPED (--skip-header-refresh)' || echo 'ON (default) — fails the run on error, never proceeds with stale headers')"
echo "  steps:"
echo "    1. cp -a \"\$SRC\" \"\$COPY_DIR\"  (throwaway wedged target datadir)"
echo "    1b. header refresh (unless --skip-header-refresh):"
echo "       \"\$NODE_BIN\" --importblockindex \"\$ZD_DATADIR\" \"\$COPY_DIR/node.db\""
echo "       (proven read-only recipe step 1 — refreshes the copy's header chain"
echo "       + blocks.sapling_root before phase 1; see the HEADER-REFRESH STEP"
echo "       notice at the top of this script)"
echo "    2. \"\$NODE_BIN\" -datadir=\"\$COPY_DIR\" -import-complete-shielded=\"\$ZD_DATADIR\""
echo "       (terminal; the importer FNV-stable-snapshots \$ZD_DATADIR/chainstate"
echo "       itself — zclassicd is read-only, never stopped; require literal"
echo "       'IMPORT COMPLETE (committed=' banner + exit 0 — SEE THE DEPENDENCY"
echo "       NOTICE at the top of this script if this flag does not exist yet)"
echo "    3b. clear any stale \$COPY_DIR/auto_reindex_request left over from the"
echo "       wedged source datadir — never let the proving boot silently"
echo "       detour into -reindex-chainstate instead of exercising the import"
echo "    4. \"\$NODE_BIN\" -datadir=\"\$COPY_DIR\" -rpcport=\$RPCPORT -port=\$P2PPORT"
echo "       -connect=127.0.0.1:39999 -nolegacyimport -nofilesync (isolated boot)"
echo "    5. poll getblockcount + dumpstate reducer_frontier/blocker until"
echo "       height > \$EXPECT_CLIMB_PAST or deadline; GATE (a)"
echo "    6. GATE (b): coins_applied_height == hstar + 1"
echo "    7. GATE (c): getblockhash at wedge / wedge+1 / final tip on the copy =="
echo "       getblockhash at the same heights on zclassicd (unless --skip-zclassicd-check)"
echo "    8. print PASS/FAIL with full evidence; kill the copy's node"
echo "======================================================================"

if [ "$DRY_RUN" = "1" ]; then
    echo "[import-copy-prove] --dry-run: no filesystem or process action taken."
    exit 0
fi

RUN_START_EPOCH=$(date +%s)

# ── step 1: copy the datadir ────────────────────────────────────────────

echo "[import-copy-prove] cp -a $SRC -> $COPY_DIR"
cp -a "$SRC" "$COPY_DIR"
rm -f "$COPY_DIR"/*.pid "$COPY_DIR"/.lock 2>/dev/null || true

# ── step 2: NO manual chainstate copy ──────────────────────────────────
# The importer reads $ZD_DATADIR/chainstate through its OWN FNV-stable
# point-in-time snapshot (ldb_snapshot_make with manifest-changed retry,
# src/main.c import_complete_shielded_mode), which is strictly tear-safe —
# a plain `cp -a` of the live, actively-written LevelDB could hand it a torn
# manifest (the 2026-06-09 bug class). We therefore pass the live zclassicd
# datadir directly and let the importer make the stable copy; zclassicd is
# only ever read, never stopped.

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

# ── step 1b: header refresh (default ON, before phase 1) ───────────────
# See the HEADER-REFRESH STEP notice at the top of this script: the
# 2026-07-16 tip-bind refusal ("hashFinalSaplingRoot=0000...0000") is caused
# by the copy's own header chain (and blocks.sapling_root) being stale
# relative to zclassicd's current chainstate tip. Refresh it here, read-only,
# via the proven two-step recipe's step 1, BEFORE the importer runs. Never
# proceeds into phase 1 with headers we know are stale: any failure here
# FAILs the whole run.
if [ "$SKIP_HEADER_REFRESH" = "0" ]; then
    HEADER_REFRESH_LOG="$COPY_DIR/header_refresh.log"
    echo "[import-copy-prove] step 1b: header refresh — $NODE_BIN --importblockindex $ZD_DATADIR $COPY_DIR/node.db"
    href_rc=0
    HOME="$ISO_HOME" "$NODE_BIN" --importblockindex "$ZD_DATADIR" "$COPY_DIR/node.db" \
        > "$HEADER_REFRESH_LOG" 2>&1 || href_rc=$?
    if [ "$href_rc" != "0" ]; then
        echo "======================================================================"
        echo "  import-copy-prove VERDICT: FAIL"
        echo "  copy:    $COPY_DIR"
        echo "  reason:  header-refresh (--importblockindex) failed (exit=$href_rc)."
        echo "           Inspect $HEADER_REFRESH_LOG. Phase 1 (-import-complete-shielded)"
        echo "           was NOT run: proceeding with headers known to be stale risks"
        echo "           reproducing the 2026-07-16 tip-bind refusal"
        echo "           (hashFinalSaplingRoot=0000...0000) rather than exercising a"
        echo "           genuine anchor/nullifier gap. Pass --skip-header-refresh only"
        echo "           if the copy's headers are already known current by other means."
        echo "======================================================================"
        exit 1
    fi
    echo "[import-copy-prove] header refresh done — copy's headers now cover zclassicd's tip"
else
    echo "[import-copy-prove] step 1b SKIPPED (--skip-header-refresh) — copy's headers are assumed already current"
fi

# ── step 3: terminal import call (phase 1) ─────────────────────────────

IMPORT_LOG="$COPY_DIR/import_shielded_history.log"
echo "[import-copy-prove] phase 1: terminal -import-complete-shielded=$ZD_DATADIR (timeout ${DEADLINE}s)"
import_rc=0
if command -v timeout >/dev/null 2>&1; then
    HOME="$ISO_HOME" timeout "${DEADLINE}s" \
        "$NODE_BIN" -datadir="$COPY_DIR" -rpcport="$RPCPORT" -port="$P2PPORT" \
        $NODE_ISO_ARGS -import-complete-shielded="$ZD_DATADIR" \
        > "$IMPORT_LOG" 2>&1 || import_rc=$?
else
    HOME="$ISO_HOME" \
        "$NODE_BIN" -datadir="$COPY_DIR" -rpcport="$RPCPORT" -port="$P2PPORT" \
        $NODE_ISO_ARGS -import-complete-shielded="$ZD_DATADIR" \
        > "$IMPORT_LOG" 2>&1 || import_rc=$?
fi

if [ "$import_rc" != "0" ] || ! grep -q '^IMPORT COMPLETE (committed=' "$IMPORT_LOG"; then
    echo "======================================================================"
    echo "  import-copy-prove VERDICT: FAIL"
    echo "  copy:    $COPY_DIR"
    echo "  reason:  terminal -import-complete-shielded did not report IMPORT COMPLETE"
    echo "           (exit=$import_rc). Inspect $IMPORT_LOG. Nothing further was booted."
    echo "  NOTE:    if $IMPORT_LOG shows no mention of -import-complete-shielded at all,"
    echo "           the importer (docs/work/fast-sync-to-tip-plan-2026-07-16.md section 4)"
    echo "           is not yet merged on this build — see the DEPENDENCY NOTICE at the"
    echo "           top of this script. The argv loop silently ignores unknown flags, so"
    echo "           an unimplemented flag looks like a normal (still-wedged) boot, not a"
    echo "           crash — this is exactly why phase 1 requires the explicit banner"
    echo "           instead of trusting a plain exit-0."
    echo "======================================================================"
    exit 1
fi
echo "[import-copy-prove] import reported IMPORT COMPLETE — booting normally to prove H* CLIMB"

# ── step 3b: clear any stale auto_reindex_request ──────────────────────
# The copy was cp -a'd from a wedged datadir; if that datadir had ever armed
# a self-rebuild request (config/src/boot_crashonly.c
# boot_crashonly_consume_reindex_request(), storage/boot_auto_reindex.h) the
# sentinel file <datadir>/auto_reindex_request rides along in the copy.
# import_complete_shielded_mode() is a separate terminal argv path (src/main.c)
# that returns before the normal boot sequence ever runs, so phase 1 above
# does NOT consume or clear it — left in place, the very next normal boot
# (phase 2, right below) would silently detour into -reindex-chainstate
# instead of exercising the state this harness just imported, which would
# either make GATE (a)'s climb take far longer than --deadline expects or
# (worse) mask whether the IMPORT itself, rather than a full block replay,
# is what carried the copy past the wedge. Clear it unconditionally here —
# it is a top-level sentinel, never part of derived state, and the whole
# point of this run is to prove the FRESH import; a rebuild-from-blocks
# would prove something else.
if [ -e "$COPY_DIR/auto_reindex_request" ]; then
    echo "[import-copy-prove] step 3b: stale auto_reindex_request found in the copy — clearing before boot"
    rm -f "$COPY_DIR/auto_reindex_request"
else
    echo "[import-copy-prove] step 3b: no stale auto_reindex_request present — nothing to clear"
fi

# ── step 4: normal boot (phase 2) ──────────────────────────────────────

HOME="$ISO_HOME" ZCL_MIRROR_SYNC=0 \
"$NODE_BIN" -datadir="$COPY_DIR" -rpcport="$RPCPORT" -port="$P2PPORT" \
    $NODE_ISO_ARGS \
    > "$COPY_DIR/import_node.log" 2>&1 &
NODE_PID=$!

rpc() { HOME="$ISO_HOME" ZCL_DATADIR="$COPY_DIR" ZCL_RPCPORT="$RPCPORT" "$RPC_BIN" "$@" 2>/dev/null || true; }
tip() {
    resp="$(rpc getblockcount)"
    printf '%s\n' "$resp" |
        sed -n 's/.*"result"[[:space:]]*:[[:space:]]*\(-\{0,1\}[0-9][0-9]*\).*/\1/p' | head -1
}
copy_blockhash() {
    resp="$(rpc getblockhash "$1")"
    printf '%s\n' "$resp" |
        sed -n 's/.*"result"[[:space:]]*:[[:space:]]*"\([0-9a-fA-F]*\)".*/\1/p' | head -1
}
zd_rpc() { HOME="$ISO_HOME" ZCL_DATADIR="$ZD_DATADIR" ZCL_RPCPORT="$ZD_RPCPORT" "$RPC_BIN" "$@" 2>/dev/null || true; }
zd_blockhash() {
    resp="$(zd_rpc getblockhash "$1")"
    printf '%s\n' "$resp" |
        sed -n 's/.*"result"[[:space:]]*:[[:space:]]*"\([0-9a-fA-F]*\)".*/\1/p' | head -1
}
zd_blockcount() {
    resp="$(zd_rpc getblockcount)"
    printf '%s\n' "$resp" |
        sed -n 's/.*"result"[[:space:]]*:[[:space:]]*\(-\{0,1\}[0-9][0-9]*\).*/\1/p' | head -1
}

# ── step 5: poll for H* CLIMB (GATE a, part 1) ─────────────────────────

deadline_epoch=$(( $(date +%s) + DEADLINE ))
max_tip=-1
first_tip=-1
climbed_past=0
cookie="$COPY_DIR/.cookie"
while [ "$(date +%s)" -lt "$deadline_epoch" ]; do
    if ! kill -0 "$NODE_PID" 2>/dev/null; then
        echo "[import-copy-prove] node exited early (see $COPY_DIR/import_node.log)"
        break
    fi
    if [ -f "$cookie" ]; then
        t="$(tip)"; t="${t:--1}"
        if [ "$t" -ge 0 ] 2>/dev/null; then
            [ "$first_tip" -lt 0 ] && first_tip="$t" && echo "[import-copy-prove] first tip: $t"
            [ "$t" -gt "$max_tip" ] && max_tip="$t"
            if [ "$t" -gt "$EXPECT_CLIMB_PAST" ] 2>/dev/null; then
                climbed_past=1
                echo "[import-copy-prove] H* CLIMBED to $t (> $EXPECT_CLIMB_PAST)"
                break
            fi
        fi
    fi
    sleep 5
done

# ── GATE (a) part 2: both backfill-gap blockers absent ─────────────────

blocker_json="$(rpc dumpstate '"blocker"')"
anchor_gap_present=0
nullifier_gap_present=0
printf '%s' "$blocker_json" | grep -q '"id":"utxo_apply\.anchor_backfill_gap"' && anchor_gap_present=1
printf '%s' "$blocker_json" | grep -q '"id":"utxo_apply\.nullifier_backfill_gap"' && nullifier_gap_present=1
blockers_clear=0
[ "$anchor_gap_present" = "0" ] && [ "$nullifier_gap_present" = "0" ] && blockers_clear=1

gate_a_ok=0
[ "$climbed_past" = "1" ] && [ "$blockers_clear" = "1" ] && gate_a_ok=1

# ── GATE (b): coins_applied_height == hstar + 1 ────────────────────────

frontier_json="$(rpc dumpstate '"reducer_frontier"')"
coins_applied="$(printf '%s' "$frontier_json" | sed -n 's/.*"coins_applied_height":\([0-9-]*\).*/\1/p' | head -1)"
hstar_now="$(printf '%s' "$frontier_json" | sed -n 's/.*"hstar":\([0-9-]*\).*/\1/p' | head -1)"
gate_b_ok=0
if [ -n "${coins_applied:-}" ] && [ -n "${hstar_now:-}" ] && \
   [ "$coins_applied" = "$((hstar_now + 1))" ] 2>/dev/null; then
    gate_b_ok=1
fi

# ── GATE (c): exact same-height tip-hash parity vs zclassicd ───────────

wedge_h="$EXPECT_CLIMB_PAST"
wedge_plus1_h=$((EXPECT_CLIMB_PAST + 1))
tip_h="$max_tip"

gate_c_ok=0
gate_c_note="skipped (--skip-zclassicd-check)"
if [ "$SKIP_ZD_CHECK" = "0" ] && kill -0 "$NODE_PID" 2>/dev/null; then
    zd_tip="$(zd_blockcount)"; zd_tip="${zd_tip:--1}"
    if [ "${zd_tip:-'-1'}" -ge 0 ] 2>/dev/null && [ "$tip_h" -ge 0 ] 2>/dev/null && \
       [ "$zd_tip" -ge "$tip_h" ] 2>/dev/null; then
        match_count=0
        checked_count=0
        for h in "$wedge_h" "$wedge_plus1_h" "$tip_h"; do
            [ "$h" -ge 0 ] 2>/dev/null || continue
            checked_count=$((checked_count + 1))
            copy_hash="$(copy_blockhash "$h")"
            zd_hash="$(zd_blockhash "$h")"
            if [ -n "$copy_hash" ] && [ -n "$zd_hash" ] && [ "$copy_hash" = "$zd_hash" ]; then
                match_count=$((match_count + 1))
                echo "[import-copy-prove] height $h: copy=$copy_hash zclassicd=$zd_hash MATCH"
            else
                echo "[import-copy-prove] height $h: copy=${copy_hash:-<empty>} zclassicd=${zd_hash:-<empty>} MISMATCH"
            fi
        done
        if [ "$checked_count" -gt 0 ] && [ "$match_count" = "$checked_count" ]; then
            gate_c_ok=1
            gate_c_note="$match_count/$checked_count heights matched (wedge=$wedge_h wedge+1=$wedge_plus1_h tip=$tip_h)"
        else
            gate_c_note="$match_count/$checked_count heights matched — MISMATCH is the missing-anchor/nullifier signature"
        fi
    else
        gate_c_note="zclassicd unreachable or behind the copy's tip (zd_tip=${zd_tip:-<none>}, copy tip=$tip_h) — cannot cross-check"
    fi
fi

# ── best-effort sovereignty posture (never fails the verdict) ──────────

sovereignty_json="$(rpc dumpstate '"sovereignty"')"
if printf '%s' "$sovereignty_json" | grep -q '"trust_mode"'; then
    trust_mode="$(printf '%s' "$sovereignty_json" | sed -n 's/.*"trust_mode":"\([^"]*\)".*/\1/p' | head -1)"
    sovereignty_note="trust_mode=${trust_mode:-<unparsed>} (expected release_assisted after a real import)"
else
    sovereignty_note="'sovereignty' dumpstate subsystem not registered on this build (design doc section 5.5, not yet implemented) — informational only"
fi

DURATION_SECS=$(( $(date +%s) - RUN_START_EPOCH ))

echo "======================================================================"
if [ "$gate_a_ok" = "1" ] && [ "$gate_b_ok" = "1" ] && [ "$gate_c_ok" = "1" ]; then
    verdict="PASS"
    rc=0
elif [ "$gate_a_ok" = "1" ] && [ "$gate_b_ok" = "1" ] && [ "$SKIP_ZD_CHECK" = "1" ]; then
    verdict="PASS-INCOMPLETE (gate c skipped — NOT sufficient for a live cutover decision)"
    rc=0
else
    verdict="FAIL"
    rc=1
fi
echo "  import-copy-prove VERDICT: $verdict"
echo "  copy:                    $COPY_DIR"
echo "  first_tip:               $first_tip"
echo "  max_tip:                 $max_tip"
echo "  expect_climb_past:       $EXPECT_CLIMB_PAST"
echo "  GATE (a) H* climb:       climbed_past=$climbed_past blockers_clear=$blockers_clear (anchor_gap_present=$anchor_gap_present nullifier_gap_present=$nullifier_gap_present) ok=$gate_a_ok"
echo "  GATE (b) continuity:     coins_applied_height=$coins_applied hstar=$hstar_now (want coins_applied==hstar+1) ok=$gate_b_ok"
echo "  GATE (c) hash parity:    $gate_c_note  ok=$gate_c_ok"
echo "  sovereignty (info only): $sovereignty_note"
echo "  duration_secs:           $DURATION_SECS"
echo "  import_log:              $IMPORT_LOG"
echo "  node_log:                $COPY_DIR/import_node.log"
if [ "$verdict" = "FAIL" ]; then
    echo "  NOTE: this is a false-green trap if you only look at climbed_past —"
    echo "        the gate requires (a) AND (b) AND (c). A partial import that"
    echo "        flipped the activation cursor while the historical set was"
    echo "        actually incomplete shows up EXACTLY as a GATE (c) mismatch"
    echo "        (folds a different chain) or a GATE (a) re-wedge — never as a"
    echo "        silent pass. Do not cut over to the live datadir on this result."
fi
echo "======================================================================"
exit "$rc"
