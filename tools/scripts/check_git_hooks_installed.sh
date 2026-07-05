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
    echo "check_git_hooks_installed: FAIL — core.hooksPath='$actual' (want '$want')" >&2
    echo "  Run: make install-hooks" >&2
    echo "  This arms tools/githooks/pre-push so local make pre-push-ci runs before push." >&2
    exit 1
fi

hook="$want/pre-push"
if [[ ! -x "$hook" ]]; then
    echo "check_git_hooks_installed: FAIL — $hook is missing or not executable" >&2
    echo "  Run: chmod +x $hook && make install-hooks" >&2
    exit 1
fi

if ! awk '
    /^[[:space:]]*#/ { next }
    /^[[:space:]]*CMD="\$\{ZCL_PREPUSH_CMD:-make pre-push-ci\}"[[:space:]]*$/ { default_cmd=1 }
    /^[[:space:]]*if[[:space:]]+![[:space:]]+\$CMD;[[:space:]]*then[[:space:]]*$/ { invokes_cmd=1 }
    END { exit !(default_cmd && invokes_cmd) }
' "$hook"; then
    echo "check_git_hooks_installed: FAIL — $hook does not run the local make pre-push-ci gate" >&2
    echo "  Restore the tracked pre-push hook or run: git checkout -- $hook" >&2
    exit 1
fi

echo "check_git_hooks_installed: clean — core.hooksPath=$want"
