#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# run_lint.sh — timed, parallel driver for the `make lint` check-* umbrella.
#
# Why this exists: the umbrella grew to ~87 serial gates (measured p50 ~56 s
# wall on the dev reference host, 2026-07-18) with zero per-gate timing, and
# every gate paid a multi-second Make parse when run standalone. This driver
# execs each gate's script directly (same ZCL_LINT_PRODUCTION_SCAN=1 contract
# the Makefile `check-%` pattern rule sets), times every gate in ms, records
# results under .cache/lint-timing/ (dev-loop-bench artifact pattern), and
# runs independent gates concurrently.
#
# Parallel-safety contract (verified 2026-07-18): gates are read-only over
# the source tree in their default modes; ratchet baselines are written only
# under an explicit manual ZCL_LINT_MODE=UPDATE (never via `make lint`);
# every selftest-style gate works in a mktemp dir. The ONE exception runs in
# a SERIAL prologue before the parallel pool:
#   check-git-hooks-installed — may normalize core.hooksPath via `git config`
#     (a write; keep it away from concurrent git readers like check-core-seal).
# check-no-stray-untracked-source was also verified read-only; it no longer
# needs to run first because the driver never circuit-breaks on the first
# failure — all gates run and every failure is reported, strays included.
#
# Usage:
#   tools/lint/run_lint.sh [--jobs N] [--bin-dir DIR] [--list] GATE...
#   tools/lint/run_lint.sh --worker GATE        (internal: xargs child)
#
# Env:
#   ZCL_LINT_JOBS        default worker count when --jobs absent (default 8)
#   ZCL_LINT_BUDGET_SEC  soft wall-time budget, warn-only past it (default 75)
#   ZCL_LINT_TIMING_DIR  artifact dir (default .cache/lint-timing)
#   ZCL_LINT_VERBOSE=1   print the full per-gate timing table, not just top 10
#
# Exit: 0 iff every gate passed; 1 if any gate failed (all gates still run,
# so one `make lint` reports every violation); 2 on driver misuse/unknown gate.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

STATE_DIR="${ZCL_LINT_TIMING_DIR:-$ROOT/.cache/lint-timing}"
GATES_DIR="$STATE_DIR/gates"
JOBS="${ZCL_LINT_JOBS:-8}"
BIN_DIR="build/bin"
BUDGET_SEC="${ZCL_LINT_BUDGET_SEC:-75}"

# Gates that must run serially (and first) — see the header contract.
SERIAL_PROLOGUE=" check-git-hooks-installed "

# ── Gate invocation table ────────────────────────────────────────────────
# One entry per check-* gate in the Makefile lint umbrella (LINT_GATES). The
# command must reproduce the gate's Make recipe EXACTLY (script path, args,
# and any ZCL_LINT_MODE prefix). A gate added to LINT_GATES without a table
# entry is a LOUD driver error (exit 2), never a silent skip.
gate_command() {
    case "$1" in
        check-no-retired-agent-protocol)   echo './tools/lint/check_no_retired_agent_protocol.sh' ;;
        check-build-epoch-integrity)       echo 'tools/dev/build-epoch-integrity-cached.sh' ;;
        check-checkout-lock)               echo 'tools/dev/checkout-lock-selftest.sh' ;;
        check-no-stray-untracked-source)   echo './tools/lint/check_no_stray_untracked_source.sh' ;;
        check-scanner-immunity)            echo './tools/lint/selftest_scanner_immunity.sh' ;;
        check-git-hooks-installed)         echo './tools/scripts/check_git_hooks_installed.sh' ;;
        check-malloc)                      echo './tools/lint/check_malloc.sh' ;;
        check-hotswap-dev-only)            echo './tools/lint/check_hotswap_dev_only.sh' ;;
        check-hotswap-eligible-scope)      echo 'tools/lint/check_hotswap_eligible_scope.sh' ;;
        check-hotswap-static-state)        echo 'tools/lint/check_hotswap_static_state.sh' ;;
        check-hotswap-swappable-shape)     echo 'tools/lint/check_hotswap_swappable_shape.sh' ;;
        check-release-no-dev-symbols)      echo 'tools/lint/check_release_no_dev_symbols.sh' ;;
        check-stable-publish-contained)    echo 'bash tools/scripts/check_stable_publish_containment.sh --self-test && bash tools/scripts/check_stable_publish_containment.sh' ;;
        check-raw-sqlite)                  echo 'tools/scripts/check_raw_sqlite.sh' ;;
        check-raw-malloc)                  echo 'tools/scripts/check_raw_malloc.sh' ;;
        check-blob-read-bounds)            echo 'bash tools/lint/check_blob_read_bounds.sh' ;;
        check-coins-lookup-nullcheck)      echo 'tools/scripts/check_coins_lookup_nullcheck.sh' ;;
        check-observability-pairing)       echo '"$ZCL_LINT_BIN_DIR/check_observability_pairing"' ;;
        check-silent-errors-services)      echo './tools/lint/check_silent_error_returns.sh app/services/src services service "use LOG_ERR/LOG_FAIL/LOG_RETURN, prev-line error log, or mark // raw-return-ok:<reason>"' ;;
        check-silent-errors-controllers)   echo './tools/lint/check_silent_error_returns.sh app/controllers/src controllers controller "use LOG_ERR/LOG_RETURN, prev-line fprintf, or mark // raw-return-ok:<reason>"' ;;
        check-silent-errors-jobs)          echo './tools/lint/check_silent_error_returns.sh app/jobs/src jobs job "use LOG_ERR/LOG_FAIL/LOG_RETURN, prev-line error log, or mark // raw-return-ok:<reason>"' ;;
        check-silent-errors-conditions)    echo './tools/lint/check_silent_error_returns.sh app/conditions/src conditions condition "use LOG_ERR/LOG_FAIL/LOG_RETURN, prev-line error log, or mark // raw-return-ok:<reason>"' ;;
        check-silent-errors-bool)          echo 'ZCL_LINT_MODE=FAIL ./tools/lint/check_silent_bool_errors.sh' ;;
        check-log-macro-return-type)       echo './tools/lint/check_log_macro_return_type.sh' ;;
        check-wallet-raw-prepare-log)      echo 'ZCL_LINT_MODE=FAIL ./tools/lint/check_wallet_raw_prepare_log.sh' ;;
        check-before-save-hooks)           echo './tools/lint/check_before_save_hooks.sh' ;;
        check-pthread-create)              echo './tools/lint/check_pthread_create.sh' ;;
        check-model-validation)            echo './tools/scripts/check_model_validation.sh' ;;
        check-model-ar-lifecycle)          echo './tools/scripts/check_model_ar_lifecycle.sh' ;;
        check-long-functions)              echo './tools/scripts/check_long_functions.sh' ;;
        check-rpc-registrar)               echo './tools/scripts/check_rpc_registrar.sh' ;;
        check-lag-slo-observable)          echo './tools/scripts/check_lag_slo_observable.sh' ;;
        check-lib-layering)                echo './tools/scripts/check_lib_layering.sh' ;;
        check-domain-purity)               echo './tools/scripts/check_domain_purity.sh' ;;
        check-core-include-boundary)       echo './tools/scripts/check_core_include_boundary.sh' ;;
        check-core-seal)                   echo '__core_seal__' ;;
        check-supervisor-registration)     echo './tools/scripts/check_supervisor_registration.sh' ;;
        check-test-registration)           echo './tools/scripts/check_test_registration.sh' ;;
        check-typed-blocker)               echo './tools/scripts/check_typed_blocker.sh' ;;
        check-blocker-escape-registered)   echo './tools/scripts/check_blocker_escape_registered.sh' ;;
        check-blocker-remedy)              echo './tools/scripts/check_blocker_remedy.sh' ;;
        check-framework-shape)             echo 'ZCL_LINT_MODE=RATCHET ./tools/lint/framework_shape_check.sh' ;;
        check-framework-filename-suffix)   echo './tools/lint/check_framework_filename_suffix.sh' ;;
        check-no-raw-clock-outside-platform) echo './tools/lint/check_no_raw_clock_outside_platform.sh' ;;
        check-sysinit-ordering)            echo './tools/lint/check_sysinit_ordering.sh' ;;
        check-sandbox-wired)               echo './tools/lint/check_sandbox_wired.sh' ;;
        check-no-shellouts)                echo './tools/lint/check_no_shellouts.sh' ;;
        check-peer-floor-single-source)    echo './tools/lint/check_peer_floor_single_source.sh' ;;
        check-proc-self-shim)              echo './tools/lint/check_proc_self_shim.sh' ;;
        check-no-raw-sqlite-in-controllers) echo 'ZCL_LINT_MODE=RATCHET ./tools/lint/check_no_raw_sqlite_in_controllers.sh' ;;
        check-supervisor-domain)           echo './tools/lint/check_supervisor_domain.sh' ;;
        check-thread-supervision)          echo './tools/lint/check_thread_supervision.sh' ;;
        check-file-purpose)                echo 'ZCL_LINT_MODE=RATCHET ./tools/lint/check_file_purpose.sh' ;;
        check-group-purpose)               echo 'ZCL_LINT_MODE=FAIL ./tools/lint/check_group_purpose.sh' ;;
        check-no-orphan-placement)         echo 'ZCL_LINT_MODE=RATCHET ./tools/lint/check_no_orphan_placement.sh' ;;
        check-file-size-ceiling)           echo './tools/scripts/check_file_size_ceiling.sh' ;;
        check-operator-needed-sink)        echo './tools/scripts/check_operator_needed_sink.sh' ;;
        check-systemd-memory-budget)       echo './tools/scripts/check_systemd_memory_budget.sh' ;;
        check-condition-cooldown)          echo './tools/scripts/check_condition_cooldown.sh' ;;
        check-doc-accuracy)                echo './tools/scripts/check_doc_accuracy.sh' ;;
        check-doc-counts)                  echo './tools/scripts/check_doc_counts.sh' ;;
        check-no-stale-pinned-facts)       echo './tools/lint/check_no_stale_pinned_facts.sh' ;;
        check-markdown-links)              echo './tools/lint/check_markdown_links.sh .' ;;
        check-one-result-type)             echo './tools/scripts/check_one_result_type.sh' ;;
        check-service-result-convergence)  echo './tools/scripts/check_service_result_convergence.sh' ;;
        check-shape-includes-header)       echo './tools/scripts/check_shape_includes_header.sh' ;;
        check-projections-pure)            echo './tools/scripts/check_projections_pure.sh' ;;
        check-one-write-path)              echo './tools/scripts/check_one_write_path.sh' ;;
        check-no-authoritative-ram-state)  echo './tools/scripts/check_no_authoritative_ram_state.sh' ;;
        check-no-dev-history-in-contracts) echo './tools/scripts/check_no_dev_history_in_contracts.sh' ;;
        check-stage-advances-or-blocks)    echo './tools/scripts/check_stage_advances_or_blocks.sh' ;;
        check-no-silent-ready)             echo './tools/scripts/check_no_silent_ready.sh' ;;
        check-honest-witness)              echo 'ZCL_LINT_MODE=FAIL ./tools/lint/check_honest_witness.sh' ;;
        check-consensus-parity)            echo './tools/scripts/check_consensus_parity.sh' ;;
        check-no-new-repair-rung)          echo './tools/scripts/check_no_new_repair_rung.sh' ;;
        check-no-new-borrowed-seed)        echo './tools/lint/check_no_new_borrowed_seed.sh .' ;;
        check-no-new-coin-backfill-caller) echo './tools/lint/check_no_new_coin_backfill_caller.sh .' ;;
        check-route-command-parity)        echo './tools/lint/check_route_command_parity.sh .' ;;
        check-doc-no-false-deleted)        echo './tools/lint/gate_doc_no_false_deleted.sh .' ;;
        check-zclassicd-reach-allowlist)   echo './tools/lint/gate_zclassicd_reach_allowlist.sh .' ;;
        check-stage-log-reorg-unsafe)      echo './tools/scripts/gate_stage_log_reorg_unsafe_ratchet.sh' ;;
        check-no-csr-lock-on-finalize-drive) echo './tools/lint/gate_no_csr_lock_on_finalize_drive.sh .' ;;
        check-mint-skip-crypto-offline-only) echo './tools/lint/check_mint_skip_crypto_offline_only.sh .' ;;
        check-wire-harness-security-gate)  echo 'bash tools/scripts/check_wire_harness_security_gate.sh' ;;
        check-vcs-no-git)                  echo 'tools/scripts/check_vcs_no_git.sh' ;;
        check-vcs-no-sha1)                 echo 'tools/scripts/check_vcs_no_sha1.sh && tools/dev/source-identity-selftest.sh' ;;
        check-vendor-provenance)           echo 'tools/scripts/test_vendor_provenance.sh' ;;
        check-command-contract)            echo './tools/lint/check_command_contract.sh' ;;
        check-privileged-transition-receipt) echo './tools/lint/check_privileged_transition_receipt.sh' ;;
        check-no-trust-state-ordering)     echo './tools/scripts/check_no_trust_state_ordering.sh' ;;
        *) return 1 ;;
    esac
}

now_ms() {
    local ns
    ns="$(date +%s%N 2>/dev/null || true)"
    [[ "$ns" =~ ^[0-9]+$ ]] && printf '%s' $((ns / 1000000)) || printf '%s000' "$(date +%s)"
}

# Execute one gate's command with the same semantics as its Make recipe.
run_gate_body() {
    local gate="$1" cmd
    cmd="$(gate_command "$gate")" || return 127
    if [ "$cmd" = '__core_seal__' ]; then
        # Mirror the check-core-seal recipe: an unseal token lifts the HARD
        # seal failure for exactly that commit (owner unseal ritual).
        if [ -f .core-unseal-token ]; then
            echo "check-core-seal: unseal token present — seal check lifted for this commit"
            echo "  (owner unseal ritual active; re-run 'make core-seal' to refreeze before commit.)"
            git ls-files -z core/ | "$ZCL_LINT_BIN_DIR/core_seal" check core/MANIFEST.sha3 || true
        else
            git ls-files -z core/ | "$ZCL_LINT_BIN_DIR/core_seal" check core/MANIFEST.sha3
        fi
        return
    fi
    eval "$cmd"
}

# Worker: run one gate, capture log + ms + rc artifacts, print a one-line
# receipt. Line stays well under PIPE_BUF so concurrent workers never tear.
worker() {
    local gate="$1" start end ms rc log
    log="$GATES_DIR/$gate.log"
    start="$(now_ms)"
    run_gate_body "$gate" > "$log" 2>&1
    rc=$?
    end="$(now_ms)"
    ms=$((end - start))
    printf '%s\n' "$ms" > "$GATES_DIR/$gate.ms"
    printf '%s\n' "$rc" > "$GATES_DIR/$gate.rc"
    if [ "$rc" -eq 0 ]; then
        printf 'PASS %-42s %7s ms\n' "$gate" "$ms"
    else
        printf 'FAIL %-42s %7s ms (rc=%s)\n' "$gate" "$ms" "$rc"
    fi
    return 0
}

usage() {
    sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
}

main() {
    local -a gates=()
    while [ $# -gt 0 ]; do
        case "$1" in
            --jobs)    JOBS="${2:?--jobs needs N}"; shift 2 ;;
            --jobs=*)  JOBS="${1#--jobs=}"; shift ;;
            --bin-dir) BIN_DIR="${2:?--bin-dir needs a dir}"; shift 2 ;;
            --bin-dir=*) BIN_DIR="${1#--bin-dir=}"; shift ;;
            --worker)  worker "$2"; exit $? ;;
            --list)
                # Gate names = the case labels in gate_command() (read from
                # this file so reformatting cannot desync the probe).
                grep -oE '\bcheck-[a-z0-9-]+\)' "$0" | tr -d ')' | sort -u
                exit 0 ;;
            --help|-h) usage; exit 0 ;;
            check-*)   gates+=("$1"); shift ;;
            *) echo "run_lint.sh: unknown argument '$1'" >&2; exit 2 ;;
        esac
    done
    [[ "$JOBS" =~ ^[0-9]+$ ]] && [ "$JOBS" -ge 1 ] || {
        echo "run_lint.sh: --jobs must be a positive integer (got '$JOBS')" >&2; exit 2; }
    [ "$JOBS" -le 32 ] || JOBS=32
    [ "${#gates[@]}" -gt 0 ] || { echo "run_lint.sh: no gates given" >&2; exit 2; }

    # Fail loud on table drift: every requested gate needs an entry, and any
    # table gate absent from the request is named (nonfatal) so the Makefile
    # umbrella and this table cannot silently diverge.
    local g missing=0
    for g in "${gates[@]}"; do
        if ! gate_command "$g" >/dev/null; then
            echo "run_lint.sh: FATAL — gate '$g' has no invocation-table entry." >&2
            echo "  Add its exact recipe command to gate_command() in $0" >&2
            missing=1
        fi
    done
    [ "$missing" -eq 0 ] || exit 2
    local requested=" ${gates[*]} " extra
    while read -r extra; do
        [ -n "$extra" ] || continue
        case "$requested" in
            *" $extra "*) ;;
            *) echo "run_lint.sh: note — table gate '$extra' not in this run's gate list" >&2 ;;
        esac
    done < <("$0" --list 2>/dev/null | grep '^check-' || true)

    mkdir -p "$GATES_DIR"
    rm -f "$GATES_DIR"/*.log "$GATES_DIR"/*.ms "$GATES_DIR"/*.rc 2>/dev/null

    # Same scan-exclusion contract as the Makefile `check-%` pattern rule.
    export ZCL_LINT_PRODUCTION_SCAN=1
    export ZCL_LINT_BIN_DIR="$BIN_DIR"
    [[ "$BUDGET_SEC" =~ ^[0-9]+$ ]] || BUDGET_SEC=75

    local -a serial_gates=() par_gates=()
    for g in "${gates[@]}"; do
        case "$SERIAL_PROLOGUE" in
            *" $g "*) serial_gates+=("$g") ;;
            *)        par_gates+=("$g") ;;
        esac
    done

    local run_start run_end wall_ms
    run_start="$(now_ms)"
    for g in "${serial_gates[@]}"; do
        worker "$g"
    done
    if [ "${#par_gates[@]}" -gt 0 ]; then
        printf '%s\n' "${par_gates[@]}" | \
            xargs -r -P "$JOBS" -n1 "$0" --worker
    fi
    run_end="$(now_ms)"
    wall_ms=$((run_end - run_start))

    # Aggregate. A gate that produced no .rc file crashed the worker itself.
    local -a failed=()
    local total=0 rc ms
    for g in "${gates[@]}"; do
        total=$((total + 1))
        if [ ! -f "$GATES_DIR/$g.rc" ]; then
            failed+=("$g (worker crash — no rc artifact)")
            continue
        fi
        rc="$(cat "$GATES_DIR/$g.rc")"
        [ "$rc" = "0" ] || failed+=("$g")
    done

    # Slowest gates first (per-gate ms artifacts make regressions visible).
    local ranked
    ranked="$(for f in "$GATES_DIR"/*.ms; do
        [ -f "$f" ] || continue
        printf '%s %s\n' "$(cat "$f")" "$(basename "$f" .ms)"
    done | sort -nr)"
    echo "── lint timing: ${total} gates, wall ${wall_ms} ms, jobs ${JOBS} ──"
    local show=10 line
    [ "${ZCL_LINT_VERBOSE:-0}" = "1" ] && show="$total"
    printf '%s\n' "$ranked" | head -n "$show" | while read -r ms g; do
        [ -n "$g" ] && printf '  %7s ms  %s\n' "$ms" "$g"
    done
    [ "$show" -ge "$total" ] || echo "  … (ZCL_LINT_VERBOSE=1 for the full table)"

    # Machine-readable artifact (dev-loop-bench pattern).
    {
        printf '{\n'
        printf '  "schema":"zcl.lint_timing.v1",\n'
        printf '  "generated_at_utc":"%s",\n' "$(date -u +%FT%TZ)"
        printf '  "wall_ms":%s,\n' "$wall_ms"
        printf '  "jobs":%s,\n' "$JOBS"
        printf '  "budget_sec":%s,\n' "$BUDGET_SEC"
        printf '  "gate_count":%s,\n' "$total"
        printf '  "failed_count":%s,\n' "${#failed[@]}"
        printf '  "gates":[\n'
        local first=1
        printf '%s\n' "$ranked" | while read -r ms g; do
            [ -n "$g" ] || continue
            [ "$first" -eq 0 ] && printf ',\n'
            first=0
            printf '    {"name":"%s","ms":%s,"rc":%s}' \
                "$g" "$ms" "$(cat "$GATES_DIR/$g.rc" 2>/dev/null || echo 2)"
        done
        printf '\n  ]\n}\n'
    } > "$STATE_DIR/last-run.json.tmp" && mv -f "$STATE_DIR/last-run.json.tmp" "$STATE_DIR/last-run.json"

    if [ "$wall_ms" -gt $((BUDGET_SEC * 1000)) ]; then
        echo "run_lint.sh: NOTE — wall ${wall_ms} ms exceeds the ${BUDGET_SEC}s soft budget" \
            "(ZCL_LINT_BUDGET_SEC; see the budget comment above the lint target)" >&2
    fi

    if [ "${#failed[@]}" -gt 0 ]; then
        echo "" >&2
        echo "══ LINT: ${#failed[@]} of ${total} gates FAILED ══" >&2
        for g in "${failed[@]}"; do
            echo "  ✗ $g" >&2
        done
        for g in "${failed[@]}"; do
            local name="${g%% *}"
            if [ -f "$GATES_DIR/$name.log" ]; then
                echo "" >&2
                echo "──── FAIL log: $name ────────────────────────────" >&2
                cat "$GATES_DIR/$name.log" >&2
            fi
        done
        exit 1
    fi
    exit 0
}

main "$@"
