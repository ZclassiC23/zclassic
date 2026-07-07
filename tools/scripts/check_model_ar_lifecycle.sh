#!/usr/bin/env bash
# check_model_ar_lifecycle.sh
#
# Model saves must not hand-run the ActiveRecord callback internals. Use
# AR_BEGIN_SAVE / AR_ADHOC_SAVE / AR_FINISH_SAVE so validation, callbacks,
# logging, and future lifecycle hooks stay mechanically consistent.

set -euo pipefail

cd "$(dirname "$0")/../.."

shopt -s nullglob
files=(app/models/src/*.c)
if (( ${#files[@]} < 20 )); then
    echo "FAIL: check_model_ar_lifecycle scanned only ${#files[@]} model file(s)"
    echo "      expected app/models/src/*.c; gate would be hollow"
    exit 2
fi

violations=()
for f in "${files[@]}"; do
    while IFS= read -r line; do
        trimmed="${line#*:}"
        trimmed="${trimmed#*:}"
        trimmed="${trimmed#"${trimmed%%[![:space:]]*}"}"
        case "$trimmed" in
            '/*'*|'*'*|'//'*) continue ;;
        esac
        if printf '%s\n' "$line" | grep -qE 'ar-lifecycle-ok:[A-Za-z][A-Za-z0-9_-]*'; then
            continue
        fi
        violations+=("$line")
    done < <(grep -nHE '\bar_run_(before|after)_save[[:space:]]*\(' "$f" || true)
done

if (( ${#violations[@]} == 0 )); then
    echo "check_model_ar_lifecycle: clean — model saves use AR lifecycle macros"
    exit 0
fi

echo "FAIL: app model sources call AR save callbacks directly:"
printf '  %s\n' "${violations[@]}"
echo ""
echo "Use AR_BEGIN_SAVE / AR_ADHOC_SAVE / AR_FINISH_SAVE so validation and"
echo "before/after-save hooks remain in one defensive lifecycle."
exit 1
