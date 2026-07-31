#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# test-group-list.sh — print the REGISTERED test-group names without building.
#
# ── WHY THIS EXISTS ────────────────────────────────────────────────────────
# The documented way to enumerate test groups was an incantation in the
# project's front-page CLAUDE.md:
#
#     git grep -hoE 'X\([a-z_0-9]+\)' lib/test/src/test_parallel.c | tr -d 'X()'
#
# It has two defects and both were being hit. Dropping -h glues the filename
# onto every name (CLAUDE.md warns about it, which means everyone trips it).
# And it drops the registry's PREFIXES: g_groups[] is built by two X-macro
# expansions, ROW_TEST stamping "test_" onto every TEST_LIST row and ROW_SPEC
# stamping "spec_" onto every SPEC_LIST row. The incantation prints 28 SPEC
# names as if they were "test_<name>", which is not what the runner prints,
# not what --only matches against, and not what a report can diff against a
# run artifact. Verified on this tree: naive output vs the runner's own
# .cache/test-timing/last-run.json disagreed on 32 names, of which 28 were
# pure prefix error.
#
# The parse here mirrors the translation the compiler performs, not a
# line-shape guess:
#   * A comment — including a MULTI-LINE one — is replaced by one space
#     (translation phase 3), so a comment inside TEST_LIST does not need
#     trailing backslashes and does NOT end the macro. TEST_LIST contains
#     exactly such a comment before the make_lint_gates shard rows; a parser
#     that ends the macro at the first line lacking a backslash silently loses
#     the 12 shard groups.
#   * Only after comments are removed does a trailing backslash decide
#     continuation.
#
# Output order is REGISTRY ORDER (TEST_LIST rows, then SPEC_LIST rows), which
# is g_groups[] order — the order the runner reports in.
#
# Modes (all read-only, no build, no network):
#   (none)             every registered group name, one per line
#   --count            the number of registered groups
#   --match SUBSTR     the groups a `--only=SUBSTR` run would select, using the
#                      runner's own rule (plain substring of the FULL name,
#                      lib/test/src/test_parallel.c strstr()). Exit 1, no
#                      output, when nothing matches.
#   --suggest SUBSTR   nearest candidates for a SUBSTR that matched nothing
#   --params-gated     the groups group_is_params_heavy() excludes from a
#                      default full run
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO"
# shellcheck source=tools/scripts/sh_str.sh
. "$REPO/tools/scripts/sh_str.sh"  # str_contains / str_lacks — see F-note

REGISTRY="${ZCL_TEST_REGISTRY_SRC:-lib/test/src/test_parallel.c}"

[ -f "$REGISTRY" ] || {
    echo "test-group-list: FATAL — missing registry source $REGISTRY" >&2
    exit 2
}

# ── the registry parse ─────────────────────────────────────────────────────
# One awk pass. State: whether we are inside a /* */ comment, and which
# X-macro list (if any) we are inside. Names are emitted with the prefix the
# matching ROW_* macro stamps on.
registered() {
    awk '
    function strip_comments(line,   out, i, c) {
        out = ""
        i = 1
        while (i <= length(line)) {
            c = substr(line, i, 2)
            if (incomment) {
                if (c == "*/") { incomment = 0; i += 2 } else { i += 1 }
            } else {
                if (c == "/*") { incomment = 1; i += 2 }
                else { out = out substr(line, i, 1); i += 1 }
            }
        }
        return out
    }
    BEGIN { incomment = 0; prefix = "" }
    {
        code = strip_comments($0)

        if (prefix == "") {
            if (code ~ /^[[:space:]]*#[[:space:]]*define[[:space:]]+TEST_LIST\(X\)/)
                prefix = "test_"
            else if (code ~ /^[[:space:]]*#[[:space:]]*define[[:space:]]+SPEC_LIST\(X\)/)
                prefix = "spec_"
            else
                next
        }

        # Emit every X(name) on this (comment-free) line, in order.
        rest = code
        # NOTE the leading char class allows a DIGIT: the registry really does
        # contain X(100_stories), which becomes the identifier spec_100_stories
        # only after ## concatenation. An [A-Za-z_] first-char rule silently
        # loses exactly that one group — 865 instead of 866.
        while (match(rest, /X\([A-Za-z_0-9][A-Za-z_0-9]*\)/)) {
            tok = substr(rest, RSTART + 2, RLENGTH - 3)
            print prefix tok
            rest = substr(rest, RSTART + RLENGTH)
        }

        # Continuation: a comment still open at end-of-line spans the newline
        # (phase 3 makes it one space), so the macro continues. Otherwise the
        # comment-free text must end in a backslash.
        if (incomment) next
        trimmed = code
        sub(/[[:space:]]+$/, "", trimmed)
        if (trimmed !~ /\\$/) prefix = ""
    }
    ' "$REGISTRY"
}

# ── the params-heavy gate, read from the function that enforces it ─────────
# group_is_params_heavy() in the runner names its groups with strcmp against a
# prefix-stripped name. Deriving the list from that function (rather than
# retyping it here) means a change to the policy shows up in the report
# instead of silently drifting away from it. Refuses loudly if the function's
# shape changes, because an empty list would read as "nothing is gated".
params_gated() {
    awk '
        /^static bool group_is_params_heavy/ { inside = 1; next }
        inside && /^}/ { inside = 0 }
        inside {
            rest = $0
            while (match(rest, /strcmp\(name,[[:space:]]*"[A-Za-z_0-9]+"\)/)) {
                seg = substr(rest, RSTART, RLENGTH)
                match(seg, /"[A-Za-z_0-9]+"/)
                print "test_" substr(seg, RSTART + 1, RLENGTH - 2)
                rest = substr(rest, RSTART + RLENGTH)
            }
        }
    ' "$REGISTRY"
}

mode="${1:-list}"
case "$mode" in
    list)
        registered
        ;;
    --count)
        registered | wc -l | tr -d ' '
        ;;
    --match)
        needle="${2:-}"
        [ -n "$needle" ] || {
            echo "test-group-list: --match needs a substring" >&2
            exit 2
        }
        # Substring semantics MUST equal the runner's strstr() over the full
        # name. Pipeline-free per-name test (see tools/scripts/sh_str.sh): a
        # `grep -q` here carries a decision and can invert under pipefail.
        found=0
        while IFS= read -r g; do
            if str_contains "$g" "$needle"; then
                printf '%s\n' "$g"
                found=1
            fi
        done < <(registered)
        [ "$found" = 1 ] || exit 1
        ;;
    --suggest)
        needle="${2:-}"
        [ -n "$needle" ] || exit 0
        # Cheap, dependency-free nearest-candidate. No edit-distance library
        # and no new dependency; two passes that between them cover the two
        # ways an ONLY= is actually wrong here:
        #
        #   1. WRONG SEPARATOR / STRAY PUNCTUATION (`test-bloom`, `<substring>`).
        #      Split on every character a group name cannot contain and try the
        #      resulting tokens LONGEST FIRST. `test-bloom` yields {test, bloom};
        #      trying `bloom` before `test` is what turns a useless "here are
        #      the first 8 of 866" into `test_bloom`.
        #   2. TRAILING TYPO (`boot_phasez`). Shrink the longest token from the
        #      right until it matches.
        #
        # The shrink stops at 3 characters. Below that a probe matches dozens of
        # unrelated groups, and a suggestion list that is mostly noise trains
        # the reader to ignore it.
        GROUPS_CACHE="$(registered)"
        # Caps at 8 hits WITHOUT `| head -8`: an early-exiting downstream
        # SIGPIPEs printf, and under `set -o pipefail` that 141 becomes the
        # status of a function whose status is a decision. Counting in the
        # loop keeps the whole thing pipeline-free (tools/scripts/sh_str.sh).
        suggest_probe() {
            local p="$1" hits="" n=0
            while IFS= read -r g; do
                if str_contains "$g" "$p"; then
                    hits="${hits}${g}
"
                    n=$(( n + 1 ))
                    [ "$n" -ge 8 ] && break
                fi
            done <<<"$GROUPS_CACHE"
            [ -n "$hits" ] || return 1
            printf '%s' "$hits"
            return 0
        }

        tokens="$(printf '%s' "$needle" | tr -c 'A-Za-z_0-9' '\n' |
                  awk 'length($0) >= 3 { print length($0) "\t" $0 }' |
                  sort -rn | cut -f2-)"
        while IFS= read -r tok; do
            [ -n "$tok" ] || continue
            suggest_probe "$tok" && exit 0
        done <<<"$tokens"

        probe="${tokens%%$'\n'*}"
        while [ "${#probe}" -ge 3 ]; do
            suggest_probe "$probe" && exit 0
            probe="${probe%?}"
        done
        exit 0
        ;;
    --params-gated)
        out="$(params_gated)"
        [ -n "$out" ] || {
            echo "test-group-list: FATAL — group_is_params_heavy() in $REGISTRY" \
                 "yielded no names; its shape changed and this parse is stale" >&2
            exit 2
        }
        printf '%s\n' "$out"
        ;;
    *)
        echo "usage: $0 [--count | --match SUBSTR | --suggest SUBSTR | --params-gated]" >&2
        exit 2
        ;;
esac
