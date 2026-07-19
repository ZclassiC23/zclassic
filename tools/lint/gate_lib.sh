# shellcheck shell=bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# gate_lib.sh — shared helpers that make lint gates FAIL-LOUD instead of
# fail-silent. See docs/work/lint-gate-hollowness-audit.md.
#
# A hollow gate reports "clean" exit 0 while a real violation is present:
# its scan set silently emptied (a renamed/moved dir, a `for f in glob`
# that found nothing, a non-GNU grep that exited >=2) and the violation
# loop ran zero times. This file provides two primitives every gate should
# reach for so that "scanned nothing" becomes a LOUD exit 2, never a quiet
# pass:
#
#   gate_require_scanned <count> <floor> <gate-name> [hint]
#       Abort (exit 2) when the realized scan count is below a known floor.
#       Use floor=1 for "must scan at least one file" or a higher integer
#       for a known-population gate.
#
#   gate_grep <args...>
#       A `grep` wrapper that treats exit >=2 (a real grep error: bad flag,
#       unreadable file, broken regex on a non-GNU grep) as FATAL (exit 2)
#       instead of masking it as "no match" the way `grep ... || true` does.
#       Exit 0 (match) and 1 (no match) pass through unchanged. Captures
#       and re-emits grep's stdout so callers can `gate_grep ... < <(...)`.
#
#   gate_load_list_file <path> <array-name> [<count-var-name>]
#       Load a baseline/allowlist file into the nameref'd associative array
#       as a set: one entry per non-blank, non-`#`-comment line, trimmed,
#       ARRAY["<line>"]=1. A missing <path> leaves the array empty (no
#       error) — callers that need the file to exist for a later append
#       should `touch` it themselves before calling. If <count-var-name> is
#       given, that nameref'd variable is set to the number of entries
#       loaded. This is the ~9-line `declare -A ...; while IFS= read -r
#       line; do ...; done < "$FILE"` block that used to be hand-rolled in
#       most gates.
#
#   gate_load_kv_file <path> <array-name>
#       Same file format and comment/blank handling as gate_load_list_file,
#       but for baselines of the form "<key> <value>" per line (e.g. a
#       recorded legacy-count): ARRAY["<first token>"]="<last token>".
#
# Sourcing contract: a gate sources this AFTER `set -euo pipefail` and its
# own `cd` to the repo root. These helpers do not change cwd.

# Abort LOUD if the scan set is smaller than a known floor. This is the
# single most important anti-hollow primitive: a gate that derives its scan
# set from find/glob/grep -rl and never asserts non-emptiness will silently
# pass when the producer empties.
gate_require_scanned() {
    local count="$1" floor="$2" name="$3" hint="${4:-}"
    if ! [ "$count" -ge "$floor" ] 2>/dev/null; then
        echo "$name: FATAL — scan set is '${count}' (< floor ${floor})." >&2
        echo "  The scan producer (find/glob/grep) returned too little; a" >&2
        echo "  scanned dir/file was likely renamed, moved, or deleted." >&2
        echo "  Refusing to report 'clean' off a hollow (empty) scan." >&2
        [ -n "$hint" ] && echo "  $hint" >&2
        exit 2
    fi
}

# grep wrapper: exit 0/1 pass through; exit >=2 (real error) is FATAL.
# Echoes grep's stdout so it composes in pipelines / process substitution.
gate_grep() {
    local out rc
    set +e
    out=$(grep "$@")
    rc=$?
    set -e
    if [ "$rc" -ge 2 ]; then
        echo "gate_grep: FATAL — grep failed (exit $rc) for: grep $*" >&2
        echo "  A grep error (bad flag, unreadable file, regex unsupported by" >&2
        echo "  this grep) was about to be masked as 'no match'. Refusing to" >&2
        echo "  report 'clean' off a broken scan." >&2
        exit 2
    fi
    [ -n "$out" ] && printf '%s\n' "$out"
    return "$rc"
}

# Load a baseline/allowlist file into the nameref'd associative array as a
# set: one entry per non-blank, non-`#`-comment line, trimmed, ARRAY["<line
# >"]=1. Missing FILE leaves the array empty — no error, no side effect.
gate_load_list_file() {
    local file="$1" _array_name="$2" _count_name="${3:-}"
    local -n _gllf_arr="$_array_name"
    local line _gllf_count=0
    # bash nameref quirk (reproduced on 5.2): a `local -n` array ref that is
    # never actually written through before the function returns leaves the
    # CALLER's array unbound under `set -u` — even though it was already
    # `declare -A`'d. Force one real write+delete so the nameref always
    # resolves, whether or not FILE exists or has any real (non-comment,
    # non-blank) lines.
    _gllf_arr["__gate_lib_probe__"]=1
    unset '_gllf_arr[__gate_lib_probe__]'
    if [[ -f "$file" ]]; then
        while IFS= read -r line; do
            line="${line%%#*}"
            line="${line#"${line%%[![:space:]]*}"}"
            line="${line%"${line##*[![:space:]]}"}"
            [[ -z "$line" ]] && continue
            _gllf_arr["$line"]=1
            _gllf_count=$((_gllf_count + 1))
        done < "$file"
    fi
    if [[ -n "$_count_name" ]]; then
        local -n _gllf_cnt="$_count_name"
        _gllf_cnt="$_gllf_count"
    fi
}

# Same file format/comment/blank handling as gate_load_list_file, but for
# "<key> <value>" baselines (e.g. a recorded legacy count): the nameref'd
# associative array gets ARRAY["<first token>"]="<last token>" per line.
gate_load_kv_file() {
    local file="$1" _array_name="$2"
    local -n _glkf_arr="$_array_name"
    local line key val
    # See the nameref-probe comment in gate_load_list_file above — same fix.
    _glkf_arr["__gate_lib_probe__"]=1
    unset '_glkf_arr[__gate_lib_probe__]'
    if [[ -f "$file" ]]; then
        while IFS= read -r line; do
            line="${line%%#*}"
            line="${line#"${line%%[![:space:]]*}"}"
            line="${line%"${line##*[![:space:]]}"}"
            [[ -z "$line" ]] && continue
            key="${line%% *}"
            val="${line##* }"
            _glkf_arr["$key"]="$val"
        done < "$file"
    fi
}
