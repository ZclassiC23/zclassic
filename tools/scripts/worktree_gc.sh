#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Worktree + merged-branch garbage collector (environment hygiene).
#
# DRY-RUN BY DEFAULT — prints the classification table and the reclaimable
# byte total; nothing is removed unless --apply is passed explicitly.
# Bucket method (formalized from a one-off 2026-07-16 manual audit; recover
# that doc with `git log --follow -- docs/work/worktree-cleanup-2026-07-16.md`
# if the original write-up is ever needed):
#
#   locked         -> skip unconditionally (git worktree lock = live lane)
#   merged + clean -> removal candidate (git worktree remove + git branch -D)
#   merged + dirty -> CAUTION, kept (uncommitted content — review by hand)
#   unmerged       -> KEEP (commits not on the main ref)
#
# Orphan dirs (present under .claude/worktrees/ but absent from
# `git worktree list --porcelain`) are REPORTED only — never auto-removed,
# even with --apply.
#
# HARD PROTECT LIST (never removed, even with --apply):
#   - the main checkout itself;
#   - $HOME/.local/state/zclassic23-quality/* (registered quality checkouts);
#   - /tmp/zcl-pristine-* (registered pristine reference checkout);
#   - the 2026-07-18 ACTIVE LANES: wf_rom-fetch-engine, wf_install-e2e,
#     wf_env-hygiene, wf_crashlog, wf_sanitizer, wf_loop-speed;
#   - any branch with commits not merged into the main ref (CRITICAL
#     EXAMPLE: lane/rom-keystone holds the unmerged shielded ROM keystone
#     commit — it must survive every sweep);
#   - any worktree with uncommitted changes (git status --porcelain).
#
# Second sweep: merged local branches with NO worktree. Ancestor-of-main and
# not checked out anywhere => deletion candidate (`git branch -D` on apply).
#
# `git worktree remove` is run WITHOUT --force: it independently re-checks
# for uncommitted/untracked content and refuses if a lane drifted between
# classification and execution. That refusal is the safety net, not a bug.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
MAIN_REF="${ZCL_GC_MAIN_REF:-main}"

APPLY=0

usage() {
    cat <<'USAGE'
usage: tools/scripts/worktree_gc.sh [--apply] [--dry-run]

Classify every registered worktree and every local branch into
SAFE / KEEP / CAUTION / LOCKED / PROTECTED / ORPHAN buckets and print the
summary table with total reclaimable bytes.

Default is DRY-RUN: classify and report only. --apply executes removals for
the SAFE bucket (git worktree remove, no --force; then git branch -D) and
deletes merged branches that have no worktree. Orphan dirs are never
auto-removed, even under --apply.

Environment:
  ZCL_GC_MAIN_REF   main ref used for merge checks (default: main)
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --apply) APPLY=1 ;;
        --dry-run|--plan) APPLY=0 ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'worktree-gc: unknown arg %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

# The 2026-07-18 active lanes — hard protected by name, never removed.
PROTECTED_NAMES=" wf_rom-fetch-engine wf_install-e2e wf_env-hygiene wf_crashlog wf_sanitizer wf_loop-speed "

is_protected_name() {
    case "$PROTECTED_NAMES" in
        *" $1 "*) return 0 ;;
        *) return 1 ;;
    esac
}

# Registered quality-lane / pristine checkouts live outside .claude/worktrees
# and are out of scope anyway; these prefixes are belt-and-braces.
is_protected_path() {
    case "$1" in
        "$HOME"/.local/state/zclassic23-quality/*) return 0 ;;
        /tmp/zcl-pristine-*) return 0 ;;
        *) return 1 ;;
    esac
}

human() {
    if command -v numfmt >/dev/null 2>&1; then
        numfmt --to=iec --suffix=B "$1" 2>/dev/null || printf '%s' "$1"
    else
        printf '%s' "$1"
    fi
}

# Left-truncate to width w (keep the distinguishing tail, mark with ...).
fit() {
    local w="$1" s="$2"
    if [ "${#s}" -gt "$w" ]; then
        printf '...%s' "${s: -$((w - 3))}"
    else
        printf '%s' "$s"
    fi
}

dir_bytes() {
    du -sb -- "$1" 2>/dev/null | awk '{print $1}' || printf '0'
}

# The main checkout is always the first record of `git worktree list`.
MAIN_ROOT="$(git -C "$REPO_ROOT" worktree list --porcelain |
    sed -n '1s/^worktree //p')"
if [ -z "$MAIN_ROOT" ]; then
    echo "worktree-gc: FATAL — cannot resolve the main checkout" >&2
    exit 2
fi
if ! git -C "$MAIN_ROOT" rev-parse --verify --quiet "$MAIN_REF" >/dev/null; then
    echo "worktree-gc: FATAL — ref '$MAIN_REF' does not resolve" >&2
    exit 2
fi
WT_BASE="$MAIN_ROOT/.claude/worktrees"

# --- enumerate registered worktrees -----------------------------------------
# tab-separated: path <TAB> head <TAB> branch-or-(detached) <TAB> locked(yes|no)
REGISTERED="$(
    git -C "$MAIN_ROOT" worktree list --porcelain | awk '
        /^worktree /{ if (path != "") print path "\t" head "\t" branch "\t" locked;
                      path=$2; head=""; branch=""; locked="no"; next }
        /^HEAD /{ head=$2 }
        /^branch /{ branch=$2 }
        /^locked/{ locked="yes" }
        /^detached/{ branch="(detached)" }
        END{ if (path != "") print path "\t" head "\t" branch "\t" locked }
    ')"
REGISTERED_PATHS="$(printf '%s\n' "$REGISTERED" | cut -f1 | sort)"

# Branches checked out in some worktree (full refs/heads/<name> form).
CHECKED_OUT_REFS="$(printf '%s\n' "$REGISTERED" | cut -f3 |
    sed -n 's:^refs/heads/::p' | sort -u)"
is_checked_out() {
    printf '%s\n' "$CHECKED_OUT_REFS" | grep -qxF "$1"
}

# --- result accumulators ----------------------------------------------------
ROWS=""                 # bucket|action|bytes|display-path|branch|why
SAFE_PATHS=""           # newline list of paths approved for removal
SAFE_BRANCHES=""        # newline list of branches whose worktree was removed
PRUNE_NEEDED=0

n_safe=0; n_keep=0; n_caution=0; n_locked=0; n_protected=0; n_orphan=0; n_prunable=0
bytes_safe=0; bytes_orphan=0

add_row() {
    ROWS="${ROWS}$1|$2|$3|$4|$5|$6
"
}

# --- classify each registered worktree --------------------------------------
while IFS=$'\t' read -r path head branch locked; do
    [ -n "$path" ] || continue
    branch_short="${branch#refs/heads/}"
    disp="$path"
    case "$path" in
        "$MAIN_ROOT"/*) disp="${path#"$MAIN_ROOT"/}" ;;
    esac

    if [ "$path" = "$MAIN_ROOT" ]; then
        n_protected=$((n_protected + 1))
        add_row PROTECTED keep - "$disp" "$branch_short" "main checkout — never removed"
        continue
    fi
    if is_protected_path "$path"; then
        n_protected=$((n_protected + 1))
        add_row PROTECTED keep - "$disp" "$branch_short" "hard protect list (quality/pristine checkout)"
        continue
    fi
    if is_protected_name "$(basename "$path")"; then
        n_protected=$((n_protected + 1))
        add_row PROTECTED keep - "$disp" "$branch_short" "active lane 2026-07-18 (hard protect)"
        continue
    fi
    case "$path" in
        "$WT_BASE"/*) : ;;  # in scope
        *)
            n_protected=$((n_protected + 1))
            add_row PROTECTED keep - "$disp" "$branch_short" "outside $WT_BASE — out of scope"
            continue
            ;;
    esac
    if [ ! -d "$path" ]; then
        n_prunable=$((n_prunable + 1))
        PRUNE_NEEDED=1
        add_row PRUNABLE prune - "$disp" "$branch_short" "registered but directory is gone — git worktree prune"
        continue
    fi
    if [ "$locked" = "yes" ]; then
        n_locked=$((n_locked + 1))
        add_row LOCKED skip - "$disp" "$branch_short" "git worktree lock — a live lane; never touched"
        continue
    fi
    if git -C "$MAIN_ROOT" merge-base --is-ancestor "$head" "$MAIN_REF" 2>/dev/null; then
        merged=yes
    else
        merged=no
    fi
    if [ "$merged" = "no" ]; then
        n_keep=$((n_keep + 1))
        add_row KEEP keep - "$disp" "$branch_short" "branch tip has commits NOT on $MAIN_REF — unmerged work"
        continue
    fi
    if ! status="$(git -C "$path" status --porcelain=v1 -uall 2>/dev/null)"; then
        n_caution=$((n_caution + 1))
        add_row CAUTION keep - "$disp" "$branch_short" "git status failed — inspect by hand"
        continue
    fi
    if [ -n "$status" ]; then
        n_caution=$((n_caution + 1))
        add_row CAUTION keep - "$disp" "$branch_short" "uncommitted changes — review the diff by hand (hard protect)"
        continue
    fi
    sz="$(dir_bytes "$path")"
    sz="${sz:-0}"
    n_safe=$((n_safe + 1))
    bytes_safe=$((bytes_safe + sz))
    SAFE_PATHS="${SAFE_PATHS}${path}
"
    if [ "$branch_short" != "(detached)" ]; then
        SAFE_BRANCHES="${SAFE_BRANCHES}${branch_short}
"
        add_row SAFE "worktree+branch" "$sz" "$disp" "$branch_short" "merged into $MAIN_REF and clean"
    else
        add_row SAFE "worktree-only" "$sz" "$disp" "$branch_short" "merged into $MAIN_REF and clean (detached HEAD — no branch)"
    fi
done <<EOF_WORKTREES
$REGISTERED
EOF_WORKTREES

# --- orphan dirs: on disk under .claude/worktrees but not registered --------
if [ -d "$WT_BASE" ]; then
    while IFS= read -r dir; do
        [ -n "$dir" ] || continue
        if printf '%s\n' "$REGISTERED_PATHS" | grep -qxF "$dir"; then
            continue
        fi
        sz="$(dir_bytes "$dir")"
        sz="${sz:-0}"
        n_orphan=$((n_orphan + 1))
        bytes_orphan=$((bytes_orphan + sz))
        add_row ORPHAN report-only "$sz" "${dir#"$MAIN_ROOT"/}" "-" "not registered with git — REPORT only, never auto-removed"
    done <<EOF_ORPHANS
$(find "$WT_BASE" -mindepth 1 -maxdepth 1 -type d | sort)
EOF_ORPHANS
fi

# --- branch sweep: merged local branches with no worktree -------------------
# Branches checked out in a SAFE worktree are removed together with that
# worktree (the SAFE rows above) — they are counted separately, not double
# reported. Branches checked out in any OTHER worktree are protected.
BRANCH_ROWS=""
BRANCH_DELETE=""
n_branch_delete=0; n_branch_keep=0; n_branch_protected=0; n_branch_covered=0
while IFS= read -r b; do
    [ -n "$b" ] || continue
    [ "$b" = "$MAIN_REF" ] && continue
    if is_protected_name "$b"; then
        n_branch_protected=$((n_branch_protected + 1))
        BRANCH_ROWS="${BRANCH_ROWS}PROTECTED|$b|hard protect list
"
        continue
    fi
    if printf '%s\n' "$SAFE_BRANCHES" | grep -qxF "$b"; then
        n_branch_covered=$((n_branch_covered + 1))
        continue
    fi
    if is_checked_out "$b"; then
        n_branch_protected=$((n_branch_protected + 1))
        BRANCH_ROWS="${BRANCH_ROWS}PROTECTED|$b|checked out in a kept worktree
"
        continue
    fi
    if git -C "$MAIN_ROOT" merge-base --is-ancestor "$b" "$MAIN_REF" 2>/dev/null; then
        n_branch_delete=$((n_branch_delete + 1))
        BRANCH_DELETE="${BRANCH_DELETE}${b}
"
        BRANCH_ROWS="${BRANCH_ROWS}WOULD-DELETE|$b|merged into $MAIN_REF, no worktree
"
    else
        n_branch_keep=$((n_branch_keep + 1))
        BRANCH_ROWS="${BRANCH_ROWS}KEEP|$b|commits NOT on $MAIN_REF (e.g. lane/rom-keystone class)
"
    fi
done <<EOF_BRANCHES
$(git -C "$MAIN_ROOT" for-each-ref refs/heads/ --format='%(refname:short)')
EOF_BRANCHES

# --- summary table ----------------------------------------------------------
mode="DRY-RUN"
[ "$APPLY" = "1" ] && mode="APPLY"
printf 'worktree-gc: %s (main ref: %s, main checkout: %s)\n' \
    "$mode" "$MAIN_REF" "$MAIN_ROOT"
if [ "$APPLY" != "1" ]; then
    echo "worktree-gc: dry-run default — pass --apply to execute the SAFE removals"
fi
echo

printf '%-10s %-16s %10s  %-52s %-40s %s\n' \
    BUCKET ACTION BYTES WORKTREE BRANCH WHY
printf '%s\n' "$ROWS" |
    while IFS='|' read -r bucket action bytes path branch why; do
        [ -n "$bucket" ] || continue
        case "$bytes" in
            ''|'-') hbytes='-' ;;
            *) hbytes="$(human "$bytes")" ;;
        esac
        printf '%-10s %-16s %10s  %-52s %-40s %s\n' \
            "$bucket" "$action" "$hbytes" "$(fit 52 "$path")" "$(fit 40 "$branch")" "$why"
    done
echo

printf 'branch sweep (%s, no worktree):\n' "$MAIN_REF"
printf '%s\n' "$BRANCH_ROWS" | sort |
    while IFS='|' read -r verdict b why; do
        [ -n "$verdict" ] || continue
        printf '  %-12s %-44s %s\n' "$verdict" "$b" "$why"
    done
echo

echo "TOTALS"
printf '  worktrees SAFE (would-remove) : %d  (%s)\n' "$n_safe" "$(human "$bytes_safe")"
printf '  worktrees KEEP (unmerged)     : %d\n' "$n_keep"
printf '  worktrees CAUTION (dirty)     : %d\n' "$n_caution"
printf '  worktrees LOCKED (skip)       : %d\n' "$n_locked"
printf '  worktrees PROTECTED           : %d\n' "$n_protected"
printf '  worktrees PRUNABLE            : %d\n' "$n_prunable"
printf '  orphan dirs (REPORT only)     : %d  (%s)\n' "$n_orphan" "$(human "$bytes_orphan")"
printf '  branches WOULD-DELETE         : %d\n' "$n_branch_delete"
printf '  branches KEEP (unmerged)      : %d\n' "$n_branch_keep"
printf '  branches PROTECTED            : %d\n' "$n_branch_protected"
printf '  branches removed with SAFE wt : %d\n' "$n_branch_covered"
printf '  TOTAL RECLAIMABLE (SAFE only) : %s\n' "$(human "$bytes_safe")"
echo
if [ "$n_orphan" -gt 0 ]; then
    echo "orphan dirs are NOT auto-removed. Inspect one with:"
    echo "  ls <path> && du -sh <path>   # then rm -rf -- <path> by hand if truly dead"
fi

# --- apply ------------------------------------------------------------------
rc=0
if [ "$APPLY" = "1" ]; then
    echo
    echo "APPLY: removing SAFE worktrees (git worktree remove, no --force)"
    while IFS= read -r path; do
        [ -n "$path" ] || continue
        # Re-verify immediately before removal: merged, clean, and not
        # protected — conditions may have drifted since classification.
        head_now="$(git -C "$path" rev-parse HEAD 2>/dev/null || true)"
        if [ -z "$head_now" ] ||
           ! git -C "$MAIN_ROOT" merge-base --is-ancestor "$head_now" "$MAIN_REF" 2>/dev/null; then
            printf '  SKIP %s — no longer provably merged\n' "$path"
            rc=1
            continue
        fi
        if [ -n "$(git -C "$path" status --porcelain=v1 -uall 2>/dev/null)" ]; then
            printf '  SKIP %s — went dirty since classification\n' "$path"
            rc=1
            continue
        fi
        if ! git -C "$MAIN_ROOT" worktree remove "$path"; then
            printf '  FAIL git worktree remove %s\n' "$path" >&2
            rc=1
            continue
        fi
        printf '  removed %s\n' "$path"
    done <<EOF_SAFE
$SAFE_PATHS
EOF_SAFE

    echo "APPLY: deleting branches of removed worktrees + merged stray branches"
    # Re-derive checked-out branches AFTER worktree removal: a branch whose
    # worktree was just removed is now deletable, and one whose removal
    # failed is still checked out (git branch -D would refuse it anyway).
    CHECKED_OUT_REFS="$(git -C "$MAIN_ROOT" worktree list --porcelain |
        sed -n 's:^branch refs/heads/::p' | sort -u)"
    while IFS= read -r b; do
        [ -n "$b" ] || continue
        [ "$b" = "$MAIN_REF" ] && continue
        if is_protected_name "$b" || is_checked_out "$b"; then
            printf '  SKIP branch %s — protected or checked out\n' "$b"
            continue
        fi
        if ! git -C "$MAIN_ROOT" merge-base --is-ancestor "$b" "$MAIN_REF" 2>/dev/null; then
            printf '  SKIP branch %s — no longer provably merged\n' "$b"
            rc=1
            continue
        fi
        if ! git -C "$MAIN_ROOT" branch -D "$b" >/dev/null; then
            printf '  FAIL git branch -D %s\n' "$b" >&2
            rc=1
            continue
        fi
        printf '  deleted branch %s\n' "$b"
    done <<EOF_DEL
$(printf '%s\n%s\n' "$SAFE_BRANCHES" "$BRANCH_DELETE" | sed '/^$/d' | sort -u)
EOF_DEL

    if [ "$PRUNE_NEEDED" = "1" ]; then
        echo "APPLY: git worktree prune"
        git -C "$MAIN_ROOT" worktree prune -v
    fi
    echo "APPLY: done"
else
    echo "dry-run: nothing removed. Re-run with --apply to execute."
fi

exit "$rc"
