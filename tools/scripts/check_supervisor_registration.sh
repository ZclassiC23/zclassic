#!/usr/bin/env bash
# Lint gate #15 — supervisor registration for long-running services.
#
# Goal: every long-running service in `app/services/src/*.c`
# either registers a liveness contract with the supervisor
# (Round 5 — lib/util/supervisor.h), or appears in this gate's
# baseline file of grandfathered exceptions.
#
# Why: on 2026-05-21 the node ran for 8.6 h with `watchdog.checks_run`
# stuck at 0 because the lib/health sweeper wedged. The supervisor
# primitive (Round 5 C1) provides an independent time-driven driver,
# but only for services that opt in via supervisor_register_in_domain().
# gate is the ratchet that drives opt-in: new long-running services
# cannot land without a contract; baseline shrinks over Rounds 6-8.
#
# Scope: ALL app/services/src/*.c, not just *_service.c — a background
# daemon loop (disk_monitor, db_maintenance, mempool_limits, ...) is a
# long-running service whatever the filename suffix, and must be visible
# to `zcl_state subsystem=supervisor` so a wedged loop is not silent.
#
# A file is "long-running" if it contains either:
#   - thread_registry_spawn      (the project's long-running wrapper)
#   - health_register_periodic(  (lib/health sweeper subscriber)
#   - pthread_create(            (raw thread spawn) — EXCEPT when the
#     only such call is a short-burst worker carrying a `raw-pthread-ok`
#     marker (on the call line or the line above). Those are joined
#     within the spawning function, have bounded lifetime, and need no
#     liveness contract — mirrors the check-pthread-create Makefile gate.
#
# Such a file must contain ≥1 call to `supervisor_register_in_domain(`, OR an
# entry in `tools/scripts/supervisor_baseline.txt`, OR a per-file
# override marker `// supervisor-ok:<tag>` on a line in the file.
#
# To clean up debt: pick a baseline entry, register a liveness
# contract for that service (mirror what sync_watchdog_service.c
# does), delete the baseline line, re-run `make lint`.
set -euo pipefail

BASELINE=tools/scripts/supervisor_baseline.txt
[ -f "$BASELINE" ] || touch "$BASELINE"

declare -A baseline
baseline_count=0
while IFS= read -r line; do
    [[ -z "$line" || "$line" =~ ^[[:space:]]*# ]] && continue
    baseline["$line"]=1
    baseline_count=$((baseline_count + 1))
done < "$BASELINE"

# Decide whether $1 spawns a *long-running* (supervisable) thread.
# thread_registry_spawn / health_register_periodic always qualify. A
# raw pthread_create qualifies only when it is NOT a short-burst worker
# (i.e. NOT covered by a raw-pthread-ok marker on the call line or the
# line above) — same exemption the check-pthread-create gate uses.
file_is_long_running() {
    local f="$1"
    if grep -qE 'thread_registry_spawn|health_register_periodic\(' "$f"; then
        return 0
    fi
    # Any unmarked pthread_create line ⇒ long-running.
    grep -nE 'pthread_create\s*\(' "$f" | while IFS=: read -r n _; do
        if sed -n "${n}p" "$f" | grep -q 'raw-pthread-ok'; then
            continue
        fi
        prev=$((n - 1))
        if [ "$prev" -gt 0 ] && \
           sed -n "${prev}p" "$f" | grep -q 'raw-pthread-ok'; then
            continue
        fi
        echo "unmarked"
    done | grep -q unmarked
}

fail=0
new_violations=()
for f in app/services/src/*.c; do
    [ -f "$f" ] || continue
    # Long-running? Check for spawn / periodic-subscriber markers.
    file_is_long_running "$f" || continue
    # Already registered with the supervisor? Pass.
    if grep -qE 'supervisor_register(_in_domain)?\(' "$f"; then
        continue
    fi
    # Per-file override marker? Pass.
    if grep -qE '//[[:space:]]*supervisor-ok:[A-Za-z][A-Za-z0-9_-]*' "$f"; then
        continue
    fi
    # In baseline? Pass.
    if [ -n "${baseline[$f]+x}" ]; then
        continue
    fi
    new_violations+=("$f")
    fail=1
done

if [ "$fail" = "0" ]; then
    echo "check_supervisor_registration: clean — ${baseline_count} grandfathered, no new ones"
    exit 0
fi

echo ""
echo "check_supervisor_registration: ${#new_violations[@]} NEW long-running service(s) without supervisor_register_in_domain"
echo ""
for v in "${new_violations[@]}"; do
    echo "  $v"
done
echo ""
echo "Fix options (preferred → fallback):"
echo "  1. Add a liveness contract: declare struct liveness_contract,"
echo "     init it, register it. See app/services/src/sync_watchdog_service.c"
echo "     (g_wd_contract + supervisor_register) for the canonical pattern."
echo "  2. Add a per-file override marker '// supervisor-ok:<tag>' explaining"
echo "     why this service intentionally manages its own lifecycle."
echo "  3. As last resort, add the file to $BASELINE."
exit 1
