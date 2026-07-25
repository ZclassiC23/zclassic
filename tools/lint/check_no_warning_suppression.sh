#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_no_warning_suppression.sh — a blanket warning suppression may not
# enter or re-enter a build surface unannounced.
#
# Why this gate exists
# --------------------
# `-Wunused-result` is the SAME diagnostic GCC and Clang use to report
# `[[nodiscard]]`. `-Wno-unused-result` therefore does not merely silence
# glibc's `warn_unused_result` on write/read/realloc — it disables the
# repository's own result-type discipline project-wide, silently. A result
# type could be annotated `[[nodiscard]]` and every dropped return would
# still compile clean. That flag rode in as an unexplained copy-forward
# default and survived for the life of the repo because nothing watched it,
# and it had propagated by copy-paste into six more compile rules.
#
# `-Wno-stringop-overflow` came from the same copy-forward and hides a
# memory-safety diagnostic class, so it is watched here too.
#
# The contract
# ------------
# Neither suppression may appear on a tracked BUILD SURFACE — a makefile, a
# C translation unit or header (the `#pragma GCC diagnostic ignored` spelling
# is the same suppression by another route), or a tracked shell script that
# drives a compiler — unless the line, or the line
# immediately above it, carries an explicit
#
#     suppression-ok: <reason>
#
# marker. Any comment syntax works; the marker must be followed by a
# non-empty reason. The point is not to forbid the flag forever, it is to
# make every instance a decision somebody wrote down and a reviewer can grep,
# instead of a default nobody ever chose.
#
# vendor/ is out of scope: those are third-party build recipes this project
# does not author. `.clangd` is out of scope too — it tunes editor
# diagnostics, never the code the compiler emits, so a suppression there
# cannot defeat [[nodiscard]] in a build. The build is the contract.
#
# Hollow-gate protection: the production path runs the detector fixtures
# before it scans, so the gate cannot report "clean" while its own matcher is
# broken, and an empty scan set is FATAL (exit 2), never a pass.
set -euo pipefail

SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
SELF_REL="tools/lint/check_no_warning_suppression.sh"

# The two suppressions under contract, in flag form and in pragma form. Kept
# as one list so the two spellings can never drift apart.
FLAG_RE='-Wno-(unused-result|stringop-overflow)'
PRAGMA_RE='diagnostic[[:space:]]+ignored[[:space:]]+"-W(unused-result|stringop-overflow)'
MARKER_RE='suppression-ok:[[:space:]]*[^[:space:]]'

# --------------------------------------------------------------------------
# Scanner. Takes a repository root; prints one line per unmarked hit.
#   0 = clean, 1 = unmarked suppression found, 2 = the scan itself was blind.
# --------------------------------------------------------------------------
scan_repo() {
    local root="$1" tracked files scanned=0 hits f

    # NUL-delimited through a file: a command substitution would drop the
    # separators and a pipeline would hide the file list in a subshell.
    tracked="$(mktemp "${TMPDIR:-/tmp}/zcl-warnsupp-files.XXXXXX")"
    git -C "$root" ls-files -z -- \
        'Makefile' 'makefile' 'GNUmakefile' '*.mk' '*.mak' '*.make' \
        '*.c' '*.h' '*.sh' > "$tracked" 2>/dev/null || true

    files=()
    while IFS= read -r -d '' f; do
        case "$f" in
            vendor/*)    continue ;;  # third-party recipes, not ours
            "$SELF_REL") continue ;;  # this gate names the patterns it bans
        esac
        [ -f "$root/$f" ] || continue   # tracked-but-deleted: in-progress rm
        files+=("$f")
    done < "$tracked"
    rm -f "$tracked"

    scanned=${#files[@]}
    if [ "$scanned" -lt 1 ]; then
        echo "check_no_warning_suppression: FATAL — build-surface scan set is empty" >&2
        return 2
    fi

    hits="$(cd "$root" && awk \
        -v flag_re="$FLAG_RE" -v pragma_re="$PRAGMA_RE" -v marker_re="$MARKER_RE" '
        FNR == 1 { prev = "" }
        {
            if ($0 ~ flag_re || $0 ~ pragma_re) {
                if ($0 !~ marker_re && prev !~ marker_re)
                    printf "%s:%d:%s\n", FILENAME, FNR, $0
            }
            prev = $0
        }
    ' "${files[@]}" || true)"

    if [ -n "$hits" ]; then
        printf '%s\n' "$hits" | while IFS= read -r line; do
            echo "FAIL: unmarked warning suppression — $line" >&2
        done
        echo "check_no_warning_suppression: FAIL — hits=$(printf '%s\n' "$hits" | grep -c .) scanned=$scanned" >&2
        echo "  -Wno-unused-result also disables [[nodiscard]] reporting; -Wno-stringop-overflow hides" >&2
        echo "  a memory-safety diagnostic. Delete the flag, or state the reason on the line above it:" >&2
        echo "      # suppression-ok: <why this build surface genuinely needs it>" >&2
        return 1
    fi

    echo "check_no_warning_suppression: clean — scanned=$scanned build surfaces"
    return 0
}

# --------------------------------------------------------------------------
# Detector fixtures on an isolated throwaway repository. Proves the matcher
# still (a) passes a clean tree, (b) trips on the flag form, (c) trips on the
# pragma form, (d) is exempted by a same-line marker, (e) is exempted by a
# preceding-line marker, (f) is NOT exempted by a marker with no reason,
# (g) ignores vendor/, and (h) fails loud on an empty scan set.
# --------------------------------------------------------------------------
fixture_fail() { echo "check_no_warning_suppression selftest: FAIL — $*" >&2; exit 1; }

run_fixtures() {
    local tmp repo empty rc out
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/zcl-warnsupp.XXXXXX")"
    FIXTURE_TMP="$tmp"
    trap 'rm -rf "${FIXTURE_TMP:-}"' EXIT HUP INT TERM
    repo="$tmp/repo"; empty="$tmp/empty"
    mkdir -p "$repo/tools/lint" "$repo/vendor" "$empty"
    git -C "$repo" init -q
    git -C "$empty" init -q

    printf 'CFLAGS = -std=c23 -Wall -Wextra -Werror\n' > "$repo/Makefile"
    printf 'int main(void){return 0;}\n'               > "$repo/a.c"
    printf 'CFLAGS += -Wno-unused-result\n'            > "$repo/vendor/third_party.mk"
    git -C "$repo" add Makefile a.c vendor/third_party.mk

    # (a) clean tree passes, with a receipt
    rc=0; out="$(scan_repo "$repo" 2>&1)" || rc=$?
    [ "$rc" -eq 0 ] || fixture_fail "clean fixture rejected (rc=$rc): $out"
    grep -Fq 'check_no_warning_suppression: clean' <<<"$out" ||
        fixture_fail "clean fixture omitted receipt: $out"

    # (g) vendor/ stayed out of scope though it carries the flag
    ! grep -Fq 'vendor/third_party.mk' <<<"$out" ||
        fixture_fail "vendor/ must be out of scope: $out"

    # (b) flag form trips
    printf 'CFLAGS += -Wno-unused-result\n' >> "$repo/Makefile"
    rc=0; out="$(scan_repo "$repo" 2>&1)" || rc=$?
    [ "$rc" -eq 1 ] || fixture_fail "flag form expected exit 1, got $rc: $out"
    grep -Fq 'Makefile:2' <<<"$out" ||
        fixture_fail "flag form omitted source line: $out"

    # (f) a marker word with no reason does NOT exempt
    printf '# suppression-ok:\n' > "$repo/Makefile"
    printf 'CFLAGS += -Wno-unused-result\n' >> "$repo/Makefile"
    rc=0; out="$(scan_repo "$repo" 2>&1)" || rc=$?
    [ "$rc" -eq 1 ] || fixture_fail "empty-reason marker must not exempt, got $rc: $out"

    # (e) preceding-line marker with a reason exempts
    printf '# suppression-ok: fixture proves the marker is honoured\n' > "$repo/Makefile"
    printf 'CFLAGS += -Wno-unused-result\n' >> "$repo/Makefile"
    rc=0; out="$(scan_repo "$repo" 2>&1)" || rc=$?
    [ "$rc" -eq 0 ] || fixture_fail "preceding-line marker not honoured (rc=$rc): $out"

    # (d) same-line marker exempts
    printf 'CFLAGS += -Wno-stringop-overflow  # suppression-ok: fixture\n' > "$repo/Makefile"
    rc=0; out="$(scan_repo "$repo" 2>&1)" || rc=$?
    [ "$rc" -eq 0 ] || fixture_fail "same-line marker not honoured (rc=$rc): $out"

    # (c) pragma form trips — the other route to the same suppression
    printf 'CFLAGS = -Wall\n' > "$repo/Makefile"
    printf '#pragma GCC diagnostic ignored "-Wunused-result"\nint main(void){return 0;}\n' \
        > "$repo/a.c"
    rc=0; out="$(scan_repo "$repo" 2>&1)" || rc=$?
    [ "$rc" -eq 1 ] || fixture_fail "pragma form expected exit 1, got $rc: $out"
    grep -Fq 'a.c:1' <<<"$out" || fixture_fail "pragma form omitted source line: $out"

    # (h) an empty scan set is FATAL, never a pass
    rc=0; out="$(scan_repo "$empty" 2>&1)" || rc=$?
    [ "$rc" -eq 2 ] || fixture_fail "empty scan expected exit 2, got $rc: $out"
    grep -Fq 'FATAL' <<<"$out" || fixture_fail "empty scan omitted FATAL: $out"

    rm -rf "$tmp"; FIXTURE_TMP=""; trap - EXIT HUP INT TERM
    return 0
}

if [ "${1:-}" = "--self-test" ]; then
    run_fixtures
    echo "check_no_warning_suppression selftest: PASS"
    exit 0
fi

ROOT="${1:-$(cd "$(dirname "$SCRIPT_PATH")/../.." && pwd)}"
[ -d "$ROOT" ] || {
    echo "check_no_warning_suppression: FATAL — root is not a directory: $ROOT" >&2
    exit 2
}
git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
    echo "check_no_warning_suppression: FATAL — not a Git worktree: $ROOT" >&2
    exit 2
}

# The detector proves itself before it certifies the tree: a silently broken
# matcher must not be able to print "clean".
run_fixtures

scan_repo "$(cd "$ROOT" && pwd)"
