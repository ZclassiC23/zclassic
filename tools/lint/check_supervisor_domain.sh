#!/usr/bin/env bash
# Gate #21: production supervisor registration must specify a domain.

set -e

cd "$(dirname "$0")/../.."
# shellcheck source=tools/lint/gate_lib.sh
. tools/lint/gate_lib.sh
# shellcheck source=tools/lint/scan_exclusions.sh
. tools/lint/scan_exclusions.sh

# Scan roots are overridable via ZCL_SUPDOM_SCAN_ROOTS so the lint-gate
# self-test can point the gate at an EMPTY dir and prove the non-empty-floor
# preflight fires (exit 2). Production scans app/ lib/ config/.
read -r -a SUPDOM_ROOTS <<< "${ZCL_SUPDOM_SCAN_ROOTS:-app lib config}"

# Fail-loud preflight: the .c surface across the scan roots must be non-empty.
# A renamed/moved core dir would empty the grep surface; combined with the old
# `|| true` masking grep's exit, the gate would print PASS over zero files — a
# hollow pass. Assert a floor first.
mapfile -t supdom_files < <(find "${SUPDOM_ROOTS[@]}" -type f -name '*.c' 2>/dev/null)
gate_require_scanned "${#supdom_files[@]}" 1 check_supervisor_domain \
    "no *.c under: ${SUPDOM_ROOTS[*]}"

# First grep: 0=match, 1=no-match, >=2=real error. The old `... || true`
# masked >=2 (a non-GNU grep rejecting \s, an unreadable file) as "no hits"
# → PASS off a broken scan. Check the exit explicitly.
set +e
RAW=$(grep -rnE '(^|[^A-Za-z0-9_])supervisor_register\s*\(' "${SUPDOM_ROOTS[@]}" \
      --include='*.c' "${LINT_GREP_EXCLUDE_ARGS[@]}")
grc=$?
set -e
if [ "$grc" -ge 2 ]; then
    echo "check_supervisor_domain: FATAL — scan grep failed (exit $grc); refusing" >&2
    echo "  to report PASS off a broken scan." >&2
    exit 2
fi
HITS=$(printf '%s\n' "$RAW" \
    | grep -v 'supervisor_register_in_domain' \
    | grep -v 'lib/util/src/supervisor.c' \
    | grep -v 'lib/test/' \
    | grep -v '// supervisor-root-ok:' || true)
# The filter chain above may legitimately empty HITS (every hit was an
# in_domain call); printf of an empty RAW yields a blank line that the wc -l
# below already strips via sed '/^$/d'.

# Note: '%s\n' (not '%s') so a lone unterminated final hit is still counted
# by wc -l — otherwise a single violation undercounts to 0 and slips through.
COUNT=$(printf "%s\n" "$HITS" | sed '/^$/d' | wc -l)
MODE="${ZCL_LINT_MODE:-FAIL}"

if [ "$COUNT" -gt 0 ]; then
    printf "%s\n" "$HITS"
    echo "[check_supervisor_domain] $COUNT violation(s) (mode: $MODE)"
    if [ "$MODE" = "FAIL" ]; then exit 1; fi
fi

# ── Background-worker supervision (config/src boot worker TUs) ──
#
# The boot background workers (payment_processor, background_utxo_replay,
# address_backfill, projection_backfill, hodl_history — in
# boot_background_workers.c — and build_snapshot_offer in
# boot_snapshot_offer.c) keep their own pthread but must each gain a supervisor
# liveness contract so a wedged loop is not silent (Shape 5 — Supervisor,
# MONITOR / disk_monitor.c exemplar). This block widens Gate #21 to the
# boot worker file the same way Gate #15 (check_supervisor_registration.sh)
# guards app/services/src/*.c: any spawn — a raw pthread_create /
# thread_registry_spawn, OR a call to the shared thread-start wrapper
# boot_start_thread_service( (which a split-out worker TU uses instead of a
# raw spawn) — in a worker file must be paired with at least one
# supervisor_register_in_domain( call in that same file, OR the file must
# appear in tools/scripts/supervisor_baseline.txt (which is — and must stay
# — empty: register the workers, do not baseline them).
#
# Scan set is overridable via ZCL_SUPERVISOR_WORKER_FILES (space-separated
# paths) so the lint-gate self-test can point it at a planted fixture; it
# defaults to the production boot worker file.
WORKER_FILES="${ZCL_SUPERVISOR_WORKER_FILES:-config/src/boot_background_workers.c config/src/boot_snapshot_offer.c}"
BASELINE="tools/scripts/supervisor_baseline.txt"

worker_violations=""
for f in $WORKER_FILES; do
    [ -f "$f" ] || continue
    # Spawns a worker thread? — a raw pthread_create / thread_registry_spawn,
    # OR a call to the shared boot_start_thread_service( wrapper (a worker TU
    # split out of the boot file spawns through the wrapper, carrying no raw
    # token). A bare comment mention is fine because the gate only fails files
    # that ALSO lack a registration.
    if ! grep -qE 'pthread_create\s*\(|thread_registry_spawn|boot_start_thread_service\s*\(' "$f"; then
        continue
    fi
    # Paired with a domain registration? — a raw supervisor_register_in_domain,
    # OR a call to the shared boot_register_worker_supervisor( wrapper (which
    # init's the contract + registers it, the canonical boot-worker path). Pass.
    if grep -qE 'supervisor_register_in_domain\s*\(|boot_register_worker_supervisor\s*\(' "$f"; then
        continue
    fi
    # Grandfathered in the (intentionally empty) baseline? Pass.
    if [ -f "$BASELINE" ] && grep -qxF "$f" "$BASELINE"; then
        continue
    fi
    worker_violations="${worker_violations}${f}"$'\n'
done

WORKER_COUNT=$(printf "%s\n" "$worker_violations" | sed '/^$/d' | wc -l)
if [ "$WORKER_COUNT" -gt 0 ]; then
    printf "%s" "$worker_violations"
    echo "[check_supervisor_domain] $WORKER_COUNT background worker file(s) spawn threads without supervisor_register_in_domain (mode: $MODE)"
    echo "  Fix: in each boot_start_*_service, init a static liveness_contract"
    echo "  and call supervisor_register_in_domain(g_op_sup|g_chain_sup, &c)."
    echo "  See app/services/src/disk_monitor.c (dm_register_supervisor)."
    if [ "$MODE" = "FAIL" ]; then exit 1; fi
fi

echo "[check_supervisor_domain] PASS"
