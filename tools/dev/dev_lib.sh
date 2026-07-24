# shellcheck shell=bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# dev_lib.sh — shared primitives that were byte-identical copies across the
# tools/dev/ dev-loop scripts. One canonical definition per function.
# Sourcing contract: no cwd/global side effects beyond defining the
# functions below. is_true() calls fail() on an unrecognized value — the
# sourcing script must already define fail() (all current callers do).

json_escape() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

is_uint() {
    [[ "${1:-}" =~ ^[0-9]+$ ]]
}

is_true()
{
    case "${1:-}" in
        1|true|yes|on) return 0 ;;
        0|false|no|off|"") return 1 ;;
        *) fail "expected boolean value, got: $1" ;;
    esac
}
