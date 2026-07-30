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
#   - the serving peer has NO default and must be stated (ZCL_CS_PEER /
#     --peer / ZCL_PEER= via make); with nothing set the run SKIPs rather
#     than falling back to whatever is listening on the canonical port,
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
#   2  SKIP           — prerequisite absent (binary not built / no peer stated
#                        / peer unreachable). Not a verdict on C3 either way.
#                        A peer that accepts the TCP connection and closes it
#                        immediately is NOT a SKIP — it is labelled
#                        peer_precheck=accept_close, warned about loudly, and
#                        the run still reports the verdict the node earned.
#   5  FRONTIER-BUSY-TIMEOUT — `dumpstate reducer_frontier` kept returning a
#                        partial `{"snapshot_status":"progress_store_busy",
#                        "retryable":true}` doc that carried no usable provable
#                        sample (neither "hstar" NOR a cached_provable_tip
#                        proxy) for the entire busy-timeout window
#                        (--busy-timeout=SECS /
#                        ZCL_CS_FRONTIER_BUSY_TIMEOUT_SECS, default 120s) —
#                        this harness never observed a real frontier sample.
#                        Distinct from FAIL: this is an instrument failure
#                        ("we could not read the node's state"), not a claim
#                        about the node's actual progress.
#   6  READBACK-FAILED — no forward progress was OBSERVED and no blocker was
#                        named, but the final frontier readback (after bounded
#                        retries) yielded neither an authoritative "hstar" nor a
#                        cached_provable_tip proxy — the instrument could not
#                        read the node's provable tip at end-of-run. Carries the
#                        last good provable sample. Distinct from the silent-
#                        stall FAIL (exit 1): "we could not observe" is NOT "we
#                        observed nothing happening." Never PASS, never silent-
#                        stall — a FAIL with a named, honest cause.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tools/scripts/stopwatch_json_lib.sh
. "$REPO_ROOT/tools/scripts/stopwatch_json_lib.sh"
# shellcheck source=tools/scripts/source_identity_lib.sh
# The ONE source-identity reader (zcl_binary_source_id / zcl_json_first_string /
# zcl_json_first_sha256). Do NOT inline a tenth copy of that parser here —
# tools/lint/check_identity_parser_single.sh counts them and this file carries
# no baseline row, so it may carry ZERO.
. "$REPO_ROOT/tools/scripts/source_identity_lib.sh"

NODE_BIN="${ZCL_CS_NODE_BIN:-$REPO_ROOT/build/bin/zclassic23}"
# NO DEFAULT PEER, ON PURPOSE. This used to default to 127.0.0.1:8033 — the
# canonical/live node's P2P port on an operator host — so a bare
# `make mvp-coldstart-to-tip-stopwatch` silently pulled a full chain-data sync
# off the operator's own node without anyone asking for it. The peer a proof
# lane dials is part of the proof, so it must be stated, not inherited: with
# nothing set the run SKIPs (exit 2) and names the variable. See the
# no_peer_configured skip below.
PEER="${ZCL_CS_PEER:-}"
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
# Classification of what the peer did with a bare TCP connect (see
# peer_precheck below). Recorded in proof.json; never changes the verdict.
PEER_PRECHECK="unknown"
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
# Provable-sample tracking. The provable sample is the authoritative full-read
# H* when available, else the lock-free cached_provable_tip proxy the busy
# partial doc still carries (see rpc_frontier / frontier_provable_sample). This
# is the honest "did the node climb" signal: a busy-but-healthy fold that only
# ever exposed cached_provable_tip under load must NEVER be denied as a stall.
# The PASS predicate stays authoritative-only (max_hstar/network_tip); the proxy
# proves CLIMB, never mints a PASS.
first_ps=""
max_ps="-1"
last_ps="-1"
saw_ps=0
final_readback_failed="false"

# ── PER-PHASE MEASUREMENT STATE (the baseline instrument) ────────────────────
# Every one of these is either read from a real source or left at its
# never-measured sentinel. NOTHING here is ever defaulted to 0 to make a field
# look populated: -1 means "no honest source produced this", and a reader must
# be able to tell that apart from a genuine zero.
#
# The measurement contract this harness now keeps, on EVERY verdict including
# PASS (see capture_run_bundle / write_artifact): a run leaves the same
# per-phase record whether it passed or failed. Before this, the diagnostic
# bundle was captured only when `verdict != pass`, so a SUCCESSFUL run destroyed
# the exact per-stage fold-cost evidence needed to make the next run faster —
# an artifact set that exists only when something went wrong cannot be a
# baseline, and the owner's rule is no optimization before a baseline.
SAMPLES_TSV=""            # <artifact>/samples.tsv — the per-tick climb trace
LOOP_START_UNIX=0         # exact: date +%s at sample-loop entry
HEADER_IMPORT_START=0     # exact: date +%s before --importblockindex (0 = not run)
HEADER_IMPORT_MS=-1       # exact: measured import duration, -1 = phase not run
NODE_BIN_SOURCE_ID=""     # baked source identity of the binary under test
# Cumulative counters of the FINAL node process, refreshed every sample tick.
# Cumulative PER PROCESS: a followed self-respawn starts a new PID, so these
# reset at each boot boundary. samples.tsv carries the boot ordinal on every row
# precisely so a reader can see where that reset happened instead of reading a
# reset as negative progress.
LAST_CPU_SECONDS="-1"
LAST_RSS_KB="-1"
LAST_DISK_READ_BYTES="-1"
LAST_DISK_WRITE_BYTES="-1"
# getconf values are constants for the life of the process; read once.
CLK_TCK="$(getconf CLK_TCK 2>/dev/null || echo 100)"
case "$CLK_TCK" in ''|*[!0-9]*) CLK_TCK=100 ;; esac
PAGE_KB=$(( $(getconf PAGESIZE 2>/dev/null || echo 4096) / 1024 ))
[ "$PAGE_KB" -gt 0 ] 2>/dev/null || PAGE_KB=4

# parse_proc_stat_cpu_ticks <contents-of-/proc/PID/stat> — utime+stime in clock
# ticks, or -1. PURE (no /proc access) so --selftest can exercise it on a canned
# fixture. Field 2 (comm) is parenthesized and MAY CONTAIN SPACES AND
# PARENTHESES, so a naive $14/$15 read is wrong; everything is indexed from
# after the LAST ')' instead, which is what proc(5) itself prescribes. With that
# split, utime is field 12 and stime field 13.
parse_proc_stat_cpu_ticks() {
    printf '%s' "${1:-}" | awk '
        { i = index($0, ")"); last = 0
          while (i > 0) { last += i; rest = substr($0, last + 1); i = index(rest, ")") }
          if (last == 0) { print -1; exit }
          n = split(substr($0, last + 2), f, /[ \t]+/)
          if (n < 13) { print -1; exit }
          if (f[12] !~ /^[0-9]+$/ || f[13] !~ /^[0-9]+$/) { print -1; exit }
          print f[12] + f[13] }
        END { if (NR == 0) print -1 }'
}

# parse_proc_stat_rss_pages <contents-of-/proc/PID/stat> — the rss field in
# pages, or -1. Same after-the-last-')' indexing as above; rss is field 22
# there. PURE.
parse_proc_stat_rss_pages() {
    printf '%s' "${1:-}" | awk '
        { i = index($0, ")"); last = 0
          while (i > 0) { last += i; rest = substr($0, last + 1); i = index(rest, ")") }
          if (last == 0) { print -1; exit }
          n = split(substr($0, last + 2), f, /[ \t]+/)
          if (n < 22 || f[22] !~ /^[0-9]+$/) { print -1; exit }
          print f[22] }
        END { if (NR == 0) print -1 }'
}

# parse_proc_io_field <contents-of-/proc/PID/io> <key> — the integer value of
# `<key>: N`, or -1 when absent/unreadable. PURE. read_bytes/write_bytes are the
# BLOCK-LAYER counters (bytes that actually hit the storage device), which is
# what "disk" means for a fold-cost baseline — rchar/wchar would also count
# page-cache hits.
parse_proc_io_field() {
    printf '%s\n' "${1:-}" | awk -v k="${2:-}" '
        $1 == k ":" && $2 ~ /^[0-9]+$/ { print $2; found = 1; exit }
        END { if (!found) print -1 }'
}

# refresh_process_counters — read the watched PID's cumulative CPU/RSS/disk
# counters out of /proc into LAST_*. Every field independently degrades to -1;
# an unreadable /proc entry is a missing measurement, never a zero.
refresh_process_counters() {
    LAST_CPU_SECONDS="-1"; LAST_RSS_KB="-1"
    LAST_DISK_READ_BYTES="-1"; LAST_DISK_WRITE_BYTES="-1"
    [ -n "${PID:-}" ] || return 0
    local statline ioblob ticks pages
    statline="$(cat "/proc/$PID/stat" 2>/dev/null)"
    if [ -n "$statline" ]; then
        ticks="$(parse_proc_stat_cpu_ticks "$statline")"
        if [ "$ticks" != "-1" ]; then
            # Two decimals of CPU seconds without floating-point shell math.
            LAST_CPU_SECONDS="$(( ticks * 100 / CLK_TCK ))"
            LAST_CPU_SECONDS="$(( LAST_CPU_SECONDS / 100 )).$(printf '%02d' "$(( LAST_CPU_SECONDS % 100 ))")"
        fi
        pages="$(parse_proc_stat_rss_pages "$statline")"
        [ "$pages" != "-1" ] && LAST_RSS_KB="$(( pages * PAGE_KB ))"
    fi
    ioblob="$(cat "/proc/$PID/io" 2>/dev/null)"
    if [ -n "$ioblob" ]; then
        LAST_DISK_READ_BYTES="$(parse_proc_io_field "$ioblob" read_bytes)"
        LAST_DISK_WRITE_BYTES="$(parse_proc_io_field "$ioblob" write_bytes)"
    fi
    return 0
}

# samples_tsv_init — create the per-tick sink and write its header row. Called
# once, before the sample loop, so the trace survives even a SIGKILLed harness.
# The rows were previously printf-to-stdout ONLY and were lost with the
# terminal: the shape of the climb, not just its endpoint, is what tells you
# WHICH phase to optimize.
samples_tsv_init() {
    [ -n "$SAMPLES_TSV" ] || return 0
    printf 't_s\tunix_s\tboot\thstar\tprovable\tnetwork_tip\ttip_ok\tfrontier_busy\tblocker_count\tcpu_seconds\trss_kb\tdisk_read_bytes\tdisk_write_bytes\tblocker_ids\n' \
        >"$SAMPLES_TSV" 2>/dev/null || SAMPLES_TSV=""
}

# samples_tsv_row <t_s> <hstar> <provable> <net_tip> <tip_ok> <busy> <blocker_ct> <blocker_ids>
# Append one tick. Every numeric column is either a real reading or -1.
samples_tsv_row() {
    [ -n "$SAMPLES_TSV" ] && [ -f "$SAMPLES_TSV" ] || return 0
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$1" "$(date +%s)" "$boots" "$2" "$3" "$4" "$5" "$6" "$7" \
        "$LAST_CPU_SECONDS" "$LAST_RSS_KB" \
        "$LAST_DISK_READ_BYTES" "$LAST_DISK_WRITE_BYTES" "$8" \
        >>"$SAMPLES_TSV" 2>/dev/null || true
}

# json_escape/json_string/json_number_or_null/is_busy_response/jget: see
# stopwatch_json_lib.sh (sourced above) — used by rpc_frontier() and the
# failure-bundle capture below.

# ── phases[]: WHERE THE TIME WENT, AND FROM WHICH SOURCE ────────────────────
#
# PROVENANCE RULE FOR THIS WHOLE SECTION: every field emitted names the source
# that produced it, and a field with no honest source today is OMITTED, never
# emitted as 0. A zero that a later reader mistakes for a measurement is worse
# than an absent field, because it silently anchors the next optimization pass
# to a number nobody measured.
#
# Deliberately NOT emitted, because nothing in this tree sources them per phase:
#   * network bytes — no dumpstate subsystem exposes a peer/socket byte counter
#     (`network`, `connman`, `peer_lifecycle` carry connection and handshake
#     counts, not bytes), and /proc has no per-process network accounting. The
#     download manager's total_bytes_received exists in C
#     (lib/net/src/download.c) but reaches no diagnostics dumper, so this
#     harness cannot read it.
#   * per-phase CPU / disk for the BOOT phases — boot.c's markers carry a
#     duration and nothing else, and /proc counters are only sampled once the
#     harness's own loop is running (boot has already finished by the first
#     tick). CPU/disk are therefore attached ONLY to the harness-bracketed
#     phase, where the harness genuinely bracketed the window it is reporting.
#   * an exact wall-clock start per boot phase — boot.c's `[boot]` marker lines
#     carry NO timestamp of their own, and the top-level marks do not tile the
#     boot (measured on the one real PASS artifact on disk,
#     20260728T000207Z-2102851: 15,623ms of named top-level phases against a
#     51,755ms `total` — ~36s unattributed), so a cumulative-sum "start" would
#     be fabricated. What IS honest is start_ts_lower_bound: the most recent
#     TIMESTAMPED node.log line at or before the marker, i.e. a real bound
#     ("this phase did not start before this"), named as the bound it is.

# boot_timings_median_pairs <boot_timings.json> — emit `stage<TAB>median_ms`
# rows from a captured `dumpstate boot_timings` doc (the flight recorder's
# durable per-stage history, config/src/boot_flight_recorder.c: rows are
# {"stage":..,"last_ms":..,"median_ms":..}, median_ms present only once a stage
# has >=3 retained samples). Splitting on '}' isolates each row so the shared
# readers below anchor within one row instead of across the whole doc.
boot_timings_median_pairs() {
    [ -s "${1:-}" ] || return 0
    local chunk st md
    tr '}' '\n' <"$1" 2>/dev/null | while IFS= read -r chunk; do
        case "$chunk" in *'"stage"'*) ;; *) continue ;; esac
        st="$(zcl_json_first_string "$chunk" stage)"
        md="$(jget "$chunk" median_ms)"
        [ -n "$st" ] && [ -n "$md" ] && printf '%s\t%s\n' "$st" "$md"
    done
    return 0
}

# phases_json_from_log <node.log> <median-tsv> — emit the boot-phase elements of
# phases[] (comma-separated JSON objects, NO enclosing brackets) parsed from the
# node's own `[boot]` markers.
#
# The marker format is boot.c's boot_topmark/boot_submark:
#   "[boot] %-30s %lldms\n"      (top-level phase, ONE space after "[boot]")
#   "[boot]   %-28s %lldms\n"    (sub-phase,     THREE spaces)
# so a marker line is EXACTLY two whitespace-separated tokens after the prefix:
# a [a-z_.0-9] name and an integer followed by "ms". Requiring exactly two is
# what keeps the many prose "[boot] ..." lines out (there are 74 "[boot]" lines
# in the real PASS artifact and only 59 are markers) — a looser match pulls in
# "[boot] block_index: 1 entries, 296 bytes/entry, ..." and invents phases.
#
# `boot` is the 1-based boot ordinal within the run: it increments on each
# top-level "prologue" marker, which boot.c emits as the first topmark of every
# boot (config/src/boot.c boot_topmark("prologue", t_boot_start)). A run that
# followed a self-respawn therefore reports each boot's phases separately rather
# than silently blending two boots' timings into one set of names.
phases_json_from_log() {
    local log="${1:-}" med="${2:-}"
    [ -s "$log" ] || return 0
    awk -v medfile="$med" '
        BEGIN {
            if (medfile != "")
                while ((getline ln < medfile) > 0)
                    if (split(ln, mf, "\t") >= 2) med[mf[1]] = mf[2]
            boot = 0; seq = 0; lastts = ""; first = 1
        }
        /^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]T[0-9][0-9]:[0-9][0-9]:[0-9][0-9]Z/ {
            lastts = substr($0, 1, 20)
        }
        {
            lvl = ""
            if ($0 ~ /^\[boot\]   [^ ]/)     lvl = "subphase"
            else if ($0 ~ /^\[boot\] [^ ]/)  lvl = "phase"
            if (lvl == "") next
            line = $0
            sub(/^\[boot\][ ]+/, "", line)
            if (split(line, g, /[ \t]+/) != 2) next
            nm = g[1]; msf = g[2]
            if (nm !~ /^[a-z_.0-9]+$/) next
            if (msf !~ /^[0-9]+ms$/) next
            ms = msf; sub(/ms$/, "", ms)
            if (nm == "prologue" && lvl == "phase") { boot++; seq = 0 }
            if (boot == 0) boot = 1
            seq++
            printf "%s{\"phase\":%c%s%c,\"boot\":%d,\"seq\":%d,\"level\":%c%s%c",
                   (first ? "" : ","), 34, nm, 34, boot, seq, 34, lvl, 34
            printf ",\"duration_ms\":%s,\"duration_source\":%cnode_log_boot_marker%c",
                   ms, 34, 34
            if (lastts != "")
                printf ",\"start_ts_lower_bound\":%c%s%c,\"start_source\":%cnearest_preceding_timestamped_node_log_line%c",
                       34, lastts, 34, 34, 34
            if (nm in med)
                printf ",\"median_ms\":%s,\"median_source\":%cdumpstate_boot_timings%c",
                       med[nm], 34, 34
            printf "}"
            first = 0
        }
    ' "$log" 2>/dev/null
    return 0
}

# harness_phases_json <captured_at_unix> — emit the elements of phases[] the
# HARNESS itself bracketed, and so can timestamp exactly: it took `date +%s` on
# both sides of each window. These are the only phases carrying cpu/disk, and
# they carry them because the harness sampled /proc across that same window.
harness_phases_json() {
    local captured_at="${1:-0}" out="" dur
    if [ "${HEADER_IMPORT_MS:--1}" != "-1" ] && [ "${HEADER_IMPORT_START:-0}" -gt 0 ] 2>/dev/null; then
        out="{\"phase\":\"harness.header_import\",\"level\":\"harness\""
        out="$out,\"start_unix\":$HEADER_IMPORT_START"
        out="$out,\"duration_ms\":$HEADER_IMPORT_MS"
        out="$out,\"duration_source\":\"harness wall clock around --importblockindex\"}"
    fi
    if [ "${LOOP_START_UNIX:-0}" -gt 0 ] 2>/dev/null; then
        dur=-1
        [ "$captured_at" -ge "$LOOP_START_UNIX" ] 2>/dev/null &&
            dur=$(( (captured_at - LOOP_START_UNIX) * 1000 ))
        [ -n "$out" ] && out="$out,"
        out="$out{\"phase\":\"harness.observed_sync\",\"level\":\"harness\""
        out="$out,\"boot\":$(json_number_or_null "$boots")"
        out="$out,\"start_unix\":$LOOP_START_UNIX"
        out="$out,\"duration_ms\":$(json_number_or_null "$dur")"
        out="$out,\"duration_source\":\"harness wall clock from sample-loop entry to artifact capture\""
        out="$out,\"cpu_seconds\":$LAST_CPU_SECONDS"
        out="$out,\"rss_kb\":$(json_number_or_null "$LAST_RSS_KB")"
        out="$out,\"disk_read_bytes\":$(json_number_or_null "$LAST_DISK_READ_BYTES")"
        out="$out,\"disk_write_bytes\":$(json_number_or_null "$LAST_DISK_WRITE_BYTES")"
        out="$out,\"counters_source\":\"/proc/<pid>/stat utime+stime over CLK_TCK and rss pages; /proc/<pid>/io read_bytes and write_bytes (block layer)\""
        out="$out,\"counters_scope\":\"cumulative for the FINAL node process only — a followed self-respawn starts a new pid and resets these; see samples.tsv boot column\"}"
    fi
    printf '%s' "$out"
    return 0
}

# omitted_fields_json — the elements of omitted_fields[] (comma-separated JSON
# objects, NO enclosing brackets): every field the owner's measurement brief
# named that this run did NOT record, BY NAME, with the reason and the nearest
# honest substitute.
#
# WHY THIS IS A FIRST-CLASS ARTIFACT SECTION AND NOT A COMMENT. The brief asks
# for phase time, bytes, CPU, disk, blocker, final H*, peer tip, and source
# identity. Some of those have no honest per-phase source in this tree today.
# A silently absent field reads to the next reader as "measured, and fine" —
# which is how a baseline acquires a number nobody took. So an unmeasurable
# field is RECORDED as unmeasured, named exactly as the brief named it, and the
# reason is stated. The rule this enforces: never fabricate a field, never
# estimate one and present it as measured, never quietly drop one.
#
# Two classes of row:
#   structural — no source exists in this tree at all, for any run. These are
#                constant and are the honest to-do list for the instrument.
#   this_run   — a source EXISTS but this particular run could not read it
#                (binary unidentifiable, /proc unreadable, node.log absent).
#                This is the degrade-don't-crash path: the run still reports the
#                verdict the node earned, and says which readings it lost.
omitted_fields_json() {
    local out="" f
    _of_row() {  # field, scope, reason, substitute
        [ -n "$out" ] && out="$out,"
        out="$out{\"field\":$(json_string "$1"),\"scope\":$(json_string "$2")"
        out="$out,\"reason\":$(json_string "$3")"
        out="$out,\"nearest_honest_substitute\":$(json_string "$4")}"
    }
    # ── structural: nothing in this tree sources these ──────────────────────
    _of_row "phases[].network_bytes" "structural" \
        "no diagnostics dumper exposes a peer/socket byte counter (network, connman and peer_lifecycle carry connection and handshake COUNTS, not bytes), and /proc has no per-process network accounting. lib/net/src/download.c does track total_bytes_received in C but it reaches no dumper, so this harness has nothing to read." \
        "none. Bytes are UNMEASURED this run — do not infer them from wall-clock time."
    _of_row "phases[].cpu_seconds (boot-level phases)" "structural" \
        "config/src/boot.c boot_topmark/boot_submark emit a phase NAME and a DURATION and nothing else. The harness's own /proc sampling only starts once its sample loop is running, by which time boot has already finished, so there is no window it genuinely bracketed." \
        "harness.observed_sync carries cpu_seconds for the window the harness DID bracket; samples.tsv carries the per-tick series."
    _of_row "phases[].disk_read_bytes / phases[].disk_write_bytes (boot-level phases)" "structural" \
        "same as cpu_seconds: the boot markers carry only a duration, and the harness was not yet sampling /proc during boot." \
        "harness.observed_sync + samples.tsv, for the observed-sync window only."
    _of_row "phases[].start_unix (boot-level phases)" "structural" \
        "the [boot] marker lines carry no timestamp of their own, and the top-level marks do not tile the boot (on the one real PASS artifact on disk, 20260728T000207Z-2102851, named top-level phases sum to 15,623ms against a 51,755ms total — ~36s unattributed), so a cumulative-sum start would be fabricated." \
        "start_ts_lower_bound: the nearest PRECEDING timestamped node.log line. It is a bound ('this phase did not start before this'), named as one, never presented as a start."
    _of_row "phases[].blocker (per boot-level phase)" "structural" \
        "a named blocker is raised by a REDUCER STAGE (dumpstate blocker / stage-*.json), not by a boot marker; there is no mapping from a boot phase name to a blocker id, and inventing one would attribute a stall to a phase that did not raise it." \
        "samples.tsv blocker_count/blocker_ids per tick, plus blocker.json and stage-*.json in this artifact dir."
    _of_row "phases[].hstar / phases[].peer_tip (per boot-level phase)" "structural" \
        "H* and network_tip are read over RPC, which is not serving during most of boot; a boot phase therefore has no H* of its own." \
        "samples.tsv hstar/network_tip per tick, and the run-level final_hstar / final_network_tip / measured_identity.peer_advertised_tip in this file."
    # ── this_run: a source exists, this run could not read it ───────────────
    if [ -z "${NODE_BIN_SOURCE_ID:-}" ]; then
        _of_row "measured_identity.node_bin_source_id_sha256" "this_run" \
            "zcl_binary_source_id (tools/scripts/source_identity_lib.sh) returned no 64-hex source id for this binary — it is absent, not executable, or its agentbuild output carried none. The field is null rather than guessed." \
            "measured_identity.node_bin (the path that was run). The BUILD behind it is unidentified for this run."
    fi
    if [ -z "${SAMPLES_TSV:-}" ] || [ ! -f "${SAMPLES_TSV:-/nonexistent}" ]; then
        _of_row "samples.tsv" "this_run" \
            "the per-tick sink could not be created or the run ended before the sample loop was armed (an early skip/fail has no ticks to record)." \
            "the summary first/max/final fields in this file. The SHAPE of the climb is unrecorded for this run."
    fi
    if [ ! -f "$ARTIFACT_DIR/node.log" ]; then
        _of_row "phases[] boot-level elements" "this_run" \
            "node.log was not captured into this artifact dir, so the node's own [boot] markers could not be parsed. Boot phase durations are unrecorded for this run." \
            "the harness.* elements, which the harness bracketed itself and does not need node.log for."
    fi
    for f in cpu_seconds rss_kb disk_read_bytes disk_write_bytes; do
        case "$f" in
            cpu_seconds)       [ "${LAST_CPU_SECONDS:--1}" = "-1" ] || continue ;;
            rss_kb)            [ "${LAST_RSS_KB:--1}" = "-1" ] || continue ;;
            disk_read_bytes)   [ "${LAST_DISK_READ_BYTES:--1}" = "-1" ] || continue ;;
            disk_write_bytes)  [ "${LAST_DISK_WRITE_BYTES:--1}" = "-1" ] || continue ;;
        esac
        _of_row "harness.observed_sync.$f" "this_run" \
            "/proc/<pid>/stat or /proc/<pid>/io was unreadable at capture (most often: the node process had already exited). Reported as -1, which is the never-measured sentinel and is deliberately distinguishable from a real zero." \
            "samples.tsv, whose earlier rows may carry a reading from while the process was alive."
    done
    unset -f _of_row
    printf '%s' "$out"
    return 0
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

# frontier_hstar_full <frontier-doc> — the AUTHORITATIVE reducer-frontier H*,
# present only in a FULL read (after the progress-store trylock succeeds). Echoes
# -1 for a busy partial doc / an empty response — the proxy is deliberately NOT
# substituted here, so the PASS predicate can never be minted from a proxy.
frontier_hstar_full() {
    local h; h="$(jget "$1" hstar)"; [ -z "$h" ] && h="-1"; printf '%s' "$h"
}

# frontier_provable_sample <frontier-doc> — the provable tip actually usable for
# PROGRESS honesty: the authoritative full-read "hstar" when present, else the
# lock-free "cached_provable_tip" the busy partial doc still carries (it is
# emitted BEFORE the trylock in reducer_frontier_dump.c, exactly so a diagnostic
# read during a busy fold still learns the served provable tip — the same proxy
# ~/.local/state/zclassic23-cure/run-anchor-refold-proof-9.sh read_hstar() falls
# back to). Echoes -1 only when NEITHER is available (opaque/empty response), so
# a read miss is never faked into a sample.
frontier_provable_sample() {
    local h c
    h="$(jget "$1" hstar)"
    if [ -n "$h" ] && [ "$h" != "-1" ]; then printf '%s' "$h"; return 0; fi
    c="$(jget "$1" cached_provable_tip)"; [ -z "$c" ] && c="-1"
    printf '%s' "$c"
}

# blocker_ids <blocker-doc> — comma-joined "id" values of a `dumpstate blocker`
# doc (empty if none / unreadable).
blocker_ids() {
    printf '%s' "$1" | tr -d '\n' |
        grep -oE '"id"[[:space:]]*:[[:space:]]*"[^"]*"' |
        sed -E 's/.*"id"[[:space:]]*:[[:space:]]*"([^"]*)"/\1/' | paste -sd, -
}

# classify_final_verdict — the PURE end-of-run decision, factored out of the
# artifact/exit plumbing so its precedence is unit-testable (see --selftest).
# Echoes exactly one token:
#   pass            — H* reached network_tip (authoritative), decided upstream.
#   seam            — the PROVABLE SAMPLE (authoritative H* OR cached_provable_tip
#                     proxy) strictly climbed but did not catch tip in budget:
#                     real forward progress. A busy-but-healthy fold that only
#                     ever exposed the proxy lands HERE, never in silent-stall.
#   stalled-named   — no observed climb, but a named blocker explains why.
#   readback-failed — no observed climb, no blocker, and the final readback
#                     failed (or no sample was ever taken): an INSTRUMENT
#                     failure, not an observed stall.
#   silent-stall    — no observed climb, no blocker, and we COULD read the
#                     provable tip throughout — a genuine silent stall.
# Args: reached first_ps max_ps saw_ps final_readback_failed last_blocker_count
classify_final_verdict() {
    local reached="$1" f_ps="$2" m_ps="$3" saw="$4" rbf="$5" bc="$6"
    [ "$reached" = 1 ] && { printf 'pass'; return 0; }
    if [ -n "$f_ps" ] && [ "$m_ps" -gt "$f_ps" ] 2>/dev/null; then
        printf 'seam'; return 0
    fi
    if [ "${bc:-0}" -gt 0 ] 2>/dev/null; then
        printf 'stalled-named'; return 0
    fi
    if [ "$rbf" = "true" ] || [ "$saw" = "0" ]; then
        printf 'readback-failed'; return 0
    fi
    printf 'silent-stall'
}

# classify_peer_precheck <probe-rc> — pure mapping from the peer_precheck()
# probe's exit code to one token. Kept separate from the probe itself so its
# precedence is unit-testable (see --selftest).
#   unreachable  — TCP connect failed or the whole probe timed out.
#   held_open    — the peer kept the socket open (or spoke first): the serving
#                  shape. An outbound handshake can proceed (we send version).
#   accept_close — the peer accepted the TCP connection and closed it
#                  immediately, before a single byte could be exchanged. No
#                  handshake is possible, so `network_tip` can never be read
#                  and the PASS predicate is unreachable by construction.
classify_peer_precheck() {
    case "${1:-}" in
        10|11) printf 'held_open' ;;
        12)    printf 'accept_close' ;;
        *)     printf 'unreachable' ;;
    esac
}

# peer_precheck <host> <port> — connect and observe WITHOUT sending a byte.
#
# This exists because a bare "did TCP connect succeed" test is only a valid
# serving-peer test on LOOPBACK. Against a remote peer, a serving node can
# accept() and then immediately close — e.g. the per-IP inbound sybil cap in
# lib/net/src/net.c ("too many inbound connections from same IP: count=%d",
# max 3), which another node on the SAME host can have already saturated. The
# connect still succeeds, so the old check reported "reachable" and the run
# burned its entire budget against a peer that would never handshake.
#
# Deliberately ADVISORY: it labels the run, it never converts a verdict. A
# refusing peer still produces the honest STALLED-NAMED/SEAM/FAIL class the
# node actually earned — it is never rounded down to SKIP.
peer_precheck() {
    ZCL_PP_HOST="$1" ZCL_PP_PORT="$2" timeout 8 bash -c '
        exec 3<>/dev/tcp/$ZCL_PP_HOST/$ZCL_PP_PORT || exit 9
        if IFS= read -r -t 5 -n 1 -u 3 _b; then exit 10; fi
        rc=$?
        if [ "$rc" -gt 128 ]; then exit 11; fi
        exit 12
    ' >/dev/null 2>&1
    classify_peer_precheck "$?"
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

    # Provable-sample extraction: a FULL read yields the authoritative hstar; a
    # busy partial doc (no hstar) falls back to cached_provable_tip WITHOUT ever
    # promoting the proxy into the authoritative hstar; a busy doc whose proxy is
    # still -1 (pre-fold) and a truly empty response both yield NO usable sample.
    st_full_json='{"cached_provable_tip":3107000,"hstar":3107923,"network_tip":3190019,"network_tip_read_ok":true}'
    st_busy_cpt_json='{"cached_provable_tip":3107923,"snapshot_status":"progress_store_busy","retryable":true}'
    st_busy_nocpt_json='{"cached_provable_tip":-1,"snapshot_status":"progress_store_busy","retryable":true}'
    st_ps_check() {  # desc, expect, actual
        if [ "$3" = "$2" ]; then
            echo "  ok: $1"
        else
            echo "  FAIL: $1 (expected '$2' got '$3')"
            st_fail=1
        fi
    }
    st_ps_check "full read: provable sample IS the authoritative hstar" 3107923 "$(frontier_provable_sample "$st_full_json")"
    st_ps_check "full read: hstar_full IS the authoritative hstar" 3107923 "$(frontier_hstar_full "$st_full_json")"
    st_ps_check "busy doc: provable sample falls back to cached_provable_tip" 3107923 "$(frontier_provable_sample "$st_busy_cpt_json")"
    st_ps_check "busy doc: hstar_full stays -1 (proxy never becomes authoritative)" -1 "$(frontier_hstar_full "$st_busy_cpt_json")"
    st_ps_check "busy doc, proxy=-1 (pre-fold): NO usable provable sample" -1 "$(frontier_provable_sample "$st_busy_nocpt_json")"
    st_ps_check "empty response: NO usable provable sample" -1 "$(frontier_provable_sample "")"
    st_ps_check "full read: network_tip extracted, not confused with network_tip_read_ok" 3190019 "$(jget "$st_full_json" network_tip)"

    # Final-verdict precedence. Args: reached first_ps max_ps saw rbf blocker_ct.
    # The headline regression this fixes: the 20260724T060944Z-880436 run — a
    # healthy fold whose provable tip climbed 3056758 -> 3117923 under load —
    # must classify SEAM, NEVER silent-stall.
    st_ps_check "reached tip -> pass" pass "$(classify_final_verdict 1 3056758 3192164 1 false 0)"
    st_ps_check "climbing proxy under load (the real failing run) -> seam, not silent-stall" \
        seam "$(classify_final_verdict 0 3056758 3117923 1 false 0)"
    st_ps_check "climb wins even if final readback later failed -> seam" \
        seam "$(classify_final_verdict 0 3056758 3117923 1 true 0)"
    st_ps_check "flat + named blocker -> stalled-named" \
        stalled-named "$(classify_final_verdict 0 3100000 3100000 1 false 2)"
    st_ps_check "never sampled all run (all-empty readback) -> readback-failed, not silent-stall" \
        readback-failed "$(classify_final_verdict 0 '' -1 0 true 0)"
    st_ps_check "sampled then final readback failed, no climb/blocker -> readback-failed" \
        readback-failed "$(classify_final_verdict 0 3100000 3100000 1 true 0)"
    st_ps_check "genuinely flat, readable throughout, no blocker -> silent-stall (preserved)" \
        silent-stall "$(classify_final_verdict 0 3100000 3100000 1 false 0)"

    # Peer-precheck classification. The headline case this fixes: a REMOTE
    # serving peer whose per-IP inbound cap is already saturated accepts the
    # TCP connection and closes it instantly — TCP-connect "reachable" but
    # no handshake is possible. Loopback can never show this.
    st_ps_check "connect refused/timed out -> unreachable" unreachable "$(classify_peer_precheck 9)"
    st_ps_check "whole probe timed out -> unreachable" unreachable "$(classify_peer_precheck 124)"
    st_ps_check "peer spoke first -> held_open" held_open "$(classify_peer_precheck 10)"
    st_ps_check "peer kept the socket open waiting for our version -> held_open" held_open "$(classify_peer_precheck 11)"
    st_ps_check "peer closed at accept, zero bytes -> accept_close" accept_close "$(classify_peer_precheck 12)"
    st_ps_check "unknown probe rc -> unreachable (never silently held_open)" unreachable "$(classify_peer_precheck 77)"

    # No-implicit-peer guardrail. This harness once defaulted its serving peer
    # to 127.0.0.1:8033 — the canonical node's own P2P port — so a bare
    # `make mvp-coldstart-to-tip-stopwatch` quietly pulled a full chain sync off
    # the operator's live node. The fix is a stated peer or a SKIP, and these
    # three checks are what stop it from creeping back: the source must carry no
    # `ZCL_CS_PEER` fallback value, must not hardcode the canonical port as the
    # PEER default, and must still contain the refusal. Patterns are assembled
    # from concatenated literals so they cannot match their own source lines.
    st_pat_fallback='ZCL_CS_PEER'':-[^}]'
    st_pat_port='^PEER=.*8033'
    grep -qE "$st_pat_fallback" "${BASH_SOURCE[0]}"
    st_check "source carries no ZCL_CS_PEER fallback value (peer must be stated)" 1 $?
    grep -qE "$st_pat_port" "${BASH_SOURCE[0]}"
    st_check "PEER is not defaulted to the canonical 8033 P2P port" 1 $?
    grep -q 'no_peer_configured' "${BASH_SOURCE[0]}"
    st_check "the empty-peer refusal is still wired" 0 $?

    # ── MEASURED-BASELINE GUARDRAILS ────────────────────────────────────────
    # The defect these three exist to stop from growing back: the diagnostic
    # bundle was captured only when `verdict != pass`, so a PASSING run left no
    # per-phase evidence and the one artifact class worth optimizing against
    # destroyed its own measurements. Same shape as the no-implicit-peer
    # guardrail above — assert on this file's own source text, with patterns
    # assembled from concatenated literals so they cannot match their own
    # source lines.
    st_pat_pass_guard='verdict" != "pass" \]'' && capture'
    grep -qE "$st_pat_pass_guard" "${BASH_SOURCE[0]}"
    st_check "capture is NOT gated behind a non-pass verdict check (the pass/non-pass asymmetry stays deleted)" 1 $?
    grep -qE '^ {4}capture_run_bundle$' "${BASH_SOURCE[0]}"
    st_check "capture_run_bundle is called UNCONDITIONALLY in write_artifact" 0 $?
    # These next two are ANCHORED ON THE CALL SITE, not on the bare name, and
    # that is the whole point. Both started life as `grep -q 'samples_tsv_row '`
    # and `grep -q 'dumpstate boot_timings'` — and a mutation run proved both
    # were hollow: the names also occur in this file's own comments and function
    # definitions, so prefixing the real call with `:` (neutering it completely)
    # or repointing the capture at a different dumpstate key left BOTH checks
    # green. A source-text assertion has to pin the LINE THAT DOES THE WORK: the
    # call at its loop indentation with its first real argument, and the capture
    # invocation with the binary that performs it.
    grep -qE '^ {4}samples_tsv_row "\$elapsed"' "${BASH_SOURCE[0]}"
    st_check "the per-tick samples.tsv sink is CALLED in the sample loop (not merely mentioned)" 0 $?
    grep -qE '^ {8}"\$NODE_BIN".*dumpstate boot_timings' "${BASH_SOURCE[0]}"
    st_check "boot_timings (the median source for phases[]) is actually captured by the bundle" 0 $?

    # /proc parsers. The comm field is parenthesized AND may itself contain
    # spaces and parentheses, which is exactly what breaks a naive $14/$15 read
    # — the second fixture is a process literally named ") x (y z" and both
    # parsers must still land on the right fields.
    st_stat_plain='4242 (zclassic23) S 1 4242 4242 0 -1 4194560 900 0 3 0 731 219 0 0 20 0 12 0 55 9999 262144 0 0 0 0 0 0 0 0 0 0 0 0 0 17 3'
    st_stat_nasty='4242 () x (y z) S 1 4242 4242 0 -1 4194560 900 0 3 0 731 219 0 0 20 0 12 0 55 9999 262144 0'
    st_ps_check "proc stat: cpu ticks are utime+stime (731+219)" 950 "$(parse_proc_stat_cpu_ticks "$st_stat_plain")"
    st_ps_check "proc stat: a comm containing spaces AND parens does not shift the cpu fields" 950 "$(parse_proc_stat_cpu_ticks "$st_stat_nasty")"
    st_ps_check "proc stat: rss pages read from the right field" 262144 "$(parse_proc_stat_rss_pages "$st_stat_plain")"
    st_ps_check "proc stat: rss survives the nasty comm too" 262144 "$(parse_proc_stat_rss_pages "$st_stat_nasty")"
    st_ps_check "proc stat: a truncated line yields -1, never a fabricated 0" -1 "$(parse_proc_stat_cpu_ticks '4242 (x) S 1 2')"
    st_ps_check "proc stat: empty input yields -1, never 0" -1 "$(parse_proc_stat_cpu_ticks '')"
    st_io_blob='rchar: 111
wchar: 222
read_bytes: 4096000
write_bytes: 8192000
cancelled_write_bytes: 0'
    st_ps_check "proc io: read_bytes is the block-layer counter, not rchar" 4096000 "$(parse_proc_io_field "$st_io_blob" read_bytes)"
    st_ps_check "proc io: write_bytes is the block-layer counter, not wchar" 8192000 "$(parse_proc_io_field "$st_io_blob" write_bytes)"
    st_ps_check "proc io: write_bytes is not confused with cancelled_write_bytes" 8192000 "$(parse_proc_io_field "$st_io_blob" write_bytes)"
    st_ps_check "proc io: an absent key yields -1, never 0" -1 "$(parse_proc_io_field "$st_io_blob" nonesuch)"
    st_ps_check "proc io: empty input yields -1, never 0" -1 "$(parse_proc_io_field '' read_bytes)"

    # Boot-marker phase parsing. The real hazard is over-matching: the one real
    # PASS artifact has 74 "[boot]" lines and only 59 are markers, so a looser
    # pattern invents phases out of prose ("[boot] block_index: 1 entries, 296
    # bytes/entry, ..."). This fixture carries one top-level marker, one
    # sub-phase marker, three prose decoys, and a SECOND boot.
    st_log="$(mktemp)" ; st_med="$(mktemp)"
    {
        printf '2026-07-28T00:02:12Z INFO something happened\n'
        printf '[boot] prologue                       63ms\n'
        printf '[boot]   sqlite.quick_check           7ms\n'
        printf '[boot] block_index: 1 entries, 296 bytes/entry, index=0MB\n'
        printf '[boot] First boot or marker absent (no WAL)\n'
        printf '[boot] system_ram=95654MB block_index_estimate=1121MB (3000000 entries)\n'
        printf '2026-07-28T00:03:00Z INFO later\n'
        printf '[boot] prologue                       101ms\n'
        printf '[boot] total                          51755ms\n'
    } >"$st_log"
    printf 'prologue\t70\n' >"$st_med"
    st_phases="$(phases_json_from_log "$st_log" "$st_med")"
    st_ps_check "boot markers: exactly 4 phases parsed from 9 lines (3 prose + 2 timestamped non-markers rejected)" \
        4 "$(printf '%s' "$st_phases" | grep -o '"phase":' | wc -l | tr -d ' ')"
    st_ps_check "boot markers: prose 'block_index:' line did NOT become a phase" \
        0 "$(printf '%s' "$st_phases" | grep -c 'block_index' | tr -d ' ')"
    st_ps_check "boot markers: prose 'system_ram=' line did NOT become a phase" \
        0 "$(printf '%s' "$st_phases" | grep -c 'system_ram' | tr -d ' ')"
    st_ps_check "boot markers: a top-level marker is level=phase" \
        1 "$(printf '%s' "$st_phases" | grep -c '"phase":"total","boot":2,"seq":2,"level":"phase"' | tr -d ' ')"
    st_ps_check "boot markers: an indented marker is level=subphase" \
        1 "$(printf '%s' "$st_phases" | grep -c '"phase":"sqlite.quick_check","boot":1,"seq":2,"level":"subphase"' | tr -d ' ')"
    st_ps_check "boot markers: the second prologue starts boot 2 (respawn is not blended into boot 1)" \
        1 "$(printf '%s' "$st_phases" | grep -c '"phase":"prologue","boot":2,"seq":1' | tr -d ' ')"
    st_ps_check "boot markers: duration_ms comes off the marker itself" \
        1 "$(printf '%s' "$st_phases" | grep -c '"duration_ms":51755' | tr -d ' ')"
    st_ps_check "boot markers: start bound is the nearest PRECEDING timestamped line, and is named a bound" \
        1 "$(printf '%s' "$st_phases" | grep -c '"phase":"total","boot":2,"seq":2,"level":"phase","duration_ms":51755,"duration_source":"node_log_boot_marker","start_ts_lower_bound":"2026-07-28T00:03:00Z"' | tr -d ' ')"
    st_ps_check "boot markers: median_ms is joined from boot_timings for the stage that has one" \
        2 "$(printf '%s' "$st_phases" | grep -o '"median_source":"dumpstate_boot_timings"' | wc -l | tr -d ' ')"
    st_ps_check "boot markers: a stage with NO median gets no median field (never a fabricated 0)" \
        0 "$(printf '%s' "$st_phases" | grep -o '"median_ms":0' | wc -l | tr -d ' ')"
    st_ps_check "boot markers: an absent log yields no phases, not a malformed element" \
        "" "$(phases_json_from_log "$st_log.nonesuch" "$st_med")"
    rm -f "$st_log" "$st_med"

    # boot_timings median extraction, via the SHARED readers (no local parser).
    st_bt="$(mktemp)"
    printf '{"last_boot_epoch":1,"stages":[{"stage":"prologue","last_ms":63,"median_ms":70},{"stage":"utxo_import","last_ms":5}]}\n' >"$st_bt"
    st_ps_check "boot_timings: a stage WITH a median yields one pair" \
        "prologue	70" "$(boot_timings_median_pairs "$st_bt")"
    st_ps_check "boot_timings: a stage with <3 samples (no median_ms) yields NO pair" \
        0 "$(boot_timings_median_pairs "$st_bt" | grep -c utxo_import | tr -d ' ')"
    st_ps_check "boot_timings: an absent doc yields no pairs" "" "$(boot_timings_median_pairs "$st_bt.nonesuch")"
    rm -f "$st_bt"

    # ── omitted_fields[]: named absence, not silent absence ─────────────────
    # The defect: a field the measurement brief asked for that this tree cannot
    # source was simply not emitted, and a silently absent field reads to the
    # next reader as "measured, and fine". Every structural row must be present
    # on EVERY run, and a this_run row must appear exactly when the reading was
    # genuinely lost. These fixtures drive omitted_fields_json() through both.
    st_of_names() { printf '%s' "$1" | grep -o '"field":"[^"]*"' | sed 's/.*:"//;s/"$//' | sort; }
    ARTIFACT_DIR="$(mktemp -d)"
    NODE_BIN_SOURCE_ID="1111111111111111111111111111111111111111111111111111111111111111"
    SAMPLES_TSV="$ARTIFACT_DIR/samples.tsv"; : >"$SAMPLES_TSV"
    : >"$ARTIFACT_DIR/node.log"
    LAST_CPU_SECONDS="9.50"; LAST_RSS_KB="262144"
    LAST_DISK_READ_BYTES="4096000"; LAST_DISK_WRITE_BYTES="8192000"
    st_of_best="$(omitted_fields_json)"
    st_ps_check "omitted_fields: the 6 structural rows are present even when EVERYTHING measurable was measured" \
        6 "$(printf '%s' "$st_of_best" | grep -o '"scope":"structural"' | wc -l | tr -d ' ')"
    st_ps_check "omitted_fields: a fully-measured run reports NO this_run rows" \
        0 "$(printf '%s' "$st_of_best" | grep -o '"scope":"this_run"' | wc -l | tr -d ' ')"
    st_ps_check "omitted_fields: network bytes are named as omitted, never emitted as 0" \
        1 "$(printf '%s' "$st_of_best" | grep -c 'phases\[\].network_bytes' | tr -d ' ')"
    st_ps_check "omitted_fields: every row carries a reason" \
        6 "$(printf '%s' "$st_of_best" | grep -o '"reason":"' | wc -l | tr -d ' ')"
    st_ps_check "omitted_fields: every row carries a nearest_honest_substitute" \
        6 "$(printf '%s' "$st_of_best" | grep -o '"nearest_honest_substitute":"' | wc -l | tr -d ' ')"
    st_ps_check "omitted_fields: no row claims a substitute of a fabricated zero" \
        0 "$(printf '%s' "$st_of_best" | grep -c '"nearest_honest_substitute":"0"' | tr -d ' ')"
    # Now lose every optional reading and confirm each loss is NAMED.
    NODE_BIN_SOURCE_ID=""
    SAMPLES_TSV=""
    rm -f "$ARTIFACT_DIR/node.log"
    LAST_CPU_SECONDS="-1"; LAST_RSS_KB="-1"
    LAST_DISK_READ_BYTES="-1"; LAST_DISK_WRITE_BYTES="-1"
    st_of_worst="$(omitted_fields_json)"
    st_ps_check "omitted_fields: an unidentifiable binary is NAMED as unmeasured, not silently null" \
        1 "$(printf '%s' "$st_of_worst" | grep -c 'measured_identity.node_bin_source_id_sha256' | tr -d ' ')"
    st_ps_check "omitted_fields: a missing samples.tsv is NAMED" \
        1 "$(printf '%s' "$st_of_worst" | grep -o '"field":"samples.tsv"' | wc -l | tr -d ' ')"
    st_ps_check "omitted_fields: an absent node.log names the lost boot phases" \
        1 "$(printf '%s' "$st_of_worst" | grep -c 'phases\[\] boot-level elements' | tr -d ' ')"
    st_ps_check "omitted_fields: all four unreadable /proc counters are named individually" \
        4 "$(printf '%s' "$st_of_worst" | grep -o '"field":"harness.observed_sync\.[a-z_]*"' | wc -l | tr -d ' ')"
    st_ps_check "omitted_fields: the structural rows survive the worst case too" \
        6 "$(printf '%s' "$st_of_worst" | grep -o '"scope":"structural"' | wc -l | tr -d ' ')"
    st_ps_check "omitted_fields: the worst case names strictly MORE fields than the best case" \
        1 "$([ "$(st_of_names "$st_of_worst" | wc -l)" -gt "$(st_of_names "$st_of_best" | wc -l)" ] && echo 1 || echo 0)"
    st_ps_check "omitted_fields: no field name is reported twice" \
        "" "$(st_of_names "$st_of_worst" | uniq -d)"
    rm -rf "$ARTIFACT_DIR"
    ARTIFACT_DIR=""; NODE_BIN_SOURCE_ID=""; SAMPLES_TSV=""

    # The artifact must NAME its own omitted set — a proof.json that emits
    # phases[] but no omitted_fields[] is back to silent absence.
    grep -q '"omitted_fields": \[' "${BASH_SOURCE[0]}"
    st_check "proof.json emits an omitted_fields[] array" 0 $?

    if [ "$st_fail" = 0 ]; then
        echo "cold-start-wipe-stopwatch: --selftest PASS"
        exit 0
    fi
    echo "cold-start-wipe-stopwatch: --selftest FAIL" >&2
    exit 1
fi

# capture_run_bundle — on EVERY verdict, PASS INCLUDED, snapshot the live
# diagnostic state a human/agent needs to root-cause OR re-cost the run WITHOUT
# re-running the harness.
#
# THIS USED TO BE capture_failure_bundle, called only when `verdict != pass`.
# That asymmetry meant a SUCCESSFUL run left three files (proof.json +
# node.log + node.tail.log) and no per-phase evidence at all, while a failing
# run left the full set — so the one artifact class you actually want to
# optimize against was the one class that threw its measurements away. The one
# real PASS artifact on disk (build/c3-stopwatch/20260728T000207Z-2102851/) is
# exactly that: three files, no reducer_stage_profile.json, no stage-*.json.
# There is no cheaper time to read the node's own per-stage cost than the moment
# it just finished the run, and a baseline that only exists on failure is not a
# baseline. Capture is now unconditional; only the LABELS differ by verdict.
#
# Captured: frontier.json (dumpstate reducer_frontier),
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
capture_run_bundle() {
    BUNDLE_CAPTURE_FAILED="false"
    FRONTIER_BUSY_AT_CAPTURE="false"
    local got_frontier=0 got_drive=0 got_profile=0 got_stages=0 got_blocker=0 got_logs=0 got_net=0
    local got_timings=0
    if [ -n "${PID:-}" ] && kill -0 "$PID" 2>/dev/null && [ -x "${NODE_BIN:-}" ] && [ -n "${DATADIR:-}" ]; then
        # Frontier read goes through rpc_frontier (bounded retries + busy
        # handling) so a lock-contention blip during capture doesn't drop the
        # artifact to 0 bytes. Under heavy fold load this single one-shot was
        # exactly what returned empty and mislabelled a healthy climb — capture
        # must survive the same busy window the sample loop does.
        rpc_frontier >"$ARTIFACT_DIR/frontier.json" 2>/dev/null
        [ -s "$ARTIFACT_DIR/frontier.json" ] && got_frontier=1
        if [ "$got_frontier" = 1 ] && is_busy_response "$(cat "$ARTIFACT_DIR/frontier.json" 2>/dev/null)"; then
            FRONTIER_BUSY_AT_CAPTURE="true"
        fi
        "$NODE_BIN" -rpcport="$RPC" -datadir="$DATADIR" dumpstate reducer_drive \
            >"$ARTIFACT_DIR/reducer_drive.json" 2>/dev/null && [ -s "$ARTIFACT_DIR/reducer_drive.json" ] && got_drive=1
        "$NODE_BIN" -rpcport="$RPC" -datadir="$DATADIR" dumpstate reducer_stage_profile \
            >"$ARTIFACT_DIR/reducer_stage_profile.json" 2>/dev/null && [ -s "$ARTIFACT_DIR/reducer_stage_profile.json" ] && got_profile=1
        # boot_timings: the flight recorder's durable per-stage boot history
        # (config/src/boot_flight_recorder.c). It is the ONLY source of the
        # median_ms a phases[] row can be compared against, so phases[] would
        # be un-baselineable without it — one boot's ms with nothing to judge it
        # by is a number, not a measurement.
        "$NODE_BIN" -rpcport="$RPC" -datadir="$DATADIR" dumpstate boot_timings \
            >"$ARTIFACT_DIR/boot_timings.json" 2>/dev/null && [ -s "$ARTIFACT_DIR/boot_timings.json" ] && got_timings=1
        got_stages=1
        for stage_name in header_admit validate_headers body_fetch body_persist \
                          script_validate proof_validate utxo_apply tip_finalize; do
            "$NODE_BIN" -rpcport="$RPC" -datadir="$DATADIR" dumpstate "$stage_name" \
                >"$ARTIFACT_DIR/stage-$stage_name.json" 2>/dev/null &&
                [ -s "$ARTIFACT_DIR/stage-$stage_name.json" ] || got_stages=0
        done
        # Network/peer capture. On a loopback run this is uninteresting (a
        # local peer always handshakes), which is exactly why it was missing.
        # On a REMOTE run it answers the first question any non-pass verdict
        # raises — did we ever complete a P2P handshake at all, or did the peer
        # refuse us pre-version? net-peer_lifecycle.json carries the
        # attempted/connected/version_sent/verack_received/handshake_complete/
        # pre_handshake_disconnects counters; net-connman.json carries the
        # per-addnode dial ledger (tcp_failures vs protocol_failures, backoff);
        # net-network.json carries the chain_view/census rollup.
        got_net=1
        for net_name in connman peer_lifecycle network; do
            "$NODE_BIN" -rpcport="$RPC" -datadir="$DATADIR" dumpstate "$net_name" \
                >"$ARTIFACT_DIR/net-$net_name.json" 2>/dev/null &&
                [ -s "$ARTIFACT_DIR/net-$net_name.json" ] || got_net=0
        done
        # Blocker read is retried too — it feeds the STALLED-NAMED verdict, so a
        # busy-window miss that empties it must not silently erase a real named
        # blocker.
        rpc_retry_nonempty dumpstate blocker \
            >"$ARTIFACT_DIR/blocker.json" 2>/dev/null
        [ -s "$ARTIFACT_DIR/blocker.json" ] && got_blocker=1
        "$NODE_BIN" -rpcport="$RPC" -datadir="$DATADIR" ops logs \
            --pattern='.' --since_secs=3600 --max_lines=500 --level=all \
            >"$ARTIFACT_DIR/ops.log.tail.txt" 2>/dev/null && [ -s "$ARTIFACT_DIR/ops.log.tail.txt" ] && got_logs=1
    fi
    if [ "$got_logs" = 0 ] && [ -n "${DATADIR:-}" ] && [ -f "$DATADIR/node.log" ]; then
        tail -200 "$DATADIR/node.log" >"$ARTIFACT_DIR/ops.log.tail.txt" 2>/dev/null && got_logs=1
    fi
    # The banlist is the one piece that is a FILE, not an RPC: copy it when the
    # node wrote one so a "did we ban our only peer" question is answerable
    # from the artifact alone. Absent is the normal case and is NOT a capture
    # failure — banlist_present in proof.json records which it was.
    BANLIST_PRESENT="false"
    if [ -n "${DATADIR:-}" ] && [ -f "$DATADIR/banlist.dat" ] &&
       cp -p -- "$DATADIR/banlist.dat" "$ARTIFACT_DIR/banlist.dat" 2>/dev/null; then
        BANLIST_PRESENT="true"
    fi
    [ "$got_frontier" = 1 ] && [ "$got_drive" = 1 ] && [ "$got_profile" = 1 ] &&
        [ "$got_stages" = 1 ] && [ "$got_blocker" = 1 ] && [ "$got_logs" = 1 ] &&
        [ "$got_net" = 1 ] && [ "$got_timings" = 1 ] ||
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
    BANLIST_PRESENT="false"
    # UNCONDITIONAL — every verdict, PASS INCLUDED. Do NOT re-introduce a
    # `[ "$verdict" != "pass" ]` guard here: that is the exact asymmetry that
    # made a successful run destroy its own per-phase evidence. --selftest
    # asserts on this file's source text that the guard has not grown back.
    capture_run_bundle
    # Refresh the process counters one last time so the harness.observed_sync
    # phase reports the counters as of capture, not as of the last sample tick.
    refresh_process_counters
    NODE_LOG_CAPTURED="false"
    if [ -n "${DATADIR:-}" ] && [ -f "$DATADIR/node.log" ] &&
       cp -p -- "$DATADIR/node.log" "$ARTIFACT_DIR/node.log" 2>/dev/null; then
        NODE_LOG_CAPTURED="true"
    fi

    # ── phases[] assembly (before the JSON block, so a parse hiccup cannot
    # truncate proof.json mid-object). Boot phases are parsed from the COPY of
    # node.log just placed in the artifact dir, so proof.json and the log it was
    # derived from always agree.
    SAMPLES_ROWS=""
    if [ -n "$SAMPLES_TSV" ] && [ -f "$SAMPLES_TSV" ]; then
        SAMPLES_ROWS="$(awk 'NR > 1 { n++ } END { print n + 0 }' "$SAMPLES_TSV" 2>/dev/null)"
    fi
    PHASES_JSON=""
    _ph_harness="$(harness_phases_json "$captured_at")"
    _ph_boot=""
    if [ -f "$ARTIFACT_DIR/node.log" ]; then
        _ph_med="$ARTIFACT_DIR/.boot-timings-medians.tsv"
        boot_timings_median_pairs "$ARTIFACT_DIR/boot_timings.json" >"$_ph_med" 2>/dev/null || : >"$_ph_med"
        _ph_boot="$(phases_json_from_log "$ARTIFACT_DIR/node.log" "$_ph_med")"
        rm -f "$_ph_med" 2>/dev/null || true
    fi
    if [ -n "$_ph_harness" ] && [ -n "$_ph_boot" ]; then
        PHASES_JSON="$_ph_harness,$_ph_boot"
    else
        PHASES_JSON="$_ph_harness$_ph_boot"
    fi
    PHASES_PROVENANCE="harness.* elements: this harness bracketed the window itself (date +%s on both sides). Boot elements: parsed from the node's own [boot] boot_topmark/boot_submark markers in node.log, median_ms joined from dumpstate boot_timings. OMITTED for lack of an honest source: network bytes (no diagnostics dumper exposes a byte counter and /proc has no per-process network accounting), per-boot-phase cpu/disk (boot.c markers carry only a duration), exact per-boot-phase wall-clock start (the markers carry no timestamp and the top-level marks do not tile the boot) — start_ts_lower_bound is given instead and is a bound, not a start."

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
        printf '  "peer_precheck": %s,\n' "$(json_string "$PEER_PRECHECK")"
        printf '  "file_peer": %s,\n' "$(json_string "$FILE_PEER")"
        printf '  "header_source": %s,\n' "$(json_string "$HEADER_SOURCE")"
        printf '  "staged_bundle": %s,\n' "$(json_string "$BUNDLE_PATH")"
        printf '  "node_bin": %s,\n' "$(json_string "$NODE_BIN")"
        printf '  "first_hstar": %s,\n' "$(json_number_or_null "${first_hstar:-}")"
        printf '  "max_hstar": %s,\n' "$(json_number_or_null "$max_hstar")"
        printf '  "final_hstar": %s,\n' "$(json_number_or_null "$last_hstar")"
        printf '  "final_network_tip": %s,\n' "$(json_number_or_null "$last_network_tip")"
        printf '  "first_provable_sample": %s,\n' "$(json_number_or_null "${first_ps:-}")"
        printf '  "max_provable_sample": %s,\n' "$(json_number_or_null "$max_ps")"
        printf '  "final_provable_sample": %s,\n' "$(json_number_or_null "$last_ps")"
        printf '  "saw_provable_sample": %s,\n' "$([ "${saw_ps:-0}" = 1 ] && printf true || printf false)"
        printf '  "final_readback_failed": %s,\n' "${final_readback_failed:-false}"
        printf '  "reached_network_tip": %s,\n' "$([ "$verdict" = "pass" ] && printf true || printf false)"
        printf '  "scratch_datadir": %s,\n' "$(json_string "${DATADIR:-}")"
        printf '  "scratch_datadir_removed": true,\n'
        printf '  "node_log_captured": %s,\n' "$NODE_LOG_CAPTURED"
        printf '  "bundle_capture_failed": %s,\n' "$BUNDLE_CAPTURE_FAILED"
        printf '  "banlist_present": %s,\n' "${BANLIST_PRESENT:-false}"
        printf '  "frontier_busy_at_capture": %s,\n' "$FRONTIER_BUSY_AT_CAPTURE"
        printf '  "samples_tsv": %s,\n' \
            "$([ -n "$SAMPLES_TSV" ] && [ -f "$SAMPLES_TSV" ] && printf '"samples.tsv"' || printf 'null')"
        printf '  "samples_rows": %s,\n' "$(json_number_or_null "$SAMPLES_ROWS")"
        # WHAT WAS MEASURED. A phase timing with no identity attached is not a
        # baseline for anything: the next run cannot tell whether it got faster
        # or just ran a different binary against a different peer.
        # source_id_sha256 comes from the ONE canonical reader
        # (tools/scripts/source_identity_lib.sh zcl_binary_source_id — read once
        # at startup, before the run, so it describes the binary that was
        # actually timed).
        printf '  "measured_identity": {\n'
        printf '    "node_bin": %s,\n' "$(json_string "$NODE_BIN")"
        printf '    "node_bin_source_id_sha256": %s,\n' \
            "$([ -n "$NODE_BIN_SOURCE_ID" ] && json_string "$NODE_BIN_SOURCE_ID" || printf 'null')"
        printf '    "node_bin_source_id_source": "zcl_binary_source_id (tools/scripts/source_identity_lib.sh) over `%s agentbuild`",\n' \
            "$(json_escape "$(basename -- "$NODE_BIN")")"
        printf '    "peer": %s,\n' "$(json_string "$PEER")"
        printf '    "peer_precheck": %s,\n' "$(json_string "$PEER_PRECHECK")"
        printf '    "peer_advertised_tip": %s,\n' "$(json_number_or_null "$last_network_tip")"
        printf '    "peer_advertised_tip_source": "dumpstate reducer_frontier network_tip (best height any handshake-complete peer advertised); -1 means no handshake ever completed"\n'
        printf '  },\n'
        # phases[] — see the phases_json_from_log / harness_phases_json headers
        # for the provenance rule. Every element names the source of its
        # duration; a field with no honest source is absent, never zero.
        printf '  "phases_provenance": %s,\n' "$(json_string "$PHASES_PROVENANCE")"
        printf '  "phases": [%s],\n' "$PHASES_JSON"
        # omitted_fields[] — see omitted_fields_json(). Every field the
        # measurement brief named that this run did NOT record, BY NAME, with
        # the reason. A silently absent field reads as "measured and fine";
        # this section is what makes that impossible here.
        printf '  "omitted_fields_provenance": "each row is a field the measurement brief asked for that this run did not record. scope=structural means no source exists in this tree for any run; scope=this_run means a source exists but this run could not read it.",\n'
        printf '  "omitted_fields": [%s]\n' "$(omitted_fields_json)"
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

# Identity of WHAT IS ABOUT TO BE MEASURED, read once, before the run, via the
# ONE canonical reader (tools/scripts/source_identity_lib.sh). Empty output is a
# normal answer there ("nothing to report"), never a failure, so this cannot
# abort a run — an unidentifiable binary yields a null field in proof.json and
# the run still reports the verdict the node earned.
NODE_BIN_SOURCE_ID="$(zcl_binary_source_id "$NODE_BIN")"
echo "cold-start-wipe-stopwatch: node_bin_source_id=${NODE_BIN_SOURCE_ID:-<unavailable>}"

# ── the peer must be STATED (see the PEER assignment above) ──────────────────
# A proof lane that inherits its serving peer from a default is a proof lane
# that can be run by accident against whatever happens to be listening. The old
# default was the canonical node's own P2P port, so `make
# mvp-coldstart-to-tip-stopwatch` with no arguments dialled the operator's live
# node and pulled chain data off it. Refuse instead: SKIP is already this
# harness's "prerequisite absent" verdict (exit 2, which the Make wrapper turns
# into a clean no-op), and it records an honest artifact naming what is missing.
if [ -z "$PEER" ]; then
    skip "no_peer_configured — set ZCL_CS_PEER=HOST:PORT (or pass --peer=HOST:PORT / ZCL_PEER= via make). This harness has NO default peer on purpose: it used to default to 127.0.0.1:8033, the operator's canonical node, so a bare run silently synced off it. Point it at a stopwatch fixture peer (e.g. 127.0.0.1:39070), or name the canonical node explicitly if that is genuinely what you mean."
fi

peer_host="${PEER%:*}"
peer_port="${PEER##*:}"
[ -n "$peer_host" ] && [ -n "$peer_port" ] && [ "$peer_host" != "$peer_port" ] \
    || skip "invalid peer address: $PEER"
PEER_PRECHECK="$(peer_precheck "$peer_host" "$peer_port")"
if [ "$PEER_PRECHECK" = "unreachable" ]; then
    skip "serving peer not reachable: $PEER"
fi
if [ "$PEER_PRECHECK" = "accept_close" ]; then
    # ADVISORY, never a verdict: the peer accept()ed and closed before a byte
    # moved, so no P2P handshake can complete, `network_tip` stays unreadable,
    # and the PASS predicate is unreachable for the whole budget. Say so up
    # front instead of leaving an operator to infer it from 600s of -1 rows.
    # Most likely cause on a shared host: the peer's per-IP inbound sybil cap
    # (lib/net/src/net.c, max 3 inbound per IP) is already consumed by another
    # node on THIS machine. Check with:
    #   ss -tn state established "dst $peer_host:$peer_port"
    echo "cold-start-wipe-stopwatch: WARNING peer $PEER accepted the TCP connection and CLOSED IT IMMEDIATELY (zero bytes)."
    echo "cold-start-wipe-stopwatch: WARNING no P2P handshake can complete, so network_tip will stay unreadable and PASS is unreachable this run."
    echo "cold-start-wipe-stopwatch: WARNING likely the peer's per-IP inbound cap (max 3/IP, lib/net/src/net.c) — count local sockets with: ss -tn state established \"dst $PEER\""
    echo "cold-start-wipe-stopwatch: WARNING continuing anyway — the run still reports the verdict the node genuinely earned, never a SKIP."
fi
echo "cold-start-wipe-stopwatch: peer_precheck=$PEER_PRECHECK"
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
    HEADER_IMPORT_START=$(date +%s)
    if ! env HOME="$ISO_HOME" "$NODE_BIN" --importblockindex \
        "$HEADER_SOURCE" "$DATADIR/node.db" >>"$DATADIR/node.log" 2>&1; then
        die "header import from datadir COPY failed"
    fi
    HEADER_IMPORT_MS=$(( ($(date +%s) - HEADER_IMPORT_START) * 1000 ))
    echo "cold-start-wipe-stopwatch: header import complete (${HEADER_IMPORT_MS}ms)"
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

# rpc_retry_nonempty <dumpstate-arg...> — a generic bounded-retry read (growing
# backoff 1,1,2,3s) that returns the first NON-EMPTY response, so a busy-window
# miss during failure-bundle capture doesn't drop a diagnostic artifact to 0
# bytes. Echoes the last response (possibly empty if every retry missed). The
# node RPC deadline (RPC_TIMEOUT_DEFAULT_MS, 10s) is what returns empty under a
# heavy fold; retrying across a longer window catches a load gap.
rpc_retry_nonempty() {
    local out="" backoff
    for backoff in 1 1 2 3 0; do
        out="$(rpc "$@")"
        [ -n "$out" ] && { printf '%s' "$out"; return 0; }
        [ "$backoff" = 0 ] && break
        sleep "$backoff"
    done
    printf '%s' "$out"
}

# dumpstate reads can transiently miss while the reducer drive holds the
# progress_store lock — `dumpstate reducer_frontier` then returns a PARTIAL
# doc, {"snapshot_status":"progress_store_busy","retryable":true}, with NO
# "hstar" field but STILL carrying cached_provable_tip (emitted lock-free before
# the trylock). Under a heavy fold the RPC deadline can also be missed entirely,
# returning empty. Retry with bounded, growing backoff (1,1,2,3,5s — ~12s worst
# case per call), preferring a FULL read (has "hstar"); failing that, return the
# best BUSY partial we saw (it carries the cached_provable_tip PROGRESS proxy)
# in preference to an empty response, so a later empty retry can never erase an
# earlier proxy. Sets FRONTIER_LAST_BUSY=1 when the best we got across retries
# was a busy partial (0 on a full read or a truly empty response). The caller
# tracks an unreadable STREAK across sample ticks so a progress_store that never
# yields even a proxy gets its own named verdict (see FRONTIER_BUSY_TIMEOUT_SECS
# in the main loop) instead of silently degrading into "no forward progress, no
# blocker" (silent-stall FAIL) — those are different claims: "we could not
# observe" is not "we observed nothing happening".
FRONTIER_LAST_BUSY=0
rpc_frontier() {
    local out="" best_busy="" backoff
    for backoff in 1 1 2 3 5 0; do
        out="$(rpc dumpstate reducer_frontier)"
        if printf '%s' "$out" | grep -q '"hstar"'; then
            FRONTIER_LAST_BUSY=0
            printf '%s' "$out"
            return 0
        fi
        # Not a full read. Keep the busy partial (it still carries
        # cached_provable_tip) so a subsequent empty retry can't lose it.
        is_busy_response "$out" && best_busy="$out"
        [ "$backoff" = 0 ] && break
        sleep "$backoff"
    done
    if [ -n "$best_busy" ]; then
        FRONTIER_LAST_BUSY=1
        printf '%s' "$best_busy"
        return 0
    fi
    FRONTIER_LAST_BUSY=0
    printf '%s' "$out"
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

# Arm the per-tick sink BEFORE the first tick. The artifact dir is created here
# rather than at write_artifact time so samples.tsv accumulates DURING the run:
# a harness that is SIGKILLed at t=550s of a 600s budget still leaves the whole
# climb trace on disk, and the shape of the climb is what says which phase to
# optimize. These rows used to be printf-to-stdout only and died with the
# terminal.
mkdir -p "$ARTIFACT_DIR" 2>/dev/null || true
if [ -d "$ARTIFACT_DIR" ]; then
    SAMPLES_TSV="$ARTIFACT_DIR/samples.tsv"
    samples_tsv_init
fi
LOOP_START_UNIX=$(date +%s)

printf '%-8s %-10s %-10s %-10s %-8s %s\n' "t(s)" "hstar" "prov" "net_tip" "tip_ok" "blockers"
printf '%-8s %-10s %-10s %-10s %-8s %s\n' "----" "-----" "----" "-------" "------" "--------"

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

    hs="$(frontier_hstar_full "$fj")"      # authoritative full-read H* (-1 = read miss / busy)
    ps="$(frontier_provable_sample "$fj")" # progress sample: full H* or cached_provable_tip proxy (-1 = neither)
    nt="$(jget "$fj" network_tip)";        [ -z "$nt" ] && nt="-1"
    nt_ok="$(printf '%s' "$fj" | grep -oE '"network_tip_read_ok"[[:space:]]*:[[:space:]]*true')"
    bc="$(jget "$bj" active_count)";       [ -z "$bc" ] && bc="0"
    bids="$(blocker_ids "$bj")"
    [ -z "$bids" ] && bids="-"
    park_gate="$(log_named_park)"
    if [ -n "$park_gate" ] && [ "$bc" = "0" ]; then
        bc=1; bids="boot_park:$park_gate"
    fi

    printf '%-8s %-10s %-10s %-10s %-8s %s\n' "$elapsed" "$hs" "$ps" "$nt" "${nt_ok:+yes}" "b=$bc:$bids"
    # ...and the SAME tick, durably, with the node process's own CPU/RSS/disk
    # counters alongside it. stdout is for the operator watching; samples.tsv is
    # the record the next optimization pass reads.
    refresh_process_counters
    samples_tsv_row "$elapsed" "$hs" "$ps" "$nt" \
        "$([ -n "$nt_ok" ] && printf yes || printf no)" \
        "$FRONTIER_LAST_BUSY" "$bc" "$bids"

    # Bounded unreadable-streak check: only accumulates when NO usable provable
    # sample was obtained (ps == -1) AND the node kept answering the busy
    # partial doc (retryable:true). Once a fold starts publishing a
    # cached_provable_tip, ps>=0 and this resets — a busy-but-climbing node is
    # observed via the proxy and never mistaken for an instrument blackout. A
    # busy store that never yields even a proxy for the entire window is the
    # honest FRONTIER-BUSY-TIMEOUT instrument-failure class (see D6), never
    # folded into "no forward progress, no blocker" (silent-stall FAIL).
    if [ "$ps" = "-1" ] && [ "$FRONTIER_LAST_BUSY" = "1" ]; then
        [ "$busy_streak_start" = 0 ] && busy_streak_start="$now"
        busy_elapsed=$((now - busy_streak_start))
        if [ "$busy_elapsed" -ge "$FRONTIER_BUSY_TIMEOUT_SECS" ]; then
            echo "=== cold-start-wipe-stopwatch: FRONTIER-BUSY-TIMEOUT — progress_store_busy persisted ${busy_elapsed}s (>= ${FRONTIER_BUSY_TIMEOUT_SECS}s) with no hstar/cached_provable_tip sample ever observed in that window ==="
            write_artifact "frontier_busy_timeout" 5 \
                "progress_store_busy persisted >= ${FRONTIER_BUSY_TIMEOUT_SECS}s (--busy-timeout); last raw frontier response: $fj"
            exit 5
        fi
    else
        busy_streak_start=0
    fi

    # Authoritative H* must never regress (a real regression is a correctness
    # bug, not a timing seam). Only the FULL-read hstar is checked — read-misses
    # (-1) are excluded, and the advisory proxy is deliberately NOT a regression
    # tripwire (a cache read may lag without the ledger having regressed).
    if [ "$hs" != "-1" ] && [ "$last_hstar" != "-1" ] && [ "$hs" -lt "$last_hstar" ] 2>/dev/null; then
        die "H* REGRESSED: $last_hstar -> $hs at t=${elapsed}s (this is a correctness bug, not a budget seam)"
    fi
    if [ "$hs" != "-1" ]; then
        [ -z "$first_hstar" ] && first_hstar="$hs"
        last_hstar="$hs"
        [ "$hs" -gt "$max_hstar" ] 2>/dev/null && max_hstar="$hs"
    fi
    # Provable-sample tracking (full H* OR proxy): the honest "did it climb"
    # signal a busy-but-healthy fold must never be denied. A run whose
    # successive samples strictly increase can never be classed silent-stall.
    if [ "$ps" != "-1" ]; then
        saw_ps=1
        [ -z "$first_ps" ] && first_ps="$ps"
        last_ps="$ps"
        [ "$ps" -gt "$max_ps" ] 2>/dev/null && max_ps="$ps"
    fi
    [ "$nt" != "-1" ] && last_network_tip="$nt"
    last_blocker_ids="$bids"; last_blocker_count="$bc"

    # PASS predicate — deliberately AUTHORITATIVE-ONLY: a full read (hstar +
    # network_tip both present, network_tip_read_ok true) whose authoritative H*
    # has caught the peer tip. The proxy can prove CLIMB but can NEVER mint a
    # PASS (network_tip is absent from the busy partial doc anyway).
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

# FINAL CAPTURE READBACK — one more bounded-retry frontier + blocker read so the
# closing verdict reflects the freshest observable state, not a single
# end-of-budget miss. It refreshes the provable-sample / authoritative / blocker
# trackers and decides final_readback_failed: true iff even this retried read
# yielded NEITHER an authoritative hstar NOR a cached_provable_tip proxy. A run
# that could not be observed at the end is an INSTRUMENT failure, never a claim
# the node stalled.
final_fj="$(rpc_frontier)"
final_hs="$(frontier_hstar_full "$final_fj")"
final_ps="$(frontier_provable_sample "$final_fj")"
final_nt="$(jget "$final_fj" network_tip)"; [ -z "$final_nt" ] && final_nt="-1"
if [ "$final_hs" != "-1" ]; then
    [ -z "$first_hstar" ] && first_hstar="$final_hs"
    last_hstar="$final_hs"
    [ "$final_hs" -gt "$max_hstar" ] 2>/dev/null && max_hstar="$final_hs"
fi
if [ "$final_ps" != "-1" ]; then
    saw_ps=1
    [ -z "$first_ps" ] && first_ps="$final_ps"
    last_ps="$final_ps"
    [ "$final_ps" -gt "$max_ps" ] 2>/dev/null && max_ps="$final_ps"
fi
[ "$final_nt" != "-1" ] && last_network_tip="$final_nt"
final_readback_failed="false"
[ "$final_ps" = "-1" ] && final_readback_failed="true"

# Refresh the named-blocker view with a retried read too, so a busy-window miss
# doesn't erase a real blocker right before the STALLED-NAMED decision.
final_bj="$(rpc_retry_nonempty dumpstate blocker)"
final_bc="$(jget "$final_bj" active_count)"
if [ -n "$final_bc" ]; then
    last_blocker_count="$final_bc"
    final_bids="$(blocker_ids "$final_bj")"
    [ -n "$final_bids" ] && last_blocker_ids="$final_bids"
fi
final_park="$(log_named_park)"
if [ -n "$final_park" ] && [ "${last_blocker_count:-0}" = "0" ]; then
    last_blocker_count=1; last_blocker_ids="boot_park:$final_park"
fi

# The verdict precedence is decided by the pure classify_final_verdict()
# (unit-tested in --selftest). CLIMB is judged on the PROVABLE SAMPLE
# (authoritative H* OR the cached_provable_tip proxy), so a healthy fold that
# only ever exposed the proxy under load still counts as real forward progress
# and can NEVER be called a silent stall.
verdict_token="$(classify_final_verdict "$reached" "$first_ps" "$max_ps" \
                    "$saw_ps" "$final_readback_failed" "$last_blocker_count")"

case "$verdict_token" in
    seam)
        echo "=== cold-start-wipe-stopwatch: SEAM — provable tip climbed ($first_ps -> $max_ps) across $boots boot(s) but did not reach network_tip=$last_network_tip within ${BUDGET}s ==="
        write_artifact "seam" 3 "provable tip made forward progress ($first_ps -> $max_ps) across $boots boot(s) but did not catch network_tip within budget"
        exit 3
        ;;
    stalled-named)
        echo "=== cold-start-wipe-stopwatch: STALLED-NAMED — no forward progress in ${BUDGET}s; active blocker(s): $last_blocker_ids ==="
        write_artifact "stalled-named" 4 "no forward progress; named blocker(s): $last_blocker_ids"
        exit 4
        ;;
    readback-failed)
        # We could NOT read the node's provable tip at the end (final readback
        # failed) or never got a single sample all run — an INSTRUMENT failure.
        # Carries the last good provable sample; judged FAIL-with-named-cause,
        # never silent-stall, never PASS.
        echo "cold-start-wipe-stopwatch: last 20 log lines:"
        tail -20 "$DATADIR/node.log" 2>/dev/null | sed 's/^/  /'
        echo "=== cold-start-wipe-stopwatch: READBACK-FAILED — no authoritative hstar or cached_provable_tip proxy at final capture (last good provable sample: ${last_ps}, saw_sample=${saw_ps}) ==="
        write_artifact "readback-failed" 6 "frontier readback yielded neither hstar nor cached_provable_tip after bounded retries (last good provable sample=${last_ps}, saw_sample=${saw_ps}); instrument failure, not an observed stall"
        exit 6
        ;;
    *)
        # silent-stall: we COULD read the provable tip throughout, it was
        # genuinely flat, and nothing named a blocker — the real silent-stall
        # failure class.
        echo "cold-start-wipe-stopwatch: last 20 log lines:"
        tail -20 "$DATADIR/node.log" 2>/dev/null | sed 's/^/  /'
        die "no forward progress AND no named blocker in ${BUDGET}s (silent-stall failure class)"
        ;;
esac
