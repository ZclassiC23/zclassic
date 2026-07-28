#!/usr/bin/env bash
# Gate — telemetry-ontology coverage. Every network telemetry field a covered
# dump function emits must carry a MEANING row in
# lib/util/include/util/telemetry_ontology.def.
#
# Why this gate exists. `dumpstate peer_lifecycle` prints
# "pre_handshake_disconnects":27 and nothing states what that counts, what
# range is healthy, or what a bad value implies. 27-of-332 on a healthy node
# and 8-of-8 on a node that cannot start produce JSON that is identical in
# shape; only the second is the whole story. Meaning that is optional rots
# immediately, so the next field to ship must arrive with meaning attached or
# the build stops.
#
# Mechanism. tools/lint/telemetry_ontology_scan.txt declares the covered
# surface as (subsystem, file, function, json-target-variable, path-prefix)
# rows. For each row this gate slices the named function out of its file
# (tools/lint/telemetry_scan_lib.awk), extracts every scalar field emission
# inside it (json_push_kv_int/_str/_bool/_dbl/_uint — container pushes are
# structural, not values), maps the emission's target variable to that row's
# JSON path prefix, and requires the resulting "<subsystem>|<path>" key to
# exist in the ontology .def. Two ways it bites:
#
#   UNANNOTATED FIELD        a field emitted with no ontology row
#   UNMAPPED EMISSION TARGET a new json target variable the manifest does not
#                            map, so its fields could not be checked at all
#
# Anti-hollowness. Floors on the manifest row count, the ontology row count,
# the judged (non-info) row count and the extracted field count; a function
# the awk slicer cannot find is FATAL, never "zero fields".
#
# Mode: WARN | FAIL (ZCL_LINT_MODE; default FAIL).

set -euo pipefail

cd "$(dirname "$0")/../.."
# shellcheck source=tools/lint/gate_lib.sh
. tools/lint/gate_lib.sh

GATE=check_telemetry_ontology
MODE="${ZCL_LINT_MODE:-FAIL}"

# Overridable so the gate self-test can point at a planted fixture.
# `-` not `:-` deliberately: an explicitly EMPTY override means "point the scan
# at nothing", and that must reach the readability check below and exit 2, not
# silently fall back to the real manifest and report a clean tree.
MANIFEST="${ZCL_TELEMETRY_SCAN_MANIFEST-tools/lint/telemetry_ontology_scan.txt}"
# Additional manifest rows appended to the real set. The gate self-test points
# this at a checked-in fixture that emits a field with no meaning, so the trip
# path is exercised against the REAL ontology and the REAL floors — never
# against a shrunken scan that would trip for the wrong reason.
EXTRA_MANIFEST="${ZCL_TELEMETRY_SCAN_EXTRA_MANIFEST:-}"
ONTOLOGY="${ZCL_TELEMETRY_ONTOLOGY_DEF-lib/util/include/util/telemetry_ontology.def}"
AWKLIB="tools/lint/telemetry_scan_lib.awk"

for f in "$MANIFEST" "$ONTOLOGY" "$AWKLIB"; do
    if [ ! -r "$f" ]; then
        echo "$GATE: FATAL — required input missing or unreadable: $f" >&2
        exit 2
    fi
done
if [ -n "$EXTRA_MANIFEST" ] && [ ! -r "$EXTRA_MANIFEST" ]; then
    echo "$GATE: FATAL — ZCL_TELEMETRY_SCAN_EXTRA_MANIFEST is set but" >&2
    echo "  unreadable: $EXTRA_MANIFEST" >&2
    exit 2
fi

# ── 1. the ontology's declared keys ──────────────────────────────────────
# TELEMETRY_FIELD("<subsystem>", "<path>", ...  -> "<subsystem>|<path>"
ONTO_KEYS=$(sed -n \
    's/^[[:space:]]*TELEMETRY_FIELD("\([^"]*\)",[[:space:]]*"\([^"]*\)".*/\1|\2/p' \
    "$ONTOLOGY" | sort -u)
ONTO_COUNT=$(printf "%s\n" "$ONTO_KEYS" | sed '/^$/d' | wc -l)
gate_require_scanned "$ONTO_COUNT" 150 "$GATE" \
    "ontology row population collapsed — $ONTOLOGY parsed as $ONTO_COUNT rows"

# Judged rows: a table that is all TFR_INFO would technically be "covered"
# while judging nothing. Floor the judged population too.
JUDGED=$(grep -cE 'TFR_(EXPECT_ZERO|EXPECT_NONZERO|EXPECT_TRUE|EXPECT_FALSE|MIN_ABS|MAX_ABS|MIN_RATIO_OF|MAX_RATIO_OF)' \
    "$ONTOLOGY" || true)
gate_require_scanned "$JUDGED" 20 "$GATE" \
    "judged (non-info) ontology rows collapsed to $JUDGED"

QUESTIONS=$(grep -c '^[[:space:]]*TELEMETRY_QUESTION(' "$ONTOLOGY" || true)
gate_require_scanned "$QUESTIONS" 8 "$GATE" \
    "discovery-index question rows collapsed to $QUESTIONS"

# ── 2. placeholder meanings ──────────────────────────────────────────────
# `means` is the argument right after the severity token, so an empty string
# literal on the line that follows a TFS_* severity is a hollow row.
PLACEHOLDER=$(grep -nE 'TFS_(INFO|WARN|CRITICAL),[[:space:]]*""' "$ONTOLOGY" || true)
if [ -n "$PLACEHOLDER" ]; then
    printf "%s\n" "$PLACEHOLDER" >&2
    echo "[$GATE] row(s) above ship an EMPTY \`means\` — every field must" >&2
    echo "  state what it counts. See util/telemetry_ontology.h." >&2
    [ "$MODE" = "FAIL" ] && exit 1
fi

# A judged row must also say what an unhealthy value implies and what to read
# next; only TFR_INFO rows may leave those blank.
BAD_JUDGED=$(awk '
    /TELEMETRY_FIELD\(/ { buf = $0; judged = 0; next }
    buf != "" { buf = buf " " $0 }
    buf ~ /TFR_(EXPECT_ZERO|EXPECT_NONZERO|EXPECT_TRUE|EXPECT_FALSE|MIN_ABS|MAX_ABS|MIN_RATIO_OF|MAX_RATIO_OF)/ { judged = 1 }
    buf != "" && /\)[[:space:]]*$/ {
        if (judged && (buf ~ /,[[:space:]]*""[[:space:]]*,[[:space:]]*""[[:space:]]*\)/ ||
                       buf ~ /,[[:space:]]*""[[:space:]]*\)[[:space:]]*$/)) {
            sub(/^[[:space:]]*/, "", buf)
            printf "%s: %.90s...\n", FILENAME, buf
        }
        buf = ""; judged = 0
    }
' "$ONTOLOGY" || true)
if [ -n "$BAD_JUDGED" ]; then
    printf "%s\n" "$BAD_JUDGED" >&2
    echo "[$GATE] judged row(s) above leave \`implies\` or \`next\` empty —" >&2
    echo "  a row that carries a health rule must say what a bad value means" >&2
    echo "  and where to look next." >&2
    [ "$MODE" = "FAIL" ] && exit 1
fi

# ── 3. what the dump functions actually emit ─────────────────────────────
MANIFEST_ROWS=0
declare -A PREFIX_OF=()      # "<file>|<fn>|<objvar>" -> prefix
declare -A SUBSYS_OF=()      # "<file>|<fn>|<objvar>" -> subsystem
declare -A FUNCS=()          # "<file>|<fn>" -> 1

while IFS= read -r line; do
    line="${line%%#*}"
    # shellcheck disable=SC2086
    set -- $line
    [ "$#" -eq 0 ] && continue
    if [ "$#" -ne 5 ]; then
        echo "$GATE: FATAL — malformed manifest row: $line" >&2
        exit 2
    fi
    sub=$1 file=$2 fn=$3 obj=$4 prefix=$5
    [ "$prefix" = "-" ] && prefix=""
    if [ ! -r "$file" ]; then
        echo "$GATE: FATAL — manifest names an unreadable file: $file" >&2
        exit 2
    fi
    PREFIX_OF["$file|$fn|$obj"]="$prefix"
    SUBSYS_OF["$file|$fn|$obj"]="$sub"
    FUNCS["$file|$fn"]=1
    MANIFEST_ROWS=$((MANIFEST_ROWS + 1))
done < <(cat "$MANIFEST" ${EXTRA_MANIFEST:+"$EXTRA_MANIFEST"})

gate_require_scanned "$MANIFEST_ROWS" 20 "$GATE" \
    "scan manifest collapsed to $MANIFEST_ROWS rows"

EXTRACTED=0
UNANNOTATED=""
UNMAPPED=""

for key in "${!FUNCS[@]}"; do
    file="${key%%|*}"
    fn="${key##*|}"
    set +e
    slice=$(awk -v fn="$fn" -f "$AWKLIB" "$file")
    arc=$?
    set -e
    if [ "$arc" -ne 0 ]; then
        echo "$GATE: FATAL — could not slice function '$fn' out of $file" >&2
        echo "  (awk exit $arc). Refusing to report PASS off a hollow scan." >&2
        exit 2
    fi
    [ -z "$slice" ] && continue
    while read -r ln obj field; do
        [ -z "$ln" ] && continue
        mapkey="$file|$fn|$obj"
        if [ -z "${PREFIX_OF[$mapkey]+set}" ]; then
            # Only complain once per (file,fn,objvar).
            case "$UNMAPPED" in
                *"$mapkey"*) ;;
                *) UNMAPPED="${UNMAPPED}${file}:${ln}: ${fn}() emits into unmapped target '${obj}' (first field: ${field})"$'\n' ;;
            esac
            continue
        fi
        EXTRACTED=$((EXTRACTED + 1))
        path="${PREFIX_OF[$mapkey]}${field}"
        sub="${SUBSYS_OF[$mapkey]}"
        if ! printf "%s\n" "$ONTO_KEYS" | grep -qxF "$sub|$path"; then
            UNANNOTATED="${UNANNOTATED}${file}:${ln}: ${sub}|${path}"$'\n'
        fi
    done <<< "$slice"
done

gate_require_scanned "$EXTRACTED" 150 "$GATE" \
    "extracted field population collapsed to $EXTRACTED — a covered dump \
function was probably renamed"

FAILED=0
if [ -n "$UNMAPPED" ]; then
    printf "%s" "$UNMAPPED" >&2
    echo "[$GATE] UNMAPPED EMISSION TARGET — the scan manifest does not say" >&2
    echo "  which JSON path the target(s) above sit at, so their fields could" >&2
    echo "  not be checked for meaning at all. Add a row to $MANIFEST." >&2
    FAILED=1
fi
if [ -n "$UNANNOTATED" ]; then
    printf "%s" "$UNANNOTATED" >&2
    N=$(printf "%s" "$UNANNOTATED" | wc -l)
    echo "[$GATE] UNANNOTATED FIELD x$N — the field(s) above are emitted by a" >&2
    echo "  covered dump function with no meaning row. Add a TELEMETRY_FIELD" >&2
    echo "  row to $ONTOLOGY stating what it counts, its health rule, what an" >&2
    echo "  unhealthy value implies, and the next command to run." >&2
    FAILED=1
fi
if [ "$FAILED" -eq 1 ] && [ "$MODE" = "FAIL" ]; then
    exit 1
fi

echo "[$GATE] PASS ($EXTRACTED emitted fields over $MANIFEST_ROWS manifest" \
     "rows, all annotated; $ONTO_COUNT ontology rows, $JUDGED judged," \
     "$QUESTIONS questions)"
