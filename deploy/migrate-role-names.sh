#!/usr/bin/env bash
# migrate-role-names.sh — rename a numbered instance unit/datadir onto the
# canonical role name.
#
# NAMING LAW (owner directive): unit names and datadirs are ROLE-based
# ("serve", "mint"), never instance-numbered ("serve1", "mint3"). This
# script is the one-time, idempotent bridge from an old numbered install to
# the canonical name, for an operator who already has a numbered unit/datadir
# running on this host. It is operator-run tooling — it never runs from an
# agent workflow and never touches anything outside this host's own
# `systemctl --user` units and `$HOME/.zclassic-c23-*` datadirs.
#
# What it does, in order:
#   1. Finds an installed ~/.config/systemd/user/zcl-serve<N>.service unit
#      (if any), stops it, writes a renamed copy as zcl-serve.service
#      (ExecStart, working paths, and everything else are copied through
#      unchanged except the "<N>" instance-number strings — a content
#      -addressed "-<githash>" suffix in an ExecStart path is untouched, it
#      is not an instance number), enables + starts the new unit, and
#      disables (but does not delete) the old one.
#   2. Renames ~/.zclassic-c23-serve<N> -> ~/.zclassic-c23-serve, ONLY if the
#      destination does not already exist.
#   3. Renames a numbered ~/.zclassic-c23-mint<N> -> ~/.zclassic-c23-mint,
#      ONLY if the destination does not already exist.
#
# Every step is logged. Every failure is loud (set -euo pipefail) and this
# script refuses to guess when a step is ambiguous (e.g. two numbered units
# found) rather than pick one silently.
#
# Usage:
#   deploy/migrate-role-names.sh --dry-run     # print the plan, change nothing
#   deploy/migrate-role-names.sh               # execute the plan
#
# Idempotent: a second run after a successful migration finds the canonical
# names already in place and reports NOOP for each step instead of failing.

set -euo pipefail
shopt -s nullglob

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNIT_DIR="$HOME/.config/systemd/user"
NEW_UNIT_NAME="zcl-serve.service"
NEW_UNIT_PATH="$UNIT_DIR/$NEW_UNIT_NAME"
TEMPLATE_UNIT_PATH="$SCRIPT_DIR/zcl-serve.service"

DRY_RUN=0

log() {
    printf '[migrate-role-names] %s\n' "$*"
}

fail() {
    printf '[migrate-role-names] REFUSING: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage: deploy/migrate-role-names.sh [--dry-run]

Renames a numbered zcl-serve<N>.service unit and its
~/.zclassic-c23-serve<N> / ~/.zclassic-c23-mint<N> datadirs onto the
canonical role names (zcl-serve.service, ~/.zclassic-c23-serve,
~/.zclassic-c23-mint). --dry-run prints the plan and changes nothing.
EOF
}

for arg in "$@"; do
    case "$arg" in
        --dry-run)
            DRY_RUN=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            fail "unknown argument: $arg"
            ;;
    esac
done

if [ "$(id -u)" -eq 0 ]; then
    fail "refuse to run as root — this migrates a per-user (systemctl --user) install"
fi

if [ "$DRY_RUN" -eq 1 ]; then
    log "DRY RUN — no changes will be made"
fi

if ! command -v systemctl >/dev/null 2>&1; then
    fail "systemctl not found on PATH"
fi

# ── step 1: find and rename numbered zcl-serve<N>.service ─────────────────
old_units=("$UNIT_DIR"/zcl-serve[0-9]*.service)
old_unit=""
if [ "${#old_units[@]}" -gt 1 ]; then
    fail "ambiguous: multiple numbered serve units found (${old_units[*]}) — resolve manually, then re-run"
elif [ "${#old_units[@]}" -eq 1 ]; then
    old_unit="${old_units[0]}"
fi

old_serve_datadir=""
unit_verdict="NOOP"

if [ -n "$old_unit" ]; then
    old_unit_name="$(basename "$old_unit")"
    log "found numbered unit: $old_unit_name"

    # Extract the datadir this unit was actually running with — authoritative
    # for step 2, and preserves whatever ExecStart binary path (including a
    # content-addressed mint-candidates path) the old unit already used.
    old_serve_datadir="$(grep -oE -- '-datadir=[^ \\]+' "$old_unit" | head -1 | sed 's/^-datadir=//')" || true

    if [ -e "$NEW_UNIT_PATH" ]; then
        log "WARN: $NEW_UNIT_PATH already exists — leaving $old_unit_name untouched (remove the old unit file manually once you have confirmed the new one is correct)"
        unit_verdict="SKIPPED (new unit already present)"
    elif [ "$DRY_RUN" -eq 1 ]; then
        log "[dry-run] would: systemctl --user stop $old_unit_name"
        log "[dry-run] would: write $NEW_UNIT_PATH (copy of $old_unit_name with instance-number strings removed)"
        log "[dry-run] would: systemctl --user daemon-reload && enable --now $NEW_UNIT_NAME"
        log "[dry-run] would: systemctl --user disable $old_unit_name"
        unit_verdict="DRY-RUN (would migrate)"
    else
        log "stopping $old_unit_name"
        systemctl --user stop "$old_unit_name" || log "  (stop reported non-zero — continuing; unit may already be stopped)"

        log "writing $NEW_UNIT_PATH"
        tmp_unit="$(mktemp "$UNIT_DIR/.zcl-serve.service.XXXXXX")"
        # Strip only the instance-number digits that immediately follow
        # "serve" in a datadir path or unit/description string. A
        # content-addressed "-<githash>" suffix elsewhere on the line (e.g.
        # an ExecStart mint-candidates path) has no digits immediately after
        # the literal word "serve" and is left untouched.
        sed -E \
            -e 's/(\.zclassic-c23-serve)[0-9]+/\1/g' \
            -e 's/(zcl-serve)[0-9]+/\1/g' \
            -e 's/([^-[:alnum:]]serve)[0-9]+\b/\1/g' \
            "$old_unit" > "$tmp_unit"
        chmod 644 "$tmp_unit"
        mv "$tmp_unit" "$NEW_UNIT_PATH"

        systemctl --user daemon-reload
        systemctl --user enable --now "$NEW_UNIT_NAME"
        systemctl --user disable "$old_unit_name" || log "  (disable reported non-zero — continuing)"
        log "$old_unit remains on disk, disabled and stopped — remove it by hand once you have confirmed $NEW_UNIT_NAME is healthy"
        unit_verdict="MIGRATED ($old_unit_name -> $NEW_UNIT_NAME)"
    fi
else
    log "no numbered zcl-serve<N>.service unit found under $UNIT_DIR — nothing to migrate for the unit"
    if [ ! -e "$NEW_UNIT_PATH" ] && [ -f "$TEMPLATE_UNIT_PATH" ]; then
        log "note: $NEW_UNIT_NAME is not installed either. This script only migrates an EXISTING numbered install; to install fresh, copy $TEMPLATE_UNIT_PATH to $UNIT_DIR and edit its ExecStart, then 'systemctl --user daemon-reload && systemctl --user enable --now $NEW_UNIT_NAME'."
    fi
fi

# ── rename helper: $1=old dir $2=new dir $3=label ──────────────────────────
rename_datadir() {
    local old="$1" new="$2" label="$3"

    if [ -z "$old" ]; then
        log "$label: no old numbered datadir found — nothing to rename"
        return 0
    fi
    if [ ! -d "$old" ]; then
        log "$label: recorded/expected old datadir $old does not exist — skipping"
        return 0
    fi
    if [ -e "$new" ]; then
        log "$label: WARN: $new already exists — leaving $old in place untouched"
        return 0
    fi
    if [ "$DRY_RUN" -eq 1 ]; then
        log "[dry-run] $label: would: mv $old $new"
        return 0
    fi

    local old_fs new_parent_fs
    old_fs="$(stat -c %d "$old")"
    new_parent_fs="$(stat -c %d "$(dirname "$new")")"
    if [ "$old_fs" != "$new_parent_fs" ]; then
        fail "$label: $old and $(dirname "$new") are on different filesystems — refuse to silently copy instead of rename; move it by hand"
    fi

    mv "$old" "$new"
    log "$label: renamed $old -> $new"
}

# ── step 2: serve datadir ───────────────────────────────────────────────────
new_serve_dir="$HOME/.zclassic-c23-serve"
candidate_serve_dir="$old_serve_datadir"
if [ -z "$candidate_serve_dir" ]; then
    serve_glob=("$HOME"/.zclassic-c23-serve[0-9]*)
    if [ "${#serve_glob[@]}" -gt 1 ]; then
        fail "ambiguous: multiple numbered serve datadirs found (${serve_glob[*]}) — resolve manually, then re-run"
    elif [ "${#serve_glob[@]}" -eq 1 ]; then
        candidate_serve_dir="${serve_glob[0]}"
    fi
fi
rename_datadir "$candidate_serve_dir" "$new_serve_dir" "serve datadir"

# ── step 3: mint datadir ────────────────────────────────────────────────────
new_mint_dir="$HOME/.zclassic-c23-mint"
mint_glob=("$HOME"/.zclassic-c23-mint[0-9]*)
candidate_mint_dir=""
if [ "${#mint_glob[@]}" -gt 1 ]; then
    fail "ambiguous: multiple numbered mint datadirs found (${mint_glob[*]}) — resolve manually, then re-run"
elif [ "${#mint_glob[@]}" -eq 1 ]; then
    candidate_mint_dir="${mint_glob[0]}"
fi
rename_datadir "$candidate_mint_dir" "$new_mint_dir" "mint datadir"

# ── verdict ──────────────────────────────────────────────────────────────
if [ "$DRY_RUN" -eq 1 ]; then
    log "VERDICT: DRY-RUN complete — unit: $unit_verdict. Re-run without --dry-run to apply."
else
    log "VERDICT: unit: $unit_verdict"
fi
