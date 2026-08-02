#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# build_bench.sh — `make build-bench`. The first-party baseline for what the
# build and test loop costs on THIS host, measured with a wall clock.
#
# It exists because the Merkle build/test cache work is about to make claims —
# "this is faster", "the warm path did not regress", "the hit rate matches
# ccache" — and there was no measured number to compare against. Prose timings
# in Makefile comments are not a baseline; they are somebody's memory.
#
# Relationship to the other timing tools in this repository:
#   tools/scripts/timings.sh          READS machine-written artifacts, measures
#                                     nothing, and refuses to print a duration
#                                     it did not measure. Its honesty states
#                                     (NOT MEASURED / STALE / UNTRUSTED /
#                                     FAILED / PARTIAL) are the model this
#                                     script follows.
#   tools/scripts/first_build_timing.sh  what a NEWCOMER's first clone costs.
#   tools/dev/dev-loop-bench.sh       what the edit->verified-hot-swap loop
#                                     costs.
#   this script                       what the BUILD and the TEST LOOP cost,
#                                     scenario by scenario, on an already
#                                     cloned tree.
#
# Every number written here was produced by running the command recorded next
# to it in the artifact. There is no path in this file that writes a duration
# from anywhere else.
#
# ── The honesty states ────────────────────────────────────────────────────
#   ok          measured, and the scenario's own precondition held.
#   SKIPPED     not run in this mode (quick mode skips the expensive cold
#               scenarios). No number is invented for it.
#   FAILED      the command exited nonzero, or a test run did not print its
#               pass token. The duration is real but it is not the cost of
#               work that succeeded, so it is WITHHELD and the failure named.
#   UNTRUSTED   the command succeeded but the measurement does not measure
#               what the scenario claims. The two cases that actually happen:
#               an "edit" scenario that recompiled zero objects (the edit did
#               not invalidate anything, so the timing is a no-op build), and
#               a "no-op" scenario that recompiled something (the tree was not
#               converged, so the timing includes real work). Numbers are
#               WITHHELD, because a caveated number still gets quoted.
#
# ── Why ccache is off for the primary baseline ────────────────────────────
# Makefile:6-11 auto-detects sccache/ccache and prepends it to $(CC). This host
# has a warm ccache shared across every worktree, so a default `make` here can
# report a compile time produced by another checkout's cache rather than by the
# compiler. Every primary scenario therefore runs with ZCL_USE_CCACHE=0. The
# ccache-enabled variants are measured separately and labelled, so the delta is
# visible instead of silently baked into the baseline.
#
# ── Restoration ───────────────────────────────────────────────────────────
# The edit scenarios modify real tracked files. Each one saves the file's exact
# bytes first, restores them afterwards, and the script fails loudly if the
# `git status --porcelain` snapshot it took at entry differs at exit. An EXIT
# trap restores on interrupt too.
#
# Cost: full mode runs two cold builds, the whole test suite, and a set of
# repeated incremental builds. Quick mode (--quick) skips everything cold and
# is usable in the inner loop.
#
# ── v2: resource + cache + identity measurements ──────────────────────────
# Schema zcl.build_bench.v2 is ADDITIVE over v1: every v1 field is unchanged.
# New per-scenario fields (null when not measurable, never invented):
#   cpu_user_s / cpu_sys_s / max_rss_kib   from GNU time -v wrapping each
#                                          sample; aggregated by one rule —
#                                          the values of the SAME sample that
#                                          produced min_ms (see pick_at_min_ms).
#   load1_start                            /proc/loadavg at scenario start.
#   lto_cache_{kib,files}_{start,end}      du -sk + file count of the
#                                          incremental-LTO cache dir.
#   artifact_sha256                        sha256 of build/bin/zclassic23, but
#                                          only when the scenario republished it.
# New full-mode-only scenarios accel_cold_cache / accel_warm_cache measure the
# per-TU LTO release pipeline cold-cache vs warm-cache, and the top-level
# accel_byte_identity verdict (PASS|FAIL|UNKNOWN) proves the two products are
# byte-identical.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CACHE="${ZCL_TIMINGS_CACHE:-$ROOT/.cache}"
OUT_DIR="$CACHE/build-bench"
OUT="$OUT_DIR/last-run.json"
LOG_DIR="$OUT_DIR/logs"

MODE=full
SAMPLES=3
JOBS="$(nproc 2>/dev/null || echo 1)"
GROUP="${ZCL_BUILD_BENCH_GROUP:-netbase_split_host_port}"
FAKE="${ZCL_BUILD_BENCH_FAKE:-}"

# GNU time(1) wraps every measured command for the CPU/RSS columns. Detected
# once here; when it is absent the columns are recorded as null and the run
# continues — a missing measurement is null, never invented.
TIME_BIN=""
if [ -x /usr/bin/time ] && /usr/bin/time -v true >/dev/null 2>&1; then
    TIME_BIN=/usr/bin/time
fi

# The incremental-LTO WPA cache this build uses. Overridable so a probe cache
# can be measured without touching the real one; the wipe for the cold-cache
# scenario below always targets the REAL path under build/, not this variable.
LTO_CACHE_DIR="${ZCL_BENCH_LTO_CACHE_DIR:-$ROOT/build/lto-cache}"

# The two files the edit scenarios mutate. The header is the point of the whole
# exercise: it is the highest-fan-out header in the tree, so a one-line change
# to it is the worst incremental build this repository can produce.
#
# The LOG_*/GUARD* macros live in lib/base; lib/util/include/util/log_macros.h
# is a one-line forwarder that stayed put so the ~63% of the tree that already
# spelled the include that way did not have to change. Editing the lib/base
# file therefore reaches BOTH sets. Every count below is derived at run time by
# git grep — none is pinned here.
HDR="lib/base/include/base/log_macros.h"
HDR_FORWARDER="lib/util/include/util/log_macros.h"
IMPL="lib/util/src/crc32c.c"

say()  { printf '%s\n' "$*" >&2; }
die()  { printf 'build-bench: FATAL — %s\n' "$*" >&2; exit 2; }
now_ms() { date +%s%3N; }
json_escape() { printf '%s' "${1:-}" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
rel() { printf '%s' "${1#"$ROOT"/}"; }

load1_now() { awk '{print $1}' /proc/loadavg 2>/dev/null || echo 0; }

# JSON scalar emitters: empty -> null. A measurement that was not taken is
# null in the artifact, never zero.
jnum() { case "${1:-}" in ''|*[!0-9.]*) printf 'null' ;; *) printf '%s' "$1" ;; esac; }
jstr() { if [ -n "${1:-}" ]; then printf '"%s"' "$(json_escape "$1")"; else printf 'null'; fi; }

# Snapshot of a cache directory: total KiB + file count. Empty strings (-> JSON
# null) when the directory does not exist — wipe_build rm -rf build/ takes the
# LTO cache with it, and that disappearance must be visible in the numbers,
# not smoothed over.
SNAP_KIB=""; SNAP_FILES=""
cache_snapshot() {  # $1 = dir
    SNAP_KIB=""; SNAP_FILES=""
    [ -d "$1" ] || return 0
    SNAP_KIB="$(du -sk "$1" 2>/dev/null | awk '{print $1}')"
    SNAP_FILES="$(find "$1" -type f 2>/dev/null | wc -l)"
}

# Parse a GNU time -v output file into LAST_CPU_USER / LAST_CPU_SYS /
# LAST_RSS_KIB (decimal seconds, decimal seconds, KiB). Missing file or
# missing lines -> empty strings, which the artifact writes as null.
LAST_CPU_USER=""; LAST_CPU_SYS=""; LAST_RSS_KIB=""
parse_time_v() {  # $1 = path to `time -v` output (may not exist)
    LAST_CPU_USER=""; LAST_CPU_SYS=""; LAST_RSS_KIB=""
    [ -n "${1:-}" ] && [ -f "$1" ] || return 0
    LAST_CPU_USER="$(awk '/User time \(seconds\):/{print $NF}' "$1")"
    LAST_CPU_SYS="$(awk '/System time \(seconds\):/{print $NF}' "$1")"
    LAST_RSS_KIB="$(awk '/Maximum resident set size \(kbytes\):/{print $NF}' "$1")"
}

# ── scenario table (parallel arrays; one row per measured scenario) ────────
S_NAME=(); S_STATE=(); S_CMD=(); S_CC=(); S_MS=(); S_RC=(); S_OBJ=()
S_REASON=(); S_NOTE=(); S_BIN=()
S_CPUU=(); S_CPUS=(); S_RSS=(); S_LOAD1=()
S_LKS=(); S_LKE=(); S_LFS=(); S_LFE=(); S_SHA=()

record() {  # name state command ccache samples_csv rc objects reason note
            # [binaries] [cpu_user_s cpu_sys_s max_rss_kib load1_start
            #  lto_kib_start lto_kib_end lto_files_start lto_files_end sha256]
    S_NAME+=("$1"); S_STATE+=("$2"); S_CMD+=("$3"); S_CC+=("$4")
    S_MS+=("$5");   S_RC+=("$6");    S_OBJ+=("$7"); S_REASON+=("$8"); S_NOTE+=("$9")
    S_BIN+=("${10:--1}")
    S_CPUU+=("${11:-}"); S_CPUS+=("${12:-}"); S_RSS+=("${13:-}"); S_LOAD1+=("${14:-}")
    S_LKS+=("${15:-}");  S_LKE+=("${16:-}");  S_LFS+=("${17:-}");   S_LFE+=("${18:-}")
    S_SHA+=("${19:-}")
}

skip() { record "$1" SKIPPED "$2" "$3" '' 0 -1 "$4" ''; say "  SKIPPED   $1 — $4"; }

# ── restoration ───────────────────────────────────────────────────────────
BACKUP_DIR=""
GIT_SNAPSHOT=""
declare -A SAVED=()

cleanup() {
    local f
    for f in "${!SAVED[@]}"; do
        [ -f "${SAVED[$f]}" ] && cp -f -- "${SAVED[$f]}" "$ROOT/$f"
    done
    [ -n "$BACKUP_DIR" ] && rm -rf -- "$BACKUP_DIR"
    return 0
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

save_file() {   # $1 = repo-relative path
    [ -f "$ROOT/$1" ] || die "cannot benchmark an edit to a missing file: $1"
    local dest="$BACKUP_DIR/$(printf '%s' "$1" | tr / _)"
    cp -f -- "$ROOT/$1" "$dest" || die "cannot back up $1"
    SAVED["$1"]="$dest"
}

restore_file() {  # $1 = repo-relative path; content restored, mtime = now so
                  # the next build genuinely reconverges rather than trusting
                  # an object compiled from the edited bytes.
    local src="${SAVED[$1]:-}"
    [ -n "$src" ] || die "restore_file called for an unsaved path: $1"
    cp -f -- "$src" "$ROOT/$1" || die "cannot restore $1"
    touch "$ROOT/$1"
    cmp -s -- "$src" "$ROOT/$1" || die "restore of $1 did not reproduce the original bytes"
}

# ── measurement primitives ────────────────────────────────────────────────
LAST_MS=0; LAST_RC=0; LAST_LOG=""

run_timed() {   # $1 = log path, rest = command line (run through the shell)
    local log="$1"; shift
    local t0 t1 tlog="$log.timev"
    t0="$(now_ms)"
    if [ -n "$TIME_BIN" ]; then
        # time -v writes its report to tlog (-o), so the command's own stderr
        # still lands in $log exactly as it did without the wrapper.
        ( cd "$ROOT" && "$TIME_BIN" -v -o "$tlog" bash -c "$*" ) > "$log" 2>&1
    else
        ( cd "$ROOT" && eval "$*" ) > "$log" 2>&1
    fi
    LAST_RC=$?
    t1="$(now_ms)"
    LAST_MS=$(( t1 - t0 ))
    LAST_LOG="$log"
    parse_time_v "$tlog"
}

REF=""
mark_ref() { REF="$BACKUP_DIR/objref"; rm -f "$REF"; touch "$REF"; }

objects_since_ref() {
    [ -d "$ROOT/build" ] || { echo 0; return; }
    find "$ROOT/build" -name '*.o' -newer "$REF" 2>/dev/null | wc -l
}

# The compile-phase targets (build-only) produce per-TU objects. The product
# targets do NOT: `make` builds each release binary with ONE whole-program cc
# invocation over the whole source set, so there is no intermediate .o to
# count and objects_since_ref is structurally zero for them. Their falsifiable
# signal is whether a binary was actually republished.
binaries_since_ref() {
    [ -d "$ROOT/build/bin" ] || { echo 0; return; }
    find "$ROOT/build/bin" -maxdepth 1 -type f -newer "$REF" \
        ! -name '*.link.*' ! -name '*.debug' 2>/dev/null | wc -l
}

epoch_object_count() {  # objects in the largest compile epoch on disk
    local d best=0 n
    for d in "$ROOT"/build/obj/epochs/*/; do
        [ -d "$d" ] || continue
        n="$(find "$d" -name '*.o' 2>/dev/null | wc -l)"
        [ "$n" -gt "$best" ] && best="$n"
    done
    echo "$best"
}

median() {  # csv of integers -> median
    local n arr
    mapfile -t arr < <(tr ',' '\n' <<<"$1" | sort -n)
    n=${#arr[@]}
    [ "$n" -eq 0 ] && { echo 0; return; }
    echo "${arr[$((n / 2))]}"
}
mn() { tr ',' '\n' <<<"$1" | sort -n | head -1; }
mx() { tr ',' '\n' <<<"$1" | sort -n | tail -1; }

# Aggregation rule for the resource columns (cpu_user_s / cpu_sys_s /
# max_rss_kib): each scenario publishes the values of the SAME sample that
# produced min_ms — the fastest sample is the least perturbed run, so its
# resource use is the most representative. One rule, applied to every
# scenario; the wall-clock aggregates (min/median/max) are unchanged.
pick_at_min_ms() {  # $1 = ms csv; $2.. = per-sample values -> value at the min-ms index
    local ms="$1"; shift
    local -a ma va=("$@")
    IFS=',' read -ra ma <<<"$ms"
    [ "${#ma[@]}" -gt 0 ] && [ "$#" -eq "${#ma[@]}" ] || { echo ""; return; }
    local i best=0
    for i in "${!ma[@]}"; do [ "${ma[$i]}" -lt "${ma[$best]}" ] && best="$i"; done
    echo "${va[$best]:-}"
}

# ── build command construction ────────────────────────────────────────────
# Every build in this script names ZCL_USE_CCACHE explicitly. A build whose
# compiler-cache state is inherited from the environment is not a baseline.
mk() {   # $1 = "on"|"off" ccache, rest = make goals
    local cc="$1"; shift
    if [ -n "$FAKE" ]; then
        case "$FAKE" in fail) echo false ;; *) echo true ;; esac
        return
    fi
    local flag=1
    [ "$cc" = off ] && flag=0
    printf 'make -j%s ZCL_USE_CCACHE=%s%s' "$JOBS" "$flag" "${*:+ $*}"
}

wipe_build() { [ -n "$FAKE" ] && return 0; rm -rf "$ROOT/build"; }

# Surgical wipe for the accelerated release pipeline: build/rel-obj (the
# per-TU objects) and the product binary go, but the rest of build/ — other
# profiles' objects, test objects, dev objects — must survive, so wipe_build
# is wrong here. $1 = "cache" also drops the incremental-LTO WPA cache (the
# cold-cache run); the -flto-incremental link requires the cache dir to
# pre-exist, so it is recreated EMPTY — still a cold cache, and the
# scenario's lto_cache_*_start fields prove it.
wipe_accel() {  # $1 = "cache"|"keep"
    [ -n "$FAKE" ] && return 0
    rm -rf "$ROOT/build/rel-obj" "$ROOT/build/bin/zclassic23"
    if [ "${1:-}" = cache ]; then
        rm -rf "$ROOT/build/lto-cache"
        mkdir -p "$ROOT/build/lto-cache"
    fi
}

# Whether this tree has the per-TU LTO release pipeline at all. Detection is
# a Makefile marker grep: the pipeline defines REL_OBJ_ROOT, and a tree
# without it (e.g. a ZCL_LTO_ACCEL=0-only tree) gets SKIPPED rows with the
# reason named, never FAILED. A `make -n` probe would pay a full Makefile
# parse to learn the same thing.
accel_available() { grep -q '^REL_OBJ_ROOT' "$ROOT/Makefile" 2>/dev/null; }

# Verdict for the accel pair: byte identity between the cold-cache and
# warm-cache products. Either hash missing (scenario skipped or failed) ->
# UNKNOWN, because an unmeasured identity is not a PASS.
byte_identity_verdict() {  # $1 $2 = sha256 hex or empty
    [ -n "${1:-}" ] && [ -n "${2:-}" ] || { echo UNKNOWN; return; }
    [ "$1" = "$2" ] && echo PASS || echo FAIL
}

# ── the edits ─────────────────────────────────────────────────────────────
# Both are real token-level changes, not comments: a comment-only edit is a
# ccache preprocessed-mode hit and would understate every ccache-on variant.
NONCE=""
HDR_NOTE=""
edit_header() {
    # Inserted INSIDE the include guard, immediately before the closing
    # #endif, so every one of the dependents re-preprocesses to different
    # tokens exactly as a real interface change does.
    local f="$ROOT/$HDR" tmp="$BACKUP_DIR/hdr.tmp"
    awk -v nonce="$NONCE" '
        { lines[NR] = $0 }
        END {
            for (i = 1; i < NR; i++) print lines[i]
            print "#define ZCL_BUILD_BENCH_PROBE " nonce
            print lines[NR]
        }' "$f" > "$tmp" && mv -f "$tmp" "$f" || die "header edit failed"
}
edit_impl() {
    local f="$ROOT/$IMPL"
    {
        printf '\n/* build-bench transient probe — removed by tools/scripts/build_bench.sh */\n'
        printf 'int zcl_build_bench_probe_%s(void);\n' "$NONCE"
        printf 'int zcl_build_bench_probe_%s(void) { return %s; }\n' "$NONCE" "$((NONCE % 1000))"
    } >> "$f" || die "implementation edit failed"
}

# ── one scenario ──────────────────────────────────────────────────────────
# $1 name, $2 samples, $3 expectation, $4 ccache label, $5 edit hook ("-" for
# none), $6 command. The expectation is what makes a green build refuse to
# publish a misleading number; several combine with '+' (e.g.
# recompile_many+relink_some for a build that compiles AND links).
#   none          no check
#   recompile_none    a no-op build: any recompile means the tree was not converged
#   recompile_one     an implementation edit: at least one object must rebuild
#   recompile_many    an interface edit: many objects must rebuild
#   recompile_all     a cold build: most of the tree must be compiled
#   relink_none       a no-op product build: no binary may be republished
#   relink_some       a product build that had work to do: a binary must appear
run_case() {
    local name="$1" n="$2" expect="$3" cclabel="$4" hook="$5" cmd="$6"
    local i ms_csv="" rc=0 obj=0 bin=0 state=ok reason="" note=""
    local marker="$BACKUP_DIR/marker.$name" load1 lks="" lke="" lfs="" lfe="" sha=""
    local -a cpuu=() cpus=() rss=()
    say "  running   $name  ($n sample(s), ccache $cclabel)"
    : > "$marker"   # scenario-start marker: the artifact hash anchors on it
    load1="$(load1_now)"
    cache_snapshot "$LTO_CACHE_DIR"; lks="$SNAP_KIB"; lfs="$SNAP_FILES"
    for (( i = 0; i < n; i++ )); do
        NONCE="$(( $(date +%s) % 100000 + i ))"
        [ "$hook" != "-" ] && "$hook"
        mark_ref
        run_timed "$LOG_DIR/$name.$i.log" "$cmd"
        obj="$(objects_since_ref)"
        bin="$(binaries_since_ref)"
        [ "$LAST_RC" -ne 0 ] && rc="$LAST_RC"
        ms_csv="${ms_csv:+$ms_csv,}$LAST_MS"
        cpuu+=("${LAST_CPU_USER:-}"); cpus+=("${LAST_CPU_SYS:-}"); rss+=("${LAST_RSS_KIB:-}")
        say "            sample $((i + 1)): ${LAST_MS} ms, rc=$LAST_RC, objects rebuilt=$obj, binaries relinked=$bin${LAST_CPU_USER:+, cpu=${LAST_CPU_USER}s+${LAST_CPU_SYS:-?}s sys, rss=${LAST_RSS_KIB:-?}KiB}"
        if [ "$hook" != "-" ]; then
            case "$hook" in
                edit_header) restore_file "$HDR" ;;
                edit_impl)   restore_file "$IMPL" ;;
            esac
            # Unmeasured reconvergence: the next sample must start from a
            # converged tree or it would measure the restore, not the edit.
            [ -z "$FAKE" ] && ( cd "$ROOT" && eval "$cmd" ) >/dev/null 2>&1
        fi
    done
    cache_snapshot "$LTO_CACHE_DIR"; lke="$SNAP_KIB"; lfe="$SNAP_FILES"

    if [ "$rc" -ne 0 ]; then
        state=FAILED
        reason="command exited $rc — see $(rel "$LOG_DIR/$name.0.log")"
    else
        local part
        for part in ${expect//+/ }; do
            [ "$state" = ok ] || break
            case "$part" in
                recompile_none) [ "$obj" -gt 0 ] && { state=UNTRUSTED
                    reason="a no-op build recompiled $obj object(s) — the tree was not converged, so this is not a no-op cost"; } ;;
                recompile_one)  [ "$obj" -lt 1 ] && { state=UNTRUSTED
                    reason="an implementation edit recompiled 0 objects — the edit did not invalidate anything"; } ;;
                recompile_many) [ "$obj" -lt 100 ] && { state=UNTRUSTED
                    reason="an interface edit recompiled only $obj object(s) — expected the header's dependent set"; } ;;
                recompile_all)  [ "$obj" -lt 500 ] && { state=UNTRUSTED
                    reason="a cold build compiled only $obj object(s) — the build tree was not actually cold"; } ;;
                relink_none)    [ "$bin" -gt 0 ] && { state=UNTRUSTED
                    reason="a no-op product build republished $bin binary/binaries — the tree was not converged"; } ;;
                relink_some)    [ "$bin" -lt 1 ] && { state=UNTRUSTED
                    reason="no binary was republished — this timed a build that had nothing to do"; } ;;
            esac
        done
    fi
    [ "$n" -eq 1 ] && note="single sample — not a stable figure"
    [ "$hook" = edit_header ] && note="${note:+$note; }${HDR_NOTE:-}"
    case "$expect" in
        *relink_*)
            case "$name" in
                # The accel pipeline builds per-TU objects; the whole-program
                # note below would be false for it.
                accel_*) ;;
                *) note="${note:+$note; }whole-program build: one cc over the whole source set, so it produces no intermediate objects — the check is on republished binaries ($bin)" ;;
            esac ;;
    esac
    # Artifact hash: the product binary's sha256, but only when this scenario
    # actually republished it (newer than the scenario-start marker) — hashing
    # a stale binary would certify nothing, so the field is null instead.
    case "$name" in
        compile_*|link_*|accel_*)
            local b="$ROOT/build/bin/zclassic23"
            if [ -f "$b" ] && [ "$b" -nt "$marker" ]; then
                sha="$(sha256sum "$b" | awk '{print $1}')"
            fi ;;
    esac
    local agg_cpuu="" agg_cpus="" agg_rss=""
    if [ "${#cpuu[@]}" -gt 0 ]; then
        agg_cpuu="$(pick_at_min_ms "$ms_csv" "${cpuu[@]}")"
        agg_cpus="$(pick_at_min_ms "$ms_csv" "${cpus[@]}")"
        agg_rss="$(pick_at_min_ms "$ms_csv" "${rss[@]}")"
    fi
    record "$name" "$state" "$cmd" "$cclabel" "$ms_csv" "$rc" "$obj" "$reason" "$note" "$bin" \
        "$agg_cpuu" "$agg_cpus" "$agg_rss" "$load1" "$lks" "$lke" "$lfs" "$lfe" "$sha"
}

# ── test scenarios (pass token, not exit code) ────────────────────────────
run_test_case() {
    local name="$1" n="$2" cclabel="$3" cmd="$4" warm="${5:-no}"
    local i ms_csv="" rc=0 state=ok reason="" note="" groups=""
    local load1 lks="" lke="" lfs="" lfe=""
    local -a cpuu=() cpus=() rss=()
    # The first `make t-fast` on a tree that has never linked the fast harness
    # pays for that link. Measuring it as sample 1 and the steady state as
    # samples 2..N produces a spread that describes the harness, not the test.
    # The link is a real cost, but it is a different one; it gets its own
    # unmeasured warm-up here and is named in the scenario note.
    if [ "$warm" = warm ] && [ -z "$FAKE" ]; then
        say "  warming   (unmeasured) $cmd"
        ( cd "$ROOT" && eval "$cmd" ) >/dev/null 2>&1
    fi
    say "  running   $name  ($n sample(s))"
    load1="$(load1_now)"
    cache_snapshot "$LTO_CACHE_DIR"; lks="$SNAP_KIB"; lfs="$SNAP_FILES"
    for (( i = 0; i < n; i++ )); do
        mark_ref
        run_timed "$LOG_DIR/$name.$i.log" "$cmd"
        [ "$LAST_RC" -ne 0 ] && rc="$LAST_RC"
        ms_csv="${ms_csv:+$ms_csv,}$LAST_MS"
        cpuu+=("${LAST_CPU_USER:-}"); cpus+=("${LAST_CPU_SYS:-}"); rss+=("${LAST_RSS_KIB:-}")
        say "            sample $((i + 1)): ${LAST_MS} ms, rc=$LAST_RC"
        if [ -z "$FAKE" ]; then
            if ! grep -q 'ALL TESTS PASSED' "$LAST_LOG" || grep -q 'SOME TESTS FAILED' "$LAST_LOG"; then
                state=FAILED
                reason="the runner did not print its pass token — a duration from a suite that did not pass is not a cost"
            fi
            grep -q 'matched no groups' "$LAST_LOG" && { state=UNTRUSTED
                reason="--only matched no groups; nothing was run"; }
            groups="$(grep -oE '[0-9]+/[0-9]+ groups failed' "$LAST_LOG" | tail -1)"
        fi
    done
    cache_snapshot "$LTO_CACHE_DIR"; lke="$SNAP_KIB"; lfe="$SNAP_FILES"
    [ "$rc" -ne 0 ] && [ "$state" = ok ] && { state=FAILED; reason="command exited $rc"; }
    [ "$n" -eq 1 ] && note="single sample — not a stable figure"
    [ "$warm" = warm ] && note="${note:+$note; }harness link paid by an unmeasured warm-up first"
    [ -n "$groups" ] && note="${note:+$note; }$groups"
    local agg_cpuu="" agg_cpus="" agg_rss=""
    if [ "${#cpuu[@]}" -gt 0 ]; then
        agg_cpuu="$(pick_at_min_ms "$ms_csv" "${cpuu[@]}")"
        agg_cpus="$(pick_at_min_ms "$ms_csv" "${cpus[@]}")"
        agg_rss="$(pick_at_min_ms "$ms_csv" "${rss[@]}")"
    fi
    record "$name" "$state" "$cmd" "$cclabel" "$ms_csv" "$rc" -1 "$reason" "$note" -1 \
        "$agg_cpuu" "$agg_cpus" "$agg_rss" "$load1" "$lks" "$lke" "$lfs" "$lfe" ""
}

# ── code index scenarios ──────────────────────────────────────────────────
CI_BIN="build/bin/zclassic23"
CI_STORE=".codeindex/index.kv"

run_codeindex() {
    local n="$SAMPLES"
    if [ -n "$FAKE" ]; then
        record codeindex_cold SKIPPED true n/a '' 0 -1 "self-test run — the node binary is not exercised" ''
        record codeindex_warm SKIPPED true n/a '' 0 -1 "self-test run — the node binary is not exercised" ''
        return
    fi
    if [ ! -x "$ROOT/$CI_BIN" ]; then
        skip codeindex_cold "$CI_BIN code map" n/a "$CI_BIN is not built — run make first"
        skip codeindex_warm "$CI_BIN code map" n/a "$CI_BIN is not built — run make first"
        return
    fi
    local cmd="$CI_BIN code map >/dev/null"
    local i ms_cold="" ms_warm="" rc=0 state=ok reason=""
    local load1_c load1_w lks="" lke="" lfs="" lfe=""
    local -a cpuu=() cpus=() rss=()

    load1_c="$(load1_now)"
    cache_snapshot "$LTO_CACHE_DIR"; lks="$SNAP_KIB"; lfs="$SNAP_FILES"
    for (( i = 0; i < n; i++ )); do
        rm -rf "$ROOT/.codeindex"
        run_timed "$LOG_DIR/codeindex_cold.$i.log" "$cmd"
        [ "$LAST_RC" -ne 0 ] && rc="$LAST_RC"
        ms_cold="${ms_cold:+$ms_cold,}$LAST_MS"
        cpuu+=("${LAST_CPU_USER:-}"); cpus+=("${LAST_CPU_SYS:-}"); rss+=("${LAST_RSS_KIB:-}")
        [ -f "$ROOT/$CI_STORE" ] || { state=UNTRUSTED
            reason="no $CI_STORE after a cold run — nothing was built to time"; }
    done
    cache_snapshot "$LTO_CACHE_DIR"; lke="$SNAP_KIB"; lfe="$SNAP_FILES"
    [ "$rc" -ne 0 ] && { state=FAILED; reason="command exited $rc"; }
    record codeindex_cold "$state" "rm -rf .codeindex && $cmd" n/a "$ms_cold" "$rc" -1 "$reason" \
        'cold = store deleted first, so the query pays a full deterministic rebuild' -1 \
        "$(pick_at_min_ms "$ms_cold" "${cpuu[@]}")" "$(pick_at_min_ms "$ms_cold" "${cpus[@]}")" \
        "$(pick_at_min_ms "$ms_cold" "${rss[@]}")" "$load1_c" "$lks" "$lke" "$lfs" "$lfe" ""

    state=ok; reason=""; rc=0
    cpuu=(); cpus=(); rss=()
    local before after
    load1_w="$(load1_now)"
    cache_snapshot "$LTO_CACHE_DIR"; lks="$SNAP_KIB"; lfs="$SNAP_FILES"
    for (( i = 0; i < n; i++ )); do
        before="$(stat -c '%Y-%i-%s' "$ROOT/$CI_STORE" 2>/dev/null)"
        run_timed "$LOG_DIR/codeindex_warm.$i.log" "$cmd"
        [ "$LAST_RC" -ne 0 ] && rc="$LAST_RC"
        ms_warm="${ms_warm:+$ms_warm,}$LAST_MS"
        cpuu+=("${LAST_CPU_USER:-}"); cpus+=("${LAST_CPU_SYS:-}"); rss+=("${LAST_RSS_KIB:-}")
        after="$(stat -c '%Y-%i-%s' "$ROOT/$CI_STORE" 2>/dev/null)"
        [ "$before" = "$after" ] || { state=UNTRUSTED
            reason="the store was republished during a warm query — this timed a rebuild, not a warm read"; }
    done
    cache_snapshot "$LTO_CACHE_DIR"; lke="$SNAP_KIB"; lfe="$SNAP_FILES"
    [ "$rc" -ne 0 ] && { state=FAILED; reason="command exited $rc"; }
    record codeindex_warm "$state" "$cmd" n/a "$ms_warm" "$rc" -1 "$reason" \
        'warm = unchanged tree, store reused (asserted: the store file was not republished)' -1 \
        "$(pick_at_min_ms "$ms_warm" "${cpuu[@]}")" "$(pick_at_min_ms "$ms_warm" "${cpus[@]}")" \
        "$(pick_at_min_ms "$ms_warm" "${rss[@]}")" "$load1_w" "$lks" "$lke" "$lfs" "$lfe" ""
}

# ── the run ───────────────────────────────────────────────────────────────
measure() {
    BACKUP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/zcl-build-bench.XXXXXX")" ||
        die "cannot create a scratch directory"
    rm -rf "$LOG_DIR"
    mkdir -p "$LOG_DIR" || die "cannot create $LOG_DIR"
    GIT_SNAPSHOT="$(git -C "$ROOT" status --porcelain 2>/dev/null)"

    save_file "$HDR"
    save_file "$IMPL"

    local hdr_fanout hdr_via hdr_direct cc_bin cc_ver load_start load_end commit
    # Derived, never pinned, and scoped three ways on purpose.
    #
    # By INCLUDE DIRECTIVE, not by "the filename appears in the file": a
    # substring count also matches prose and conflates the two spellings.
    #
    # To '*.c' and '*.h', because only a translation unit can be recompiled.
    # An unscoped grep also hits tools/new_shape.sh (a template generator that
    # emits the include line) and THIS FILE, whose own search strings would
    # otherwise inflate the count it reports by one -- the count would grow
    # because the benchmark exists. That is the self-match the scope prevents.
    local via_pat='#include "util/log_macros.h"' direct_pat='#include "base/log_macros.h"'
    hdr_via="$(cd "$ROOT" && git grep -l "$via_pat" -- '*.c' '*.h' 2>/dev/null | wc -l)"
    hdr_direct="$(cd "$ROOT" && git grep -l "$direct_pat" -- '*.c' '*.h' 2>/dev/null | wc -l)"
    hdr_fanout="$(cd "$ROOT" && { git grep -l "$via_pat" -- '*.c' '*.h'
                                  git grep -l "$direct_pat" -- '*.c' '*.h'; } 2>/dev/null |
                 sort -u | wc -l)"
    cc_bin="$(command -v sccache 2>/dev/null || command -v ccache 2>/dev/null || true)"
    [ -n "$cc_bin" ] && cc_ver="$("$cc_bin" --version 2>/dev/null | head -1)"
    load_start="$(awk '{print $1}' /proc/loadavg 2>/dev/null || echo 0)"
    commit="$(git -C "$ROOT" log -1 --format='%H' 2>/dev/null || echo unknown)"

    say "══ build-bench: measuring the build + test loop on $(uname -n) ══"
    say "  mode:     $MODE      jobs: $JOBS      samples: $SAMPLES"
    say "  ccache:   ${cc_bin:-none on PATH} — DISABLED for every primary scenario"
    say "  header:   $HDR — $hdr_via .c/.h reach it through $HDR_FORWARDER,"
    say "            $hdr_direct include it directly, $hdr_fanout distinct .c/.h in all"
    say "  loadavg:  $load_start at start (this host is shared; see the artifact)"
    say ""

    # 1-2. Cold. Full mode only: each one wipes build/ and pays the whole tree.
    if [ "$MODE" = full ]; then
        wipe_build
        run_case compile_cold 1 recompile_all off - "$(mk off build-only)"
        wipe_build
        run_case link_cold 1 relink_some off - "$(mk off)"
        # Same cold compile with the host's ccache in play, so the delta
        # between "the compiler ran" and "ccache answered" is visible rather
        # than silently folded into the baseline.
        wipe_build
        run_case compile_cold_ccache 1 recompile_all on - "$(mk on build-only)"
        wipe_build
    else
        skip compile_cold "$(mk off build-only)" off "quick mode skips cold builds"
        skip link_cold "$(mk off)" off "quick mode skips cold builds"
        skip compile_cold_ccache "$(mk on build-only)" on "quick mode skips cold builds"
    fi

    # Converge (unmeasured) so every incremental scenario below starts warm.
    if [ -z "$FAKE" ]; then
        say "  warming   (unmeasured) $(mk off build-only)"
        ( cd "$ROOT" && eval "$(mk off build-only)" ) >/dev/null 2>&1 ||
            die "the warm-up build failed — nothing below would be measurable"
        say "  warming   (unmeasured) $(mk off)"
        ( cd "$ROOT" && eval "$(mk off)" ) >/dev/null 2>&1 ||
            die "the warm-up link failed — nothing below would be measurable"
    fi

    # 3. Warm no-op.
    run_case compile_noop "$SAMPLES" recompile_none off - "$(mk off build-only)"
    run_case link_noop "$SAMPLES" relink_none off - "$(mk off)"

    # 4. One-line implementation edit, low fan-out .c.
    run_case compile_edit_impl "$SAMPLES" recompile_one off edit_impl "$(mk off build-only)"

    # 5. One-line INTERFACE edit to the highest-fan-out header in the tree.
    #    This is the scenario the cache design exists to improve. The edit lands
    #    in the lib/base file; the dependents reach it through the util/
    #    forwarder, and the artifact records both counts.
    HDR_NOTE="interface edit: $hdr_via translation unit(s) reach this header through $HDR_FORWARDER, $hdr_direct directly, $hdr_fanout distinct .c/.h in all"
    run_case compile_edit_header "$SAMPLES" recompile_many off edit_header "$(mk off build-only)"
    run_case compile_edit_header_ccache "$SAMPLES" recompile_many on edit_header "$(mk on build-only)"
    if [ "$MODE" = full ]; then
        run_case link_edit_header 1 relink_some off edit_header "$(mk off)"
    else
        skip link_edit_header "$(mk off)" off "quick mode skips whole-program relinks"
    fi

    # 6. Tests.
    if [ "$MODE" = full ]; then
        run_test_case test_suite_cold 1 off "$(mk off test-parallel)"
    else
        skip test_suite_cold "$(mk off test-parallel)" off "quick mode skips the full suite"
    fi
    run_test_case test_group_one "$SAMPLES" off "$(mk off "t-fast ONLY=$GROUP")" warm

    # 7. Code index.
    run_codeindex

    # 8. Accelerated release pipeline: cold-cache vs warm-cache incremental
    #    LTO. Full mode only. The pair exists to prove two things at once:
    #    the warm-cache speedup, and that the warm product is byte-identical
    #    to the cold one (the top-level accel_byte_identity verdict compares
    #    the two artifact_sha256 fields). ccache is ON for both — that is the
    #    pipeline's intended configuration.
    if [ "$MODE" = full ]; then
        if accel_available; then
            wipe_accel cache
            run_case accel_cold_cache 1 recompile_many+relink_some on - "$(mk on zclassic23)"
            wipe_accel keep
            run_case accel_warm_cache 1 recompile_many+relink_some on - "$(mk on zclassic23)"
        else
            skip accel_cold_cache "$(mk on zclassic23)" on \
                "no REL_OBJ_ROOT in the Makefile — the per-TU LTO release pipeline is not in this tree"
            skip accel_warm_cache "$(mk on zclassic23)" on \
                "no REL_OBJ_ROOT in the Makefile — the per-TU LTO release pipeline is not in this tree"
        fi
    else
        skip accel_cold_cache "$(mk on zclassic23)" on "quick mode skips the accelerated cold build"
        skip accel_warm_cache "$(mk on zclassic23)" on "quick mode skips the accelerated warm build"
    fi

    load_end="$(awk '{print $1}' /proc/loadavg 2>/dev/null || echo 0)"

    # ── restoration proof ─────────────────────────────────────────────────
    local f drift=0
    for f in "${!SAVED[@]}"; do
        cmp -s -- "${SAVED[$f]}" "$ROOT/$f" || { say "  DRIFT     $f differs from its pre-benchmark bytes"; drift=1; }
    done
    local snap_now
    snap_now="$(git -C "$ROOT" status --porcelain 2>/dev/null)"
    if [ "$snap_now" != "$GIT_SNAPSHOT" ]; then
        say "  DRIFT     git status changed across the run:"
        diff <(printf '%s\n' "$GIT_SNAPSHOT") <(printf '%s\n' "$snap_now") >&2
        drift=1
    fi
    [ "$drift" -eq 0 ] && say "  restored  every edited file is byte-identical; git status is unchanged"

    write_artifact "$hdr_fanout" "$cc_bin" "${cc_ver:-}" "$load_start" "$load_end" "$commit" \
        "$drift" "$hdr_via" "$hdr_direct"
    report
    local bad=0 i
    for i in "${!S_NAME[@]}"; do [ "${S_STATE[$i]}" = FAILED ] && bad=1; done
    [ "$drift" -ne 0 ] && bad=2
    return "$bad"
}

write_artifact() {
    local fanout="$1" cc_bin="$2" cc_ver="$3" l0="$4" l1="$5" commit="$6" drift="$7"
    local via="${8:-0}" direct="${9:-0}"
    mkdir -p "$OUT_DIR" || die "cannot create $OUT_DIR"
    {
        printf '{"schema":"zcl.build_bench.v2"'
        printf ',"generated_at_utc":"%s"' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf ',"mode":"%s","jobs":%s,"samples":%s' "$MODE" "$JOBS" "$SAMPLES"
        printf ',"host":"%s","cores":%s' "$(json_escape "$(uname -n)")" \
            "$(nproc 2>/dev/null || echo 1)"
        printf ',"kernel":"%s"' "$(json_escape "$(uname -r)")"
        printf ',"mem_total_kib":%s' "$(awk '/MemTotal/{print $2}' /proc/meminfo 2>/dev/null || echo 0)"
        printf ',"commit":"%s"' "$(json_escape "$commit")"
        printf ',"cc":"%s"' "$(json_escape "$(cc --version 2>/dev/null | head -1)")"
        printf ',"compiler_cache":{"tool":"%s","version":"%s","primary_baseline":"disabled"}' \
            "$(json_escape "${cc_bin:-none}")" "$(json_escape "$cc_ver")"
        printf ',"loadavg_start":"%s","loadavg_end":"%s"' \
            "$(json_escape "$l0")" "$(json_escape "$l1")"
        printf ',"high_fanout_header":{"path":"%s","forwarder":"%s"' \
            "$(json_escape "$HDR")" "$(json_escape "$HDR_FORWARDER")"
        printf ',"including_files":%s,"via_forwarder":%s,"direct":%s' \
            "${fanout:-0}" "${via:-0}" "${direct:-0}"
        printf ',"derivation":"git grep -l on each include directive, scoped to *.c and *.h, unique union"}'
        printf ',"low_fanout_impl":"%s"' "$(json_escape "$IMPL")"
        # The denominator for every objects_recompiled below: how many objects
        # one compile epoch contains. A scenario that recompiled this many
        # recompiled the whole tree, whatever the edit was.
        printf ',"objects_per_epoch":%s' "$(epoch_object_count)"
        printf ',"test_group":"%s"' "$(json_escape "$GROUP")"
        printf ',"tree_restored":%s' "$( [ "$drift" -eq 0 ] && echo true || echo false )"
        # Byte identity of the accel pair, compared here from the hashes the
        # two scenarios recorded. UNKNOWN when either side has no hash — an
        # unmeasured identity is not a PASS.
        local i h_cold="" h_warm=""
        for i in "${!S_NAME[@]}"; do
            case "${S_NAME[$i]}" in
                accel_cold_cache) h_cold="${S_SHA[$i]:-}" ;;
                accel_warm_cache) h_warm="${S_SHA[$i]:-}" ;;
            esac
        done
        printf ',"accel_byte_identity":"%s"' "$(byte_identity_verdict "$h_cold" "$h_warm")"
        printf ',"scenarios":['
        for i in "${!S_NAME[@]}"; do
            [ "$i" -gt 0 ] && printf ','
            printf '{"name":"%s","state":"%s","command":"%s","ccache":"%s"' \
                "$(json_escape "${S_NAME[$i]}")" "${S_STATE[$i]}" \
                "$(json_escape "${S_CMD[$i]}")" "${S_CC[$i]}"
            local csv="${S_MS[$i]}" n=0
            [ -n "$csv" ] && n="$(tr ',' '\n' <<<"$csv" | grep -c .)"
            printf ',"samples":%s,"samples_ms":[%s]' "$n" "$csv"
            # Aggregates exist ONLY for a scenario whose own precondition held.
            # A FAILED or UNTRUSTED row publishes no duration to quote.
            if [ "${S_STATE[$i]}" = ok ] && [ "$n" -gt 0 ]; then
                local lo hi md spread
                lo="$(mn "$csv")"; hi="$(mx "$csv")"; md="$(median "$csv")"
                spread=0
                [ "$md" -gt 0 ] && spread=$(( (hi - lo) * 100 / md ))
                printf ',"min_ms":%s,"median_ms":%s,"max_ms":%s,"spread_pct":%s' \
                    "$lo" "$md" "$hi" "$spread"
                # Resource columns ride inside the same honesty gate as the
                # durations: a FAILED/UNTRUSTED row publishes none of them.
                printf ',"cpu_user_s":%s,"cpu_sys_s":%s,"max_rss_kib":%s' \
                    "$(jnum "${S_CPUU[$i]:-}")" "$(jnum "${S_CPUS[$i]:-}")" "$(jnum "${S_RSS[$i]:-}")"
            fi
            printf ',"rc":%s' "${S_RC[$i]}"
            [ "${S_OBJ[$i]}" -ge 0 ] && printf ',"objects_recompiled":%s' "${S_OBJ[$i]}"
            [ "${S_BIN[$i]}" -ge 0 ] && printf ',"binaries_relinked":%s' "${S_BIN[$i]}"
            printf ',"load1_start":%s' "$(jstr "${S_LOAD1[$i]:-}")"
            printf ',"lto_cache_kib_start":%s,"lto_cache_kib_end":%s' \
                "$(jnum "${S_LKS[$i]:-}")" "$(jnum "${S_LKE[$i]:-}")"
            printf ',"lto_cache_files_start":%s,"lto_cache_files_end":%s' \
                "$(jnum "${S_LFS[$i]:-}")" "$(jnum "${S_LFE[$i]:-}")"
            printf ',"artifact_sha256":%s' "$(jstr "${S_SHA[$i]:-}")"
            [ -n "${S_REASON[$i]}" ] && printf ',"reason":"%s"' "$(json_escape "${S_REASON[$i]}")"
            [ -n "${S_NOTE[$i]}" ] && printf ',"note":"%s"' "$(json_escape "${S_NOTE[$i]}")"
            printf '}'
        done
        printf ']}\n'
    } > "$OUT" || die "cannot write $OUT"
    say ""
    say "wrote $(rel "$OUT")"
}

# ── reader ────────────────────────────────────────────────────────────────
# Same contract as tools/scripts/timings.sh: an artifact older than HEAD is
# STALE, and a row that is not `ok` never gets a duration printed for it.
report() {
    [ -f "$OUT" ] || { say "build-bench: NOT MEASURED on this host — no $(rel "$OUT")"
                       say "             produce it with: make build-bench"; return; }
    local head_epoch head_desc mtime stale=""
    head_epoch="$(git -C "$ROOT" log -1 --format=%ct 2>/dev/null || echo 0)"
    head_desc="$(git -C "$ROOT" log -1 --format='%h %cI' 2>/dev/null || echo unknown)"
    mtime="$(stat -c %Y "$OUT" 2>/dev/null || echo 0)"
    [ "$head_epoch" -gt 0 ] && [ "$mtime" -lt "$head_epoch" ] && stale=1

    echo
    echo "══ build-bench: $(grep -o '"host":"[^"]*"' "$OUT" | head -1 | cut -d'"' -f4), $(grep -o '"generated_at_utc":"[^"]*"' "$OUT" | cut -d'"' -f4) ══"
    [ -n "$stale" ] && echo "  STALE — measured before HEAD ($head_desc); re-run: make build-bench"
    printf '  %s | %s\n' \
        "$(grep -o '"cc":"[^"]*"' "$OUT" | head -1 | cut -d'"' -f4)" \
        "$(grep -o '"kernel":"[^"]*"' "$OUT" | cut -d'"' -f4)"
    printf '  cores %s, %s jobs, load %s -> %s, compiler cache %s (disabled for the baseline)\n' \
        "$(grep -o '"cores":[0-9]*' "$OUT" | cut -d: -f2)" \
        "$(grep -o '"jobs":[0-9]*' "$OUT" | cut -d: -f2)" \
        "$(grep -o '"loadavg_start":"[^"]*"' "$OUT" | cut -d'"' -f4)" \
        "$(grep -o '"loadavg_end":"[^"]*"' "$OUT" | cut -d'"' -f4)" \
        "$(grep -o '"tool":"[^"]*"' "$OUT" | cut -d'"' -f4)"
    printf '  interface-edit header: %s\n' \
        "$(grep -o '"path":"[^"]*"' "$OUT" | cut -d'"' -f4)"
    printf '    reached by %s .c/.h through %s, %s directly; %s distinct .c/.h in all\n' \
        "$(grep -o '"via_forwarder":[0-9]*' "$OUT" | cut -d: -f2)" \
        "$(grep -o '"forwarder":"[^"]*"' "$OUT" | cut -d'"' -f4)" \
        "$(grep -o '"direct":[0-9]*' "$OUT" | cut -d: -f2)" \
        "$(grep -o '"including_files":[0-9]*' "$OUT" | cut -d: -f2)"
    printf '  tree restored after the run: %s\n' \
        "$(grep -o '"tree_restored":[a-z]*' "$OUT" | cut -d: -f2)"
    local verdict
    verdict="$(grep -o '"accel_byte_identity":"[^"]*"' "$OUT" | cut -d'"' -f4)"
    printf '  accel cold/warm LTO-cache byte identity: %s\n' "${verdict:-n/a (pre-v2 artifact)}"
    printf '  one compile epoch holds %s objects — OBJECTS at or near that number\n' \
        "$(grep -o '"objects_per_epoch":[0-9]*' "$OUT" | cut -d: -f2)"
    printf '  means the scenario recompiled the whole tree, not a dependent subset.\n'
    echo
    printf '  %-26s %-10s %8s %8s %8s %6s %8s %4s\n' \
        SCENARIO STATE MIN MEDIAN MAX SPREAD OBJECTS BINS
    local line name state lo md hi sp obj bin reason note
    while IFS= read -r line; do
        name="$(grep -o '"name":"[^"]*"' <<<"$line" | cut -d'"' -f4)"
        state="$(grep -o '"state":"[^"]*"' <<<"$line" | cut -d'"' -f4)"
        lo="$(grep -o '"min_ms":[0-9]*' <<<"$line" | cut -d: -f2)"
        md="$(grep -o '"median_ms":[0-9]*' <<<"$line" | cut -d: -f2)"
        hi="$(grep -o '"max_ms":[0-9]*' <<<"$line" | cut -d: -f2)"
        sp="$(grep -o '"spread_pct":[0-9]*' <<<"$line" | cut -d: -f2)"
        obj="$(grep -o '"objects_recompiled":[0-9]*' <<<"$line" | cut -d: -f2)"
        bin="$(grep -o '"binaries_relinked":[0-9]*' <<<"$line" | cut -d: -f2)"
        reason="$(grep -o '"reason":"[^"]*"' <<<"$line" | cut -d'"' -f4)"
        note="$(grep -o '"note":"[^"]*"' <<<"$line" | cut -d'"' -f4)"
        if [ "$state" = ok ]; then
            printf '  %-26s %-10s %7sms %7sms %7sms %5s%% %8s %4s\n' \
                "$name" "$state" "${lo:-?}" "${md:-?}" "${hi:-?}" "${sp:-?}" \
                "${obj:--}" "${bin:--}"
            # v2 detail line: only the pieces that were actually measured.
            local cpu sys rss l1 lk0 lk1 lf0 lf1 sha det=""
            cpu="$(grep -o '"cpu_user_s":[0-9.]*' <<<"$line" | cut -d: -f2)"
            sys="$(grep -o '"cpu_sys_s":[0-9.]*' <<<"$line" | cut -d: -f2)"
            rss="$(grep -o '"max_rss_kib":[0-9]*' <<<"$line" | cut -d: -f2)"
            l1="$(grep -o '"load1_start":"[^"]*"' <<<"$line" | cut -d'"' -f4)"
            lk0="$(grep -o '"lto_cache_kib_start":[0-9]*' <<<"$line" | cut -d: -f2)"
            lk1="$(grep -o '"lto_cache_kib_end":[0-9]*' <<<"$line" | cut -d: -f2)"
            lf0="$(grep -o '"lto_cache_files_start":[0-9]*' <<<"$line" | cut -d: -f2)"
            lf1="$(grep -o '"lto_cache_files_end":[0-9]*' <<<"$line" | cut -d: -f2)"
            sha="$(grep -o '"artifact_sha256":"[0-9a-f]*"' <<<"$line" | cut -d'"' -f4)"
            [ -n "$cpu" ] && det="cpu ${cpu}s user / ${sys:-?}s sys"
            [ -n "$rss" ] && det="${det:+$det, }rss $(( rss / 1024 )) MiB"
            [ -n "$l1" ] && det="${det:+$det, }load1 $l1"
            if [ -n "$lk0" ] && [ -n "$lk1" ]; then
                det="${det:+$det, }lto-cache ${lk0}->${lk1} KiB (delta $(( lk1 - lk0 ))), files ${lf0:-?}->${lf1:-?}"
            fi
            [ -n "$sha" ] && det="${det:+$det, }sha256 ${sha:0:12}…"
            [ -n "$det" ] && printf '      %s\n' "$det"
            # The single-sample caveat rides in the row's own note; printing it
            # here as well duplicated it on every one-sample row.
        else
            printf '  %-26s %-10s %s\n' "$name" "$state" "— no duration published"
            [ -n "$reason" ] && printf '      %s\n' "$reason"
        fi
        [ -n "$note" ] && [ "$state" = ok ] && printf '      %s\n' "$note"
    done <<<"$(grep -o '{"name":"[^"]*","state":[^}]*}' "$OUT")"
    echo
    echo "  Durations above are wall clock. Every one was produced by running the"
    echo "  command recorded beside it in $(rel "$OUT")."
}

# ── self-test ─────────────────────────────────────────────────────────────
# Runs the real edit/restore machinery with the build commands replaced by
# shell no-ops. That is the point: it proves the tree is restored byte-for-byte
# and proves that a build which recompiled nothing is refused as UNTRUSTED
# rather than published as a fast build.
run_selftest() {
    local tmp out self art
    SELFTEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/zcl-bb-selftest.XXXXXX")"
    trap 'rm -rf "${SELFTEST_TMP:-}"' EXIT HUP INT TERM
    tmp="$SELFTEST_TMP"
    self="${BASH_SOURCE[0]}"

    local h0 i0
    h0="$(sha256sum "$ROOT/$HDR" | awk '{print $1}')"
    i0="$(sha256sum "$ROOT/$IMPL" | awk '{print $1}')"

    # (1) A run whose builds are no-ops: the artifact is written, and every
    #     edit scenario reads UNTRUSTED with no duration.
    out="$(ZCL_BUILD_BENCH_FAKE=ok ZCL_TIMINGS_CACHE="$tmp/ok" bash "$self" --quick --samples=2 2>&1)"
    art="$tmp/ok/build-bench/last-run.json"
    [ -f "$art" ] ||
        { echo "build-bench selftest: FAIL — no artifact written: $out" >&2; exit 1; }
    grep -q '"schema":"zcl.build_bench.v2"' "$art" ||
        { echo "build-bench selftest: FAIL — wrong schema" >&2; exit 1; }
    grep -q '"name":"compile_edit_header","state":"UNTRUSTED"' "$art" ||
        { echo "build-bench selftest: FAIL — a build that recompiled nothing must read UNTRUSTED" >&2
          exit 1; }
    if grep -o '{"name":"compile_edit_header","state":"UNTRUSTED"[^}]*}' "$art" | grep -q 'median_ms'; then
        echo "build-bench selftest: FAIL — an UNTRUSTED scenario must publish no duration" >&2
        exit 1
    fi
    grep -q '"name":"compile_cold","state":"SKIPPED"' "$art" ||
        { echo "build-bench selftest: FAIL — quick mode must SKIP the cold build, not fake it" >&2
          exit 1; }
    grep -q '"name":"accel_cold_cache","state":"SKIPPED"' "$art" ||
        { echo "build-bench selftest: FAIL — quick mode must SKIP the accel cold build" >&2
          exit 1; }
    grep -q '"tree_restored":true' "$art" ||
        { echo "build-bench selftest: FAIL — the run did not certify the tree as restored" >&2
          exit 1; }
    grep -q '"including_files":[1-9]' "$art" ||
        { echo "build-bench selftest: FAIL — header fan-out was not derived" >&2; exit 1; }
    # v2 fields exist on every row (null where not measurable); the accel
    # verdict is UNKNOWN here because both accel scenarios were skipped.
    grep -q '"load1_start":' "$art" ||
        { echo "build-bench selftest: FAIL — v2 rows must carry load1_start" >&2; exit 1; }
    grep -q '"lto_cache_kib_start":' "$art" ||
        { echo "build-bench selftest: FAIL — v2 rows must carry lto_cache_kib_start" >&2; exit 1; }
    grep -q '"artifact_sha256":' "$art" ||
        { echo "build-bench selftest: FAIL — v2 rows must carry artifact_sha256" >&2; exit 1; }
    grep -q '"cpu_user_s":' "$art" ||
        { echo "build-bench selftest: FAIL — an ok row must carry the cpu/rss columns" >&2; exit 1; }
    grep -q '"accel_byte_identity":"UNKNOWN"' "$art" ||
        { echo "build-bench selftest: FAIL — skipped accel scenarios must verdict UNKNOWN" >&2
          exit 1; }

    # (2) The edited files are byte-identical to what they were before.
    [ "$(sha256sum "$ROOT/$HDR" | awk '{print $1}')" = "$h0" ] ||
        { echo "build-bench selftest: FAIL — $HDR was not restored" >&2; exit 1; }
    [ "$(sha256sum "$ROOT/$IMPL" | awk '{print $1}')" = "$i0" ] ||
        { echo "build-bench selftest: FAIL — $IMPL was not restored" >&2; exit 1; }

    # (3) A failing build is recorded as FAILED, publishes no duration, and
    #     makes the command itself exit nonzero.
    if ZCL_BUILD_BENCH_FAKE=fail ZCL_TIMINGS_CACHE="$tmp/bad" \
       bash "$self" --quick --samples=1 >/dev/null 2>&1; then
        echo "build-bench selftest: FAIL — a failing build exited 0" >&2; exit 1
    fi
    art="$tmp/bad/build-bench/last-run.json"
    grep -q '"state":"FAILED"' "$art" ||
        { echo "build-bench selftest: FAIL — a nonzero build was not recorded FAILED" >&2; exit 1; }
    if grep -o '{"name":"compile_noop","state":"FAILED"[^}]*}' "$art" | grep -q 'median_ms'; then
        echo "build-bench selftest: FAIL — a FAILED scenario must publish no duration" >&2
        exit 1
    fi
    [ "$(sha256sum "$ROOT/$HDR" | awk '{print $1}')" = "$h0" ] ||
        { echo "build-bench selftest: FAIL — $HDR was not restored after a failing run" >&2; exit 1; }

    # (4) With nothing on disk the reader says so instead of inventing a number.
    out="$(ZCL_TIMINGS_CACHE="$tmp/empty" bash "$self" --report 2>&1)"
    grep -q 'NOT MEASURED' <<<"$out" ||
        { echo "build-bench selftest: FAIL — an absent artifact must read NOT MEASURED: $out" >&2
          exit 1; }

    # (5) v2 unit checks, in-process against canned fixtures: the GNU time -v
    #     parser, the cache snapshot, the min_ms-sample aggregation rule, and
    #     the byte-identity verdict (PASS and FAIL).
    cat > "$tmp/timev.txt" <<'TIMEV'
	Command being timed: "make -j32 zclassic23"
	User time (seconds): 412.34
	System time (seconds): 56.78
	Elapsed (wall clock) time (h:mm:ss or m:ss): 8:01.23
	Maximum resident set size (kbytes): 7340032
	Exit status: 0
TIMEV
    parse_time_v "$tmp/timev.txt"
    [ "$LAST_CPU_USER" = 412.34 ] && [ "$LAST_CPU_SYS" = 56.78 ] && [ "$LAST_RSS_KIB" = 7340032 ] ||
        { echo "build-bench selftest: FAIL — time -v parsing: '$LAST_CPU_USER' '$LAST_CPU_SYS' '$LAST_RSS_KIB'" >&2
          exit 1; }
    parse_time_v "$tmp/does-not-exist"
    [ -z "$LAST_CPU_USER" ] && [ -z "$LAST_CPU_SYS" ] && [ -z "$LAST_RSS_KIB" ] ||
        { echo "build-bench selftest: FAIL — a missing time -v file must yield nulls" >&2; exit 1; }

    mkdir -p "$tmp/fakecache/sub"
    printf x > "$tmp/fakecache/f1"; printf y > "$tmp/fakecache/sub/f2"
    cache_snapshot "$tmp/fakecache"
    [ "$SNAP_FILES" = 2 ] && [ -n "$SNAP_KIB" ] ||
        { echo "build-bench selftest: FAIL — cache snapshot of a real dir: kib='$SNAP_KIB' files='$SNAP_FILES'" >&2
          exit 1; }
    cache_snapshot "$tmp/no-such-dir"
    [ -z "$SNAP_KIB" ] && [ -z "$SNAP_FILES" ] ||
        { echo "build-bench selftest: FAIL — an absent cache dir must snapshot to nulls" >&2; exit 1; }

    [ "$(pick_at_min_ms '100,50,80' 9 7 8)" = 7 ] ||
        { echo "build-bench selftest: FAIL — resource values must come from the min_ms sample" >&2
          exit 1; }
    [ "$(byte_identity_verdict abc123 abc123)" = PASS ] ||
        { echo "build-bench selftest: FAIL — identical hashes must verdict PASS" >&2; exit 1; }
    [ "$(byte_identity_verdict abc123 def456)" = FAIL ] ||
        { echo "build-bench selftest: FAIL — differing hashes must verdict FAIL" >&2; exit 1; }
    [ "$(byte_identity_verdict '' abc123)" = UNKNOWN ] ||
        { echo "build-bench selftest: FAIL — a missing hash must verdict UNKNOWN" >&2; exit 1; }

    echo "build-bench selftest: PASS"
}

# ── argv ──────────────────────────────────────────────────────────────────
DO_REPORT_ONLY=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --quick)      MODE=quick ;;
        --full)       MODE=full ;;
        --report)     DO_REPORT_ONLY=1 ;;
        --self-test)  run_selftest; exit 0 ;;
        --samples=*)  SAMPLES="${1#*=}" ;;
        --jobs=*)     JOBS="${1#*=}" ;;
        --group=*)    GROUP="${1#*=}" ;;
        '')           ;;
        *)            die "unknown argument: $1 (usage: $0 [--quick|--full] [--samples=N] [--jobs=N] [--group=NAME] [--report|--self-test])" ;;
    esac
    shift
done

case "$SAMPLES" in ''|*[!0-9]*) die "--samples must be a positive integer" ;; esac
[ "$SAMPLES" -ge 1 ] || die "--samples must be at least 1"

if [ "$DO_REPORT_ONLY" -eq 1 ]; then
    report
    exit 0
fi

measure
