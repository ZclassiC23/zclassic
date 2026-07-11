#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# check_vcs_no_git.sh — the ZVCS sovereignty gate (HARD).
#
# ZVCS is our OWN in-binary content-addressed VCS. The node binary must never
# shell out to git (or any external process) to do version control: git stays
# only as the out-of-band GitHub sha1 publish bridge (a script under
# tools/scripts/), never linked into or invoked from lib/vcs/.
#
# This gate fails if any lib/vcs/ source references git or a process-spawning
# primitive: the word `git`, or exec*/system(/popen(/fork(. Keeping lib/vcs/
# free of these is what makes the VCS sovereign and reproducible.
#
# Fail-loud: a grep exit >=2 (bad pattern / unreadable tree / non-GNU grep) is
# a real error and aborts, never a hollow pass.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"

VCS_DIR="lib/vcs"
if [[ ! -d "$VCS_DIR" ]]; then
    echo "check_vcs_no_git: FATAL — $VCS_DIR missing; refusing to pass hollow." >&2
    exit 2
fi

# `\bgit\b` as a whole word, plus the process-spawning primitives. The word
# boundary keeps identifiers like "digit"/"legit" from matching; a real git
# call ("git ", `execvp("git"...`) trips it. The `(?<!\.)` lookbehind exempts
# the string ".git" — lib/vcs/ legitimately names the .git DIRECTORY as an
# ignore target; that is not a git invocation. Any bare `git` still fails.
PATTERN='(?<!\.)\bgit\b|\bexec[lv][ep]*[[:space:]]*\(|\bexecve[[:space:]]*\(|\bsystem[[:space:]]*\(|\bpopen[[:space:]]*\(|\bfork[[:space:]]*\('

hits=$(grep -rnP "$PATTERN" "$VCS_DIR" --include='*.c' --include='*.h')
rc=$?
if [[ $rc -ge 2 ]]; then
    echo "check_vcs_no_git: FATAL — grep exited $rc scanning $VCS_DIR." >&2
    exit 2
fi

if [[ -n "$hits" ]]; then
    echo "$hits"
    echo "FAIL: lib/vcs/ must not reference git or spawn processes"
    echo "  (no \`git\`, exec*, system(, popen(, fork()). The ZVCS is sovereign;"
    echo "  the git bridge lives only in tools/scripts/, never in lib/vcs/."
    exit 1
fi

echo "check_vcs_no_git: clean — lib/vcs/ is git-free and spawns no processes"
exit 0
