#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_installed_acceptance_tools.sh — the public installed-Commons target
# must run with every optional variable unset.
#
# `make c23-commons-installed-acceptance` installs an ordinary product into a
# throwaway prefix and then hands that prefix to the canonical DHT harness.
# The harness refuses to start a single node until every binary it names is
# executable. One of those binaries — its own assertion tool,
# arena_product_journey_c23 — was installed only when
# C23_BETA_INSTALL_ARENA_RUNNER=1, while DHT_ACCEPTANCE_C23 was pointed at the
# prefix unconditionally. So the public target died on the harness's own
# precondition before any composition hook ran, and passed only for whoever
# knew the undocumented flag. A flag that the documented command must be given
# is not optional; it is a missing install.
#
# The invariant, DERIVED on both sides so neither list is hand-written here:
#
#   every binary tools/dev/zcode_dht_acceptance.sh tests with `[ -x ]` before
#   it starts a node must be placed in the install prefix by
#   tools/dev/c23_commons_beta_acceptance.sh OUTSIDE every conditional.
#
# A harness that starts naming a new required binary trips this gate until the
# installed lane installs it. Binaries the harness does not name — arena_runner
# and zclassic23-dev, read only by the arena journey hook — are genuinely
# optional, stay behind their flag, and this gate says nothing about them.
#
# Mode is always FAIL (no baseline): a required binary behind an optional flag
# is never something to grandfather.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/lint/gate_lib.sh"

HARNESS="$ROOT/tools/dev/zcode_dht_acceptance.sh"
LIFECYCLE="$ROOT/tools/dev/node_lifecycle.sh"
INSTALLER="$ROOT/tools/dev/c23_commons_beta_acceptance.sh"
for path in "$HARNESS" "$LIFECYCLE" "$INSTALLER"; do
    [ -r "$path" ] || {
        echo "FAIL: check-installed-acceptance-tools cannot read $path"
        exit 2
    }
done

# 1. What the harness demands of its caller, in the harness's own words.
mapfile -t REQUIRED_VARS < <(
    gate_grep -oE '\[ -x "\$[A-Za-z_][A-Za-z0-9_]*" \]' "$HARNESS" |
        sed -E 's/.*\$([A-Za-z0-9_]+).*/\1/' | sort -u
)
gate_require_scanned "${#REQUIRED_VARS[@]}" 3 check-installed-acceptance-tools \
    "no '[ -x \"\$VAR\" ]' precondition found in $HARNESS"

# 2. tools/dev/node_lifecycle.sh owns those names; each is an override of a
#    caller-facing variable (NODE_BIN <- ZCL_NODE_BIN). The installed lane
#    exports the caller-facing name, so resolve through the owner rather than
#    assuming the two spellings match.
resolve_caller_var() {
    local var="$1" line
    line="$(gate_grep -E "^${var}=\"\\\$\{[A-Za-z_][A-Za-z0-9_]*:-" "$LIFECYCLE" |
        head -1 || true)"
    if [ -n "$line" ]; then
        printf '%s' "$line" | sed -E 's/^[A-Za-z0-9_]+="\$\{([A-Za-z0-9_]+):-.*/\1/'
    else
        printf '%s' "$var"
    fi
}

# 3. The installer as logical lines, each tagged with the conditional depth it
#    executes at. Backslash continuations are joined so a `for product in a \
#    b; do` list reads as one record. Depth counts `if`/`fi` and function
#    bodies: a line inside either is not something the public target is
#    guaranteed to run.
installer_records() {
    awk '
        { line = $0 }
        pending != "" { line = pending " " line; pending = "" }
        line ~ /\\$/ { sub(/[ \t]*\\$/, "", line); pending = line; next }
        {
            stripped = line
            sub(/^[ \t]+/, "", stripped)
            # A brace group (`cmd || { ... }`) and a function body open with a
            # trailing `{` and close with a bare `}`; `if` closes with `fi`.
            # Counting only if/fi would let a brace group closer cancel an
            # enclosing `if` and report guarded lines as unconditional.
            if (stripped ~ /^fi([ \t;].*)?$/ || stripped ~ /^\}([ \t;].*)?$/) {
                if (depth > 0) depth--
            }
            print depth "\t" line
            if (stripped ~ /^if[ \t]/ || stripped ~ /\{$/) depth++
        }
    ' "$INSTALLER"
}
# Comments are dropped: a comment that happens to name an install path is
# prose, not a guarantee.
mapfile -t UNCONDITIONAL < <(installer_records | sed -n 's/^0\t//p' |
    grep -v '^[[:space:]]*#' || true)
gate_require_scanned "${#UNCONDITIONAL[@]}" 20 check-installed-acceptance-tools \
    "no unconditional lines parsed out of $INSTALLER"

installed_unconditionally() {
    local tool="$1" record
    for record in "${UNCONDITIONAL[@]}"; do
        # Copied into the prefix by this script...
        case "$record" in
            *install*\"\$PREFIX/bin/"$tool"\"*) return 0 ;;
        esac
        # ...or produced by `make c23-portable-install` and verified present
        # by the base product loop, which is the same unconditional guarantee.
        case "$record" in
            for\ product\ in\ *)
                case " $record " in
                    *" $tool "*) return 0 ;;
                esac ;;
        esac
    done
    return 1
}

violations=0
for var in "${REQUIRED_VARS[@]}"; do
    caller_var="$(resolve_caller_var "$var")"
    export_line="$(gate_grep -E \
        "^export ${caller_var}=\"\\\$PREFIX/bin/[A-Za-z0-9_.+-]+\"$" \
        "$INSTALLER" | head -1 || true)"
    if [ -z "$export_line" ]; then
        echo "FAIL: $HARNESS requires \$$var, but $INSTALLER does not point"
        echo "      \$$caller_var at a binary in its install prefix."
        violations=$((violations + 1))
        continue
    fi
    tool="$(printf '%s' "$export_line" | sed -E 's|.*/bin/([A-Za-z0-9_.+-]+)"$|\1|')"
    if ! installed_unconditionally "$tool"; then
        echo "FAIL: $tool is required by $HARNESS (as \$$var) but $INSTALLER"
        echo "      only installs it inside a conditional. The public target"
        echo "      make c23-commons-installed-acceptance runs with every"
        echo "      optional variable unset and would refuse on its absence."
        violations=$((violations + 1))
    fi
done

if [ "$violations" -gt 0 ]; then
    echo
    echo "check-installed-acceptance-tools: $violations of ${#REQUIRED_VARS[@]}"
    echo "  harness-required binaries are not guaranteed by the installed lane."
    exit 1
fi

echo "check_installed_acceptance_tools: clean — ${#REQUIRED_VARS[@]} harness-required binaries installed unconditionally"
