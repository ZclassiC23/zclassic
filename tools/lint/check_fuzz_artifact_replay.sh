#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_fuzz_artifact_replay.sh — a saved fuzz finding that still reproduces
# fails the build. It is not allowed to just be a file.
#
# THE INCIDENT. On 2026-07-14 a fuzzer found that a five-byte script from any
# peer hangs the node forever. The bytes were committed as
# lib/test/fuzz_seeds/script/timeout-689f73ac89dc1264744711de8383742b90c892b0.bin
# and read by nobody for two weeks. It surfaced only because a human went
# looking. The galling part: THREE mechanisms had already replayed it and
# already gone red.
#
#   make fuzz-ci                     passes the seed dir to libFuzzer, so it
#                                    executes the whole corpus before mutating.
#                                    Exit 70, red since 2026-07-14 — but it is
#                                    reachable only from `make ci`, which no
#                                    hook, timer or workflow runs.
#   background_quality_lane.sh       hourly timer; it CAUGHT this, wrote the
#                                    stack trace to a log, and put its verdict
#                                    in a JSON file nothing gates on.
#   promote_fuzz_artifacts.sh        saw the same bytes a second time and
#                                    logged "SKIP duplicate", exit 0 by design.
#
# So the replay capability was never missing. The VERDICT ROUTING was. Nothing
# that could fail a build ever read the answer. This gate is that route.
#
# WHAT IT CHECKS. Every artifact-prefixed seed under lib/test/fuzz_seeds/
# (timeout- / crash- / leak- / oom- / slow-unit-, the prefixes libFuzzer writes)
# must carry exactly one verdict line in lib/test/fuzz_seeds/ARTIFACT_VERDICTS.txt,
# and that verdict must still be TRUE of what the artifact does today:
#
#   ledger says      replays clean            replays as hang/crash
#   ---------------  -----------------------  -------------------------------
#   regression-seed  pass                     FAIL — the bug came back
#   open             FAIL — reclassify it     FAIL — live bug, named + repro'd
#   accepted         FAIL — reclassify it     pass, reprinted loudly every run
#   (no line)        FAIL — untriaged         FAIL — untriaged
#
# Note what `open` does NOT do: it does not suppress anything. An unfixed bug
# stays red. The verdict only gives the redness a name, a date and an owner, so
# the next person reads a sentence instead of a hash. And an entry that has
# stopped being true fails in BOTH directions, so the ledger cannot rot into
# decoration the way a plain allowlist does.
#
# THE ONLY WAY TO PASS WHILE REPRODUCING is `accepted`: per-file, dated,
# reason >= 30 chars, and re-printed by name on every single run. There is no
# directory-wide exemption and no "skip" outcome anywhere in this script —
# a silent skip is precisely how the hole opened, so every path that cannot
# reach a verdict is a FAILURE instead. That includes a missing fuzz binary,
# a corpus with no binary behind it, and a binary with no corpus.
#
# TWO MODES, because the build dominates the cost by 4-20x:
#
#   --ledger-only   Text and git only, milliseconds. Every artifact has a live
#                   binary rule and a recorded verdict; no orphan lines; the
#                   corpus <-> binary mapping is 1:1. This is what `make lint`
#                   runs (gate: check-fuzz-artifact-ledger) — it keeps the
#                   inner loop honest without paying for a fuzz build.
#   (default)       Actually re-runs all of them. This is what `make fuzz-replay`
#                   runs, and it is wired into `make ci` and into its own CI job.
#                   Measured on the dev reference host with binaries prebuilt:
#                   22 artifacts in 18.3 s at -P6 (85.4 s serial). Building the
#                   nine sanitizer-instrumented binaries first is 5 min 45 s at
#                   -j6, which is why this is not folded into `make lint`.
#
#   --selftest      Plant / trip / recover proof that the gate still fires.
#
# REPLAY INVOCATION — three flags are load-bearing, each for a measured reason:
#   -timeout=5              libFuzzer's default is 1200 s. Without this a hang
#                           spins ~20 minutes instead of failing. 5 s is a ~70x
#                           margin over the slowest clean seed (73 ms).
#   ASAN_OPTIONS=symbolize=0
#                           With symbolization ON the process prints its verdict
#                           at t=5 s and then STALLS ~85 s inside llvm-symbolizer
#                           on a binary that large. That turns a 6 s gate into a
#                           91 s one, or into an outer wall-kill.
#   -artifact_prefix=$work/ libFuzzer writes a repro unit to CWD otherwise —
#                           which is how `make fuzz-ci` drops files into the
#                           repository root and trips check-no-stray-root-files.
#
# ANTI-FLAKE, DELIBERATELY. A unit that trips -timeout=5 and would thereby be a
# NEW accusation (the ledger says regression-seed, or there is no ledger line at
# all) is re-run once, serially, at ZCL_FUZZ_REPLAY_CONFIRM_TIMEOUT (default
# 15 s) before it is called reproducing. This exists because 8 of the 22
# artifacts in this corpus are NOT bugs: they are spurious -timeout=2 trips from
# a contended box, filed automatically with no reproduction check. A gate that
# cries wolf teaches people to ignore it, which is the same failure as silence.
# An artifact the ledger ALREADY records as reproducing is not re-confirmed —
# the result agrees with the record, and confirming all of them anyway cost
# 4 minutes instead of 18 seconds for no additional information.
#
# Mode: WARN | FAIL (ZCL_LINT_MODE; default FAIL).
set -euo pipefail

MODE="${ZCL_LINT_MODE:-FAIL}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"
# shellcheck source=tools/lint/gate_lib.sh
source "$SCRIPT_DIR/gate_lib.sh"

SEED_ROOT="lib/test/fuzz_seeds"
LEDGER="$SEED_ROOT/ARTIFACT_VERDICTS.txt"
GATE="check_fuzz_artifact_replay"

# libFuzzer's artifact filename prefixes. An artifact is a finding the fuzzer
# SAVED because it went wrong; a plain seed is hand-authored input. Only the
# former carries a verdict.
ARTIFACT_RE='^(timeout|crash|leak|oom|slow-unit)-'

REPLAY_TIMEOUT="${ZCL_FUZZ_REPLAY_TIMEOUT:-5}"
CONFIRM_TIMEOUT="${ZCL_FUZZ_REPLAY_CONFIRM_TIMEOUT:-15}"
JOBS="${ZCL_FUZZ_REPLAY_JOBS:-6}"
BIN_DIR="${ZCL_LINT_BIN_DIR:-build/bin}"

# Env equivalents of the two flags, so the C selftest registry in
# lib/test/src/test_make_lint_gates.c can drive this gate through
# run_gate_script(), which passes a mode env var and no argv. Same shape as
# ZCL_CONDITION_COOLDOWN_SELFTEST / ZCL_MARKDOWN_LINKS_SELFTEST.
LEDGER_ONLY="${ZCL_FUZZ_REPLAY_LEDGER_ONLY:-0}"
SELFTEST="${ZCL_FUZZ_REPLAY_SELFTEST:-0}"
ONLY_CORPORA=""
for arg in "$@"; do
    case "$arg" in
        --ledger-only) LEDGER_ONLY=1 ;;
        --selftest)    SELFTEST=1 ;;
        --corpus=*)    ONLY_CORPORA="${arg#--corpus=}" ;;
        -h|--help)
            echo "usage: check_fuzz_artifact_replay.sh [--ledger-only] [--selftest] [--corpus=a,b]"
            exit 0 ;;
        *) echo "$GATE: unknown arg '$arg'" >&2; exit 2 ;;
    esac
done

# One finding = one violation, however many lines it takes to explain itself.
# `report` opens a finding and counts it; `note` adds context to the finding
# already open. (Counting lines instead of findings reported "42 violations"
# for 14 broken artifacts, which is the kind of number that makes a red build
# look worse than it is and teaches people to discount it.)
violations=0
report() { violations=$((violations + 1)); echo "    $*" >&2; }
note()   { echo "      $*" >&2; }

# ── --selftest: prove the gate still FIRES ───────────────────────────────
# A gate that reports clean because it can no longer fail is the failure mode
# this whole file exists to prevent, so "clean" is only trustworthy after the
# detector has been shown to still trip. Plant an untriaged artifact into a
# real corpus dir, assert the ledger half rejects it AND names it, remove it,
# assert recovery. Uses a distinctive fixture name so a crashed run leaves
# something obvious rather than something that looks like a real finding.
if (( SELFTEST )); then
    # The child invocations below are plain --ledger-only runs. Clear the env
    # triggers first or a ZCL_FUZZ_REPLAY_SELFTEST=1 invocation re-enters the
    # selftest in every child, forever.
    unset ZCL_FUZZ_REPLAY_SELFTEST ZCL_FUZZ_REPLAY_LEDGER_ONLY
    fixture="$SEED_ROOT/block/crash-selftest-planted-not-a-real-finding.bin"
    rm -f "$fixture"
    if ! out="$("$0" --ledger-only 2>&1)"; then
        echo "$GATE --selftest: FAIL — the clean tree does not pass" >&2
        printf '%s\n' "$out" >&2
        exit 1
    fi
    printf '\x4e\xfb\xff\xff\xff' > "$fixture"
    trip_rc=0
    trip_out="$("$0" --ledger-only 2>&1)" || trip_rc=$?
    rm -f "$fixture"
    if [[ "$trip_rc" -eq 0 ]]; then
        echo "$GATE --selftest: FAIL — a planted untriaged artifact did NOT trip the gate" >&2
        exit 1
    fi
    if ! printf '%s' "$trip_out" | grep -qF "$fixture"; then
        echo "$GATE --selftest: FAIL — the gate tripped but never named $fixture" >&2
        echo "  Naming the exact file is the point: a red build must not send" >&2
        echo "  anyone hunting for which artifact broke." >&2
        printf '%s\n' "$trip_out" >&2
        exit 1
    fi
    if ! "$0" --ledger-only >/dev/null 2>&1; then
        echo "$GATE --selftest: FAIL — the gate stayed red after the fixture was removed" >&2
        exit 1
    fi
    echo "[$GATE] selftest: clean passes, planted artifact trips and is NAMED, removal recovers"
    exit 0
fi

# ── The corpus <-> binary map, DERIVED from the Makefile ─────────────────
# Never hand-written: a new fuzz target is covered the day its rule lands, and
# a corpus whose binary rule was renamed away becomes a failure instead of a
# quiet skip. The mapping the Makefile itself uses (fuzz-ci, Makefile:3932) is
# kind = basename minus "fuzz_", corpus dir = lib/test/fuzz_seeds/<kind>.
declare -A BIN_KIND=()
kind_count=0
while IFS= read -r k; do
    [[ -z "$k" ]] && continue
    BIN_KIND["$k"]=1
    kind_count=$((kind_count + 1))
done < <(gate_grep -oE '^\$\(BIN_DIR\)/fuzz_[a-z_0-9]+:' Makefile \
         | sed 's|^\$(BIN_DIR)/fuzz_||; s|:$||' | sort -u || true)

gate_require_scanned "$kind_count" 5 "$GATE" \
    "no \$(BIN_DIR)/fuzz_<kind> rules found in the Makefile — did the rule spelling change?"

# ── Load the ledger ──────────────────────────────────────────────────────
declare -A LEDGER_VERDICT=() LEDGER_DATE=() LEDGER_REASON=() LEDGER_SEEN=()
ledger_lines=0
if [[ ! -f "$LEDGER" ]]; then
    echo "$GATE: FATAL — $LEDGER is missing." >&2
    echo "  Every saved fuzz artifact needs a written verdict. Without the" >&2
    echo "  ledger this gate cannot tell a triaged finding from an unread one," >&2
    echo "  and reporting 'clean' off that is the exact hole it exists to close." >&2
    exit 2
fi
while IFS= read -r raw; do
    line="${raw%%#*}"
    reason="${raw#*#}"
    [[ "$raw" != *"#"* ]] && reason=""
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    [[ -z "$line" ]] && continue
    read -r f v d _rest <<< "$line"
    reason="${reason#"${reason%%[![:space:]]*}"}"
    if [[ -n "${LEDGER_VERDICT[$f]:-}" ]]; then
        report "$LEDGER: duplicate entry for '$f' — one line per artifact"
    fi
    LEDGER_VERDICT["$f"]="$v"
    LEDGER_DATE["$f"]="$d"
    LEDGER_REASON["$f"]="$reason"
    ledger_lines=$((ledger_lines + 1))
done < "$LEDGER"

# ── Enumerate the artifacts ──────────────────────────────────────────────
# Tracked AND untracked-but-present, because both are findings. A repro that a
# local fuzz run dropped into the corpus and nobody committed is the same hole
# in a smaller form — the bytes exist, they reproduce, and no build reads them.
# So an untracked artifact is enumerated, replayed, AND reported as its own
# violation: commit it with a verdict, or delete it.
artifacts=()
declare -A UNTRACKED=()
scanned=0

# git is the preferred enumerator because it can tell a committed finding from
# an uncommitted one. It is not always available: the lint-gate selftest runs
# every sandbox-lane gate inside a `cp -al` hardlink copy that deliberately
# omits .git, and a tarball checkout has none either. Fall back to `find` and
# SAY SO — the artifacts still all get replayed, only the tracked/untracked
# split goes unchecked. A silent degrade here would be the same class of
# problem as the hole this gate closes, so it is announced, not assumed.
if git rev-parse --git-dir >/dev/null 2>&1; then
    while IFS= read -r p; do
        [[ -z "$p" ]] && continue
        base="${p##*/}"
        [[ "$base" =~ $ARTIFACT_RE ]] || continue
        rel="${p#"$SEED_ROOT"/}"
        [[ "$rel" == */* ]] || continue        # corpus-dir-relative only
        artifacts+=("$rel")
        scanned=$((scanned + 1))
    done < <( { git ls-files "$SEED_ROOT" 2>/dev/null || true; } | sort -u )

    while IFS= read -r p; do
        [[ -z "$p" ]] && continue
        base="${p##*/}"
        [[ "$base" =~ $ARTIFACT_RE ]] || continue
        rel="${p#"$SEED_ROOT"/}"
        [[ "$rel" == */* ]] || continue
        artifacts+=("$rel")
        UNTRACKED["$rel"]=1
        scanned=$((scanned + 1))
    done < <( { git ls-files --others --exclude-standard "$SEED_ROOT" 2>/dev/null || true; } | sort -u )
else
    echo "[$GATE] no git here — enumerating with find; every artifact is still"
    echo "[$GATE] replayed and still needs a verdict, but the tracked vs"
    echo "[$GATE] uncommitted distinction is NOT being checked on this run."
    while IFS= read -r p; do
        [[ -z "$p" ]] && continue
        base="${p##*/}"
        [[ "$base" =~ $ARTIFACT_RE ]] || continue
        rel="${p#"$SEED_ROOT"/}"
        [[ "$rel" == */* ]] || continue
        artifacts+=("$rel")
        scanned=$((scanned + 1))
    done < <(find "$SEED_ROOT" -mindepth 2 -maxdepth 2 -type f 2>/dev/null | sort -u)
fi

gate_require_scanned "$scanned" 20 "$GATE" \
    "git ls-files found almost no saved artifacts under $SEED_ROOT — wrong cwd, or the corpus moved?"

echo "[$GATE] $scanned saved artifact(s); $ledger_lines ledger line(s); ${kind_count} fuzz target(s)"

# ── (1) Structural checks — the cheap half, always run ───────────────────
VALID_VERDICTS=" regression-seed open accepted "

for rel in "${artifacts[@]}"; do
    corpus="${rel%%/*}"
    LEDGER_SEEN["$rel"]=1

    if [[ -n "${UNTRACKED[$rel]:-}" ]]; then
        report "$SEED_ROOT/$rel [UNCOMMITTED — a saved repro sitting in the corpus, untracked]"
        note "A finding nobody committed is a finding nobody reads. Either"
        note "'git add' it and record a verdict in $LEDGER,"
        note "or delete it if it was local debris."
    fi

    if [[ -z "${BIN_KIND[$corpus]:-}" ]]; then
        report "$SEED_ROOT/$rel [no fuzz binary: the Makefile has no \$(BIN_DIR)/fuzz_$corpus rule]"
        note "An artifact nothing can replay is an artifact nobody will read."
        continue
    fi

    v="${LEDGER_VERDICT[$rel]:-}"
    if [[ -z "$v" ]]; then
        report "$SEED_ROOT/$rel [UNTRIAGED — no verdict recorded]"
        note "Reproduce it:  ASAN_OPTIONS=detect_leaks=0:symbolize=0 \\"
        note "  timeout -k 5 $((REPLAY_TIMEOUT + 10)) $BIN_DIR/fuzz_$corpus -timeout=$REPLAY_TIMEOUT \\"
        note "  -runs=1 -artifact_prefix=/tmp/ $SEED_ROOT/$rel"
        note "Then add one line to $LEDGER:"
        note "  $rel  <regression-seed|open|accepted>  $(date -u +%Y-%m-%d)  # why"
        continue
    fi
    case "$VALID_VERDICTS" in
        *" $v "*) ;;
        *)
            if [[ "$v" == "unaudited" ]]; then
                # Written by promote_fuzz_artifacts.sh when it files a new
                # artifact. Deliberately not a verdict: it is a placeholder that
                # keeps the build red until a human has actually replayed the
                # thing and decided something. Rejecting it here IS the feature.
                report "$SEED_ROOT/$rel [UNAUDITED — auto-filed on ${LEDGER_DATE[$rel]:-?}, nobody has triaged it]"
                note "Replay it:  ASAN_OPTIONS=detect_leaks=0:symbolize=0 \\"
                note "  timeout -k 5 $((REPLAY_TIMEOUT + 10)) $BIN_DIR/fuzz_$corpus -timeout=$REPLAY_TIMEOUT \\"
                note "  -runs=1 -artifact_prefix=/tmp/ $SEED_ROOT/$rel"
                note "Then replace its line in $LEDGER with a real"
                note "verdict: regression-seed (it is clean), open (it is a bug you"
                note "have not fixed), or accepted (it is genuinely not a bug, +why)."
            else
                report "$LEDGER: '$rel' has unknown verdict '$v' (want regression-seed | open | accepted)"
            fi ;;
    esac
    d="${LEDGER_DATE[$rel]:-}"
    [[ "$d" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]] || \
        report "$LEDGER: '$rel' has no valid YYYY-MM-DD date (got '$d')"
    r="${LEDGER_REASON[$rel]:-}"
    if [[ -z "$r" ]]; then
        report "$LEDGER: '$rel' has no reason — say what you decided and why"
    elif [[ "$v" == "accepted" && ${#r} -lt 30 ]]; then
        report "$LEDGER: '$rel' is 'accepted' with a ${#r}-char reason (need >= 30)"
        note "'accepted' is the ONLY verdict that lets a reproducing artifact"
        note "pass a build. It costs a real sentence. A bug you have not"
        note "fixed is 'open', not 'accepted'."
    fi
done

# Orphan ledger lines: an entry whose file is gone is a verdict about nothing,
# and it hides the fact that the finding left the corpus.
for f in "${!LEDGER_VERDICT[@]}"; do
    [[ -n "${LEDGER_SEEN[$f]:-}" ]] && continue
    report "$LEDGER: line for '$f' but no such tracked artifact — delete the line or restore the file"
done

# Every fuzz binary must own a corpus dir, and vice versa. This is the 9<->9
# invariant; a target with no corpus is a target whose findings have nowhere
# to land.
for k in "${!BIN_KIND[@]}"; do
    [[ -d "$SEED_ROOT/$k" ]] || \
        report "$SEED_ROOT/$k/ missing — \$(BIN_DIR)/fuzz_$k exists but has no corpus dir"
done
while IFS= read -r d; do
    [[ -z "$d" ]] && continue
    k="${d##*/}"
    [[ -n "${BIN_KIND[$k]:-}" ]] || \
        report "$SEED_ROOT/$k/ has no fuzz binary — add a \$(BIN_DIR)/fuzz_$k rule or remove the corpus"
done < <(find "$SEED_ROOT" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort)

if (( LEDGER_ONLY )); then
    echo "[$GATE] ledger-only: structure checked, no artifact replayed"
    echo "[$GATE] the replay itself is 'make fuzz-replay' (in make ci) — this half"
    echo "[$GATE] only proves every finding has a live binary and a written verdict."
    echo "[$GATE] $violations violation(s) found (mode: $MODE)"
    if (( violations > 0 )) && [[ "$MODE" == "FAIL" ]]; then exit 1; fi
    exit 0
fi

# ── (2) Replay — the half that catches a returning bug ───────────────────
work="$(mktemp -d "${TMPDIR:-/tmp}/zcl-fuzz-replay.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# One artifact -> "<rel> <state>" on stdout. state is clean | reproduces | nobin.
# Runs with CWD inside $work so nothing libFuzzer writes can reach the repo.
replay_one() {
    local rel="$1" tmo="$2"
    local corpus="${rel%%/*}"
    local bin="$ROOT/$BIN_DIR/fuzz_$corpus"
    if [[ ! -x "$bin" ]]; then echo "$rel nobin"; return 0; fi
    local rc=0
    ( cd "$work" && \
      ASAN_OPTIONS=detect_leaks=0:symbolize=0 \
      UBSAN_OPTIONS=print_stacktrace=0 \
      timeout -k 5 "$((tmo + 10))" "$bin" \
        -timeout="$tmo" -rss_limit_mb=2048 -runs=1 \
        -timeout_exitcode=70 -error_exitcode=77 \
        -artifact_prefix="$work/" \
        "$ROOT/$SEED_ROOT/$rel" >/dev/null 2>&1 ) || rc=$?
    if [[ "$rc" -eq 0 ]]; then echo "$rel clean"; else echo "$rel reproduces"; fi
}
export -f replay_one
export ROOT work BIN_DIR SEED_ROOT

selected=()
for rel in "${artifacts[@]}"; do
    if [[ -n "$ONLY_CORPORA" ]]; then
        case ",$ONLY_CORPORA," in *",${rel%%/*},"*) ;; *) continue ;; esac
    fi
    selected+=("$rel")
done

echo "[$GATE] replaying ${#selected[@]} artifact(s) at -timeout=${REPLAY_TIMEOUT}s, ${JOBS} at a time"

declare -A STATE=()
while read -r rel st; do
    [[ -z "$rel" ]] && continue
    STATE["$rel"]="$st"
done < <(printf '%s\n' "${selected[@]}" \
         | xargs -P "$JOBS" -I{} bash -c 'replay_one "$@"' _ {} "$REPLAY_TIMEOUT")

# Anti-flake confirmation: re-run ONCE, serially, on a longer clock — but ONLY
# where a hit would be a NEW accusation. Under -P6 on a busy box a genuinely
# fast unit can lose enough CPU to trip a 5 s wall, and that is exactly how the
# 8 noise artifacts in this corpus got filed in the first place; serial + 15 s
# removes that class before we call anything a regression.
#
# An artifact the ledger already records as reproducing (open / accepted) needs
# no second opinion: the result agrees with the record, and confirming it costs
# CONFIRM_TIMEOUT seconds each. Confirming all of them turned a ~20 s gate into
# a 4-minute one for no information.
for rel in "${selected[@]}"; do
    [[ "${STATE[$rel]:-}" == "reproduces" ]] || continue
    case "${LEDGER_VERDICT[$rel]:-}" in
        open|accepted) continue ;;   # expected to reproduce; nothing to confirm
    esac
    read -r _ st < <(replay_one "$rel" "$CONFIRM_TIMEOUT")
    STATE["$rel"]="$st"
    if [[ "$st" == "clean" ]]; then
        echo "[$GATE] $rel tripped ${REPLAY_TIMEOUT}s under load but is clean at" \
             "${CONFIRM_TIMEOUT}s serial — treated as load noise, not a finding"
    fi
done

n_clean=0; n_repro=0; accepted_live=()
for rel in "${selected[@]}"; do
    st="${STATE[$rel]:-}"
    v="${LEDGER_VERDICT[$rel]:-}"
    corpus="${rel%%/*}"
    repro_cmd="ASAN_OPTIONS=detect_leaks=0:symbolize=0 timeout -k 5 $((CONFIRM_TIMEOUT + 10)) $BIN_DIR/fuzz_$corpus -timeout=$CONFIRM_TIMEOUT -runs=1 -artifact_prefix=/tmp/ $SEED_ROOT/$rel"

    case "$st" in
    nobin)
        report "$SEED_ROOT/$rel [$BIN_DIR/fuzz_$corpus MISSING — cannot replay]"
        note "Build it:  make fuzz_$corpus"
        note "This is a FAILURE, not a skip: an artifact nobody replays is"
        note "an artifact nobody reads, which is the whole incident."
        ;;
    clean)
        n_clean=$((n_clean + 1))
        case "$v" in
        regression-seed) ;;   # the good case
        open)
            report "$SEED_ROOT/$rel [ledger says 'open' but it no longer reproduces]"
            note "Fixed? Then say so — reclassify to 'regression-seed' in"
            note "$LEDGER with today's date and the commit that fixed it."
            note "A stale 'open' is how a ledger rots into decoration." ;;
        accepted)
            report "$SEED_ROOT/$rel [ledger says 'accepted' but it no longer reproduces]"
            note "Reclassify to 'regression-seed' — an 'accepted' entry that"
            note "does nothing is an exemption nobody is checking." ;;
        esac ;;
    reproduces)
        n_repro=$((n_repro + 1))
        case "$v" in
        accepted)
            accepted_live+=("$rel") ;;
        regression-seed)
            report "$SEED_ROOT/$rel [REGRESSION — recorded clean on ${LEDGER_DATE[$rel]:-?}, hangs/crashes NOW]"
            note "This artifact was audited and passed. It does not any more."
            note "Reproduce it:  $repro_cmd" ;;
        open)
            report "$SEED_ROOT/$rel [LIVE — still reproduces; filed ${LEDGER_DATE[$rel]:-?}]"
            note "${LEDGER_REASON[$rel]:-}"
            note "Reproduce it:  $repro_cmd" ;;
        esac ;;
    esac
done

# Accepted-and-reproducing is the one pass-while-red path, so it is printed by
# name every single run. An exemption that stops being visible stops being a
# decision and becomes a habit.
if (( ${#accepted_live[@]} > 0 )); then
    echo "[$GATE] ${#accepted_live[@]} artifact(s) reproduce and are explicitly ACCEPTED:"
    for rel in "${accepted_live[@]}"; do
        echo "[$GATE]   $rel — ${LEDGER_REASON[$rel]:-}"
    done
fi

echo "[$GATE] replayed ${#selected[@]}: $n_clean clean, $n_repro reproducing"
echo "[$GATE] $violations violation(s) found (mode: $MODE)"
if (( violations > 0 )); then
    echo "[$GATE] A saved fuzz finding that still reproduces is a bug the node has"
    echo "[$GATE] TODAY, on a path a peer can reach. Fix it, or — if it is genuinely"
    echo "[$GATE] a non-bug — add a per-file 'accepted' line to $LEDGER"
    echo "[$GATE] saying why. There is no directory-wide exemption on purpose."
fi

if (( violations > 0 )) && [[ "$MODE" == "FAIL" ]]; then
    exit 1
fi
exit 0
