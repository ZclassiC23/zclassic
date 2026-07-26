# shellcheck shell=bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# onto_shape.sh — the ONE place a shell script learns which canonical
# registries the ontology projects, and what each one owes. Source it; never
# execute it.
#
# This is repo_shape.sh's sibling and follows the same rule for the same
# reason. repo_shape.sh refuses to let the directory taxonomy be restated in
# each gate that polices it; this refuses to let the extractor manifest be
# restated in each gate that polices THAT. Both parse their source live rather
# than shipping a generated copy, because a generated copy needs a freshness
# gate and a freshness gate is the drift detector these files replace.
#
# The source here is config/onto/extractors.def, which the build also consumes
# to generate the extractor dispatch table. That makes it load-bearing rather
# than descriptive: a row naming a function that does not exist breaks the
# link, so the rows cannot rot into fiction while the tree still builds.
#
#   ZCL_ONTO_KINDS[]      entity kinds, deduplicated, in file order
#   ZCL_ONTO_ROWS[]       one TAB-separated record per row:
#                           kind <TAB> glob <TAB> fn <TAB> edges <TAB> fields
#   onto_shape_globs <kind>          -> one glob per line (':' split)
#   onto_shape_fields <kind>         -> one required field name per line
#   onto_shape_edges <kind>          -> one edge kind per line
#   onto_shape_sources               -> every glob of every row, deduplicated
#
# An empty parse is a LOUD exit 2 via gate_require_scanned, never a quiet
# pass. A consumer that silently degraded to an empty manifest would report a
# clean ontology precisely when the manifest had been destroyed.
#
# Set ZCL_ONTO_EXTRACTORS_DEF to point at a fixture in tests.

_onto_shape_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/lint/gate_lib.sh
. "$_onto_shape_dir/gate_lib.sh"

ZCL_ONTO_EXTRACTORS_DEF="${ZCL_ONTO_EXTRACTORS_DEF:-$_onto_shape_dir/../../config/onto/extractors.def}"

if [ ! -r "$ZCL_ONTO_EXTRACTORS_DEF" ]; then
    echo "onto-shape: FATAL — cannot read '$ZCL_ONTO_EXTRACTORS_DEF'." >&2
    echo "  The extractor manifest is the ontology's only checked-in artifact;" >&2
    echo "  without it every consumer would scan an empty set and pass." >&2
    exit 2
fi

# One pass. Rows are single logical lines by contract, but the macro call may
# wrap across physical lines, so accumulate from ONTO_EXTRACTOR( to the
# matching close paren, then split the five string/identifier arguments.
_onto_shape_parse() {
    awk '
        function flush(   s, n, a, i, out) {
            if (acc == "") return
            s = acc; acc = ""
            n = 0
            # Pull quoted strings and bare identifiers in argument order.
            while (match(s, /"[^"]*"|[A-Za-z_][A-Za-z0-9_]*/)) {
                tok = substr(s, RSTART, RLENGTH)
                s = substr(s, RSTART + RLENGTH)
                if (tok == "ONTO_EXTRACTOR") continue
                gsub(/^"|"$/, "", tok)
                a[++n] = tok
            }
            if (n < 5) next
            printf "%s\t%s\t%s\t%s\t%s\n", a[1], a[2], a[3], a[4], a[5]
        }
        /^[ \t]*\/\*/ { inc = 1 }
        inc { if ($0 ~ /\*\//) inc = 0; next }
        /ONTO_EXTRACTOR[ \t]*\(/ { acc = $0; depth = gsub(/\(/, "(") - gsub(/\)/, ")")
                                   if (depth <= 0) flush(); else collecting = 1; next }
        collecting { acc = acc " " $0
                     depth += gsub(/\(/, "(") - gsub(/\)/, ")")
                     if (depth <= 0) { collecting = 0; flush() }
                     next }
        END { flush() }
    ' "$ZCL_ONTO_EXTRACTORS_DEF"
}

_onto_shape_raw="$(_onto_shape_parse)"

# shellcheck disable=SC2034  # consumed by sourcing gates
mapfile -t ZCL_ONTO_ROWS < <(printf '%s\n' "$_onto_shape_raw" | awk 'NF')
# shellcheck disable=SC2034
mapfile -t ZCL_ONTO_KINDS < <(printf '%s\n' "$_onto_shape_raw" | awk -F'\t' 'NF{print $1}' | awk '!seen[$0]++')

gate_require_scanned "${#ZCL_ONTO_ROWS[@]}" 1 onto-shape \
    "extractors.def parse came back empty — did the ONTO_EXTRACTOR row layout change?"

unset _onto_shape_raw

# Column N of every row whose kind matches, ':'-split, one token per line.
_onto_shape_col() {
    local kind="$1" col="$2" row
    for row in "${ZCL_ONTO_ROWS[@]}"; do
        [ "${row%%$'\t'*}" = "$kind" ] || continue
        printf '%s\n' "$row" | cut -d$'\t' -f"$col" | tr ':' '\n'
    done | awk 'NF'
}

onto_shape_globs()  { _onto_shape_col "$1" 2; }
onto_shape_fields() { _onto_shape_col "$1" 5; }
onto_shape_edges()  { _onto_shape_col "$1" 4; }

# Every glob named by any row, deduplicated — the set the binding gate must
# account for and the set the store's staleness stamp covers.
onto_shape_sources() {
    local row
    for row in "${ZCL_ONTO_ROWS[@]}"; do
        printf '%s\n' "$row" | cut -d$'\t' -f2 | tr ':' '\n'
    done | awk 'NF' | sort -u
}
