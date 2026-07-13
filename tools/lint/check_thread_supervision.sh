#!/usr/bin/env bash
# Gate #23 — universal thread supervision (RATCHET).
#
# Every long-running background thread the node spawns must be ACCOUNTED FOR:
# either it is on the supervisor liveness tree (so a wedged loop becomes a
# named blocker, not a silent stop — the 2026-05-21 8.6 h sweeper wedge that
# motivated Round 5), or it is a documented exemption in the baseline.
#
# Seam: the canonical long-running spawn wrapper `thread_registry_spawn(`
# (raw pthread_create is separately gated by the Makefile `check-pthread-create`
# target, which forces the wrapper or a `// raw-pthread-ok` marker; those are
# short-burst/joined workers, out of scope here). This gate scans every
# thread_registry_spawn call site under the production roots and requires each
# to be one of:
#
#   (a) SUPERVISED — its translation unit registers a liveness contract
#       (`supervisor_register(`, `supervisor_register_in_domain(`, or the
#       lib/util adapter `thread_liveness_register(`), OR the spawn call line
#       (or the line above) carries a `// supervised:<child>` marker.
#   (b) MARKED-EXEMPT — the spawn call line (or the line above) carries a
#       `// thread-supervision-ok:<reason>` marker (a bounded/joined worker
#       pool, a one-shot job, etc.).
#   (c) BASELINED — the thread's name literal appears in the baseline file
#       with a disposition + one-line justification.
#
# Anything else is a NEW unaccounted long-running thread → FAIL.
#
# Ratchet: the baseline may only SHRINK. A baseline entry whose thread no
# longer spawns as an uncovered site (renamed, removed, or since supervised)
# is a STALE entry → FAIL, which forces the list down as threads gain
# contracts. A thread that is both covered AND baselined → FAIL (remove the
# now-redundant baseline line).
#
# To clean up debt: pick a baselined thread, give it a liveness contract via
# util/thread_liveness.h (see lib/health/src/heartbeat.c for the exemplar),
# delete its baseline line, re-run `make lint`.
set -euo pipefail

cd "$(dirname "$0")/../.."
# shellcheck source=tools/lint/gate_lib.sh
. tools/lint/gate_lib.sh

# Scan roots + baseline are overridable so the lint-gate self-test can point
# the gate at a planted fixture dir / empty baseline (and prove the
# non-empty-scan floor + trip/pass behavior) without touching the live tree.
read -r -a ROOTS <<< "${ZCL_THREADSUP_SCAN_ROOTS:-app lib config}"
BASELINE="${ZCL_THREADSUP_BASELINE:-tools/lint/thread_supervision_baseline.txt}"
[ -f "$BASELINE" ] || touch "$BASELINE"

MARKER_RE='//[[:space:]]*(supervised|thread-supervision-ok):'
COVER_RE='supervisor_register[[:space:]]*\(|supervisor_register_in_domain[[:space:]]*\(|thread_liveness_register[[:space:]]*\('
SPAWN_RE='thread_registry_spawn[[:space:]]*\('

# Production .c surface. Exclude the test/fuzz/vendor trees, the two seam
# files (the registry itself and the supervisor itself — the supervisor thread
# is the root and cannot supervise itself), and the repo-scan-excluded
# `_*fixture*tmp*.c` transient fixtures (the self-test plants `_*probe_tmp.c`
# instead, which IS scanned).
mapfile -t files < <(find "${ROOTS[@]}" -type f -name '*.c' 2>/dev/null \
    | grep -v '/test/' \
    | grep -v -i 'fuzz' \
    | grep -v '/vendor/' \
    | grep -v 'lib/util/src/thread_registry.c' \
    | grep -v 'lib/util/src/supervisor.c' \
    | grep -Ev '_[^/]*fixture[^/]*tmp[^/]*\.c$' \
    | sort)
gate_require_scanned "${#files[@]}" 1 check_thread_supervision \
    "no *.c under: ${ROOTS[*]} — was a production dir renamed/moved?"

# Load baseline: "name  disposition  justification…". First token = thread name.
declare -A baseline
baseline_count=0
while read -r name _rest; do
    [[ -z "$name" || "$name" == \#* ]] && continue
    baseline["$name"]=1
    baseline_count=$((baseline_count + 1))
done < "$BASELINE"

# Emit one TAB record per spawn site: file<TAB>line<TAB>covered<TAB>marked<TAB>name
records=$(
    for f in "${files[@]}"; do
        cov=0
        if grep -qE "$COVER_RE" "$f"; then cov=1; fi
        while IFS=: read -r n line; do
            marked=0
            if printf '%s\n' "$line" | grep -qE "$MARKER_RE"; then
                marked=1
            else
                prev=$((n - 1))
                if [ "$prev" -gt 0 ] && \
                   sed -n "${prev}p" "$f" | grep -qE "$MARKER_RE"; then
                    marked=1
                fi
            fi
            name=$(printf '%s\n' "$line" | \
                sed -n 's/.*thread_registry_spawn[[:space:]]*([[:space:]]*"\([^"]*\)".*/\1/p')
            printf '%s\t%s\t%s\t%s\t%s\n' "$f" "$n" "$cov" "$marked" "$name"
        done < <(grep -nE "$SPAWN_RE" "$f")
    done
)

fail=0
new_violations=()
redundant_baseline=()
declare -A baseline_hit
declare -A baseline_appeared

if [ -n "$records" ]; then
    while IFS=$'\t' read -r f n cov marked name; do
        [ -z "$f" ] && continue
        if [ -n "$name" ] && [ -n "${baseline[$name]+x}" ]; then
            baseline_appeared[$name]=1
        fi
        if [ "$cov" = 1 ] || [ "$marked" = 1 ]; then
            # Supervised or explicitly marked-exempt. If ALSO baselined, the
            # baseline line is now redundant and must be removed (shrink).
            if [ -n "$name" ] && [ -n "${baseline[$name]+x}" ]; then
                redundant_baseline+=("$name ($f:$n is now covered)")
                fail=1
            fi
            continue
        fi
        if [ -n "$name" ] && [ -n "${baseline[$name]+x}" ]; then
            baseline_hit[$name]=1
            continue
        fi
        new_violations+=("$f:$n  thread='${name:-<dynamic-name>}'")
        fail=1
    done <<< "$records"
fi

# Stale baseline entries: listed but never seen as an uncovered spawn.
stale_baseline=()
for name in "${!baseline[@]}"; do
    [ -n "${baseline_hit[$name]+x}" ] && continue
    [ -n "${baseline_appeared[$name]+x}" ] && continue  # flagged as redundant
    stale_baseline+=("$name")
    fail=1
done

if [ "$fail" = "0" ]; then
    echo "check_thread_supervision: clean — ${baseline_count} baselined exemption(s), no new unaccounted threads"
    exit 0
fi

echo ""
echo "check_thread_supervision: FAIL"
if [ "${#new_violations[@]}" -gt 0 ]; then
    echo ""
    echo "  NEW unaccounted long-running thread(s) (${#new_violations[@]}):"
    for v in "${new_violations[@]}"; do echo "    $v"; done
    echo ""
    echo "  Fix (preferred → fallback):"
    echo "    1. Supervise it: register a liveness contract via"
    echo "       util/thread_liveness.h (exemplar: lib/health/src/heartbeat.c),"
    echo "       or add '// supervised:<child>' at the spawn site."
    echo "    2. If short-lived/joined/one-shot: add"
    echo "       '// thread-supervision-ok:<reason>' at the spawn site."
    echo "    3. Last resort: add '<name>  <disposition>  <why>' to $BASELINE."
fi
if [ "${#redundant_baseline[@]}" -gt 0 ]; then
    echo ""
    echo "  REDUNDANT baseline entries (thread is now covered — remove them):"
    for v in "${redundant_baseline[@]}"; do echo "    $v"; done
fi
if [ "${#stale_baseline[@]}" -gt 0 ]; then
    echo ""
    echo "  STALE baseline entries (no matching uncovered spawn — remove them):"
    for v in "${stale_baseline[@]}"; do echo "    $v"; done
fi
echo ""
exit 1
