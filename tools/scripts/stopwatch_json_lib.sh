# shellcheck shell=bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# stopwatch_json_lib.sh — shared JSON/frontier-read primitives for the
# stopwatch/copy-prove harnesses (cold_start_to_tip_stopwatch.sh,
# network_disruption_recovery_stopwatch.sh, and siblings). These five
# functions were byte-identical copies across those scripts; this is the
# one canonical definition. Sourcing contract: source AFTER REPO_ROOT is
# set, no cwd/global side effects beyond defining the functions below.

# json_escape <str> — backslash/quote/tab/cr-safe, newline-collapsed-to-space
# JSON string body (no surrounding quotes).
json_escape() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/\t/\\t/g; s/\r/\\r/g' | tr '\n' ' '
}

# json_string <str> — json_escape() wrapped in double quotes.
json_string() { printf '"%s"' "$(json_escape "$1")"; }

# json_number_or_null <val> — <val> if it looks like an integer, else the
# bare JSON token null (never an unquoted empty/garbage field).
json_number_or_null() {
    case "${1:-}" in
        ''|*[!0-9-]*) printf 'null' ;;
        *) printf '%s' "$1" ;;
    esac
}

# jget <json> <key> — first top-level integer value of "key". The
# "key"[..]: anchor with a required closing quote means "hstar" never
# matches "hstar_next_height" and "network_tip" never matches
# "network_tip_read_ok".
jget() {
    printf '%s' "$1" | grep -oE "\"$2\"[[:space:]]*:[[:space:]]*-?[0-9]+" | head -1 |
        grep -oE -- '-?[0-9]+$'
}

# is_busy_response <frontier-doc> — true iff a `dumpstate reducer_frontier`
# body is the PARTIAL progress_store-busy doc —
# {"snapshot_status":"progress_store_busy","retryable":true} — rather than
# a genuine empty/absent response. A naive `grep -q '"hstar"'` miss cannot
# tell "the store is busy, retry" from "the node isn't answering at all"
# apart; this lets callers tell the two apart and label busy honestly
# instead of reading it as hstar=-1 or a silent empty.
is_busy_response() {
    printf '%s' "$1" | grep -qE '"retryable"[[:space:]]*:[[:space:]]*true'
}
