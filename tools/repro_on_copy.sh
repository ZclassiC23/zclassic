#!/bin/sh
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# repro_on_copy.sh — snapshot the live datadir to a NEW labelled COPY and run
# build/bin/zclassic23 against the COPY on an isolated port, so consensus-critical and
# recovery experiments are validated BEFORE they can touch the live chain.
#
# This is the guardrail that makes the catastrophic tail structurally
# impossible: a reducer/recovery fix that would have collapsed the public tip
# (the real 3,130,701 -> 47,279 reset; the import-reset to ~199) is caught here
# on a throwaway copy instead of on the live node. The harness is also a
# TIP-REGRESSION DETECTOR: it boots the copy, watches the public tip for a
# window, and FAILS LOUD if the tip ever drops (a reset) — the exact symptom
# the import-reset (#10) and write-ordering (#7) tracks must reproduce safely.
#
# SAFETY (enforced below):
#   - copies to a brand-new $HOME/.zclassic-c23-COPY-<ts>-<slug> dir that must
#     not exist and must not equal --src; the live datadir is never written.
#   - runs the copy node with an isolated HOME plus isolated --rpcport /
#     --p2p-port / --fs-port / --https-port. It forces -connect to a dead sink,
#     -nolegacyimport, and -nofilesync unless the peer is explicitly overridden
#     with --connect. It never uses live ports or reads ~/.zclassic.
#   - leaves the copy on disk afterwards for further analysis (print its path).
#
# Usage:
#   make repro-on-copy SLUG=import-reset ARGS='-nobgvalidation'
#   tools/repro_on_copy.sh import-reset --port=18299 -- -nobgvalidation
#
# Options (before the `--` passthrough):
#   <slug>            required first arg; labels the copy + manifest
#   --src=DIR         source datadir (default $HOME/.zclassic-c23)
#   --port=N          isolated rpcport (default 18299)
#   --p2p-port=N      isolated p2p port (default 18933)
#   --fs-port=N       isolated file-service port (default 18934)
#   --https-port=N    isolated HTTPS port (default 18935)
#   --connect=ADDR    isolated -connect peer (default 127.0.0.1:39999 dead sink)
#   --full            copy the WHOLE datadir incl. blocks/ + consensus_snapshot
#                     (default is --light: node.db + progress.kv + block_index +
#                     projections; skips the 14G blocks/ + 2.3G snapshot)
#   --deadline=SECS   how long to watch the tip (default 180)
#   --no-run          snapshot + manifest only; do not launch the node
#   --                everything after is passed verbatim to build/bin/zclassic23
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
NODE_BIN="${ZCL_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
RPC_BIN="${ZCL_RPC_BIN:-$REPO_ROOT/build/bin/zcl-rpc}"

SLUG=""
SRC="$HOME/.zclassic-c23"
RPCPORT=18299
P2PPORT=18933
FSPORT=18934
HTTPSPORT=18935
CONNECT="127.0.0.1:39999"
LIGHT=1
DEADLINE=180
RUN=1
PASS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --src=*)       SRC="${1#--src=}" ;;
        --port=*)      RPCPORT="${1#--port=}" ;;
        --p2p-port=*)  P2PPORT="${1#--p2p-port=}" ;;
        --fs-port=*)   FSPORT="${1#--fs-port=}" ;;
        --https-port=*) HTTPSPORT="${1#--https-port=}" ;;
        --connect=*)   CONNECT="${1#--connect=}" ;;
        --full)        LIGHT=0 ;;
        --light)       LIGHT=1 ;;
        --deadline=*)  DEADLINE="${1#--deadline=}" ;;
        --no-run)      RUN=0 ;;
        --)            shift; PASS="$*"; break ;;
        --*)           echo "repro_on_copy: unknown option $1" >&2; exit 2 ;;
        *)             if [ -z "$SLUG" ]; then SLUG="$1"; else
                           echo "repro_on_copy: unexpected arg $1" >&2; exit 2; fi ;;
    esac
    shift
done

[ -n "$SLUG" ] || { echo "usage: tools/repro_on_copy.sh <slug> [--src=DIR] [--port=N] [--full] [-- <node args>]" >&2; exit 2; }
[ -d "$SRC" ]  || { echo "repro_on_copy: source datadir not found: $SRC" >&2; exit 1; }
[ -x "$NODE_BIN" ] || { echo "repro_on_copy: $NODE_BIN not built (run make)" >&2; exit 1; }
[ -x "$RPC_BIN" ] || { echo "repro_on_copy: $RPC_BIN not built (run make zcl-rpc)" >&2; exit 1; }

refuse_live_port() {
    p="$1"
    label="$2"
    case "$p" in
        8023|8033|8034|8232|8233|8443|18034|18232)
            echo "repro_on_copy: refusing live $label port $p" >&2
            exit 1
            ;;
    esac
}

refuse_live_port "$RPCPORT" "rpc"
refuse_live_port "$P2PPORT" "p2p"
refuse_live_port "$FSPORT" "file-service"
refuse_live_port "$HTTPSPORT" "https"
case "$CONNECT" in
    *:*) refuse_live_port "${CONNECT##*:}" "connect" ;;
esac

case " $PASS " in
    *" -addnode="*|*" -connect="*|*" -rpcport="*|*" -port="*|*" -fsport="*|*" -httpsport="*)
        echo "repro_on_copy: pass-through may not override network ports/peers; use script options" >&2
        exit 2
        ;;
esac

TS="$(date +%Y%m%d-%H%M%S)"
DEST="$HOME/.zclassic-c23-COPY-$TS-$SLUG"
# Refuse to clobber / alias the source.
[ ! -e "$DEST" ] || { echo "repro_on_copy: dest already exists: $DEST" >&2; exit 1; }
case "$DEST" in "$SRC"|"$SRC"/*) echo "repro_on_copy: dest must not be inside src" >&2; exit 1 ;; esac

# Disk precheck: need ~110% of what we are about to copy.
if [ "$LIGHT" = "1" ]; then
    NEED_KB=$( { du -sk "$SRC/node.db" "$SRC/progress.kv" "$SRC/block_index.bin" 2>/dev/null; \
                 du -sk "$SRC"/*_projection.db 2>/dev/null; } | awk '{s+=$1} END{print s+0}')
else
    NEED_KB=$(du -sk "$SRC" 2>/dev/null | awk '{print $1}')
fi
AVAIL_KB=$(df -Pk "$SRC" 2>/dev/null | awk 'NR==2{print $4}')
case "$AVAIL_KB" in
    ''|*[!0-9]*) echo "[repro] WARN: could not read free space for $SRC; skipping disk precheck" ;;
    *) if [ "$((NEED_KB + NEED_KB/10))" -gt "$AVAIL_KB" ]; then
           echo "repro_on_copy: not enough disk: need ~$((NEED_KB/1024))M (+10%), avail $((AVAIL_KB/1024))M" >&2
           exit 1
       fi ;;
esac

echo "[repro] snapshotting $SRC -> $DEST ($([ "$LIGHT" = 1 ] && echo light || echo full), ~$((NEED_KB/1024))M)"
mkdir -p "$DEST"
if [ "$LIGHT" = "1" ]; then
    # Copy the cursor/tip working set; SQLite -wal/-shm copied alongside each db
    # so the copy is self-consistent on open. Skip blocks/ (14G) + snapshot (2.3G).
    for f in node.db node.db-wal node.db-shm progress.kv progress.kv-wal progress.kv-shm \
             block_index.bin block_index.bin.sha3; do
        [ -e "$SRC/$f" ] && cp -a "$SRC/$f" "$DEST/" || true
    done
    for f in "$SRC"/*_projection.db "$SRC"/*_projection.db-wal "$SRC"/*_projection.db-shm; do
        [ -e "$f" ] && cp -a "$f" "$DEST/" || true
    done
    # control/marker files that influence boot, all tiny
    for f in last_reimport_attempted file_manifest.bin; do
        [ -e "$SRC/$f" ] && cp -a "$SRC/$f" "$DEST/" || true
    done
else
    cp -a "$SRC"/. "$DEST"/
fi

# Full copies inherit the live runtime identity and lock markers. They are not
# chain state, and leaving them in the throwaway datadir makes the isolated
# node correctly refuse to boot because it believes the live PID owns the copy.
rm -f "$DEST/zclassic23.pid" "$DEST/.cookie" "$DEST/.lock" 2>/dev/null || true

# Manifest for provenance.
{
    echo "slug:        $SLUG"
    echo "created:     $TS"
    echo "src:         $SRC"
    echo "mode:        $([ "$LIGHT" = 1 ] && echo light || echo full)"
    echo "git_head:    $(git rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "node_args:   $PASS"
    echo "rpcport:     $RPCPORT"
    echo "p2p_port:    $P2PPORT"
    echo "fs_port:     $FSPORT"
    echo "https_port:  $HTTPSPORT"
    echo "connect:     $CONNECT"
} > "$DEST/REPRO_MANIFEST.txt"
echo "[repro] manifest: $DEST/REPRO_MANIFEST.txt"

if [ "$RUN" = "0" ]; then
    echo "[repro] --no-run: snapshot ready at $DEST"
    exit 0
fi

NODE_PID=""
cleanup() {
    [ -n "$NODE_PID" ] && kill -TERM "$NODE_PID" 2>/dev/null || true
    [ -n "$NODE_PID" ] && wait "$NODE_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

ISO_HOME="$DEST/.isolated-home"
mkdir -p "$ISO_HOME"
NODE_ISO_ARGS="-fsport=$FSPORT -httpsport=$HTTPSPORT -connect=$CONNECT -nolegacyimport -nofilesync"

echo "[repro] launching $NODE_BIN on the COPY (rpcport=$RPCPORT p2p=$P2PPORT fs=$FSPORT https=$HTTPSPORT connect=$CONNECT) args: $PASS"
# shellcheck disable=SC2086
HOME="$ISO_HOME" ZCL_MIRROR_SYNC=0 \
"$NODE_BIN" -datadir="$DEST" -rpcport="$RPCPORT" -port="$P2PPORT" \
    $NODE_ISO_ARGS $PASS \
    > "$DEST/repro_node.log" 2>&1 &
NODE_PID=$!

cookie="$DEST/.cookie"
rpc() { HOME="$ISO_HOME" ZCL_DATADIR="$DEST" ZCL_RPCPORT="$RPCPORT" "$RPC_BIN" "$@" 2>/dev/null || true; }
tip()  {
    resp="$(rpc getblockcount)"
    case "$resp" in
        *\"result\"*)
            printf '%s\n' "$resp" |
                sed -n 's/.*"result"[[:space:]]*:[[:space:]]*\(-\{0,1\}[0-9][0-9]*\).*/\1/p' |
                head -1
            ;;
        *)
            printf '%s\n' "$resp" |
                sed -n 's/^[[:space:]]*\(-\{0,1\}[0-9][0-9]*\)[[:space:]]*$/\1/p' |
                head -1
            ;;
    esac
}

# Wait for RPC, then watch the tip for regressions over the deadline window.
deadline=$(( $(date +%s) + DEADLINE ))
max_tip=-1
first_tip=-1
regressed=0
seen_rpc=0
while [ "$(date +%s)" -lt "$deadline" ]; do
    if ! kill -0 "$NODE_PID" 2>/dev/null; then
        echo "[repro] node exited early (see $DEST/repro_node.log)"; break
    fi
    if [ -f "$cookie" ]; then
        t="$(tip)"; t="${t:--1}"
        if [ "$t" -ge 0 ] 2>/dev/null; then
            seen_rpc=1
            [ "$first_tip" -lt 0 ] && first_tip="$t" && echo "[repro] first tip: $t"
            if [ "$t" -gt "$max_tip" ]; then max_tip="$t"; fi
            # Regression = tip dropped meaningfully below the high-water mark.
            if [ "$max_tip" -ge 0 ] && [ "$t" -lt "$((max_tip - 5))" ]; then
                echo "[repro] !! TIP REGRESSION: $max_tip -> $t (height dropped)"
                regressed=1
                break
            fi
        fi
    fi
    sleep 3
done

post_tip="$(tip)"; post_tip="${post_tip:--1}"
# A --light copy has no blocks/ — any consensus/repair path that reads bodies
# silently degrades into read-error spam and the proof is environmentally
# invalid, not evidence about the binary. Detect and refuse the verdict.
body_read_fails=0
if [ -f "$DEST/repro_node.log" ]; then
    body_read_fails="$(grep -c 'cannot open .*/blocks/blk' "$DEST/repro_node.log" 2>/dev/null || echo 0)"
fi
echo "========================================================================"
echo "  repro-on-copy [$SLUG]"
echo "  copy:      $DEST"
echo "  first_tip: $first_tip   max_tip: $max_tip   post_tip: $post_tip"
if [ "$LIGHT" = "1" ] && [ "$body_read_fails" -gt 0 ]; then
    echo "  VERDICT:   INVALID — $body_read_fails block-body read failures on a"
    echo "             --light copy (no blocks/). Re-run with --full; this run"
    echo "             proves nothing about the binary."
    RC=2
elif [ "$seen_rpc" = "0" ]; then
    echo "  VERDICT:   INCONCLUSIVE — node never answered RPC within ${DEADLINE}s"
    echo "             (inspect $DEST/repro_node.log)"
    RC=2
elif [ "$regressed" = "1" ]; then
    echo "  VERDICT:   FAIL — public tip REGRESSED on the copy (reset reproduced)"
    RC=1
else
    echo "  VERDICT:   PASS — tip held/advanced (no regression) over ${DEADLINE}s"
    RC=0
fi
echo "  copy left on disk for analysis. remove with: rm -rf '$DEST'"
echo "========================================================================"
exit $RC
