#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# timings.sh — `make timings`. Reports where the wall time goes on THIS host,
# by reading the artifacts the tools already write. It measures nothing itself
# and it never carries a number forward from another machine or another commit.
#
# Sources (all machine-written, all gitignored):
#   .cache/lint-timing/last-run.json      written by tools/lint/run_lint.sh
#   .cache/test-timing/last-run.json      written by the parallel test runner
#   .cache/zcl-dev-loop/bench-latest.json written by tools/dev/dev-loop-bench.sh
#
# Four honesty rules, each of which exists because the failure it prevents is
# on disk right now:
#
#   NOT MEASURED  the artifact is absent. Print that, never a borrowed number.
#                 A fresh clone or a fresh worktree has no .cache at all, and a
#                 plausible-looking duration from somebody else's laptop is
#                 worse than no duration.
#   STALE         the artifact predates HEAD. The code moved after the
#                 measurement, so the measurement describes different code.
#   UNTRUSTED     the artifact recorded its own command as a shell no-op
#                 (":", "true", empty). Five of the six dev-loop bench cases
#                 are in exactly that state: they "passed" in ~3 ms because
#                 they ran `:`. Quoting 3 ms as a build time would be a lie the
#                 artifact itself tells. The reader refuses to repeat it.
#                 (The bench script is not the bug — it emits a real command
#                 for the case that has one. The reader is what needed fixing.)
#   PARTIAL       the run covered fewer units than the current suite defines,
#                 e.g. a lint artifact from a fast-subset run. The wall time is
#                 real but it is not the wall time of the full gate.
#
# Read-only. Writes nothing, measures nothing, never fails a build.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CACHE="${ZCL_TIMINGS_CACHE:-$ROOT/.cache}"

LINT_JSON="$CACHE/lint-timing/last-run.json"
TEST_JSON="$CACHE/test-timing/last-run.json"
BENCH_JSON="$CACHE/zcl-dev-loop/bench-latest.json"

# HEAD's committer time is the staleness reference: an artifact written before
# the newest tracked commit describes code that no longer exists.
head_epoch="$(git -C "$ROOT" log -1 --format=%ct 2>/dev/null || echo 0)"
head_desc="$(git -C "$ROOT" log -1 --format='%h %cI' 2>/dev/null || echo 'unknown')"

# --- tiny JSON field readers (no external deps; the writers are machine-made)
jnum() { grep -o "\"$2\"[[:space:]]*:[[:space:]]*[0-9]\+" "$1" 2>/dev/null | head -1 |
         grep -o '[0-9]\+$'; }
jstr() { grep -o "\"$2\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" "$1" 2>/dev/null | head -1 |
         sed 's/.*:[[:space:]]*"//; s/"$//'; }

is_noop_command() {
    case "$(printf '%s' "${1:-}" | tr -d '[:space:]')" in
        ''|':'|'true') return 0 ;;
        *) return 1 ;;
    esac
}

artifact_state() {   # -> FRESH | STALE ; echoes nothing when absent
    local f="$1" mtime
    mtime="$(stat -c %Y "$f" 2>/dev/null || echo 0)"
    if [ "$head_epoch" -gt 0 ] && [ "$mtime" -lt "$head_epoch" ]; then
        echo STALE
    else
        echo FRESH
    fi
}

not_measured() {     # $1 = label, $2 = path, $3 = how to produce it
    printf '  NOT MEASURED on this host — no %s\n' "${2#"$ROOT"/}"
    printf '                 produce it with: %s\n' "$3"
}

rel() { printf '%s' "${1#"$ROOT"/}"; }

# --------------------------------------------------------------------------
section_lint() {
    echo "lint  — make lint"
    if [ ! -f "$LINT_JSON" ]; then
        not_measured lint "$LINT_JSON" "make lint"; return
    fi
    local state wall jobs n stamp umbrella
    state="$(artifact_state "$LINT_JSON")"
    wall="$(jnum "$LINT_JSON" wall_ms)"
    jobs="$(jnum "$LINT_JSON" jobs)"
    n="$(jnum "$LINT_JSON" gate_count)"
    stamp="$(jstr "$LINT_JSON" generated_at_utc)"

    # The umbrella size is DERIVED from the Makefile at read time, so this
    # reader never carries a pinned gate count of its own.
    umbrella="$(awk '/^LINT_GATES[[:space:]]*:=/{i=1} i{print; if($0 !~ /\\[[:space:]]*$/) exit}' \
        "$ROOT/Makefile" 2>/dev/null | grep -oE 'check-[a-z0-9-]+' | sort -u | grep -c .)"

    printf '  %-6s %s ms wall over %s gate(s) at %s job(s)   [%s]\n' \
        "$state" "${wall:-?}" "${n:-?}" "${jobs:-?}" "${stamp:-no timestamp}"
    if [ "$state" = STALE ]; then
        printf '         measured before HEAD (%s) — re-run: make lint\n' "$head_desc"
    fi
    if [ -n "$n" ] && [ -n "$umbrella" ] && [ "$umbrella" -gt 0 ] && [ "$n" -lt "$umbrella" ]; then
        printf '  PARTIAL %s of the %s gates in the Makefile LINT_GATES umbrella ran.\n' \
            "$n" "$umbrella"
        printf '         This is a subset run (make lint-fast), not the full gate.\n'
    fi
    printf '         slowest gates: %s\n' \
        "$(grep -o '{"name":"[^"]*","ms":[0-9]*' "$LINT_JSON" 2>/dev/null | head -3 |
           sed 's/{"name":"//; s/","ms":/ /' | tr '\n' ',' | sed 's/,$//; s/,/, /g')"
    printf '         detail: %s\n' "$(rel "$LINT_JSON")"
}

section_tests() {
    echo
    echo "tests — make test-parallel"
    if [ ! -f "$TEST_JSON" ]; then
        not_measured tests "$TEST_JSON" "make test-parallel"; return
    fi
    local state wall jobs n failed skipped stamp
    state="$(artifact_state "$TEST_JSON")"
    wall="$(jnum "$TEST_JSON" wall_ms)"
    jobs="$(jnum "$TEST_JSON" jobs)"
    n="$(jnum "$TEST_JSON" group_count)"
    failed="$(jnum "$TEST_JSON" failed_count)"
    skipped="$(jnum "$TEST_JSON" skipped_count)"
    stamp="$(jstr "$TEST_JSON" generated_at_utc)"

    printf '  %-6s %s ms wall over %s group(s) at %s job(s), %s failed, %s skipped   [%s]\n' \
        "$state" "${wall:-?}" "${n:-?}" "${jobs:-?}" "${failed:-?}" "${skipped:-?}" \
        "${stamp:-no timestamp}"
    if [ "$state" = STALE ]; then
        printf '         measured before HEAD (%s) — re-run: make test-parallel\n' "$head_desc"
    fi
    printf '         slowest groups: %s\n' \
        "$(grep -o '{"name":"[^"]*","ms":[0-9]*' "$TEST_JSON" 2>/dev/null | head -3 |
           sed 's/{"name":"//; s/","ms":/ /' | tr '\n' ',' | sed 's/,$//; s/,/, /g')"
    printf '         detail: %s\n' "$(rel "$TEST_JSON")"
}

section_devloop() {
    echo
    echo "dev loop — make dev-loop-bench"
    if [ ! -f "$BENCH_JSON" ]; then
        not_measured "dev loop" "$BENCH_JSON" "make dev-loop-bench"; return
    fi
    local state stamp trusted=0 untrusted=0
    state="$(artifact_state "$BENCH_JSON")"
    stamp="$(jstr "$BENCH_JSON" generated_at_utc)"
    printf '  %-6s [%s]\n' "$state" "${stamp:-no timestamp}"
    if [ "$state" = STALE ]; then
        printf '         measured before HEAD (%s) — re-run: make dev-loop-bench\n' "$head_desc"
    fi

    # One case per line in the writer's format. A case whose recorded command
    # is a shell no-op gets its numbers WITHHELD, not printed with a caveat:
    # a caveated number still gets quoted downstream.
    while IFS= read -r line; do
        local name cmd p50 p95
        name="$(sed 's/.*"name":"\([^"]*\)".*/\1/' <<<"$line")"
        cmd="$(sed 's/.*"command":"\([^"]*\)".*/\1/' <<<"$line")"
        p50="$(grep -o '"p50_ms":[0-9]*' <<<"$line" | grep -o '[0-9]*$')"
        p95="$(grep -o '"p95_ms":[0-9]*' <<<"$line" | grep -o '[0-9]*$')"
        if is_noop_command "$cmd"; then
            untrusted=$((untrusted + 1))
            printf '  UNTRUSTED  %-16s recorded command was a shell no-op — no timing reported\n' \
                "$name"
        else
            trusted=$((trusted + 1))
            printf '  ok         %-16s p50 %s ms / p95 %s ms   (%s)\n' \
                "$name" "${p50:-?}" "${p95:-?}" "$cmd"
        fi
    done <<<"$(grep -o '{"name":"[^"]*"[^}]*"command":"[^"]*"[^}]*}' "$BENCH_JSON" 2>/dev/null)"

    if [ "$trusted" -eq 0 ]; then
        printf '         NOTHING TRUSTWORTHY here: %s of %s case(s) recorded a no-op command.\n' \
            "$untrusted" "$((trusted + untrusted))"
    fi
    printf '         detail: %s\n' "$(rel "$BENCH_JSON")"
}

# --------------------------------------------------------------------------
run_selftest() {
    local out
    SELFTEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/zcl-timings.XXXXXX")"
    trap 'rm -rf "${SELFTEST_TMP:-}"' EXIT HUP INT TERM
    local tmp="$SELFTEST_TMP"
    local self="${BASH_SOURCE[0]}"

    # (1) Nothing on disk -> NOT MEASURED for all three, never a number.
    out="$(ZCL_TIMINGS_CACHE="$tmp/empty" bash "$self" 2>&1)"
    [ "$(grep -c '^  NOT MEASURED' <<<"$out")" -eq 3 ] ||
        { echo "timings selftest: FAIL — absent artifacts must all read NOT MEASURED: $out" >&2
          exit 1; }

    # (2) An artifact older than HEAD reads STALE.
    mkdir -p "$tmp/old/lint-timing"
    printf '{"schema":"zcl.lint_timing.v1","generated_at_utc":"2000-01-01T00:00:00Z","wall_ms":1,"jobs":1,"gate_count":1,"gates":[{"name":"check-x","ms":1,"rc":0}]}\n' \
        > "$tmp/old/lint-timing/last-run.json"
    touch -d '2000-01-01 00:00:00' "$tmp/old/lint-timing/last-run.json"
    out="$(ZCL_TIMINGS_CACHE="$tmp/old" bash "$self" 2>&1)"
    grep -q 'STALE' <<<"$out" ||
        { echo "timings selftest: FAIL — pre-HEAD artifact must read STALE: $out" >&2; exit 1; }

    # (3) A no-op command is reported UNTRUSTED and its numbers are WITHHELD.
    mkdir -p "$tmp/noop/zcl-dev-loop"
    printf '{"schema":"zcl.dev_loop_bench.v1","generated_at_utc":"2099-01-01T00:00:00Z","cases":[\n{"name":"no_op","command":":","p50_ms":3,"p95_ms":3},\n{"name":"real_case","command":"make thing","p50_ms":4242,"p95_ms":4243}\n]}\n' \
        > "$tmp/noop/zcl-dev-loop/bench-latest.json"
    touch "$tmp/noop/zcl-dev-loop/bench-latest.json"
    out="$(ZCL_TIMINGS_CACHE="$tmp/noop" bash "$self" 2>&1)"
    grep -q 'UNTRUSTED  no_op' <<<"$out" ||
        { echo "timings selftest: FAIL — no-op command must read UNTRUSTED: $out" >&2; exit 1; }
    if grep -Eq 'no_op.*p50' <<<"$out"; then
        echo "timings selftest: FAIL — an UNTRUSTED case must not report a timing: $out" >&2
        exit 1
    fi
    grep -q 'real_case *p50 4242 ms' <<<"$out" ||
        { echo "timings selftest: FAIL — a real command must still report: $out" >&2; exit 1; }

    # (4) A PARTIAL lint run is labelled, not quoted as the full gate.
    mkdir -p "$tmp/partial/lint-timing"
    printf '{"schema":"zcl.lint_timing.v1","generated_at_utc":"2099-01-01T00:00:00Z","wall_ms":4090,"jobs":8,"gate_count":1,"gates":[{"name":"check-x","ms":1,"rc":0}]}\n' \
        > "$tmp/partial/lint-timing/last-run.json"
    touch "$tmp/partial/lint-timing/last-run.json"
    out="$(ZCL_TIMINGS_CACHE="$tmp/partial" bash "$self" 2>&1)"
    grep -q 'PARTIAL' <<<"$out" ||
        { echo "timings selftest: FAIL — subset lint run must read PARTIAL: $out" >&2; exit 1; }

    echo "timings selftest: PASS"
}

if [ "${1:-}" = "--self-test" ]; then
    run_selftest
    exit 0
fi

echo "══ timings: measured wall time on $(uname -n), HEAD $head_desc ══"
echo "Every number below was measured on THIS host. Absent -> NOT MEASURED."
echo "Older than HEAD -> STALE. Self-reported no-op command -> UNTRUSTED."
echo
section_lint
section_tests
section_devloop
echo
echo "Build wall time is not recorded by any artifact — time it directly:"
echo "  time make build-only     (per-TU compile, genuinely parallel under -j)"
echo "  time make -j\$(nproc)      (whole-program LTO link dominates)"
exit 0
