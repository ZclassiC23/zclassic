#!/usr/bin/env bash
# unwedge-deploy.sh — one mechanical cutover for the -refold-from-anchor un-wedge.
#
# AUTHORED 2026-06-21 against repo HEAD 9b21ec665 / binary build_commit 9b21ec665.
# This script is AUTHORED NOW and EXECUTED LATER by the orchestrator, once the
# -mint-anchor bake has produced the VERIFIED anchor artifact. Authoring it does
# NOT run anything destructive: every mutating step lives inside a function that
# is only invoked from main(), and main() refuses Phase B without "live-go".
#
# THE RUNBOOK IT ENCODES (verified against fresh source):
#   - Un-wedge needs NO code change. Drive it with -refold-from-anchor.
#     (-load-verify-boot NO-OPS on the stamped/contaminated coins_kv — the
#      COINS_KV_MIGRATION_COMPLETE_KEY is set, so a load-verify boot is a
#      false-green. We never use it to UN-wedge. It is the flag to ADD to the
#      systemd unit AFTERWARD for robust future boots — see NOTE at the bottom.)
#   - The mint writes /tmp/anchor-ram.snapshot (or $ZCL_MINT_ANCHOR_OUT). A
#     SERVICE boot cannot see /tmp (the unit has PrivateTmp=yes +
#     ProtectSystem=strict). So the artifact MUST be copied to
#     <datadir>/utxo-anchor.snapshot, and the one-shot -refold-from-anchor run
#     MANUALLY (not via systemd) with ZCL_MINT_ANCHOR_OUT UNSET, so
#     mint_snapshot_path() (config/src/boot_refold_staged.c:138) resolves
#     <datadir>/utxo-anchor.snapshot and node.log is observable.
#   - WRONG-ARTIFACT TRAP: a stale /tmp/utxo-anchor-3056758.snapshot exists whose
#     header is the CONTAMINATED TIP (height 3151901, count 1344817, sha3
#     a9c8969c...). The script verifies the REAL header before placing anything.
#
# SAFETY CONTRACT:
#   - set -euo pipefail; every gate failure / header mismatch aborts LOUDLY.
#   - NEVER pkill/pgrep by name. We SIGTERM ONLY the PID of our own offline-copy
#     boot, recorded in a variable. The live node (PID ~209797) and zclassicd
#     (PID 3012556) are NEVER signalled by this script.
#   - Phase B (LIVE) refuses to run unless Phase A PASSED in THIS invocation and
#     the operator passed the explicit argument "live-go".
#   - The only process this script stops on the live host is via
#     `systemctl --user stop zclassic23` in Phase B (the unit's own ExecStop),
#     never a raw kill of the live PID.
#
# USAGE:
#   docs/work/unwedge-deploy.sh                 # Phase A only (copy-prove). Safe.
#   docs/work/unwedge-deploy.sh copy-only       # explicit Phase A only.
#   docs/work/unwedge-deploy.sh live-go         # Phase A, then Phase B if A PASSED.

set -euo pipefail

# ────────────────────────────── constants ──────────────────────────────────
REPO="/home/rhett/github/zclassic23"
BIN="${REPO}/build/bin/zclassic23"
# sqlq is the project's read-only SQL CLI over progress.kv (SQLITE_OPEN_READONLY,
# WAL-reader safe, SELECT/PRAGMA only). The host has NO sqlite3 CLI and python is
# banned, so this is THE tool for the gate SQL. usage: sqlq <db-path> <SELECT ...>
SQLQ="${REPO}/build/bin/sqlq"
EXPECT_COMMIT="9b21ec665"
ANCHOR=3056758

LIVE_DATADIR="/home/rhett/.zclassic-c23"
COPY_DATADIR="/home/rhett/.zclassic-c23-refoldtest"

# The mint emits one of these. We check both, in this order. The orchestrator
# may also pass an explicit path as $ARTIFACT_SRC in the environment.
ARTIFACT_CANDIDATES=(
    "/tmp/anchor-ram.snapshot"
    "/dev/shm/fmram/utxo-anchor.snapshot"
    "/dev/shm/fmram/anchor-ram.snapshot"
)

# REAL (verified) anchor header — the values the un-wedge depends on.
EXPECT_MAGIC_HEX="5a434c5554584f00"                # "ZCLUTXO\0"
EXPECT_HEIGHT=3056758                              # off-16 LE u32
EXPECT_COUNT=1354771                               # off-24 LE u64
EXPECT_SHA3="00e95dbd54a791a51433d68127f9975a3b1d6f8e9002b109647343ba0c83c3e0" # off-72, 32 bytes

# The CONTAMINATED stale artifact's header — reject on sight.
BAD_HEIGHT=3151901
BAD_SHA3_PREFIX="a9c8969c"

# Gate G3 boundary: H* must climb STRICTLY PAST this height.
GATE_HSTAR_PAST=3151412

# Isolated ports for the offline copy boot (must NOT collide with the live
# 8023/18232 or the mint job's 18933/28932/28943/28974).
COPY_PORT=19923
COPY_RPCPORT=29932
COPY_HTTPSPORT=29943
COPY_FSPORT=29974

# Don't churn forever waiting for the copy to climb past the gate. The fold over
# ~95k blocks from the anchor is the real work; give it a generous ceiling.
COPY_BOOT_MAX_SECS=5400        # 90 min hard ceiling on the copy proof
COPY_POLL_SECS=20

# Recorded PID of OUR offline copy boot. ONLY this PID is ever signalled.
COPY_PID=""

# ────────────────────────────── helpers ────────────────────────────────────
die()  { printf '\nABORT: %s\n' "$*" >&2; exit 1; }
note() { printf '[unwedge] %s\n' "$*"; }
rule() { printf -- '──────────────────────────────────────────────────────────\n'; }

# Read a little-endian unsigned integer of N bytes at byte OFFSET of FILE.
le_uint() {  # le_uint FILE OFFSET NBYTES
    local f="$1" off="$2" n="$3"
    od -An -tu"$n" -j"$off" -N"$n" --endian=little "$f" 2>/dev/null | tr -d ' '
}
hex_at() {   # hex_at FILE OFFSET NBYTES  -> lowercase hex, no spaces
    local f="$1" off="$2" n="$3"
    od -An -tx1 -j"$off" -N"$n" "$f" 2>/dev/null | tr -d ' \n'
}

# Clean up ONLY our own offline copy boot, if it is still alive.
cleanup_copy_boot() {
    if [[ -n "${COPY_PID}" ]] && kill -0 "${COPY_PID}" 2>/dev/null; then
        note "stopping our offline copy boot (PID ${COPY_PID}) with SIGTERM"
        kill -TERM "${COPY_PID}" 2>/dev/null || true
        # Give it up to TimeoutStop-ish to drain WAL, then SIGKILL only IT.
        for _ in $(seq 1 60); do
            kill -0 "${COPY_PID}" 2>/dev/null || break
            sleep 2
        done
        if kill -0 "${COPY_PID}" 2>/dev/null; then
            note "copy boot did not exit on SIGTERM; SIGKILL PID ${COPY_PID} only"
            kill -KILL "${COPY_PID}" 2>/dev/null || true
        fi
    fi
    COPY_PID=""
}
trap cleanup_copy_boot EXIT INT TERM

# ─────────────────────── artifact discovery + header verify ─────────────────
# Echoes the resolved artifact path on stdout; aborts if none verifies.
resolve_and_verify_artifact() {
    local cands=()
    [[ -n "${ARTIFACT_SRC:-}" ]] && cands+=("${ARTIFACT_SRC}")
    cands+=("${ARTIFACT_CANDIDATES[@]}")

    local art=""
    for c in "${cands[@]}"; do
        if [[ -f "$c" ]]; then art="$c"; break; fi
    done
    [[ -n "$art" ]] || die "no mint artifact found. Looked at: ${cands[*]}. The bake must finish first."

    note "candidate artifact: $art" >&2

    # 1. magic bytes 0..7
    local magic; magic="$(hex_at "$art" 0 8)"
    [[ "$magic" == "${EXPECT_MAGIC_HEX}" ]] || \
        die "artifact $art bad magic: got $magic want ${EXPECT_MAGIC_HEX}"

    # 2. height off-16 LE u32
    local h; h="$(le_uint "$art" 16 4)"
    # 3. count off-24 LE u64
    local c; c="$(le_uint "$art" 24 8)"
    # 4. sha3 off-72 32 bytes
    local s; s="$(hex_at "$art" 72 32)"

    note "artifact header: height=$h count=$c sha3=${s:0:8}..." >&2

    # WRONG-ARTIFACT TRAP — explicit reject of the known contaminated header.
    if [[ "$h" == "${BAD_HEIGHT}" || "${s:0:8}" == "${BAD_SHA3_PREFIX}" ]]; then
        die "artifact $art is the CONTAMINATED TIP (height=$h sha3=${s:0:8}...). Refusing."
    fi

    # POSITIVE match against the verified anchor header.
    [[ "$h" == "${EXPECT_HEIGHT}" ]] || die "artifact $art height=$h != ${EXPECT_HEIGHT}"
    [[ "$c" == "${EXPECT_COUNT}"  ]] || die "artifact $art count=$c != ${EXPECT_COUNT}"
    [[ "$s" == "${EXPECT_SHA3}"   ]] || die "artifact $art sha3=$s != ${EXPECT_SHA3}"

    note "artifact header VERIFIED as the real anchor (h=${EXPECT_HEIGHT} count=${EXPECT_COUNT})" >&2
    printf '%s\n' "$art"
}

# ─────────────────────────────── preflight ──────────────────────────────────
preflight() {
    rule; note "PREFLIGHT (read-only)"; rule
    [[ -x "$BIN" ]] || die "binary not found/executable: $BIN"
    [[ -x "$SQLQ" ]] || die "sqlq not found/executable: $SQLQ (build it: make build-only) — gate SQL needs it"
    command -v od >/dev/null  || die "od not on PATH (needed for header byte reads)"
    local commit; commit="$(strings -a "$BIN" | grep -oE '^9b21ec66[0-9a-f]+' | head -1 || true)"
    [[ "$commit" == "${EXPECT_COMMIT}"* || -z "$commit" ]] || \
        note "WARNING: binary build_commit '$commit' != expected ${EXPECT_COMMIT} (continuing — re-verify)"
    [[ -d "$LIVE_DATADIR" ]] || die "live datadir missing: $LIVE_DATADIR"
    [[ -d "$COPY_DATADIR" ]] || die "frozen copy missing: $COPY_DATADIR"
    [[ -f "${COPY_DATADIR}/node.db" ]] || die "copy has no node.db — not a usable frozen datadir"

    # Read-only wedge signature on the LIVE datadir's node.log (do not touch it).
    if grep -aqE 'window\.consistency: I4\.3 utxo_apply log hole|coins_applied_height=[0-9]+ > hstar_cursor' \
            "${LIVE_DATADIR}/node.log" 2>/dev/null; then
        note "live datadir shows the wedge signature (I4.3 utxo_apply log hole / hstar pinned)"
    else
        note "WARNING: did not see the I4.3 wedge string in live node.log tail — verify the live node state by hand"
    fi
    note "(the frozen copy has no node.log; its wedge is only observable by booting it in Phase A)"
    note "preflight OK"
}

# ────────────────────── gate evaluation over a node.log ─────────────────────
# Returns 0 (PASS) iff all 8 gates pass against the given log + datadir.
# $1 = node.log path, $2 = datadir (for SQL over progress.kv), $3 = label.
evaluate_gates() {
    local log="$1" dd="$2" label="$3"
    rule; note "GATES (${label})"; rule
    local fail=0

    # G1 — minted snapshot loaded + SHA3 verified.
    if grep -aqF 'loaded 1354771 coins from the MINTED snapshot' "$log" 2>/dev/null || \
       grep -aqE 'loaded 1354771 coins from the MINTED.*SHA3 verified' "$log" 2>/dev/null; then
        note "G1 PASS: minted snapshot (1354771 coins) loaded + SHA3 verified"
    else
        note "G1 FAIL: no 'loaded 1354771 coins from the MINTED snapshot' line"; fail=1
    fi

    # G2 — re-seeded+verified, and NO FATAL SHA3/count failure.
    if grep -aqE 'FATAL: -refold-from-anchor:.*FAILED the SHA3/count check' "$log" 2>/dev/null; then
        note "G2 FAIL: FATAL SHA3/count check present"; fail=1
    elif grep -aqE 're-seeded anchor|refold-from-anchor: loaded' "$log" 2>/dev/null; then
        note "G2 PASS: re-seeded+verified, no FATAL SHA3/count failure"
    else
        note "G2 FAIL: no re-seed confirmation line and/or could not confirm absence of FATAL"; fail=1
    fi

    # G3 — H* (provable tip) climbs STRICTLY PAST GATE_HSTAR_PAST.
    local hstar; hstar="$(hstar_of "$dd")"
    if [[ -n "$hstar" && "$hstar" -gt "${GATE_HSTAR_PAST}" ]]; then
        note "G3 PASS: H*=${hstar} > ${GATE_HSTAR_PAST}"
    else
        note "G3 FAIL: H*=${hstar:-<none>} not > ${GATE_HSTAR_PAST}"; fail=1
    fi

    # G4 — script_validate_log @ GATE_HSTAR_PAST flips ok=0 prevout_unresolved -> ok=1.
    local g4; g4="$(sql_one "$dd" \
        "SELECT ok FROM script_validate_log WHERE height=${GATE_HSTAR_PAST};")"
    if [[ "$g4" == "1" ]]; then
        note "G4 PASS: script_validate_log@${GATE_HSTAR_PAST} ok=1"
    else
        note "G4 FAIL: script_validate_log@${GATE_HSTAR_PAST} ok='${g4:-<none>}' (want 1)"; fail=1
    fi

    # G5 — tip_finalize_log has NO ok=0 status=floor_rewind rows (count==0).
    local g5; g5="$(sql_one "$dd" \
        "SELECT COUNT(*) FROM tip_finalize_log WHERE ok=0 AND status='floor_rewind';")"
    if [[ "${g5:-x}" == "0" ]]; then
        note "G5 PASS: tip_finalize_log ok=0 status=floor_rewind count=0"
    else
        note "G5 FAIL: tip_finalize_log floor_rewind rows=${g5:-<none>} (want 0)"; fail=1
    fi

    # G6 — coins contains coinbase 7E7894BF and EXCLUDES orphan 02663FF1.
    local has_good has_bad
    has_good="$(coins_has_txid_prefix "$dd" "7e7894bf")"
    has_bad="$(coins_has_txid_prefix "$dd" "02663ff1")"
    if [[ "$has_good" -ge 1 && "$has_bad" == "0" ]]; then
        note "G6 PASS: coinbase 7E7894BF present (${has_good}), orphan 02663FF1 absent"
    else
        note "G6 FAIL: good=${has_good} (want >=1) bad=${has_bad} (want 0)"; fail=1
    fi

    # G7 — no chain_linkage hold_active / window.consistency blocker LATCHED at end.
    # We require the cleared line AND no trailing raise after it.
    if grep -aqE 'window\.consistency cleared by' "$log" 2>/dev/null && \
       ! tail_has_unbalanced_window_raise "$log"; then
        note "G7 PASS: window.consistency cleared, no trailing hold_active latch"
    elif ! grep -aqE 'window\.consistency:' "$log" 2>/dev/null; then
        note "G7 PASS: window.consistency blocker never raised"
    else
        note "G7 FAIL: window.consistency / chain_linkage hold still latched (I4.3 2nd latch)"; fail=1
    fi

    # G8 — no operator_needed; 0 ok=0 rows across the six H*-bounding logs.
    if grep -aqE 'operator_needed' "$log" 2>/dev/null; then
        note "G8 FAIL: operator_needed present in log"; fail=1
    else
        local total0=0 t
        for tbl in validate_headers_log body_fetch_log body_persist_log \
                   script_validate_log proof_validate_log utxo_apply_log tip_finalize_log; do
            t="$(sql_one "$dd" "SELECT COUNT(*) FROM ${tbl} WHERE ok=0;")"
            [[ "${t:-0}" -gt 0 ]] && { note "  ${tbl}: ${t} ok=0 rows"; total0=$((total0 + t)); }
        done
        if [[ "$total0" == "0" ]]; then
            note "G8 PASS: no operator_needed; 0 ok=0 rows across the bounding logs"
        else
            note "G8 FAIL: ${total0} ok=0 rows across the bounding logs"; fail=1
        fi
    fi

    rule
    if [[ "$fail" == "0" ]]; then note "ALL GATES PASS (${label})"; return 0
    else note "GATE FAILURE (${label}) — see G# FAIL lines above"; return 1; fi
}

# H* over progress.kv — the CONTIGUOUS ok=1 prefix of utxo_apply_log from the
# anchor (NOT MAX(height)!). The contaminated tip leaves SPARSE ok=1 rows up at
# ~3150900+ (a measured 512 rows in [anchor+1, 3151411] for a 94k span), so
# MAX(height) WHERE ok=1 returns 3151411 and would FALSE-PASS G3. The true H* is
# the deepest H where EVERY height in [anchor+1, H] has an ok=1 row — a missing
# row OR ok=0 terminates the run (exact mirror of log_contiguous_prefix,
# app/jobs/src/reducer_frontier.c:198). On the wedged copy this returns the
# anchor (3056758); after the from-anchor fold it climbs past GATE_HSTAR_PAST.
#
# SQL form: number the ok=1 rows above the anchor in height order; a row is in
# the contiguous run iff height == anchor + its_row_number. The deepest such
# height is H*. (sqlq rejects a leading WITH — the query MUST start with SELECT,
# so the CTE is inlined as a subquery.)
hstar_of() {  # hstar_of DATADIR
    local dd="$1"
    sql_one "$dd" \
"SELECT COALESCE(MAX(height), ${ANCHOR}) FROM \
(SELECT height, ROW_NUMBER() OVER (ORDER BY height) AS rn \
 FROM utxo_apply_log WHERE height > ${ANCHOR} AND ok=1) \
WHERE height = ${ANCHOR} + rn;"
}

# SELECT-only one-value SQL over the datadir's progress.kv via sqlq (read-only).
sql_one() {  # sql_one DATADIR SQL
    local dd="$1" q="$2" db="${1}/progress.kv"
    [[ -f "$db" ]] || { printf ''; return 0; }
    "$SQLQ" "$db" "$q" 2>/dev/null | head -1 || printf ''
}

# Count coins whose lower-hex txid (first 4 bytes, on-disk forward order) starts
# with PREFIX (8 hex chars). Verified on the wedged copy: the orphan 02663ff1 is
# found with this exact forward encoding, so no byte-reversal is needed.
coins_has_txid_prefix() {  # coins_has_txid_prefix DATADIR PREFIX
    local dd="$1" pfx="$2"
    local n; n="$(sql_one "$dd" \
        "SELECT COUNT(*) FROM coins WHERE lower(hex(substr(txid,1,4)))='${pfx}';")"
    printf '%s' "${n:-0}"
}

# True if, AFTER the last 'cleared' line, a NEW window.consistency raise appears.
tail_has_unbalanced_window_raise() {  # $1 = log
    local log="$1"
    awk '
        /window\.consistency cleared by/ { lastclear=NR }
        /window\.consistency: I4\.3/      { lastraise=NR }
        END { exit (lastraise > lastclear) ? 0 : 1 }
    ' "$log" 2>/dev/null
}

# ───────────────────────── PHASE A: copy-prove ──────────────────────────────
phase_a() {
    rule; note "PHASE A — COPY-PROVE on the frozen copy (${COPY_DATADIR})"; rule

    local art; art="$(resolve_and_verify_artifact)"
    note "verified artifact: $art"

    # Place it where a NON-service manual boot resolves it: <datadir>/utxo-anchor.snapshot
    local dst="${COPY_DATADIR}/utxo-anchor.snapshot"
    note "copying artifact -> $dst (atomic)"
    cp -f "$art" "${dst}.tmp"
    sync
    mv -f "${dst}.tmp" "$dst"

    # Re-verify the placed copy's header (defends against a torn copy).
    local h c; h="$(le_uint "$dst" 16 4)"; c="$(le_uint "$dst" 24 8)"
    [[ "$h" == "${EXPECT_HEIGHT}" && "$c" == "${EXPECT_COUNT}" ]] || \
        die "placed copy header drifted (h=$h c=$c) — refusing to boot"

    # Boot the COPY OFFLINE on isolated ports, with -refold-from-anchor.
    # ZCL_MINT_ANCHOR_OUT MUST be unset so mint_snapshot_path resolves the datadir.
    local copylog="${COPY_DATADIR}/node.log"
    : > "$copylog"   # truncate the copy's log so gate scans see THIS run only
    note "booting COPY offline (-refold-from-anchor), log -> $copylog"
    env -u ZCL_MINT_ANCHOR_OUT \
        "$BIN" \
        -datadir="${COPY_DATADIR}" \
        -refold-from-anchor \
        -port="${COPY_PORT}" \
        -rpcport="${COPY_RPCPORT}" \
        -httpsport="${COPY_HTTPSPORT}" \
        -fsport="${COPY_FSPORT}" \
        -connect=127.0.0.1:1 \
        -nolegacyimport \
        -nobgvalidation \
        -showmetrics=0 \
        >>"$copylog" 2>&1 &
    COPY_PID=$!
    note "copy boot PID=${COPY_PID} (ONLY this PID will be signalled)"

    # Poll until H* climbs past the gate, a FATAL appears, or we hit the ceiling.
    local waited=0 hstar=""
    while :; do
        if ! kill -0 "${COPY_PID}" 2>/dev/null; then
            note "copy boot exited (PID ${COPY_PID}) after ${waited}s — evaluating final state"
            COPY_PID=""
            break
        fi
        if grep -aqE 'FATAL: -refold-from-anchor:.*FAILED the SHA3/count check' "$copylog" 2>/dev/null; then
            die "copy boot hit the FATAL SHA3/count check — the artifact does not reproduce the checkpoint"
        fi
        hstar="$(hstar_of "${COPY_DATADIR}")"
        if [[ -n "$hstar" && "$hstar" -gt "${GATE_HSTAR_PAST}" ]]; then
            note "copy H*=${hstar} climbed past ${GATE_HSTAR_PAST} after ${waited}s"
            break
        fi
        [[ "$waited" -ge "${COPY_BOOT_MAX_SECS}" ]] && \
            die "copy boot did not climb past ${GATE_HSTAR_PAST} within ${COPY_BOOT_MAX_SECS}s (H*=${hstar:-<none>})"
        sleep "${COPY_POLL_SECS}"; waited=$((waited + COPY_POLL_SECS))
    done

    # Stop OUR copy boot so progress.kv is quiescent for the gate SQL.
    cleanup_copy_boot

    evaluate_gates "$copylog" "${COPY_DATADIR}" "COPY" \
        || die "Phase A gates FAILED — NOT proceeding to live deploy"

    note "PHASE A PASSED."
    return 0
}

# ───────────────────────── PHASE B: live deploy ─────────────────────────────
phase_b() {
    rule; note "PHASE B — LIVE DEPLOY on ${LIVE_DATADIR}"; rule

    # Re-verify the artifact independently for the live placement.
    local art; art="$(resolve_and_verify_artifact)"
    note "verified artifact (live): $art"

    note "stopping the live node via its own unit (systemctl --user stop zclassic23)"
    systemctl --user stop zclassic23 || die "systemctl --user stop zclassic23 failed"
    # Wait for the unit to be inactive (its ExecStop drains WAL + Tor, up to 300s).
    local waited=0
    while systemctl --user is-active --quiet zclassic23; do
        [[ "$waited" -ge 300 ]] && die "zclassic23 unit did not go inactive within 300s"
        sleep 3; waited=$((waited + 3))
    done
    note "unit inactive after ${waited}s"

    # Place the artifact into the LIVE datadir (service can't see /tmp).
    local dst="${LIVE_DATADIR}/utxo-anchor.snapshot"
    note "copying artifact -> $dst (atomic)"
    cp -f "$art" "${dst}.tmp"; sync; mv -f "${dst}.tmp" "$dst"
    local h c; h="$(le_uint "$dst" 16 4)"; c="$(le_uint "$dst" 24 8)"
    [[ "$h" == "${EXPECT_HEIGHT}" && "$c" == "${EXPECT_COUNT}" ]] || \
        die "placed LIVE copy header drifted (h=$h c=$c) — refusing the manual run"

    # Run -refold-from-anchor MANUALLY (NOT via systemd) so node.log is observable
    # and ZCL_MINT_ANCHOR_OUT resolves to the datadir artifact.
    local livelog="${LIVE_DATADIR}/node.log"
    note "running -refold-from-anchor MANUALLY on the live datadir (log -> $livelog)"
    env -u ZCL_MINT_ANCHOR_OUT \
        "$BIN" \
        -datadir="${LIVE_DATADIR}" \
        -refold-from-anchor \
        -port=8023 \
        -rpcport=18232 \
        -httpsport=18443 \
        -fsport=18974 \
        -connect=127.0.0.1:1 \
        -nolegacyimport \
        -nobgvalidation \
        -showmetrics=0 \
        >>"$livelog" 2>&1 &
    COPY_PID=$!   # reuse the tracked-PID slot so cleanup/trap only touches THIS run
    note "live manual refold PID=${COPY_PID}"

    local waited2=0 hstar=""
    while :; do
        if ! kill -0 "${COPY_PID}" 2>/dev/null; then
            note "live manual refold exited (PID ${COPY_PID}) after ${waited2}s"; COPY_PID=""; break
        fi
        if grep -aqE 'FATAL: -refold-from-anchor:.*FAILED the SHA3/count check' "$livelog" 2>/dev/null; then
            die "LIVE refold hit the FATAL SHA3/count check — aborting, live unit left stopped for the operator"
        fi
        hstar="$(hstar_of "${LIVE_DATADIR}")"
        if [[ -n "$hstar" && "$hstar" -gt "${GATE_HSTAR_PAST}" ]]; then
            note "LIVE H*=${hstar} climbed past ${GATE_HSTAR_PAST} after ${waited2}s"; break
        fi
        [[ "$waited2" -ge "${COPY_BOOT_MAX_SECS}" ]] && \
            die "LIVE refold did not climb past ${GATE_HSTAR_PAST} within ${COPY_BOOT_MAX_SECS}s (H*=${hstar:-<none>})"
        sleep "${COPY_POLL_SECS}"; waited2=$((waited2 + COPY_POLL_SECS))
    done

    # Stop OUR manual run so progress.kv is quiescent, THEN gate it.
    cleanup_copy_boot

    evaluate_gates "$livelog" "${LIVE_DATADIR}" "LIVE" \
        || die "LIVE gates FAILED — leaving the unit STOPPED for operator review (do NOT auto-start)"

    note "LIVE gates PASSED — handing the datadir back to systemd"
    systemctl --user start zclassic23 || die "systemctl --user start zclassic23 failed (datadir is un-wedged; start by hand)"
    note "PHASE B COMPLETE — live node restarted on the un-wedged datadir"
    note "NOTE: for robust FUTURE boots, add -load-verify-boot to the unit's ExecStart"
    note "      (after this run only — it no-ops on the stamped coins_kv; it is the"
    note "      future-boot verifier, not the un-wedge driver). Insert it on the"
    note "      ExecStart continuation in ~/.config/systemd/user/zclassic23.service"
    note "      between -txindex and -tor, then systemctl --user daemon-reload."
}

# ────────────────────────────────── main ───────────────────────────────────
main() {
    local mode="${1:-copy-only}"
    case "$mode" in
        copy-only|"") ;;
        live-go) ;;
        *) die "unknown argument '${mode}'. Use: (nothing)|copy-only|live-go" ;;
    esac

    preflight
    phase_a    # always runs; aborts on any gate failure

    if [[ "$mode" == "live-go" ]]; then
        note "Phase A PASSED and 'live-go' given — proceeding to LIVE deploy"
        phase_b
    else
        rule
        note "Phase A PASSED. Phase B (LIVE) NOT run (no 'live-go' argument)."
        note "To cut over: docs/work/unwedge-deploy.sh live-go"
        rule
    fi
}

main "$@"
