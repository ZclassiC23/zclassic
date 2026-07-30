#!/usr/bin/env bash
# check_identity_parser_single.sh — stop a tenth copy of the source-identity
# JSON parser from growing back. Mode: shrink-only ratchet (ZCL_LINT_MODE:
# FAIL default | WARN | UPDATE — same shape as check_supervisor_progress_declared.sh).
#
# WHY THIS EXISTS: "source_id_sha256" was parsed inline in ~9 shell files,
# with `is_sha256`/`is_hex64`/`json_first_string_field` each redefined
# several times over — and the copies had DIVERGED: a greedy `sed
# 's/.*"key":"\(...\)".*/\1/'` returns the LAST occurrence of a repeated
# JSON key, not the first, and `agentbuild` emits `source_id_sha256` FOUR
# times on one line (once baked, three times in nested runtime blocks).
# That produced a false "the live daemon and the dev build have identical
# identities" on 2026-07-28. tools/scripts/source_identity_lib.sh is now
# the one canonical reader (anchored on the FIRST occurrence); this gate
# is the anti-rot check that keeps a new copy from being pasted back in.
#
# Two things counted, per scanned *.sh file:
#   A. a local re-definition of one of the duplicated helper names:
#      is_sha256, is_hex64, json_first_string_field, is_source_id_sha256
#   B. an inline grep/sed extraction of the "source_id_sha256" JSON key —
#      the copy-pasted one-liner the library replaces. Plain JSON
#      construction (`printf '"source_id_sha256":"%s"'`) does NOT count:
#      only a line that also invokes grep or sed is a parser copy.
#
# Shrink-only ratchet: tools/lint/identity_parser_baseline.txt names the
# files with a deliberately-not-yet-migrated copy and how many occurrences
# each carries (a visible to-do, not just a number). A file with no
# baseline row may carry ZERO. A baselined file may carry AT MOST its
# recorded count; once it reaches zero the row is STALE and must be
# deleted, or the ratchet rusts shut at a number nobody is paying down.
#
# RATCHET_CEILING below is the total measured across the baseline the day
# this gate was introduced (2026-07-30) — 15, across 11 files (see the
# baseline header). It may only go DOWN as rows are migrated and deleted;
# summing the baseline and refusing to exceed this ceiling is what stops
# someone from quietly bumping a baseline number up to cover new debt while
# leaving the total unchanged elsewhere — raising the ceiling itself is a
# one-line diff in code review, not a silent runtime edit.
#
# tools/ship.sh carries two occurrences that are PERMANENTLY exempt, not
# ratcheted: they execute on the remote fleet host over ssh/heredoc, which
# cannot source a local library file that only exists in this checkout.
# Each is marked in-place with a `zcl-identity-parser-allow:` comment
# within a few lines of the match, and this gate treats that marker as a
# full exemption rather than counting it as debt.
#
# --selftest plants a fresh inline copy of both classes in a sandboxed
# tools/ tree, proves the gate FAILS on it, then removes it and proves
# PASS — a ratchet that cannot be shown to fail is worse than no gate.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"
# shellcheck source=tools/lint/gate_lib.sh
source tools/lint/gate_lib.sh

GATE=check_identity_parser_single
RATCHET_CEILING=15

# ── --selftest ───────────────────────────────────────────────────────────
if [ "${1:-}" = "--selftest" ]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/tools/scripts"

    plant() { # $1 = extra line to append to the sandbox file (may be empty)
        cat > "$tmp/tools/scripts/selftest_copy.sh" <<EOF
#!/usr/bin/env bash
# a fixture consumer, not a real tool
echo hello
$1
EOF
    }

    self="$PWD/tools/lint/$GATE.sh"
    : > "$tmp/empty_baseline.txt"

    run_sandbox() {
        ZCL_IDENTITY_PARSER_SCAN_ROOT="$tmp/tools" \
        ZCL_IDENTITY_PARSER_BASELINE="$tmp/empty_baseline.txt" \
        ZCL_IDENTITY_PARSER_CEILING=0 \
        ZCL_IDENTITY_PARSER_FILE_FLOOR=1 \
        ZCL_LINT_MODE=FAIL \
        bash "$self" >/dev/null 2>&1
    }

    expect() { # $1 = expected rc class (fail|pass), $2 = message, $3 = extra line
        local want="$1" msg="$2" extra="${3:-}" rc=0
        plant "$extra"
        run_sandbox || rc=$?
        if [ "$want" = "fail" ] && [ "$rc" -eq 0 ]; then
            echo "$GATE: SELFTEST FAILED — $msg" >&2; exit 2
        fi
        if [ "$want" = "pass" ] && [ "$rc" -ne 0 ]; then
            echo "$GATE: SELFTEST FAILED — $msg" >&2; exit 2
        fi
    }

    class_a_copy="$(cat <<'FIXTURE'
is_sha256() { [ "${#1}" -eq 64 ]; }
FIXTURE
)"
    class_b_copy="$(cat <<'FIXTURE'
x=$(printf %s "$1" | grep -oE '"source_id_sha256"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed -E 's/.*"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/')
FIXTURE
)"
    class_b_marked="$(cat <<'FIXTURE'
# zcl-identity-parser-allow: fixture, cannot source the lib here
x=$(printf %s "$1" | grep -oE '"source_id_sha256"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1)
FIXTURE
)"

    expect pass "a clean consumer with no copy was reported as a violation" ""
    expect fail "an inline is_sha256() definition (class A) did not fail the gate" \
        "$class_a_copy"
    expect pass "reverting the class-A copy did not clear the violation" ""
    expect fail "an inline grep/sed source_id_sha256 extraction (class B) did not fail the gate" \
        "$class_b_copy"
    expect pass "reverting the class-B copy did not clear the violation" ""
    # A marked (allowlisted) copy must NOT count as debt.
    expect pass "a zcl-identity-parser-allow-marked copy was still counted as debt" \
        "$class_b_marked"

    echo "[$GATE] SELFTEST PASS (clean passes; class-A and class-B copies fail; reverted copies pass; marked/allowlisted copy does not count)"
    exit 0
fi

# ── Scan set ─────────────────────────────────────────────────────────────
MODE="${ZCL_LINT_MODE:-FAIL}"
BASELINE="${ZCL_IDENTITY_PARSER_BASELINE:-tools/lint/identity_parser_baseline.txt}"
SCAN_ROOT="${ZCL_IDENTITY_PARSER_SCAN_ROOT:-tools}"
CEILING="${ZCL_IDENTITY_PARSER_CEILING:-$RATCHET_CEILING}"

# The canonical library and this gate itself are excluded: they are the
# thing every other file is compared against (and this file's own header
# comment, and the library's, necessarily quote the exact patterns they
# forbid elsewhere — matching those would be a self-inflicted false
# positive, not a real duplicate).
EXCLUDE_RE='(^|/)source_identity_lib\.sh$|(^|/)check_identity_parser_single\.sh$'

mapfile -t scan_files < <(find "$SCAN_ROOT" -type f -name '*.sh' 2>/dev/null | grep -Ev "$EXCLUDE_RE" || true)
gate_require_scanned "${#scan_files[@]}" "${ZCL_IDENTITY_PARSER_FILE_FLOOR:-5}" "$GATE" \
    "no *.sh files found under $SCAN_ROOT — the scan root moved"

# ── Per-file counts ──────────────────────────────────────────────────────
# Emits: path<TAB>count
#
# Class A: a local definition (start-of-line, ignoring indent) of one of
# the duplicated helper names.
# Class B: a line containing the literal JSON-key pattern
# "source_id_sha256"[:space:]*: (plain or backslash-escaped quotes, so a
# copy embedded in a heredoc/ssh command string is still counted) AND
# invoking grep or sed on the same line — plain JSON construction
# (printf '"source_id_sha256":"%s"') is excluded on purpose.
# A `zcl-identity-parser-allow` marker within the previous 8 lines exempts
# a class-B match entirely (the two ship.sh remote-side sites).
scan_counts() {
    # The class-B needle is built from sprintf("%c") rather than typed as a
    # quoted/backslashed literal: the data being searched is shell SOURCE
    # TEXT that itself contains a literal backslash and literal quote
    # characters (e.g. \"source_id_sha256\" inside a double-quoted ssh
    # command string), and typing that as an awk string literal is exactly
    # the kind of double-escaping that is easy to get subtly wrong. Building
    # it one character at a time removes any ambiguity about how many
    # backslashes awk's own lexer consumes.
    awk '
        BEGIN {
            bs = sprintf("%c", 92); q = sprintf("%c", 34)
            plain_needle = q "source_id_sha256" q "[[:space:]]*:"
            esc_needle   = bs q "source_id_sha256" bs q "[[:space:]]*:"
        }
        FNR == 1 { if (NR > 1) emit(); path = FILENAME; count = 0; last_allow = -1000 }
        {
            line = $0
            if (line ~ /^[ \t]*(is_sha256|is_hex64|json_first_string_field|is_source_id_sha256)[ \t]*\(\)/) {
                count++
            }
            if (line ~ /zcl-identity-parser-allow/) last_allow = FNR

            has_needle = (index(line, plain_needle) > 0 || index(line, esc_needle) > 0)
            has_tool = (index(line, "grep") > 0 || index(line, "sed") > 0)
            if (has_needle && has_tool) {
                if (FNR - last_allow > 8) count++
            }
        }
        END { emit() }
        function emit() {
            if (path != "" && count > 0) printf "%s\t%d\n", path, count
        }
    ' "${scan_files[@]}"
}

mapfile -t COUNT_ROWS < <(scan_counts)

declare -A BASELINED=()
gate_load_kv_file "$BASELINE" BASELINED
baseline_count="${#BASELINED[@]}"
baseline_sum=0
for path in "${!BASELINED[@]}"; do
    baseline_sum=$(( baseline_sum + ${BASELINED[$path]} ))
done

declare -A HIT=()
violations=()
tolerated=()
total_copies=0

for row in "${COUNT_ROWS[@]}"; do
    IFS=$'\t' read -r path debt <<< "$row"
    total_copies=$(( total_copies + debt ))
    allowed="${BASELINED[$path]:-}"
    if [ -n "$allowed" ]; then
        HIT["$path"]=1
        if [ "$debt" -le "$allowed" ]; then
            tolerated+=("$path ($debt/$allowed)")
            continue
        fi
        violations+=("$path — $debt copy/copies found, baseline allows $allowed")
    else
        violations+=("$path — $debt copy/copies found, not in the baseline (new file may carry ZERO)")
    fi
done

# A baseline row whose file is now clean (or gone) must be deleted, or the
# ratchet rusts shut at a stale number.
stale=()
for path in "${!BASELINED[@]}"; do
    [ -z "${HIT[$path]+x}" ] && stale+=("$path (baseline says ${BASELINED[$path]}, actual 0)")
done

# The ceiling check: the baseline FILE's own recorded total may never
# exceed the ceiling this gate was introduced with. Bumping an individual
# row up while deleting/lowering another to compensate still trips this if
# the sum grows past the original honest count; the only way to legitimately
# raise a row is by lowering the ceiling constant in this script, which is a
# visible source diff, not a data-file edit.
if [ "$baseline_sum" -gt "$CEILING" ]; then
    echo ""
    echo "[$GATE] baseline sum ($baseline_sum) exceeds the ratchet ceiling ($CEILING)"
    echo "        in $BASELINE — the baseline was edited upward. Lower it back,"
    echo "        or lower RATCHET_CEILING in this script if debt has genuinely"
    echo "        and legitimately grown (a change that belongs in code review,"
    echo "        not a quiet data-file edit)."
    violations+=("$BASELINE — baseline sum $baseline_sum exceeds ceiling $CEILING")
fi

if [ "$MODE" = "UPDATE" ]; then
    {
        echo "# check_identity_parser_single baseline — files still carrying an inline"
        echo "# copy of the source-identity JSON parser (a local is_sha256/is_hex64/"
        echo "# json_first_string_field/is_source_id_sha256 definition, or an inline"
        echo "# grep/sed extraction of the \"source_id_sha256\" JSON key) instead of"
        echo "# sourcing tools/scripts/source_identity_lib.sh."
        echo "#"
        echo "# Format: <path> <count>.  COUNTS MAY ONLY SHRINK."
        echo "#"
        echo "# Regenerate: ZCL_LINT_MODE=UPDATE tools/lint/$GATE.sh"
        for row in "${COUNT_ROWS[@]}"; do
            IFS=$'\t' read -r path debt <<< "$row"
            echo "$path $debt"
        done | sort
    } > "$BASELINE"
    echo "[$GATE] baseline UPDATED: $BASELINE"
    exit 0
fi

fail=0
if [ "${#violations[@]}" -gt 0 ]; then
    echo ""
    echo "[$GATE] ${#violations[@]} violation(s) — a new or grown inline copy of the"
    echo "        source-identity JSON parser:"
    printf '  %s\n' "${violations[@]}" | sort
    echo ""
    echo "  Source tools/scripts/source_identity_lib.sh instead:"
    echo "    zcl_is_sha256 / zcl_json_first_string / zcl_json_first_sha256 /"
    echo "    zcl_binary_source_id"
    echo "  Raising a number in $BASELINE is NOT a fix; counts may only shrink."
    fail=1
fi

if [ "${#stale[@]}" -gt 0 ]; then
    echo ""
    echo "[$GATE] ${#stale[@]} STALE baseline row(s) — the file no longer carries"
    echo "        any inline copy. Delete them from $BASELINE:"
    printf '  %s\n' "${stale[@]}" | sort
    fail=1
fi

if [ "$fail" != "0" ] && [ "$MODE" = "FAIL" ]; then
    exit 1
fi

echo "[$GATE] PASS (${#scan_files[@]} *.sh files scanned, ${#COUNT_ROWS[@]} carrying a copy, $total_copies total, $baseline_count baselined file(s) summing to $baseline_sum/$CEILING, ${#tolerated[@]} tolerated)"
