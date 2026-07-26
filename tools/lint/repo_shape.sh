# shellcheck shell=bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# repo_shape.sh — the ONE place a shell script learns the repo's directory
# taxonomy. Source it; never execute it.
#
# The set of app shapes, lib modules and top-level source roots was written
# out by hand in four gate scripts and three more places besides, with no
# gate cross-checking any of them. docs/AGENT_TRAPS.md recorded that hole
# rather than closing it, and undercounted it: it named three copies when
# there were four, the fourth being inside check_group_purpose.sh — the gate
# whose own subject is this taxonomy.
#
# Drift here is silent in the worst way. Add a top-level directory, forget
# one copy, and lint stays green while `code map` files every file in the new
# tree under the catch-all group and the orphan-placement gate reports the
# whole tree as misplaced.
#
# The Makefile is the source of truth because the BUILD already depends on
# these variables: APP_DIRS, LIB_MODULES and DOMAIN_CONTEXTS feed ALL_SRCS
# and the -I flag lists, so they cannot rot without the build breaking. That
# makes them the one representation that is load-bearing rather than
# descriptive.
#
# This file is PARSED LIVE on every source, and is deliberately NOT a
# generated artifact checked into the tree. A generated copy would need a
# freshness gate, and a freshness gate is exactly the detect-the-drift
# mechanism this file exists to replace. Parsing costs ~1.6 ms; `make lint`
# is a 23.6 s wall, so the whole-suite cost is under a tenth of a percent.
#
#   ZCL_APP_SHAPES[]       app/<shape>       from APP_DIRS
#   ZCL_LIB_MODULES[]      lib/<module>      from LIB_MODULES (line-continued)
#   ZCL_DOMAIN_CONTEXTS[]  domain/<context>  from DOMAIN_CONTEXTS
#   ZCL_REPO_TOPS[]        top-level roots   DERIVED from the -I flag lists,
#                                            not declared: a REPO_TOPS variable
#                                            the build did not need would be
#                                            one more copy to keep honest.
#   repo_shape_dirs <top> <leaf>   -> "app/models/src app/controllers/src ..."
#
# An empty parse is a LOUD exit 2 via gate_require_scanned, never a quiet
# pass — a consumer that silently degraded to an empty scan set is the
# hollow-gate failure gate_lib.sh was written to stop.
#
# Set ZCL_REPO_SHAPE_MAKEFILE to point at a fixture Makefile in tests.

_repo_shape_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/lint/gate_lib.sh
. "$_repo_shape_dir/gate_lib.sh"

ZCL_REPO_SHAPE_MAKEFILE="${ZCL_REPO_SHAPE_MAKEFILE:-$_repo_shape_dir/../../Makefile}"

if [ ! -r "$ZCL_REPO_SHAPE_MAKEFILE" ]; then
    echo "repo-shape: FATAL — cannot read '$ZCL_REPO_SHAPE_MAKEFILE'." >&2
    echo "  The directory taxonomy is derived from the Makefile; without it" >&2
    echo "  every consumer would scan an empty set and pass silently." >&2
    exit 2
fi

# One pass. Emits "VAR<TAB>tok" lines. Handles backslash line continuation
# (LIB_MODULES spans two lines) and strips any trailing comment.
_repo_shape_parse() {
    awk '
        function emit(name, rest,   i, n, a) {
            gsub(/#.*$/, "", rest)
            n = split(rest, a, /[ \t]+/)
            for (i = 1; i <= n; i++)
                if (a[i] != "" && a[i] != "\\") print name "\t" a[i]
        }
        # Top-level roots: first path component of every literal -I<dir>/ in
        # the *_INCLUDES variables. Derived, never declared. This rule must
        # precede the generic assignment rule below, which would otherwise
        # swallow these lines with its next.
        /^[A-Z_]+_INCLUDES[ \t]*=/ {
            s = $0
            while (match(s, /-I[a-zA-Z_]+\//)) {
                tok = substr(s, RSTART + 2, RLENGTH - 3)
                print "TOPS\t" tok
                s = substr(s, RSTART + RLENGTH)
            }
            next
        }
        # Accumulate backslash-continued assignments into one logical line.
        /^[A-Z_]+[ \t]*=/ && !cont {
            line = $0; name = $0
            sub(/[ \t]*=.*$/, "", name)
            sub(/^[^=]*=/, "", line)
            if (line ~ /\\[ \t]*$/) { cont = 1; acc = line; keep = name; next }
            if (name == "APP_DIRS")        emit("APP_DIRS", line)
            if (name == "LIB_MODULES")     emit("LIB_MODULES", line)
            if (name == "DOMAIN_CONTEXTS") emit("DOMAIN_CONTEXTS", line)
            next
        }
        cont {
            acc = acc " " $0
            if ($0 ~ /\\[ \t]*$/) next
            cont = 0
            if (keep == "APP_DIRS")        emit("APP_DIRS", acc)
            if (keep == "LIB_MODULES")     emit("LIB_MODULES", acc)
            if (keep == "DOMAIN_CONTEXTS") emit("DOMAIN_CONTEXTS", acc)
            next
        }
    ' "$ZCL_REPO_SHAPE_MAKEFILE"
}

_repo_shape_raw="$(_repo_shape_parse)"

# shellcheck disable=SC2034  # consumed by sourcing gates
mapfile -t ZCL_APP_SHAPES < <(printf '%s\n' "$_repo_shape_raw" | awk -F'\t' '$1=="APP_DIRS"{print $2}')
# shellcheck disable=SC2034
mapfile -t ZCL_LIB_MODULES < <(printf '%s\n' "$_repo_shape_raw" | awk -F'\t' '$1=="LIB_MODULES"{print $2}')
# shellcheck disable=SC2034
mapfile -t ZCL_DOMAIN_CONTEXTS < <(printf '%s\n' "$_repo_shape_raw" | awk -F'\t' '$1=="DOMAIN_CONTEXTS"{print $2}')
# shellcheck disable=SC2034
mapfile -t ZCL_REPO_TOPS < <(printf '%s\n' "$_repo_shape_raw" | awk -F'\t' '$1=="TOPS"{print $2}' | sort -u)

gate_require_scanned "${#ZCL_APP_SHAPES[@]}" 1 repo-shape \
    "APP_DIRS parse came back empty — Makefile:256 layout changed?"
gate_require_scanned "${#ZCL_LIB_MODULES[@]}" 1 repo-shape \
    "LIB_MODULES parse came back empty — Makefile:272 layout changed?"
gate_require_scanned "${#ZCL_DOMAIN_CONTEXTS[@]}" 1 repo-shape \
    "DOMAIN_CONTEXTS parse came back empty — Makefile:295 layout changed?"
gate_require_scanned "${#ZCL_REPO_TOPS[@]}" 1 repo-shape \
    "no -I<top>/ flags found — the *_INCLUDES layout changed?"

unset _repo_shape_raw

# "app src" -> app/models/src app/controllers/src ... for each shape.
# Only the taxonomies this file owns; anything else is a caller's own scope.
repo_shape_dirs() {
    local top="$1" leaf="${2:-}" name out=()
    local -n _rs_list=ZCL_APP_SHAPES
    case "$top" in
        app)    local -n _rs_list=ZCL_APP_SHAPES ;;
        lib)    local -n _rs_list=ZCL_LIB_MODULES ;;
        domain) local -n _rs_list=ZCL_DOMAIN_CONTEXTS ;;
        *)
            echo "repo_shape_dirs: FATAL — unknown top '$top'" >&2
            exit 2
            ;;
    esac
    for name in "${_rs_list[@]}"; do
        if [ -n "$leaf" ]; then out+=("$top/$name/$leaf"); else out+=("$top/$name"); fi
    done
    printf '%s\n' "${out[@]}"
}
