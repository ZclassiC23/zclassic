#!/usr/bin/env bash
# Lint gate #22 — framework filename suffix: no foreign-shape suffix (HARD).
#
# "The folder is the type; the filename is the entity" (FRAMEWORK.md Law 1).
# A file whose name ends in ANOTHER shape's suffix — e.g. a *_controller.c
# living in app/services/src/ — lies about its shape and re-introduces the
# exact naming drift the S1 service renames removed. This gate locks that
# in: it is the recurrence guard for "filename matches folder".
#
# Rule (NEGATIVE, not positive): a file in shape folder F may not end with
# the distinctive suffix of a DIFFERENT shape. The seven physical-folder
# shape suffixes are
#   controller service model view job supervisor condition
# (the eighth shape, Event, has no app/ folder — see FRAMEWORK.md §3 row 7 —
# so it carries no suffix to guard here). A file may freely use its OWN
# shape's suffix (services/foo_service.c) or
# a bare entity name (models/block.c, jobs/validate_headers_stage.c) — only
# a FOREIGN-shape suffix is rejected. A positive "must end in _service"
# rule would reject ~190 legitimately entity-named files; this does not.
# Note: _store / _repository name no shape, so they are not foreign
# suffixes (models/mmb_leaf_store.c is a storage entity, not a violation).
#
# Override: a file whose entity name legitimately ends in a shape word —
# e.g. models/file_service.c, where the entity is a "file service" offer,
# a Model, not the Service shape — may carry a top-of-file marker
# '// suffix-ok:<tag>' (no space after the colon, non-empty tag).
set -euo pipefail

cd "$(dirname "$0")/../.."

# folder basename -> this folder's own shape suffix (the one it may keep)
declare -A OWN=(
    [controllers]=controller
    [services]=service
    [models]=model
    [views]=view
    [jobs]=job
    [supervisors]=supervisor
    [conditions]=condition
)
ALL_SHAPES="controller service model view job supervisor condition"

fail=0
violations=()

for folder in "${!OWN[@]}"; do
    d="app/$folder/src"
    [ -d "$d" ] || continue
    own="${OWN[$folder]}"
    while IFS= read -r f; do
        [ -e "$f" ] || continue
        b="$(basename "$f" .c)"
        # Override marker anywhere in the file: skip.
        if grep -qE '//[[:space:]]*suffix-ok:[A-Za-z0-9][A-Za-z0-9_-]*' "$f" 2>/dev/null; then
            continue
        fi
        for shape in $ALL_SHAPES; do
            [ "$shape" = "$own" ] && continue
            case "$b" in
                *_"$shape")
                    violations+=("$f ends in foreign-shape suffix _$shape (this folder's shape: $own)")
                    fail=1
                    break ;;
            esac
        done
    done < <(find "$d" -maxdepth 1 -type f -name '*.c' | sort)
done

if [ "$fail" = "0" ]; then
    echo "check_framework_filename_suffix: clean — no app/ file carries a foreign-shape filename suffix"
    exit 0
fi

echo ""
echo "check_framework_filename_suffix: ${#violations[@]} foreign-shape filename suffix violation(s)"
echo ""
for v in "${violations[@]}"; do
    echo "  $v"
done
echo ""
echo "Fix options:"
echo "  1. Rename the file to its own shape's suffix or a bare entity name."
echo "  2. Move it to the folder whose shape its suffix names."
echo "  3. If the entity name legitimately ends in that shape word, add a"
echo "     top-of-file marker '// suffix-ok:<tag>' explaining why."
exit 1
