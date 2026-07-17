#!/usr/bin/env bash
# Lint gate #49 — blocker escape-action totality.
#
# blocker_supervisor_sweep() (lib/util/src/blocker.c ~:492) dispatches a
# blocker's escape by looking up its `escape_action` string in the
# blocker_register_escape() registry via exact strcmp. That lookup is not a
# compile-time check: a misspelled or never-registered action name means the
# blocker can never auto-escape, and — unlike an EMPTY escape_action, which
# app/conditions/src/blocker_stall_meta_detector.c catches as a generic
# defect class — a non-empty-but-unregistered string is invisible to that
# backstop too, so nothing catches it. This gate is the compile-time
# equivalent: it extracts every literal escape_action string assigned at a
# blocker_init/blocker_set call site in production code and fails if the
# string has no matching blocker_register_escape("<same string>") call
# anywhere in the tree.
#
# Empty escape_action strings are exempt (the meta-detector backstop already
# covers the "no escape configured" case; this gate is about strings that
# LOOK configured but silently dead-end).
#
# Scope: app/ config/ lib/ src/, excluding lib/test (production code only —
# test fixtures intentionally exercise unregistered names).
set -euo pipefail

cd "$(dirname "$0")/../.."

ROOTS=(app config lib src)

# The primitive's own file is excluded: it DEFINES blocker_register_escape
# (a function definition, not a call) and internally re-copies escape_action
# via a "%s" runtime format (already filtered below), not a literal. Mirrors
# the exclusion in check_typed_blocker.sh.
collect_registry_files() {
    for root in "${ROOTS[@]}"; do
        [ -d "$root" ] || continue
        find "$root" -type f \( -name '*.c' -o -name '*.h' \) \
            ! -path 'lib/test/*' \
            ! -path 'lib/util/src/blocker.c' \
            ! -path 'lib/util/include/util/blocker.h' \
            2>/dev/null
    done
}

# ZCL_BLOCKER_ESCAPE_SCAN_FILES overrides only the assignment-side file set,
# newline-separated — used by the gate's own negative self-test to feed an
# isolated fixture file instead of walking the real tree. The registry side
# always scans the real tree (that's what auto-escape can actually dispatch
# to at runtime).
if [ -n "${ZCL_BLOCKER_ESCAPE_SCAN_FILES:-}" ]; then
    mapfile -t assign_files <<< "${ZCL_BLOCKER_ESCAPE_SCAN_FILES}"
else
    mapfile -t assign_files < <(collect_registry_files)
fi
mapfile -t registry_files < <(collect_registry_files)

if [ "${#assign_files[@]}" -eq 0 ]; then
    echo "check_blocker_escape_registered: no files to scan" >&2
    exit 1
fi

# file<TAB>line<TAB>literal for every `snprintf(<expr>.escape_action, ...,
# "literal")` call site, joining wrapped statements onto one buffer. Skips
# any literal containing '%' — that is a runtime copy (e.g. blocker.c's
# internal `snprintf(s->escape_action, ..., "%s", r->escape_action)`
# propagation), not an action-name literal.
extract_escape_action_literals() {
    awk '
        {
            if (match($0, /snprintf\([^,]*escape_action/)) {
                startline = FNR
                buf = $0
                while (buf !~ /;/) {
                    if ((getline nextline) <= 0) break
                    buf = buf "\n" nextline
                }
                if (match(buf, /"([^"\\]|\\.)*"/)) {
                    s = substr(buf, RSTART + 1, RLENGTH - 2)
                    if (s !~ /%/ && s != "") {
                        printf "%s\t%d\t%s\n", FILENAME, startline, s
                    }
                }
            }
        }
    ' "$@"
}

# One registered name per line for every blocker_register_escape("name", fn).
extract_registered_names() {
    awk '
        {
            if (match($0, /blocker_register_escape\(/)) {
                buf = $0
                while (buf !~ /;/) {
                    if ((getline nextline) <= 0) break
                    buf = buf "\n" nextline
                }
                if (match(buf, /"([^"\\]|\\.)*"/)) {
                    s = substr(buf, RSTART + 1, RLENGTH - 2)
                    print s
                }
            }
        }
    ' "$@"
}

declare -A registered
while IFS= read -r name; do
    [ -n "$name" ] && registered["$name"]=1
done < <(extract_registered_names "${registry_files[@]}")

fail=0
violations=()
while IFS=$'\t' read -r file line lit; do
    [ -z "$file" ] && continue
    if [ -z "${registered[$lit]+x}" ]; then
        violations+=("$file:$line: \"$lit\"")
        fail=1
    fi
done < <(extract_escape_action_literals "${assign_files[@]}")

if [ "$fail" = "0" ]; then
    echo "check_blocker_escape_registered: clean — ${#registered[@]} registered escape(s), all escape_action literals resolve"
    exit 0
fi

echo ""
echo "check_blocker_escape_registered: ${#violations[@]} escape_action literal(s) with no matching blocker_register_escape()"
echo ""
for v in "${violations[@]}"; do
    echo "  $v"
done
echo ""
echo "Fix options:"
echo "  1. Register the escape: blocker_register_escape(\"<same string>\", <fn>)"
echo "     at the owning subsystem's init (see"
echo "     app/services/src/chain_activation_service.c for the pattern)."
echo "  2. If the blocker never sets escape_deadline_secs (no auto-escape is"
echo "     intended — it is dispatched some other way, e.g. its own"
echo "     condition cadence), empty the string instead: it then falls under"
echo "     the blocker_stall_meta_detector.c empty-escape backstop rather"
echo "     than silently dead-ending an unregistered name."
exit 1
