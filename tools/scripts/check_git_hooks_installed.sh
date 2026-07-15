#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_git_hooks_installed.sh — fail loud when local CI pre-push hooks are not
# armed for this clone. The project runs CI locally, so a clone with the default
# .git/hooks path can push without the tracked tools/githooks/pre-push gate.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

want="tools/githooks"
actual="${ZCL_GIT_HOOKS_PATH_FOR_TEST-}"
if [[ -z "$actual" ]]; then
    actual="$(git config --get core.hooksPath || true)"
fi

if [[ "$actual" != "$want" ]]; then
    # Worktree agents flap the shared config to an ABSOLUTE spelling of the
    # same directory. The gate enforces substance (the tracked hooks are the
    # armed hooks), not spelling — accept any path resolving to $ROOT/$want,
    # and normalize the stored value back to the canonical relative form.
    resolved="$(realpath -m -- "$actual" 2>/dev/null || true)"
    if [[ -n "$actual" && "$resolved" == "$ROOT/$want" ]]; then
        git config core.hooksPath "$want" 2>/dev/null || true
        echo "check_git_hooks_installed: normalized absolute core.hooksPath back to '$want'"
    else
        echo "check_git_hooks_installed: FAIL — core.hooksPath='$actual' (want '$want')" >&2
        echo "  Run: make install-hooks" >&2
        echo "  This arms tools/githooks/pre-push so local make pre-push-ci runs before push." >&2
        exit 1
    fi
fi

# Self-tests inspect an isolated copy so they never rewrite the tracked hook
# and wake the live development watcher. Production calls leave the override
# unset and therefore continue to verify the armed, tracked hook exactly.
hook="${ZCL_GIT_HOOK_FILE_FOR_TEST:-$want/pre-push}"
if [[ ! -x "$hook" ]]; then
    echo "check_git_hooks_installed: FAIL — $hook is missing or not executable" >&2
    echo "  Run: chmod +x $hook && make install-hooks" >&2
    exit 1
fi

if ! awk '
    /^[[:space:]]*#/ { next }
    /^[[:space:]]*CMD="\$\{ZCL_PREPUSH_CMD:-make pre-push-ci\}"[[:space:]]*$/ { default_cmd=1 }
    /refs\/heads\/main/ { main_only=1 }
    /git diff --name-only "\$rsha" "\$lsha"/ { range_diff=1 }
    /ZCL_FAST_CHANGED_FILES_FILE="\$changed_files"/ { changed_env=1 }
    /ZCL_FAST_CHANGED_FILES_ONLY=1/ { changed_only_env=1 }
    # The gate invocation must run $CMD with both changed-files env vars set
    # and its exit status captured (directly, or piped/redirected first —
    # e.g. into a log file for SIGPIPE-safety) so a nonzero exit still
    # blocks the push.
    /^[[:space:]]*ZCL_FAST_CHANGED_FILES_FILE="\$changed_files"[[:space:]]+ZCL_FAST_CHANGED_FILES_ONLY=1[[:space:]]+\$CMD([[:space:]]*>[^|]*)?[[:space:]]*\|\|[[:space:]]*rc=\$\?[[:space:]]*$/ { invokes_cmd=1 }
    /^[[:space:]]*if[[:space:]]+![[:space:]]+ZCL_FAST_CHANGED_FILES_FILE="\$changed_files"[[:space:]]+ZCL_FAST_CHANGED_FILES_ONLY=1[[:space:]]+\$CMD;[[:space:]]*then[[:space:]]*$/ { invokes_cmd=1 }
    /rc"?[[:space:]]*-ne[[:space:]]*0/ { checks_rc=1 }
    END { exit !(default_cmd && main_only && range_diff && changed_env && changed_only_env && invokes_cmd && checks_rc) }
' "$hook"; then
    echo "check_git_hooks_installed: FAIL — $hook does not run the local range-aware make pre-push-ci gate" >&2
    echo "  Restore the tracked pre-push hook or run: git checkout -- $hook" >&2
    exit 1
fi

# The gate's own verbose stdout must never stream straight through this
# hook's stdout/stderr — a downstream reader that doesn't fully drain it
# (agent harness, `| head`, ...) makes a `make` recipe's write() fail with
# EPIPE, which is reported as a fatal error even though the checks passed,
# spuriously blocking the push. Require the CI invocation to redirect into
# a regular file instead.
if ! grep -qE '\$CMD[[:space:]]*>[[:space:]]*"\$LOG_FILE"[[:space:]]+2>&1' "$hook"; then
    echo "check_git_hooks_installed: FAIL — $hook does not redirect the CI gate's verbose output to a log file (SIGPIPE-unsafe)" >&2
    echo "  Restore the tracked pre-push hook or run: git checkout -- $hook" >&2
    exit 1
fi

echo "check_git_hooks_installed: clean — core.hooksPath=$want"
