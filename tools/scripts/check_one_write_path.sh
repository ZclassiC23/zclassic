#!/usr/bin/env bash
# Lint gate E6 — one-write-path (RATCHET).
#
# The cutover destination has one consensus writer: the reducer advances
# durable cursors and authors UTXO deltas. Legacy chain-state write surfaces
# still exist while B8 extracts/deletes them, so this gate starts as a ratchet:
# any NEW production file/line that writes chain state outside the recorded
# baseline fails the build.
#
# Baseline format:
#   <file>:<line>: <matched source>
#
# Delete baseline lines as B8 removes legacy write surfaces. Do not add lines
# without an ADR; growing this baseline means a new chain writer appeared.
set -euo pipefail

cd "$(dirname "$0")/../.."

BASELINE=tools/scripts/one_write_path_baseline.txt
[ -f "$BASELINE" ] || touch "$BASELINE"

declare -A baseline
baseline_count=0
while IFS= read -r line; do
    line="${line%%#*}"
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    [ -z "$line" ] && continue
    baseline["$line"]=1
    baseline_count=$((baseline_count + 1))
done < "$BASELINE"

pattern='active_chain_set_tip[[:space:]]*\(|coins_view_sqlite_batch_write(_ex)?[[:space:]]*\(|coins_view_cache_flush[[:space:]]*\(|utxo_projection_set_author[[:space:]]*\(|process_new_block[[:space:]]*\(|connect_tip[[:space:]]*\(|disconnect_tip[[:space:]]*\('

# Fail-loud preflight: the scan set MUST be non-empty. If find silently
# produces nothing (a moved/renamed core dir), the loop runs zero times,
# `violations` stays empty, and the gate would print "clean" exit 0 — a
# hollow pass. Assert a known floor instead.
mapfile -t scan_files < <(find app domain lib config tools -type f \( -name '*.c' -o -name '*.h' \) \
    ! -path '*/test/*' \
    ! -path 'tools/scripts/*' \
    ! -path 'tools/lint/*' \
    | sort)
SCAN_FLOOR=200
if [ "${#scan_files[@]}" -lt "$SCAN_FLOOR" ]; then
    echo "check_one_write_path: FATAL — scan set is ${#scan_files[@]} files (< floor $SCAN_FLOOR)." >&2
    echo "  The find roots (app domain lib config tools) produced too few files;" >&2
    echo "  a core dir was likely renamed/moved. Refusing to pass hollow." >&2
    exit 2
fi

violations=()
for f in "${scan_files[@]}"; do
    while IFS= read -r match; do
        key=$(printf '%s:%s\n' "$f" "$match" | sed -E 's/[[:space:]]+/ /g')
        line_content="${key#*:}"
        line_content="${line_content#*:}"
        trimmed="${line_content#"${line_content%%[![:space:]]*}"}"
        case "$trimmed" in
            '/*'*|'*'*|'//'*) continue ;;
        esac
        if printf '%s\n' "$line_content" | grep -qE '//[[:space:]]*one-write-path-ok:[A-Za-z][A-Za-z0-9_-]*'; then
            continue
        fi
        if [ -n "${baseline[$key]+x}" ]; then
            continue
        fi
        violations+=("$key")
    done < <(grep -nE "$pattern" "$f" || true)
done

if [ "${#violations[@]}" -eq 0 ]; then
    echo "check_one_write_path: clean — $baseline_count grandfathered write surface(s), no new ones"
    exit 0
fi

echo ""
echo "check_one_write_path: ${#violations[@]} NEW chain-state write surface(s)"
echo ""
for v in "${violations[@]}"; do
    echo "  $v"
done
echo ""
echo "Consensus chain writes must collapse to the reducer/log path. Move the"
echo "write behind the existing reducer/stage path, delete the legacy writer,"
echo "or add a tightly-scoped '// one-write-path-ok:<tag>' marker only for a"
echo "non-authoritative compatibility wrapper."
exit 1
