# shellcheck shell=bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# source_identity_lib.sh — the ONE reader for a build's baked source
# identity ("source_id_sha256"), and the ONE sha256-string validator.
#
# WHY THIS EXISTS — read before touching any function below.
#
# `zclassic23 agentbuild` emits the key `source_id_sha256` FOUR times on a
# SINGLE line: once at the top level (the source this binary was actually
# compiled from) and again inside nested runtime blocks describing the dev
# lane as it exists right now. Because the payload is one line, a naive
# extraction of the form
#     sed 's/.*"source_id_sha256"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/'
# is GREEDY — the leading `.*` consumes as much as possible, so it returns
# the LAST occurrence (a runtime value), not the first (the baked identity).
# Piping through `head -1` does not help: sed emits at most one line per
# input line, so `head -1` is a no-op on single-line JSON. This is not
# hypothetical — it produced a false "the live daemon and the dev build have
# identical identities" on 2026-07-28, exactly the false positive a drift
# check must never produce. The fix is to anchor on the FIRST match instead:
# `grep -oE` (or `grep -o`) followed by `head -1`, where `grep -o` prints
# each match as its own line and `-oE`'s NON-greedy-per-match scanning finds
# the first occurrence first. Every function below uses that form, and nine
# copies of a subtly different version of it are exactly the defect
# tools/lint/check_identity_parser_single.sh exists to stop from growing
# back — see that gate for the anti-rot enforcement, and dev_lib.sh's
# baked_source_id() (predates this library, deliberately left as the
# tools/dev/ in-tree reference and NOT migrated here — see the lane's
# handoff notes) for the sibling implementation this library generalizes.
#
# Sourcing contract: resolves its own path (safe to source from tools/,
# tools/scripts/, or tools/dev/ regardless of the sourcing script's cwd),
# side-effect-free (defines functions only), and idempotent (safe to source
# twice — a second source is a no-op). All functions are prefixed `zcl_` to
# avoid colliding with a caller's own locals.

if [ -n "${ZCL_SOURCE_IDENTITY_LIB_SOURCED:-}" ]; then
    return 0 2>/dev/null || exit 0
fi
ZCL_SOURCE_IDENTITY_LIB_SOURCED=1

# zcl_is_sha256 <string> — true iff exactly 64 lowercase hex characters.
# Pure: no output, status only.
zcl_is_sha256() {
    case "${1:-}" in
        "") return 1 ;;
    esac
    case "$1" in
        *[!0-9a-f]*) return 1 ;;
    esac
    [ "${#1}" -eq 64 ]
}

# zcl_json_first_string <json-text> <key> — the FIRST string value of "key"
# in <json-text>. Anchored first-occurrence extraction (see header): a
# `grep -o` pass isolates the first "key":"value" token, then a `sed`
# anchored on that isolated token (not on the original text) pulls out the
# value. Empty output, success status, when the key is absent or malformed
# — an absent key is a normal answer for a caller, not an error.
zcl_json_first_string() {
    local body="${1:-}" key="${2:?zcl_json_first_string: key required}" token
    token="$(printf '%s\n' "$body" |
        grep -o "\"${key}\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" 2>/dev/null |
        head -1 || true)"
    [ -n "$token" ] || return 0
    printf '%s\n' "$token" |
        sed -n "s/^\"${key}\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\"\$/\1/p"
}

# zcl_json_first_sha256 <json-text> <key> — as zcl_json_first_string, but
# only ever returns a well-formed 64-lowercase-hex value: the first
# occurrence of "key" whose value is NOT 64 hex characters yields empty
# output rather than a truncated or garbage string. This is the semantic
# every source_id_sha256 reader in this tree actually wants, and is
# equivalent to dev_lib.sh's baked_source_id() rule (anchor first, require
# 64 hex).
zcl_json_first_sha256() {
    local body="${1:-}" key="${2:?zcl_json_first_sha256: key required}"
    printf '%s\n' "$body" |
        grep -oE "\"${key}\"[[:space:]]*:[[:space:]]*\"[0-9a-f]{64}\"" 2>/dev/null |
        head -1 |
        grep -oE '[0-9a-f]{64}' || true
}

# zcl_binary_source_id <path-to-binary> — the BAKED source identity of a
# binary: runs `<path> agentbuild` and returns its first source_id_sha256
# via zcl_json_first_sha256. Preserves dev_lib.sh's baked_source_id()
# control flow exactly: a non-executable (or missing) path is a normal
# "nothing to report" case, not a failure — it returns SUCCESS with empty
# output, so a caller doing `id="$(zcl_binary_source_id "$bin")"` never has
# to fork error handling for "binary absent" vs. "binary present but silent".
zcl_binary_source_id() {
    local bin="${1:-}"
    [ -x "$bin" ] || return 0
    zcl_json_first_sha256 "$(timeout 20 "$bin" agentbuild 2>/dev/null)" source_id_sha256
}
