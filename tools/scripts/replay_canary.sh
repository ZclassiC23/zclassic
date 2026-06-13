#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# replay_canary.sh — the standing full-history replay canary.
#
# Replays the REAL chain through the HEAD binary's reducer in an
# isolated scratch datadir on isolated ports, then asserts:
#   (a) zero consensus rejects (bg_validation reaches COMPLETE, never
#       FAILED, and getsyncdiag headers.total_rejected == 0),
#   (b) the node's recomputed SHA3 UTXO commitment passed through the
#       compiled checkpoint at anchor 3,056,758 without an integrity
#       FATAL (the boot gate refuses otherwise),
#   (c) coarse UTXO stats (bestblock/txouts/total_amount) at the node's
#       tip == co-located zclassicd `gettxoutsetinfo` (read-only, 8232).
#
# The AUTHORITATIVE verdict is a sentinel FILE written atomically
# (tmp + fsync + rename) ONLY after every assertion passes. The shell
# exit code is advisory (it drives systemd OnFailure=); proof requires a
# *positive* fresh PASS record, never the absence of a non-zero exit.
# This is the "never exit-0-as-proof" guarantee, made REAL two ways:
#   1. reset_verdict() removes ANY prior sentinel as the FIRST thing every
#      run does (live AND self-test), so a crashed/killed/OOM/timed-out run
#      leaves NO sentinel at all — not a stale PASS — and an absence-of-
#      fresh-PASS reader resolves FAIL by construction.
#   2. The sentinel carries "started_ts" and the run drops a
#      $VERDICT_DIR/.run_started_<from> stamp, so an external freshness
#      check (the `make replay-canary-*` guard's marker + `-nt` test; a
#      live-node Condition) can REJECT a stale PASS that somehow survives.
# Together: a stale PASS can never be read as the current run's proof.
#
# Variants (one --from flag, same harness):
#   --from=anchor  : the PROVEN cold recipe — --importblockindex (headers,
#                    read-only) then a NORMAL boot with legacy auto-import ON
#                    (auto-links read-only ~/.zclassic, seeds the anchor
#                    3,056,758 UTXO set), then bg-validation walks the
#                    connected extent toward tip (~45 min). Dead -connect
#                    sink, no real peer. (The -snapshot=+-nolegacyimport
#                    combo FATALs at HEAD — see iso_spawn_mainnet_node.)
#   --from=genesis : -nolegacyimport (no anchor seed); replay genesis->tip
#                    with bg-validation ON (~6 h). Dials the co-located
#                    zclassicd P2P (8033) via -addnode for bodies — the one
#                    real peer, read-only.
#
# Usage:
#   replay_canary.sh --from=anchor|genesis [--src-datadir=DIR]
#                    [--budget-sec=N] [--zclassicd-rpc=PORT]
#
# Hidden self-test mode (drives the hermetic verdict-logic gate; injects
# synthetic RPC outputs from a fixture dir, never spawns a node):
#   replay_canary.sh --self-test=pass|fail-rejects|fail-sha3|\
#                    fail-crossnode|fail-timeout
#       reads $ZCL_CANARY_SELFTEST_DIR/{getsyncdetail,getsyncdiag,
#       getutxocommitment,gettxoutsetinfo,zd_gettxoutsetinfo}.json

set -euo pipefail

# ── Defaults / arg parse ───────────────────────────────────────────
FROM="anchor"
SRC_DATADIR="${HOME:-/root}/.zclassic"
BUDGET_SEC=""
ZD_RPC="8232"
SELFTEST=""

for arg in "$@"; do
    case "$arg" in
        --from=*)          FROM="${arg#--from=}" ;;
        --src-datadir=*)   SRC_DATADIR="${arg#--src-datadir=}" ;;
        --budget-sec=*)    BUDGET_SEC="${arg#--budget-sec=}" ;;
        --zclassicd-rpc=*) ZD_RPC="${arg#--zclassicd-rpc=}" ;;
        --self-test=*)     SELFTEST="${arg#--self-test=}" ;;
        *) echo "replay-canary: unknown arg '$arg'" >&2; exit 2 ;;
    esac
done

# The expected checkpoint SHA3 + anchor (lib/chain/src/checkpoints.c:86-104,
# mirrored by REDUCER_FRONTIER_TRUSTED_ANCHOR). The canary asserts the node
# passed through this without an integrity FATAL; for the local commitment
# path the value is the recompute target.
ANCHOR_HEIGHT=3056758
EXPECTED_SHA3="00e95dbd54a791a51433d68127f9975a3b1d6f8e9002b109647343ba0c83c3e0"

# ── Elapsed-time band (the named-defect guard) ─────────────────────
# A from-anchor run that silently DEGRADES to a from-genesis-scale replay
# is the named I5 defect. The band makes that degrade a typed FAIL, not an
# accidental green: a from-anchor full-history replay through the reducer
# legitimately takes ~45 min, so a COMPLETE that arrives implausibly FAST
# (the seed never applied / the node only replayed a stub) blows the FLOOR,
# and a COMPLETE that takes ~genesis-scale time (~6 h, the silent degrade)
# blows the CEILING. The anchor band is centred on ~45 min, NOT ~6 h; the
# genesis band is the ~6 h replay. Bounds in seconds:
#   anchor : floor 300 (5 min — a real anchor replay can never be near-
#            instant), ceiling 5400 (90 min == the hard budget; a 6 h
#            from-anchor degrade blows this long before genesis scale).
#   genesis: floor 3600 (1 h), ceiling 28800 (8 h == the hard budget).
ANCHOR_ELAPSED_MIN=300
ANCHOR_ELAPSED_MAX=5400
GENESIS_ELAPSED_MIN=3600
GENESIS_ELAPSED_MAX=28800

# REPO_ROOT: the harness knows where it lives (like soak_assert.sh).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

VERDICT_DIR="${ZCL_CANARY_VERDICT_DIR:-${HOME:-/root}/.local/state/zclassic23-canary}"
mkdir -p "$VERDICT_DIR"

build_commit() {
    git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown"
}

# START_TS is fixed at the very top of the process so reset_verdict,
# write_verdict, and the elapsed band all share one run-start epoch.
START_TS="$(date +%s)"; STARTED_TS="$START_TS"; ELAPSED=0

# ── Sentinel path (one place, used by reset + write) ───────────────
sentinel_path() { printf '%s/replay_canary_%s.json' "$VERDICT_DIR" "$FROM"; }

# ── Sentinel reset (FIRST thing every run does) ────────────────────
# Remove ANY prior sentinel before the run does any work. This is the
# load-bearing half of "never exit-0-as-proof": after this point a
# killed/OOM/timed-out harness leaves NO sentinel at all (not a stale
# PASS), so an absence-of-fresh-PASS reader resolves FAIL by construction.
# A fresh PASS can therefore only appear if THIS run reaches write_verdict
# PASS after every assertion. We also stamp $VERDICT_DIR/.run_started_<from>
# with the run-start epoch so an external reader (the Makefile guard, a
# live-node Condition) can band-check the sentinel's freshness against the
# run it is judging — a stale sentinel that somehow survives is still read
# as not-fresh => FAIL.
reset_verdict() {
    local f; f="$(sentinel_path)"
    rm -f "$f" "$f".tmp.* 2>/dev/null || true
    printf '%s\n' "$STARTED_TS" > "$VERDICT_DIR/.run_started_${FROM}" 2>/dev/null || true
    sync 2>/dev/null || true
}

# ── Sentinel writer (atomic: tmp + fsync + rename) ─────────────────
# The PASS sentinel exists ONLY after every assertion passed AND after
# reset_verdict removed any prior one. write_verdict is the single place a
# sentinel is produced; it is called exactly once per run, at the very end,
# with the already-decided verdict. The "started_ts" field lets a reader
# band-check freshness without a second stamp file.
write_verdict() {
    local verdict="$1" reason="$2"
    local f; f="$(sentinel_path)"
    local tmp="$f.tmp.$$"
    local now; now="$(date +%s)"
    {
        printf '{"verdict":"%s","from":"%s","ts":%s,"started_ts":%s,"build_commit":"%s",' \
            "$verdict" "$FROM" "$now" "${STARTED_TS:-$now}" "$(build_commit)"
        printf '"tip":%s,"verified_height":%s,"bg_state":"%s",' \
            "${R_TIP:-0}" "${R_VERIFIED:-0}" "${R_BGSTATE:-unknown}"
        printf '"consensus_rejects":%s,"local_sha3":"%s","expected_sha3":"%s",' \
            "${R_REJECTS:-0}" "${R_LOCAL_SHA3:-}" "$EXPECTED_SHA3"
        printf '"txouts":%s,"zd_txouts":%s,"supply":"%s","zd_supply":"%s",' \
            "${R_TXOUTS:-0}" "${R_ZD_TXOUTS:-0}" "${R_SUPPLY:-}" "${R_ZD_SUPPLY:-}"
        printf '"reason":"%s","elapsed_sec":%s}\n' \
            "$reason" "${ELAPSED:-0}"
    } > "$tmp"
    # fsync the file contents, then atomically rename into place, then
    # fsync the directory so the rename is durable. `sync` flushes the
    # whole fs; a targeted fdatasync would be tighter but `sync` keeps
    # this dependency-free (no python, no helper binary — roadmap rule).
    sync
    mv -f "$tmp" "$f"
    sync
}

# ── Single-line operator-greppable verdict + page ──────────────────
emit_verdict_line() {
    local verdict="$1" reason="$2"
    if [ "$verdict" = "FAIL" ]; then
        echo "replay-canary: VERDICT=FAIL from=$FROM reason=$reason tip=${R_TIP:-?} bg=${R_BGSTATE:-?} elapsed=${ELAPSED:-?}s"
        # OnFailure systemd notification (File 7) is the page channel; we
        # also log to the journal here so `journalctl` greps VERDICT=FAIL.
        logger -t replay-canary "VERDICT=FAIL from=$FROM reason=$reason" 2>/dev/null || true
    else
        echo "replay-canary: VERDICT=PASS from=$FROM tip=${R_TIP:-?} verified=${R_VERIFIED:-?} elapsed=${ELAPSED:-?}s"
    fi
}

# fail <reason>: write FAIL sentinel + line, exit non-zero. The sentinel
# is the authority; the exit code only drives systemd OnFailure=.
fail() {
    local reason="$1"
    write_verdict "FAIL" "$reason"
    emit_verdict_line "FAIL" "$reason"
    # Preserve forensics BEFORE the exit trap rm -rf's the scratch datadir:
    # the isolated node's log tail is the only post-mortem for an
    # unattended (02:30 nightly) FAIL. Tail-bounded so the verdict dir
    # stays small; tmp+mv so a reader never sees a torn copy. Quietly
    # skipped in selftest/fixture mode (no ISO_DD / no node.log).
    if [ -n "${ISO_DD:-}" ] && [ -f "${ISO_DD}/node.log" ]; then
        local flog="$VERDICT_DIR/lastfail_${FROM}_node.log"
        { echo "# preserved by fail(reason=$reason) ts=$(date -u +%s) elapsed=${ELAPSED:-?}s"; \
          tail -n 400 "${ISO_DD}/node.log"; } > "${flog}.tmp" 2>/dev/null \
            && mv -f "${flog}.tmp" "$flog" 2>/dev/null || true
    fi
    exit 1
}

# pass: write PASS sentinel + line, exit 0.
pass() {
    write_verdict "PASS" ""
    emit_verdict_line "PASS" ""
    exit 0
}

# ── JSON field extraction (no python — grep/sed only) ──────────────
# Pulls "key":<number-or-"string"> out of a flat-ish JSON blob. Returns
# empty on a miss so the caller can detect rpc_unreachable / missing field
# (never silently treat a missing field as 0/pass).
json_num() {  # $1=json $2=key
    printf '%s' "$1" | grep -o "\"$2\"[[:space:]]*:[[:space:]]*-\?[0-9]\+" \
        | head -1 | grep -o -- '-\?[0-9]\+$' || true
}
json_str() {  # $1=json $2=key
    printf '%s' "$1" | grep -o "\"$2\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" \
        | head -1 | sed 's/.*:[[:space:]]*"\([^"]*\)"/\1/' || true
}
# total_amount tolerance: zclassic23 emits it QUOTED ("10364138.33747381")
# but zclassicd emits it UNQUOTED as a JSON number (10395235.80748115).
# Extract either form as a bare token so the two can be string-compared
# directly (both nodes use 8-decimal fixed-point at the same height).
json_amount() {  # $1=json $2=key
    local v
    v="$(json_str "$1" "$2")"
    if [ -n "$v" ]; then printf '%s' "$v"; return 0; fi
    printf '%s' "$1" | grep -o "\"$2\"[[:space:]]*:[[:space:]]*[0-9.]\+" \
        | head -1 | sed 's/.*:[[:space:]]*\([0-9.]*\)/\1/' || true
}

# ── Result vars (populated as we probe; consumed by write_verdict) ─
# START_TS / STARTED_TS / ELAPSED are fixed earlier (with the sentinel
# helpers) so reset_verdict and the elapsed band share the same run-start.
R_TIP=0; R_VERIFIED=0; R_BGSTATE="unknown"; R_REJECTS=0
R_LOCAL_SHA3=""; R_TXOUTS=0; R_ZD_TXOUTS=0; R_SUPPLY=""; R_ZD_SUPPLY=""

# ── Verdict logic: evaluate already-collected RPC blobs ────────────
# Inputs (set by run_live or run_self_test): SD (getsyncdetail),
# DIAG (getsyncdiag), UC (getutxocommitment), TX (node gettxoutsetinfo),
# ZD (zclassicd gettxoutsetinfo). First failure wins.
evaluate_verdict() {
    # ELAPSED is the wall-clock the run took. The self-test injects a
    # synthetic value (SELFTEST_ELAPSED) so the band can be exercised
    # hermetically without a 45-min wait; live mode measures the clock.
    if [ -n "${SELFTEST_ELAPSED:-}" ]; then ELAPSED="$SELFTEST_ELAPSED"
    else ELAPSED=$(( $(date +%s) - START_TS )); fi

    # rpc_unreachable: any required blob empty => FAIL (never a silent pass).
    [ -n "$SD" ]   || fail "rpc_unreachable_getsyncdetail"
    [ -n "$DIAG" ] || fail "rpc_unreachable_getsyncdiag"
    [ -n "$TX" ]   || fail "rpc_unreachable_gettxoutsetinfo"
    [ -n "$ZD" ]   || fail "rpc_unreachable_zd_gettxoutsetinfo"

    R_BGSTATE="$(json_str "$SD" state)"
    R_VERIFIED="$(json_num "$SD" verified_height)"; : "${R_VERIFIED:=0}"
    R_TIP="$(json_num "$TX" height)"; : "${R_TIP:=0}"
    local skipped; skipped="$(json_num "$SD" script_verif_skipped_no_undo)"; : "${skipped:=0}"
    R_REJECTS="$(json_num "$DIAG" total_rejected)"; : "${R_REJECTS:=0}"
    R_TXOUTS="$(json_num "$TX" txouts)"; : "${R_TXOUTS:=0}"
    local tx_best; tx_best="$(json_str "$TX" bestblock)"
    R_SUPPLY="$(json_amount "$TX" total_amount)"
    R_ZD_TXOUTS="$(json_num "$ZD" txouts)"; : "${R_ZD_TXOUTS:=0}"
    local zd_best; zd_best="$(json_str "$ZD" bestblock)"
    R_ZD_SUPPLY="$(json_amount "$ZD" total_amount)"
    local zd_height; zd_height="$(json_num "$ZD" height)"; : "${zd_height:=0}"

    # (a) bg_validation must reach COMPLETE. FAILED or a budget-timeout
    #     (caller records bg_state=timeout) => FAIL. A consensus reject
    #     during replay surfaces as FAILED.
    case "$R_BGSTATE" in
        complete|COMPLETE) : ;;
        timeout)  fail "budget_exceeded" ;;
        failed|FAILED) fail "bg_validation_failed" ;;
        *) fail "bg_state_${R_BGSTATE:-empty}" ;;
    esac

    # (a cont.) elapsed-time band — the named-defect guard for THIS track.
    # A COMPLETE that is too FAST (the from-anchor seed never applied, so
    # the node "completed" a stub) or too SLOW (the from-anchor run silently
    # degraded to a genesis-scale replay) both FAIL with a typed reason,
    # BEFORE the cross-node equality can mask a degraded-but-matching tip.
    local emin emax
    if [ "$FROM" = "genesis" ]; then emin="$GENESIS_ELAPSED_MIN"; emax="$GENESIS_ELAPSED_MAX"
    else emin="$ANCHOR_ELAPSED_MIN"; emax="$ANCHOR_ELAPSED_MAX"; fi
    [ "${ELAPSED:-0}" -ge "$emin" ] || fail "elapsed_too_fast"
    [ "${ELAPSED:-0}" -le "$emax" ] || fail "elapsed_too_slow"

    # (a cont.) header-admit rejects must be zero.
    [ "${R_REJECTS:-0}" -eq 0 ] || fail "consensus_rejects"

    # (b) local commitment. For from=anchor the node seeds AT the anchor
    #     and the commitment is computed at the TIP (not the anchor), so
    #     we assert it is a 64-hex value (a real recompute, not an error
    #     string) and rely on the boot integrity gate at h=3056758 for the
    #     checkpoint-sha3 proof. For from=genesis the replay crosses the
    #     anchor and the same boot gate proves the checkpoint; if the UC
    #     blob carries the anchor-height commitment it must equal EXPECTED.
    if [ -n "$UC" ]; then
        R_LOCAL_SHA3="$(json_str "$UC" sha3_hash)"
        local uc_h; uc_h="$(json_num "$UC" height)"; : "${uc_h:=0}"
        case "$R_LOCAL_SHA3" in
            [0-9a-f]*) [ "${#R_LOCAL_SHA3}" -eq 64 ] || fail "sha3_malformed" ;;
            *) fail "sha3_unreadable" ;;
        esac
        # If the commitment was taken exactly at the anchor height, it must
        # equal the compiled checkpoint hash exactly.
        if [ "$uc_h" = "$ANCHOR_HEIGHT" ] && [ "$R_LOCAL_SHA3" != "$EXPECTED_SHA3" ]; then
            fail "sha3_mismatch"
        fi
    fi

    # (b cont.) from=genesis must verify EVERY script (no post-snapshot
    #     skips). from=anchor legitimately skips post-snapshot script
    #     verification, so do not assert it there.
    if [ "$FROM" = "genesis" ] && [ "${skipped:-0}" -ne 0 ]; then
        fail "script_verif_skipped_no_undo"
    fi

    # (c) cross-node coarse stats at the node's tip vs zclassicd.
    [ "${R_TIP:-0}" = "${zd_height:-0}" ] || fail "crossnode_height"
    [ -n "$tx_best" ] && [ "$tx_best" = "$zd_best" ] || fail "crossnode_bestblock"
    [ "${R_TXOUTS:-0}" = "${R_ZD_TXOUTS:-0}" ] || fail "crossnode_txouts"
    [ -n "$R_SUPPLY" ] && [ "$R_SUPPLY" = "$R_ZD_SUPPLY" ] || fail "crossnode_supply"

    pass
}

# ── Self-test mode: feed fixture JSON, run the SAME verdict logic ──
# Drives the hermetic test_replay_canary_verdict gate. Reads fixture
# blobs from $ZCL_CANARY_SELFTEST_DIR. No node, no zclassicd, no network.
run_self_test() {
    local dir="${ZCL_CANARY_SELFTEST_DIR:-}"
    [ -n "$dir" ] && [ -d "$dir" ] || { echo "replay-canary: self-test needs ZCL_CANARY_SELFTEST_DIR" >&2; exit 2; }
    # Reset FIRST so even the self-test path proves the no-stale-sentinel
    # contract: any prior sentinel for this --from is removed before we
    # decide, and the run-start stamp is laid down.
    reset_verdict
    # Test-only mid-run pause: if ZCL_CANARY_SELFTEST_BLOCK_FIFO is set, block
    # on a read of that FIFO AFTER reset_verdict (so the stale sentinel is
    # already gone) but BEFORE evaluate_verdict (so no fresh sentinel is yet
    # written). The hermetic kill-mid-run test SIGKILLs us here and asserts
    # the post-kill read resolves FAIL (no fresh PASS) — proving the kill
    # lands inside a real run, not before the harness ever started.
    if [ -n "${ZCL_CANARY_SELFTEST_BLOCK_FIFO:-}" ]; then
        read -r _ < "$ZCL_CANARY_SELFTEST_BLOCK_FIFO" || true
    fi
    read_fixture() { [ -f "$dir/$1.json" ] && cat "$dir/$1.json" || printf ''; }
    SD="$(read_fixture getsyncdetail)"
    DIAG="$(read_fixture getsyncdiag)"
    UC="$(read_fixture getutxocommitment)"
    TX="$(read_fixture gettxoutsetinfo)"
    ZD="$(read_fixture zd_gettxoutsetinfo)"
    # Optional elapsed.json (a bare integer) drives the elapsed band
    # hermetically. Absent => default to an in-band value for the active
    # --from so the baseline pass fixtures stay green without one.
    local fx_elapsed; fx_elapsed="$(read_fixture elapsed | tr -dc '0-9')"
    if [ -n "$fx_elapsed" ]; then SELFTEST_ELAPSED="$fx_elapsed"
    elif [ "$FROM" = "genesis" ]; then SELFTEST_ELAPSED=$(( GENESIS_ELAPSED_MIN + 1 ))
    else SELFTEST_ELAPSED=$(( ANCHOR_ELAPSED_MIN + 1 )); fi
    # The self-test mode name is informational; the verdict is decided
    # purely by the fixture content, so a mislabeled fixture cannot lie.
    evaluate_verdict
}

# ── Live mode: spawn the isolated mainnet node, replay, probe ──────
run_live() {
    # Reset FIRST — before importing headers, spawning the node, or any
    # other abortable step. From here on a killed/OOM/timed-out harness
    # leaves NO sentinel (not a stale PASS), so absence-of-fresh-PASS is a
    # construction-level FAIL.
    reset_verdict

    cd "$REPO_ROOT"

    [ -x build/bin/zclassic23 ] || { echo "replay-canary: build/bin/zclassic23 missing — run make" >&2; exit 2; }
    [ -x build/bin/zcl-rpc ]    || { echo "replay-canary: build/bin/zcl-rpc missing — run make zcl-rpc" >&2; exit 2; }

    # Default budgets: anchor 5400 s (90 min, 2x the ~45-min expectation);
    # genesis 28800 s (8 h, ~1.3x the ~6-h expectation).
    local budget
    if [ -n "$BUDGET_SEC" ]; then budget="$BUDGET_SEC"
    elif [ "$FROM" = "genesis" ]; then budget=28800
    else budget=5400; fi

    # Disk preflight: the scratch chainstate + index copy is tens of GB.
    # Refuse loudly if /tmp lacks headroom (not under /tmp would be a port
    # of the spec — kept on /tmp per the iso_* discipline; the orchestrator
    # note about $HOME scratch applies if /tmp is tmpfs-small, see README).
    local avail_kb; avail_kb="$(df -Pk /tmp | awk 'NR==2{print $4}')"
    if [ "${avail_kb:-0}" -lt 83886080 ]; then   # < 80 GiB
        echo "replay-canary: REFUSE — /tmp has $((avail_kb/1024/1024)) GiB free, need >= 80 GiB" >&2
        ELAPSED=0; fail "insufficient_disk"
    fi

    # Distinct port bases so a nightly + a (rare) overlapping weekly cannot
    # collide. anchor=39050, genesis=39060. crash-soak (item 7) reserves 39070.
    if [ "$FROM" = "genesis" ]; then export ISO_PORT_BASE=39060
    else export ISO_PORT_BASE=39050; fi
    export ISO_KIND=replay

    # shellcheck source=tools/scripts/isolated_mainnet_env.sh
    . tools/scripts/isolated_mainnet_env.sh
    iso_init

    if [ "$FROM" = "anchor" ]; then
        echo "replay-canary: importing headers from $SRC_DATADIR (read-only)"
        if ! iso_import_blockindex "$SRC_DATADIR"; then
            ELAPSED=$(( $(date +%s) - START_TS )); fail "blockindex_import_failed"
        fi
        # PROVEN cold recipe (empirically verified at HEAD 2026-06-12):
        # NORMAL boot with legacy auto-import ON (NO -nolegacyimport, NO
        # -snapshot). Boot auto-links the read-only ~/.zclassic, seeds the
        # anchor UTXO set, reconciles, and serves; bg-validation walks the
        # connected extent (omit -nobgvalidation). Dead -connect sink keeps
        # peer_count 0. The -snapshot=$SRC + -nolegacyimport combo the spec
        # proposed FATALs at HEAD (torn-anchor: utxos present, coins_best
        # unset, heal refused) — do NOT use it. NOTE: legacy auto-import is
        # hardcoded to read ~/.zclassic, so for the anchor variant the
        # source IS ~/.zclassic regardless of --src-datadir (the default).
        iso_spawn_mainnet_node "-connect=127.0.0.1:$ISO_CONNECT_SINK"
    else
        # from=genesis: -nolegacyimport so boot does NOT seed to the anchor;
        # dial the co-located zclassicd for bodies. This is the ONE place a
        # real peer is dialed — the read-only co-located zclassicd.
        iso_spawn_mainnet_node "-nolegacyimport -addnode=127.0.0.1:8033"
    fi

    # 600 s: a fresh-import boot walks 3.1M-header restore phases before the
    # RPC listener starts; 180 s false-FAILed (rpc_never_ready) on a loaded
    # box (first live RED run, 2026-06-13). The hard budget still bounds the
    # whole run; this only lets the node finish booting.
    if ! iso_wait_rpc_ready 600; then
        ELAPSED=$(( $(date +%s) - START_TS )); fail "rpc_never_ready"
    fi

    # Poll until bg_validation reaches a terminal state or the budget blows.
    local deadline=$(( START_TS + budget ))
    while :; do
        local sd; sd="$(iso_rpc getsyncdetail)"
        local st; st="$(json_str "$sd" state)"
        case "$st" in
            complete|COMPLETE|failed|FAILED) break ;;
        esac
        if [ "$(date +%s)" -ge "$deadline" ]; then
            # Record a synthetic timeout bg_state so evaluate_verdict pages
            # budget_exceeded — never silently pass on a stuck RUNNING.
            R_BGSTATE="timeout"
            SD="{\"bg_validation\":{\"state\":\"timeout\"}}"
            DIAG="$(iso_rpc getsyncdiag)"; TX="$(iso_rpc gettxoutsetinfo)"
            ZD_DATADIR="$SRC_DATADIR" ZD="$(ZCL_DATADIR="$SRC_DATADIR" ZCL_RPCPORT="$ZD_RPC" build/bin/zcl-rpc gettxoutsetinfo 2>/dev/null || true)"
            evaluate_verdict
        fi
        sleep 30
    done

    # Terminal state reached — collect every probe blob, then evaluate.
    # The bg_validation section is nested in getsyncdetail under
    # "bg_validation"; evaluate_verdict reads state/verified_height/
    # script_verif_skipped_no_undo which are unique enough to grep flat.
    SD="$(iso_rpc getsyncdetail)"
    DIAG="$(iso_rpc getsyncdiag)"
    UC="$(iso_rpc getutxocommitment)"
    TX="$(iso_rpc gettxoutsetinfo)"
    # zclassicd is read-only: gettxoutsetinfo only, never stop/generate.
    ZD="$(ZCL_DATADIR="$SRC_DATADIR" ZCL_RPCPORT="$ZD_RPC" build/bin/zcl-rpc gettxoutsetinfo 2>/dev/null || true)"

    evaluate_verdict
}

# ── Dispatch ───────────────────────────────────────────────────────
case "$FROM" in
    anchor|genesis) : ;;
    *) echo "replay-canary: --from must be anchor|genesis (got '$FROM')" >&2; exit 2 ;;
esac

if [ -n "$SELFTEST" ]; then
    run_self_test
else
    run_live
fi
