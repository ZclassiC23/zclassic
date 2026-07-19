#!/usr/bin/env bash
# Gate P2 (palace-design.md §3) — check-group-purpose: every navigator group
# node emitted by ci_group_emit_all() (lib/codeindex/src/codeindex_group.c)
# must resolve to a non-empty ci_group_purpose() string, so `code group`
# always tells an agent what a module/shape/context IS without opening it.
#
# Mode: WARN | RATCHET | FAIL (controlled by ZCL_LINT_MODE; default WARN),
# modeled on tools/lint/framework_shape_check.sh (Gate #18):
#   WARN    — report violations, always exit 0 (measurement).
#   RATCHET — fail only on violations NOT in group_purpose_baseline.txt.
#   FAIL    — fail on ANY violation, baseline ignored.
# The group population is finite (~35 lib modules + 8 app shapes + 2 domain
# contexts + 9 fixed roots) and is filled in full alongside this gate, so it
# is wired FAIL in the Makefile from day one — no long ratchet needed.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"
# shellcheck source=tools/lint/gate_lib.sh
source "$SCRIPT_DIR/gate_lib.sh"

MODE="${ZCL_LINT_MODE:-WARN}"
# ZCL_GROUP_PURPOSE_SRC overrides the scanned source file — test isolation
# only (test_make_lint_gates points this at a fixture copy so the self-test
# never mutates the real codeindex_group.c). Unset in production.
SRC="${ZCL_GROUP_PURPOSE_SRC:-lib/codeindex/src/codeindex_group.c}"
BASELINE="$SCRIPT_DIR/group_purpose_baseline.txt"

[[ -f "$SRC" ]] || { echo "check_group_purpose: FATAL — missing $SRC" >&2; exit 2; }

# Extract the quoted string tokens inside a `static const char *const NAME[] =
# { ... };` C array literal. Robust to line-wrapping (the Makefile-mirror
# arrays wrap across lines) and to trailing commas.
extract_array() {
    local name="$1"
    awk -v name="$name" '
        $0 ~ "static const char \\*const " name "\\[\\] = \\{" { in_arr = 1 }
        in_arr {
            line = $0
            while (match(line, /"[^"]*"/)) {
                tok = substr(line, RSTART + 1, RLENGTH - 2)
                print tok
                line = substr(line, RSTART + RLENGTH)
            }
        }
        in_arr && /\};/ { in_arr = 0 }
    ' "$SRC"
}

mapfile -t lib_modules < <(extract_array "k_lib_modules")
mapfile -t app_shapes  < <(extract_array "k_app_shapes")
mapfile -t domain_ctxs < <(extract_array "k_domain_contexts")

gate_require_scanned "${#lib_modules[@]}" 1 check-group-purpose \
    "k_lib_modules[] parse came back empty — codeindex_group.c layout changed?"
gate_require_scanned "${#app_shapes[@]}" 1 check-group-purpose \
    "k_app_shapes[] parse came back empty — codeindex_group.c layout changed?"
gate_require_scanned "${#domain_ctxs[@]}" 1 check-group-purpose \
    "k_domain_contexts[] parse came back empty — codeindex_group.c layout changed?"

# Cross-check k_domain_contexts against the Makefile's DOMAIN_CONTEXTS (the
# comment above k_domain_contexts[] promises this; lib/app already have a
# parity test in test_codeindex.c case 6 so are not re-checked here).
mk_domain_line="$(grep -E '^DOMAIN_CONTEXTS[[:space:]]*=' Makefile || true)"
if [[ -z "$mk_domain_line" ]]; then
    echo "check_group_purpose: FATAL — Makefile has no DOMAIN_CONTEXTS line" >&2
    exit 2
fi
mk_domain_line="${mk_domain_line#*=}"
read -r -a mk_domains <<< "$mk_domain_line"
domain_mismatch=0
for c in "${mk_domains[@]}"; do
    hit=0
    for d in "${domain_ctxs[@]}"; do [[ "$c" == "$d" ]] && hit=1 && break; done
    if [[ "$hit" -eq 0 ]]; then
        echo "check_group_purpose: Makefile DOMAIN_CONTEXTS has '$c' but k_domain_contexts[] does not" >&2
        domain_mismatch=1
    fi
done
for d in "${domain_ctxs[@]}"; do
    hit=0
    for c in "${mk_domains[@]}"; do [[ "$c" == "$d" ]] && hit=1 && break; done
    if [[ "$hit" -eq 0 ]]; then
        echo "check_group_purpose: k_domain_contexts[] has '$d' but Makefile DOMAIN_CONTEXTS does not" >&2
        domain_mismatch=1
    fi
done

# The fixed top-level roots ci_group_emit_all() always writes (see
# codeindex_group.c: emit(s, "root"|"lib"|"app"|"core"|"config"|"tools"|
# "domain"|"adapters"|"ports", ...)).
fixed_roots=(root lib app core config tools domain adapters ports)

groups=("${fixed_roots[@]}")
for m in "${lib_modules[@]}"; do groups+=("lib/$m"); done
for s in "${app_shapes[@]}"; do groups+=("app/$s"); done
for d in "${domain_ctxs[@]}"; do groups+=("domain/$d"); done

gate_require_scanned "${#groups[@]}" "$((${#fixed_roots[@]} + 1))" check-group-purpose \
    "expected group list came back too small"

declare -A ALLOWED=()
gate_load_list_file "$BASELINE" ALLOWED

scanned=0
violations=0
allowlisted=0
viol_list=()

for g in "${groups[@]}"; do
    scanned=$((scanned + 1))
    # A non-empty purpose exists iff ci_group_purpose() has a case returning
    # a return value with at least one non-quote character for this exact
    # group path — i.e. NOT the terminal `return "";`.
    if grep -qE "strcmp\(group, \"${g}\"\) == 0\) return \"[^\"]" "$SRC"; then
        continue
    fi
    if [[ "$MODE" != "FAIL" && -n "${ALLOWED[$g]:-}" ]]; then
        allowlisted=$((allowlisted + 1))
        continue
    fi
    violations=$((violations + 1))
    viol_list+=("$g")
    echo "$g: ci_group_purpose() returns empty for this group node (add a case in $SRC)" >&2
done

gate_require_scanned "$scanned" "${#groups[@]}" check-group-purpose \
    "the per-group scan loop ran fewer iterations than the expected group list"

echo "[check_group_purpose] scanned $scanned group node(s) (${#lib_modules[@]} lib, ${#app_shapes[@]} app-shape, ${#domain_ctxs[@]} domain-context, ${#fixed_roots[@]} fixed root)"
echo "[check_group_purpose] $violations violation(s) found (mode: $MODE)"
if (( allowlisted > 0 )); then
    echo "[check_group_purpose] $allowlisted allowlisted violation(s) ignored"
fi
if (( violations > 0 )); then
    echo "[check_group_purpose] write to tools/lint/group_purpose_baseline.txt to allowlist existing violations"
fi

fail=0
if (( violations > 0 )) && [[ "$MODE" == "FAIL" || "$MODE" == "RATCHET" ]]; then
    fail=1
fi
if (( domain_mismatch != 0 )); then
    echo "[check_group_purpose] k_domain_contexts[] vs Makefile DOMAIN_CONTEXTS mismatch" >&2
    fail=1
fi

exit "$fail"
