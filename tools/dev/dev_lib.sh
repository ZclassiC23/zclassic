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

# The BAKED source identity of a binary — its first `source_id_sha256`.
#
# `agentbuild` emits that key FOUR times on one line: once at the top level
# (what this binary was compiled from) and again inside nested runtime blocks
# that describe the dev lane as it exists right now. Because the payload is a
# single line, a `sed 's/.*"key":"\(...\)".*/\1/'` is greedy and returns the
# LAST occurrence — a runtime value — while `head -1` does nothing to help.
# That is not hypothetical: json_string_field reported the live daemon and the
# dev build as having identical identities on 2026-07-28, which is exactly the
# false "everything matches" a drift check must never produce. Anchor on the
# first occurrence and require 64 hex chars.
#
# Canonical home is here because two independent callers need the identical
# rule: tools/dev/agent-doctor.sh (human-run) and
# tools/scripts/build_drift_probe.sh (scheduled). A second copy of this
# function is a second chance to get the greedy-match bug back.
baked_source_id() {
    local bin="$1"
    [ -x "$bin" ] || return 0
    timeout 20 "$bin" agentbuild 2>/dev/null |
        grep -oE '"source_id_sha256"[[:space:]]*:[[:space:]]*"[0-9a-f]{64}"' |
        head -1 |
        grep -oE '[0-9a-f]{64}' || true
}
