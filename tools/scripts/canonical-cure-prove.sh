#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# canonical-cure-prove.sh — ONE turnkey command for the FULL verified
# canonical-copy -> tip flow: copy a (wedged) canonical-shaped datadir,
# refresh its headers, run the fast complete-shielded-history importer
# (docs/work/fast-sync-to-tip-plan-2026-07-16.md), clear anything that would
# silently divert the proving boot, boot the copy isolated but P2P-connected
# to zclassicd for any tail block bodies, and GATE on H* CLIMB past the
# pre-import wedge height. This is the sibling of tools/scripts/
# import-copy-prove.sh (narrower 3-gate operational proof, network-isolated)
# and tools/scripts/cure-copy-prove.sh (the sovereign bundle-install cure) —
# this script is the "does it actually reach tip against the real zclassicd
# peer" turnkey rehearsal, single command, always self-cleaning.
#
# ============================================================================
# CONTRACT
# ============================================================================
# Phase 1 (import) uses the same terminal flag as import-copy-prove.sh:
#   zclassic23 -datadir=COPY -import-complete-shielded=ZCLASSICD-DATADIR
# On success it prints, to stdout, exactly one line matching
#   ^IMPORT COMPLETE \(committed=.*$
# and exits 0; the node's LOG_INFO line (captured via 2>&1) additionally
# carries "boundary=<height>" — the copy's own wedge height, i.e. the height
# BELOW which shielded history was backfilled and the reducer resumes folding
# from boundary+1. This script parses that boundary automatically so the
# operator does not have to know or pass the wedge height by hand; pass
# --expect-climb-past=H to override the parsed value (e.g. against a build
# whose importer does not yet log "boundary=").
#
# On any refusal phase 1 prints "^IMPORT REFUSED .*$" and exits nonzero
# (wedge intact) — this script enforces the exact banner contract, never
# trusting a plain exit 0 (an unmerged flag is silently ignored by the argv
# loop and would otherwise look like a normal, still-wedged boot).
# ============================================================================
#
# Safety invariants (same discipline as the sibling scripts):
#   - --copy-dir (or the generated default) MUST contain the literal
#     "/.zclassic-c23-COPY-" marker and MUST NOT equal a known live/protected
#     datadir path — refuses otherwise (mirrors
#     app/controllers/src/agent_copy_prove_controller.c's cp_path_safety_ok()).
#   - --copy-dir must not already exist (never silently overwrites/reuses).
#   - zclassicd is only ever read: its chainstate is imported via the
#     binary's own FNV-stable point-in-time snapshot (never a raw `cp -a` of
#     a live LevelDB), and its P2P port is only ever an outbound -addnode
#     target for the copy — zclassicd itself is never stopped/restarted.
#   - the copy (and everything under it, including the importer's own
#     ldb_snapshot workspace) is ALWAYS deleted on exit — PASS, FAIL, or
#     interrupted — unless --keep-copy is passed for post-mortem inspection.
#
# Usage:
#   tools/scripts/canonical-cure-prove.sh [--dry-run] \
#     [--src=DIR]                  (datadir to copy from; default: $HOME/.zclassic-c23)
#     [--copy-dir=DIR]             (default: $HOME/.zclassic-c23-COPY-<ts>-canonical;
#                                   must contain the "-COPY-" marker)
#     [--zclassicd-datadir=DIR]    (default: $HOME/.zclassic — READ ONLY)
#     [--zclassicd-rpcport=N]      (default 8232; read-only getblockcount only)
#     [--zclassicd-p2p=HOST:PORT] (default 127.0.0.1:8034 — outbound -addnode
#                                   target so the copy can fetch tail bodies)
#     [--expect-climb-past=H]      (override the auto-parsed import boundary)
#     [--deadline=SECS]            (default 3600)
#     [--rpcport=N] [--p2p-port=N] [--fs-port=N] [--https-port=N]
#     [--keep-copy]                (skip the end-of-run cleanup of --copy-dir;
#                                   default is to ALWAYS delete it on exit)
#
# Exit codes: 0 PASS, 1 FAIL (H* did not climb past the wedge), 2 usage/
#             precondition error.
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
NODE_BIN="${ZCL_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
RPC_BIN="${ZCL_RPC_BIN:-$REPO_ROOT/build/bin/zcl-rpc}"

SRC="$HOME/.zclassic-c23"
COPY_DIR=""
ZD_DATADIR="$HOME/.zclassic"
ZD_RPCPORT=8232
ZD_P2P="127.0.0.1:8034"
EXPECT_CLIMB_PAST=""
DEADLINE=3600
RPCPORT=18499
P2PPORT=19133
FSPORT=19134
HTTPSPORT=19135
KEEP_COPY=0
DRY_RUN=0

usage() {
    sed -n '2,66p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)                DRY_RUN=1 ;;
        --src=*)                  SRC="${1#--src=}" ;;
        --copy-dir=*)             COPY_DIR="${1#--copy-dir=}" ;;
        --zclassicd-datadir=*)    ZD_DATADIR="${1#--zclassicd-datadir=}" ;;
        --zclassicd-rpcport=*)    ZD_RPCPORT="${1#--zclassicd-rpcport=}" ;;
        --zclassicd-p2p=*)        ZD_P2P="${1#--zclassicd-p2p=}" ;;
        --expect-climb-past=*)    EXPECT_CLIMB_PAST="${1#--expect-climb-past=}" ;;
        --deadline=*)             DEADLINE="${1#--deadline=}" ;;
        --rpcport=*)              RPCPORT="${1#--rpcport=}" ;;
        --p2p-port=*)             P2PPORT="${1#--p2p-port=}" ;;
        --fs-port=*)              FSPORT="${1#--fs-port=}" ;;
        --https-port=*)           HTTPSPORT="${1#--https-port=}" ;;
        --keep-copy)              KEEP_COPY=1 ;;
        -h|--help)                usage; exit 0 ;;
        *) echo "canonical-cure-prove: unknown option $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# ── validation ──────────────────────────────────────────────────────────

if [ -n "$EXPECT_CLIMB_PAST" ]; then
    case "$EXPECT_CLIMB_PAST" in
        ''|*[!0-9]*)
            echo "canonical-cure-prove: --expect-climb-past=H must be a non-negative integer" >&2
            exit 2 ;;
    esac
fi

if [ -z "$COPY_DIR" ]; then
    COPY_DIR="$HOME/.zclassic-c23-COPY-$(date +%Y%m%d%H%M%S)-canonical"
fi

# Safety invariant: never a live datadir, always the throwaway marker. Same
# rule as the sibling scripts / agent_copy_prove_controller.c cp_path_safety_ok().
case "$COPY_DIR" in
    *"/.zclassic-c23-COPY-"*) : ;;
    *) echo "canonical-cure-prove: REFUSED — --copy-dir must contain the '/.zclassic-c23-COPY-' marker: $COPY_DIR" >&2
       exit 2 ;;
esac
for live in "$HOME/.zclassic-c23" "$HOME/.zclassic-c23-dev" "$HOME/.zclassic-c23-soak" \
            "$HOME/.zclassic-c23-mint" "$HOME/.zclassic-c23-mint-receipt" "$HOME/.zclassic"; do
    if [ "$COPY_DIR" = "$live" ]; then
        echo "canonical-cure-prove: REFUSED — --copy-dir aliases a known live/protected datadir: $COPY_DIR" >&2
        exit 2
    fi
done
if [ -e "$COPY_DIR" ]; then
    echo "canonical-cure-prove: REFUSED — --copy-dir already exists, refusing to overwrite: $COPY_DIR" >&2
    exit 2
fi

if [ "$DRY_RUN" = "0" ]; then
    [ -d "$SRC" ]        || { echo "canonical-cure-prove: source datadir not found: $SRC" >&2; exit 2; }
    [ -d "$ZD_DATADIR" ] || { echo "canonical-cure-prove: --zclassicd-datadir not found: $ZD_DATADIR" >&2; exit 2; }
    [ -x "$NODE_BIN" ]   || { echo "canonical-cure-prove: $NODE_BIN not built (run make build-only)" >&2; exit 2; }
    [ -x "$RPC_BIN" ]    || { echo "canonical-cure-prove: $RPC_BIN not built (run make zcl-rpc)" >&2; exit 2; }
fi

# ── plan (always printed) ──────────────────────────────────────────────

echo "======================================================================"
echo "  canonical-cure-prove plan  (turnkey canonical-copy -> tip proof)"
echo "  src (cp -a from):        $SRC"
echo "  copy_dir (must not pre-exist): $COPY_DIR"
echo "  zclassicd datadir:       $ZD_DATADIR (read-only; import + header-refresh source)"
echo "  zclassicd rpcport:       $ZD_RPCPORT (read-only getblockcount only)"
echo "  zclassicd p2p (tail):    $ZD_P2P (outbound -addnode from the copy)"
echo "  expect_climb_past:       ${EXPECT_CLIMB_PAST:-<auto-parsed from the import boundary=>}"
echo "  deadline_secs:           $DEADLINE"
echo "  node_bin:                $NODE_BIN"
echo "  ports:                   rpc=$RPCPORT p2p=$P2PPORT fs=$FSPORT https=$HTTPSPORT"
echo "  cleanup:                 $([ "$KEEP_COPY" = "1" ] && echo '--keep-copy: copy left on disk for inspection' || echo 'copy ALWAYS deleted on exit (pass, fail, or interrupt)')"
echo "  steps:"
echo "    1. cp -a \"\$SRC\" \"\$COPY_DIR\" (must carry the '-COPY-' marker; refuses a live datadir)"
echo "    2. header refresh (REQUIRED): \"\$NODE_BIN\" --importblockindex \"\$ZD_DATADIR\" \"\$COPY_DIR/node.db\""
echo "       (argv[1] header-only form — covers zclassicd's chainstate best block"
echo "       and populates blocks.sapling_root so the importer's tip bind can succeed)"
echo "    3. \"\$NODE_BIN\" -datadir=\"\$COPY_DIR\" -import-complete-shielded=\"\$ZD_DATADIR\""
echo "       (terminal; require literal 'IMPORT COMPLETE (committed=' + exit 0;"
echo "       boundary= is parsed from the combined log for the H* CLIMB gate)"
echo "    4. clear any stale \$COPY_DIR/auto_reindex_request left over from the"
echo "       wedged source datadir before the proving boot"
echo "    5. isolated boot with -addnode=\$ZD_P2P (unique ports; only outbound"
echo "       peer is zclassicd, for any tail block bodies past the copy's own"
echo "       stored blocks/)"
echo "    6. GATE on H* CLIMB: dumpstate reducer_frontier hstar AND"
echo "       coins_applied_height must both exceed the pre-import wedge height,"
echo "       or the run FAILs at --deadline"
echo "    7. print PASS/FAIL with full evidence; kill the copy's node; delete"
echo "       the copy (unless --keep-copy)"
echo "======================================================================"

if [ "$DRY_RUN" = "1" ]; then
    echo "[canonical-cure-prove] --dry-run: no filesystem or process action taken."
    exit 0
fi

RUN_START_EPOCH=$(date +%s)

# ── step 1: copy the datadir ────────────────────────────────────────────

echo "[canonical-cure-prove] cp -a $SRC -> $COPY_DIR"
cp -a "$SRC" "$COPY_DIR"
rm -f "$COPY_DIR"/*.pid "$COPY_DIR"/.lock 2>/dev/null || true

cleanup() {
    if [ -n "${NODE_PID:-}" ] && kill -0 "$NODE_PID" 2>/dev/null; then
        kill -TERM "$NODE_PID" 2>/dev/null || true
        sleep 2
        kill -KILL "$NODE_PID" 2>/dev/null || true
    fi
    if [ "$KEEP_COPY" = "0" ] && [ -e "$COPY_DIR" ]; then
        # Re-assert the safety invariant right before an rm -rf: never
        # delete anything that does not carry the throwaway marker, no
        # matter how COPY_DIR got mutated during the run.
        case "$COPY_DIR" in
            *"/.zclassic-c23-COPY-"*)
                echo "[canonical-cure-prove] cleanup: rm -rf $COPY_DIR (pass --keep-copy to retain it)"
                rm -rf "$COPY_DIR"
                ;;
            *)
                echo "[canonical-cure-prove] cleanup: REFUSING to rm -rf $COPY_DIR — lost the '-COPY-' marker, leaving it in place for manual review" >&2
                ;;
        esac
    elif [ "$KEEP_COPY" = "1" ]; then
        echo "[canonical-cure-prove] cleanup: --keep-copy — leaving $COPY_DIR on disk"
    fi
}
trap cleanup EXIT INT TERM

ISO_HOME="$COPY_DIR/.isolated-home"
mkdir -p "$ISO_HOME"
[ -e "$HOME/.zcash-params" ] && ln -sfn "$HOME/.zcash-params" "$ISO_HOME/.zcash-params"

# ── step 2: header refresh (REQUIRED — no skip flag) ────────────────────
# Same proven read-only recipe step as the sibling scripts (docs/HANDOFF.md
# "legacy TWO-step recipe" step 1): refreshes the copy's SQLite `blocks`
# header chain (and blocks.sapling_root) from zclassicd's live LevelDB block
# index, read-only. Required, not optional, because a stale header chain is
# exactly what produces the "hashFinalSaplingRoot=0000...0000" tip-bind
# refusal — this turnkey script never proceeds into phase 1 without it.

HEADER_REFRESH_LOG="$COPY_DIR/header_refresh.log"
echo "[canonical-cure-prove] step 2: header refresh — $NODE_BIN --importblockindex $ZD_DATADIR $COPY_DIR/node.db"
href_rc=0
HOME="$ISO_HOME" "$NODE_BIN" --importblockindex "$ZD_DATADIR" "$COPY_DIR/node.db" \
    > "$HEADER_REFRESH_LOG" 2>&1 || href_rc=$?
if [ "$href_rc" != "0" ]; then
    echo "======================================================================"
    echo "  canonical-cure-prove VERDICT: FAIL"
    echo "  copy:    $COPY_DIR"
    echo "  reason:  header-refresh (--importblockindex) failed (exit=$href_rc)."
    echo "           Inspect $HEADER_REFRESH_LOG before the copy is deleted."
    echo "           Phase 1 (-import-complete-shielded) was NOT run."
    echo "======================================================================"
    exit 1
fi
echo "[canonical-cure-prove] header refresh done — copy's headers now cover zclassicd's tip"

# ── step 3: terminal import call ────────────────────────────────────────

IMPORT_LOG="$COPY_DIR/import_shielded_history.log"
echo "[canonical-cure-prove] step 3: terminal -import-complete-shielded=$ZD_DATADIR (timeout ${DEADLINE}s)"
import_rc=0
if command -v timeout >/dev/null 2>&1; then
    HOME="$ISO_HOME" timeout "${DEADLINE}s" \
        "$NODE_BIN" -datadir="$COPY_DIR" -import-complete-shielded="$ZD_DATADIR" \
        > "$IMPORT_LOG" 2>&1 || import_rc=$?
else
    HOME="$ISO_HOME" \
        "$NODE_BIN" -datadir="$COPY_DIR" -import-complete-shielded="$ZD_DATADIR" \
        > "$IMPORT_LOG" 2>&1 || import_rc=$?
fi

if [ "$import_rc" != "0" ] || ! grep -q '^IMPORT COMPLETE (committed=' "$IMPORT_LOG"; then
    echo "======================================================================"
    echo "  canonical-cure-prove VERDICT: FAIL"
    echo "  copy:    $COPY_DIR"
    echo "  reason:  terminal -import-complete-shielded did not report IMPORT COMPLETE"
    echo "           (exit=$import_rc). Inspect $IMPORT_LOG before the copy is deleted."
    echo "           Nothing further was booted."
    echo "======================================================================"
    exit 1
fi
echo "[canonical-cure-prove] import reported IMPORT COMPLETE"

# Auto-derive the pre-import wedge height from the import's own "boundary="
# report unless the operator pinned one explicitly.
if [ -z "$EXPECT_CLIMB_PAST" ]; then
    EXPECT_CLIMB_PAST="$(sed -n 's/.*boundary=\([0-9][0-9]*\).*/\1/p' "$IMPORT_LOG" | head -1)"
    if [ -z "$EXPECT_CLIMB_PAST" ]; then
        echo "======================================================================"
        echo "  canonical-cure-prove VERDICT: FAIL"
        echo "  copy:    $COPY_DIR"
        echo "  reason:  IMPORT COMPLETE banner present but no 'boundary=<height>' could"
        echo "           be parsed from $IMPORT_LOG, and no --expect-climb-past=H override"
        echo "           was given — refusing to gate blind. Pass --expect-climb-past=H"
        echo "           explicitly, or check that this build's importer still logs"
        echo "           boundary= (app/services/src/shielded_history_import_service.c)."
        echo "======================================================================"
        exit 1
    fi
    echo "[canonical-cure-prove] auto-parsed pre-import wedge height: boundary=$EXPECT_CLIMB_PAST"
else
    echo "[canonical-cure-prove] using operator-supplied --expect-climb-past=$EXPECT_CLIMB_PAST (override)"
fi

# ── step 4: clear any stale auto_reindex_request ────────────────────────
# See tools/scripts/import-copy-prove.sh's step 3b for the full rationale:
# import_complete_shielded_mode() is a separate terminal argv path that
# returns before the normal boot sequence runs, so it never consumes a
# leftover self-rebuild request riding along from the wedged source datadir.
# Left in place, the very next normal boot would silently detour into
# -reindex-chainstate instead of exercising the state just imported.
if [ -e "$COPY_DIR/auto_reindex_request" ]; then
    echo "[canonical-cure-prove] step 4: stale auto_reindex_request found in the copy — clearing before boot"
    rm -f "$COPY_DIR/auto_reindex_request"
else
    echo "[canonical-cure-prove] step 4: no stale auto_reindex_request present — nothing to clear"
fi

# ── step 5: isolated boot, P2P-connected to zclassicd for tail bodies ───

NODE_ISO_ARGS="-fsport=$FSPORT -httpsport=$HTTPSPORT -addnode=$ZD_P2P -nolegacyimport -nofilesync"

HOME="$ISO_HOME" ZCL_MIRROR_SYNC=0 \
"$NODE_BIN" -datadir="$COPY_DIR" -rpcport="$RPCPORT" -port="$P2PPORT" \
    $NODE_ISO_ARGS \
    > "$COPY_DIR/canonical_cure_node.log" 2>&1 &
NODE_PID=$!

rpc() { HOME="$ISO_HOME" ZCL_DATADIR="$COPY_DIR" ZCL_RPCPORT="$RPCPORT" "$RPC_BIN" "$@" 2>/dev/null || true; }
tip() {
    resp="$(rpc getblockcount)"
    printf '%s\n' "$resp" |
        sed -n 's/.*"result"[[:space:]]*:[[:space:]]*\(-\{0,1\}[0-9][0-9]*\).*/\1/p' | head -1
}

# ── step 6: GATE on H* CLIMB past the pre-import wedge height ───────────

deadline_epoch=$(( $(date +%s) + DEADLINE ))
max_tip=-1
first_tip=-1
gate_ok=0
hstar_final=""
coins_applied_final=""
cookie="$COPY_DIR/.cookie"
while [ "$(date +%s)" -lt "$deadline_epoch" ]; do
    if ! kill -0 "$NODE_PID" 2>/dev/null; then
        echo "[canonical-cure-prove] node exited early (see $COPY_DIR/canonical_cure_node.log)"
        break
    fi
    if [ -f "$cookie" ]; then
        t="$(tip)"; t="${t:--1}"
        if [ "$t" -ge 0 ] 2>/dev/null; then
            [ "$first_tip" -lt 0 ] && first_tip="$t" && echo "[canonical-cure-prove] first tip: $t"
            [ "$t" -gt "$max_tip" ] && max_tip="$t"
        fi
        frontier_json="$(rpc dumpstate '"reducer_frontier"')"
        hstar_now="$(printf '%s' "$frontier_json" | sed -n 's/.*"hstar":\([0-9-]*\).*/\1/p' | head -1)"
        coins_applied_now="$(printf '%s' "$frontier_json" | sed -n 's/.*"coins_applied_height":\([0-9-]*\).*/\1/p' | head -1)"
        if [ -n "${hstar_now:-}" ] && [ -n "${coins_applied_now:-}" ] && \
           [ "$hstar_now" -gt "$EXPECT_CLIMB_PAST" ] 2>/dev/null && \
           [ "$coins_applied_now" -gt "$EXPECT_CLIMB_PAST" ] 2>/dev/null; then
            gate_ok=1
            hstar_final="$hstar_now"
            coins_applied_final="$coins_applied_now"
            echo "[canonical-cure-prove] H* CLIMBED: hstar=$hstar_now coins_applied_height=$coins_applied_now (both > $EXPECT_CLIMB_PAST)"
            break
        fi
        hstar_final="${hstar_now:-<none>}"
        coins_applied_final="${coins_applied_now:-<none>}"
    fi
    sleep 5
done

DURATION_SECS=$(( $(date +%s) - RUN_START_EPOCH ))

echo "======================================================================"
if [ "$gate_ok" = "1" ]; then
    verdict="PASS"
    rc=0
else
    verdict="FAIL"
    rc=1
fi
echo "  canonical-cure-prove VERDICT: $verdict"
echo "  copy:                    $COPY_DIR $([ "$KEEP_COPY" = "0" ] && echo '(will be deleted on exit)' || echo '(kept — --keep-copy)')"
echo "  first_tip:               $first_tip"
echo "  max_tip:                 $max_tip"
echo "  pre_import_wedge_height: $EXPECT_CLIMB_PAST"
echo "  GATE H* CLIMB:           hstar=$hstar_final coins_applied_height=$coins_applied_final (want both > $EXPECT_CLIMB_PAST) ok=$gate_ok"
echo "  duration_secs:           $DURATION_SECS"
echo "  header_refresh_log:      $HEADER_REFRESH_LOG"
echo "  import_log:              $IMPORT_LOG"
echo "  node_log:                $COPY_DIR/canonical_cure_node.log"
if [ "$verdict" = "FAIL" ]; then
    echo "  NOTE: this proof is H* CLIMB only (no hash-parity cross-check) — it is"
    echo "        the fast turnkey rehearsal, not the rigorous 3-gate proof."
    echo "        Use tools/scripts/import-copy-prove.sh for GATE (a)+(b)+(c)"
    echo "        (continuity + same-height hash parity vs zclassicd) before any"
    echo "        live cutover decision."
fi
echo "======================================================================"
exit "$rc"
