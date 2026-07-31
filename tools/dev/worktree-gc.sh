#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# worktree-gc.sh — classify every git worktree on this checkout and remove only
# the provably dead ones. DRY RUN unless CONFIRM=1.
#
# ── WHY THIS EXISTS ────────────────────────────────────────────────────────
# Workflow runs create a worktree each and do not always clean up. The pool
# grows, and a previous session left worktrees holding root-owned or
# permission-denied build artifacts that `git worktree remove` refuses to
# delete — so the obvious cleanup (`git worktree remove` in a loop) both
# under-deletes silently and over-deletes dangerously.
#
# ── THE DANGEROUS DIRECTION ────────────────────────────────────────────────
# Deleting a LIVE lane's worktree destroys in-flight work that exists nowhere
# else. So the classifier is deliberately biased to KEEP, and every keep prints
# its reason. Measured on this checkout while writing it: the critical-path
# lane's worktree was at ZERO commits ahead of main — a merged-branch test
# alone would have called it dead and deleted 23 modified files of in-flight
# work. That is why "no unique commits" is necessary but nowhere near
# sufficient.
#
# A worktree is LIVE — never removed — if ANY of these hold:
#   L1  uncommitted or untracked changes            (work that exists nowhere else)
#   L2  commits not reachable from main             (work not yet merged)
#   L3  git-locked                                  (someone said don't)
#   L4  a running process has its cwd inside it     (a lane is in it right now)
#   L5  touched within MIN_AGE_HOURS                (a lane may simply be idle
#                                                    between tool calls)
#   L6  it is the main checkout, or the worktree this script is running from
#
# Only a worktree that fails ALL of those is DEAD. L5 is a HOLD rather than a
# verdict: the report always prints the underlying L1-L4 verdict next to it, so
# an operator can see what would happen at a shorter floor and choose one,
# rather than being told "recent" and left guessing.
#
# ── SCOPE ──────────────────────────────────────────────────────────────────
# Default scope is the repo's own agent worktree pool, <repo>/.claude/worktrees/
# — the thing that actually accumulates. Every other worktree (sibling lane
# checkouts, the scheduled quality harness under ~/.local/state, scratch
# checkouts under /tmp) is listed and classified but held as out-of-scope,
# because those belong to tools that did not ask this script to manage them.
# SCOPE=all widens to every worktree; the LIVE rules still apply.
#
# ── KNOBS ──────────────────────────────────────────────────────────────────
#   CONFIRM=1           actually remove. Without it: dry run, nothing changes.
#   SCOPE=pool|all      default pool (<repo>/.claude/worktrees)
#   MIN_AGE_HOURS=N     recency hold, default 24
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO"
# shellcheck source=tools/scripts/sh_str.sh
. "$REPO/tools/scripts/sh_str.sh"  # str_contains — pipefail-safe, see F-note

CONFIRM="${CONFIRM:-0}"
SCOPE="${SCOPE:-pool}"
MIN_AGE_HOURS="${MIN_AGE_HOURS:-24}"
BASE_REF="${ZCL_WORKTREE_GC_BASE:-main}"

case "$SCOPE" in
    pool|all) ;;
    *) echo "worktree-gc: SCOPE must be 'pool' or 'all' (got '$SCOPE')" >&2; exit 2 ;;
esac
[[ "$MIN_AGE_HOURS" =~ ^[0-9]+$ ]] || {
    echo "worktree-gc: MIN_AGE_HOURS must be a non-negative integer" >&2; exit 2
}

COMMON_DIR="$(git rev-parse --git-common-dir)"
COMMON_DIR="$(cd "$COMMON_DIR" && pwd)"
MAIN_WT="$(dirname "$COMMON_DIR")"
POOL_DIR="$MAIN_WT/.claude/worktrees"
SELF_WT="$(git rev-parse --show-toplevel)"

now="$(date +%s)"
age_cutoff=$(( now - MIN_AGE_HOURS * 3600 ))

# ── L4: is a running process sitting in this worktree? ─────────────────────
# /proc cwd links for our own processes. Cheap, no lsof, no new dependency.
# Only processes this uid can read are visible — which is exactly the set of
# processes that could be an agent lane on this host.
PROC_CWDS=""
for p in /proc/[0-9]*; do
    c="$(readlink "$p/cwd" 2>/dev/null || true)"
    [ -n "$c" ] && PROC_CWDS="${PROC_CWDS}${c}"$'\n'
done

in_use_by_process() {
    local wt="$1" c
    while IFS= read -r c; do
        [ -n "$c" ] || continue
        [ "$c" = "$wt" ] && return 0
        case "$c" in "$wt"/*) return 0 ;; esac
    done <<<"$PROC_CWDS"
    return 1
}

# ── the permission probe ───────────────────────────────────────────────────
# A worktree can be provably dead and still undeletable: a prior run left files
# owned by another uid, or a directory we cannot write. `git worktree remove`
# fails on those. Probing is the difference between REPORTING that and silently
# skipping it, and silence is what let these accumulate. Short-circuits at the
# first offender, and is only ever run on a worktree already classified DEAD.
undeletable_reason() {
    local wt="$1" hit
    hit="$(find "$wt" -xdev ! -user "$(id -u)" -print -quit 2>/dev/null || true)"
    if [ -n "$hit" ]; then
        printf 'foreign-owner at %s' "$hit"
        return 0
    fi
    hit="$(find "$wt" -xdev -type d ! -writable -print -quit 2>/dev/null || true)"
    if [ -n "$hit" ]; then
        printf 'unwritable dir at %s' "$hit"
        return 0
    fi
    return 1
}

human_age() {
    local secs="$1"
    if [ "$secs" -lt 3600 ]; then
        printf '%dm' "$(( secs / 60 ))"
    else
        printf '%dh' "$(( secs / 3600 ))"
    fi
}

# Newest mtime among the worktree root and its immediate children. Depth 2 is
# deliberate: it catches an edit anywhere in the tree (the containing directory's
# mtime moves) without walking a multi-GB build/ on 23 worktrees.
newest_mtime() {
    local wt="$1" newest=0 m
    while IFS= read -r m; do
        [ -n "$m" ] || continue
        [ "$m" -gt "$newest" ] && newest="$m"
    done < <(find "$wt" -maxdepth 2 -printf '%T@\n' 2>/dev/null | cut -d. -f1)
    printf '%s' "$newest"
}

echo "── git worktree inventory ──────────────────────────────────────────"
echo "main_checkout=$MAIN_WT"
echo "self=$SELF_WT"
echo "scope=$SCOPE  pool_dir=$POOL_DIR"
echo "base_ref=$BASE_REF  min_age_hours=$MIN_AGE_HOURS"
if [ "$CONFIRM" = "1" ]; then
    echo "mode=CONFIRM (worktrees classified DEAD WILL BE REMOVED)"
else
    echo "mode=DRY-RUN (nothing will be changed; re-run with CONFIRM=1 to act)"
fi
echo

worktrees=()
while IFS= read -r line; do
    case "$line" in worktree\ *) worktrees+=("${line#worktree }") ;; esac
done < <(git worktree list --porcelain)

echo "total_worktrees=${#worktrees[@]}"
echo

dead=()
n_protected=0 n_live=0 n_hold=0 n_dead=0 n_oos=0 n_prunable=0 n_blocked=0

for wt in "${worktrees[@]}"; do
    verdict=""
    reason=""
    detail=""

    if [ "$wt" = "$MAIN_WT" ]; then
        verdict="KEEP"; reason="main-checkout"
    elif [ "$wt" = "$SELF_WT" ]; then
        verdict="KEEP"; reason="self (this script is running here)"
    elif [ ! -d "$wt" ]; then
        verdict="PRUNABLE"; reason="path no longer exists"
        n_prunable=$(( n_prunable + 1 ))
    fi

    if [ -z "$verdict" ] && [ "$SCOPE" = "pool" ]; then
        case "$wt" in
            "$POOL_DIR"/*) ;;
            *) verdict="KEEP"; reason="out-of-scope (SCOPE=all to include)" ;;
        esac
    fi

    if [ -z "$verdict" ]; then
        branch="$(git -C "$wt" rev-parse --abbrev-ref HEAD 2>/dev/null || echo '?')"
        dirty="$(git -C "$wt" status --porcelain 2>/dev/null | wc -l | tr -d ' ')"
        ahead="$(git -C "$wt" rev-list --count "$BASE_REF"..HEAD 2>/dev/null || echo '?')"
        # awk stops itself rather than being stopped by `head -1`: a pipeline
        # whose downstream exits early hands the upstream a SIGPIPE, and under
        # `set -o pipefail` that 141 becomes the pipeline's status (see
        # tools/scripts/sh_str.sh). This one's haystack is small enough today
        # that it could not bite, which is exactly how the shape survives long
        # enough to be copied somewhere it does.
        locked="$(git worktree list --porcelain |
                  awk -v w="$wt" '$1=="worktree" {cur=$2}
                                  $1=="locked" && cur==w {print "yes"; exit}')"

        # L1-L4: the substantive verdict, computed independently of recency so
        # the report can show both.
        core="DEAD"; core_why="no uncommitted changes, no unmerged commits"
        if [ "$dirty" != "0" ]; then
            core="LIVE"; core_why="$dirty uncommitted/untracked change(s)"
        elif [ "$ahead" = "?" ]; then
            core="LIVE"; core_why="cannot compare to $BASE_REF"
        elif [ "$ahead" != "0" ]; then
            core="LIVE"; core_why="$ahead commit(s) not in $BASE_REF"
        elif [ "$locked" = "yes" ]; then
            core="LIVE"; core_why="git-locked"
        elif in_use_by_process "$wt"; then
            core="LIVE"; core_why="a running process has its cwd here"
        fi

        mt="$(newest_mtime "$wt")"
        age_secs=$(( now - mt ))
        age="$(human_age "$age_secs")"

        if [ "$core" = "LIVE" ]; then
            verdict="KEEP"; reason="LIVE — $core_why"
        elif [ "$mt" -gt "$age_cutoff" ]; then
            # Recency HOLD, not a verdict. Say what the verdict would be.
            verdict="KEEP"
            reason="recency hold — touched ${age} ago (< ${MIN_AGE_HOURS}h); would be DEAD ($core_why)"
        else
            verdict="DEAD"; reason="$core_why; untouched ${age}"
        fi
        detail="branch=$branch dirty=$dirty ahead=$ahead age=$age"
    fi

    case "$verdict" in
        DEAD)
            if why="$(undeletable_reason "$wt")"; then
                verdict="BLOCKED"
                reason="DEAD but not removable: $why"
                n_blocked=$(( n_blocked + 1 ))
            else
                dead+=("$wt")
                n_dead=$(( n_dead + 1 ))
            fi
            ;;
        PRUNABLE) ;;
        *)
            case "$reason" in
                out-of-scope*)  n_oos=$(( n_oos + 1 )) ;;
                main-checkout*|self*) n_protected=$(( n_protected + 1 )) ;;
                recency\ hold*) n_hold=$(( n_hold + 1 )) ;;
                *)              n_live=$(( n_live + 1 )) ;;
            esac
            ;;
    esac

    printf '%-8s %s\n' "$verdict" "$wt"
    printf '         %s\n' "$reason"
    [ -n "$detail" ] && printf '         %s\n' "$detail"
done

echo
echo "── summary ─────────────────────────────────────────────────────────"
printf '%-24s %s\n' "kept_protected=$n_protected"      "# main checkout + self"
printf '%-24s %s\n' "kept_live=$n_live"                "# uncommitted work, unmerged commits, locked, or in use"
printf '%-24s %s\n' "kept_recency_hold=$n_hold"        "# would be DEAD but touched < ${MIN_AGE_HOURS}h ago"
printf '%-24s %s\n' "kept_out_of_scope=$n_oos"         "# SCOPE=all to include"
printf '%-24s %s\n' "blocked_undeletable=$n_blocked"   "# DEAD but file permissions prevent removal"
printf '%-24s %s\n' "prunable=$n_prunable"             "# path vanished; git worktree prune"
printf '%-24s %s\n' "dead=$n_dead"                     "# removable with CONFIRM=1"

if [ "$n_dead" -eq 0 ] && [ "$n_prunable" -eq 0 ]; then
    echo
    echo "Nothing to remove."
    exit 0
fi

echo
if [ "$CONFIRM" != "1" ]; then
    echo "DRY RUN — would remove:"
    printf '  %s\n' "${dead[@]:-}"
    [ "$n_prunable" -gt 0 ] && echo "  (plus 'git worktree prune' for $n_prunable vanished path(s))"
    echo
    echo "Re-run with CONFIRM=1 to act."
    exit 0
fi

rc=0
for wt in "${dead[@]:-}"; do
    [ -n "$wt" ] || continue
    if out="$(git worktree remove "$wt" 2>&1)"; then
        echo "removed  $wt"
    else
        # Never silent. A removal that failed is a finding, not a no-op.
        echo "FAILED   $wt"
        printf '         %s\n' "$out"
        rc=1
    fi
done
if [ "$n_prunable" -gt 0 ]; then
    git worktree prune
    echo "pruned   $n_prunable vanished worktree path(s)"
fi
exit "$rc"
