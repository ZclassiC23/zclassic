#!/usr/bin/env bash
# Build one admitted stateless generation and commit it inside the running,
# isolated dev node through the dev-only RPC bridge.  Exit 69 means the
# persistent transport is unavailable, allowing `dev-watch auto` to fall back
# to a transactional process reload.  Any generation/ABI/probe rejection is a
# real failure (exit 1) and must not be hidden by a reload.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${ZCL_DEV_WATCH_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
FILES_FILE="${ZCL_DEV_HOTSWAP_FILES_FILE:-}"
REQUESTED_PROBE="${ZCL_DEV_HOTSWAP_PROBE:-}"
PROBE=""
STATE_DIR="${ZCL_DEV_WATCH_STATE_DIR:-$HOME/.local/state/zclassic23-dev}"
RESULT_FILE="${ZCL_DEV_HOTSWAP_RESULT:-$STATE_DIR/hotswap-latest.json}"
CYCLE_ID="${ZCL_DEV_CYCLE_ID:-standalone-$(date +%s)-$$}"

fail()
{
    printf '[dev-hotswap] FATAL: %s\n' "$*" >&2
    exit 2
}

fail 'runtime publication and resident probing are contained; use make hotswap-so plus build/test verification'

json_escape()
{
    printf '%s' "$1" | sed \
        -e 's/\\/\\\\/g' -e 's/"/\\"/g' \
        -e ':a;N;$!ba;s/\n/\\n/g' -e 's/\r/\\r/g' -e 's/\t/\\t/g'
}

[ -n "$FILES_FILE" ] && [ -r "$FILES_FILE" ] ||
    fail 'ZCL_DEV_HOTSWAP_FILES_FILE must name a readable changed-file list'

mapfile -t files < <(sed '/^[[:space:]]*$/d' "$FILES_FILE")
[ "${#files[@]}" -eq 1 ] ||
    fail "v2 pilot requires exactly one admitted provider (got ${#files[@]})"
source_file="${files[0]}"
case "$source_file" in
    *[!A-Za-z0-9_./-]*|/*|*..*) fail "unsafe source path: $source_file" ;;
esac

# Resolve the route probe from the same manifest row that admits the source.
# A caller may repeat that value for an explicit assertion, but may not
# override it: planner/transport drift must fail before compilation.
while IFS=$'\t' read -r entry_path entry_probe; do
    if [ "$entry_path" = "$source_file" ]; then
        PROBE="$entry_probe"
        break
    fi
done < <(sed -n \
    's/^[[:space:]]*HOTSWAP_ELIGIBLE("\([^"]*\)")[[:space:]]*HOTSWAP_PROBE("\([^"]*\)").*/\1\	\2/p' \
    "$ROOT/config/hotswap_eligible.def" 2>/dev/null || true)
[ -n "$PROBE" ] || fail "manifest has no probe for admitted source: $source_file"
if [ -n "$REQUESTED_PROBE" ] && [ "$REQUESTED_PROBE" != "$PROBE" ]; then
    fail "probe drift for $source_file: requested=$REQUESTED_PROBE manifest=$PROBE"
fi
case "$PROBE" in
    "") fail "manifest probe is empty for $source_file" ;;
    *[!A-Za-z0-9_]*) fail "unsafe probe tool name: $PROBE" ;;
esac

cd "$ROOT" || fail "cannot enter repository: $ROOT"

# Bind the artifact/probe transaction to the complete dirty source epoch.  A
# standalone invocation may capture its own epoch only when the one requested
# provider is the exact dirty set (a clean-tree reapply remains allowed).  The
# watcher passes the identity it captured before proofs, which is compared now
# and again at the resident commit boundary.
complete_text="$("$ROOT/tools/dev/source-identity.sh" paths)" ||
    fail 'complete dirty-set discovery failed'
complete_dirty=()
if [ -n "$complete_text" ]; then
    mapfile -t complete_dirty <<< "$complete_text"
fi
if [ "${#complete_dirty[@]}" -gt 0 ] &&
   { [ "${#complete_dirty[@]}" -ne 1 ] ||
     [ "${complete_dirty[0]}" != "$source_file" ]; }; then
    fail "requested provider is not the exact complete dirty set"
fi
captured_source_id="$("$ROOT/tools/dev/source-identity.sh" capture)" ||
    fail 'source identity capture failed'
expected_source_id="${ZCL_DEV_SOURCE_ID:-$captured_source_id}"
[[ "$expected_source_id" =~ ^[0-9a-fA-F]{64}$ ]] ||
    fail 'ZCL_DEV_SOURCE_ID must be 64 hex characters'
[ "${expected_source_id,,}" = "${captured_source_id,,}" ] ||
    fail 'source epoch was already superseded before hot-swap build'

so="$(make --no-print-directory hotswap-so FILES="$source_file")" || exit 1
[ -n "$so" ] && [ -r "$so" ] || fail 'hotswap-so produced no readable artifact'
case "$so" in /*) ;; *) so="$ROOT/$so" ;; esac

bin="$ROOT/build/bin/zclassic23-dev"
[ -x "$bin" ] || {
    printf '[dev-hotswap] persistent RPC client binary is not built: %s\n' "$bin" >&2
    exit 69
}

err_file="$(mktemp "${TMPDIR:-/tmp}/zcl-dev-hotswap.XXXXXX")" ||
    fail 'could not allocate RPC stderr capture'
trap 'rm -f "$err_file"' EXIT HUP INT TERM

# Exercise the same artifact in a short-lived dispatcher first. This runs the
# generation probe before the resident process can publish anything. A probe
# or manifest failure therefore leaves the running node untouched.
args="{\"so_path\":\"$(json_escape "$so")\""
if [ -n "$PROBE" ]; then
    args="$args,\"probe_tool\":\"$PROBE\""
fi
args="$args}"
set +e
smoke_output="$("$bin" -datadir="$HOME/.zclassic-c23-dev" -rpcport=18252 \
    mcpcall zcl_agent_hotswap "$args" 2>"$err_file")"
smoke_rc=$?
set -e
[ ! -s "$err_file" ] || sed 's/^/[dev-hotswap-preflight] /' "$err_file" >&2
if [ "$smoke_rc" -ne 0 ] ||
   ! printf '%s' "$smoke_output" |
       grep -q '"ok"[[:space:]]*:[[:space:]]*true' ||
   printf '%s' "$smoke_output" | grep -q '"probe_error"[[:space:]]*:' ||
   ! printf '%s' "$smoke_output" | grep -q '"probe"[[:space:]]*:' ||
   printf '%s' "$smoke_output" | grep -q '"error"[[:space:]]*:'; then
    printf '%s\n' "$smoke_output"
    printf '[dev-hotswap] pre-commit generation/probe smoke failed; resident node was untouched\n' >&2
    exit 1
fi

# Final source compare-and-swap occurs after the real candidate probe and
# immediately before the resident registry commit.  Save/rename/delete races
# publish nothing and return a distinct refusal instead.
"$ROOT/tools/dev/source-identity.sh" verify "$expected_source_id" >/dev/null || {
    printf '[dev-hotswap] source epoch superseded; resident node was untouched\n' >&2
    exit 3
}

: > "$err_file"
set +e
output="$("$bin" -datadir="$HOME/.zclassic-c23-dev" -rpcport=18252 \
    dev_hotswap "$so" "$PROBE" 2>"$err_file")"
rc=$?
set -e
printf '%s\n' "$output"
[ ! -s "$err_file" ] || sed 's/^/[dev-hotswap-rpc] /' "$err_file" >&2

# Persist the exact resident response for the enclosing zcl.dev_cycle.v1.
# stdout from the RPC CLI is one JSON value; if it is not, fail closed and do
# not publish a misleading provenance record.
if printf '%s' "$output" | grep -q '^[[:space:]]*{' &&
   printf '%s' "$output" | grep -q '}[[:space:]]*$'; then
    mkdir -p "$(dirname "$RESULT_FILE")"
    result_tmp="$(mktemp "$(dirname "$RESULT_FILE")/.hotswap-result.XXXXXX")" ||
        fail 'could not allocate hot-swap result record'
    {
        printf '{"schema":"zcl.dev_hotswap_result.v1","cycle_id":"%s",' \
            "$(json_escape "$CYCLE_ID")"
        printf '"captured_at_utc":"%s","artifact":"%s","response":' \
            "$(date -u +%FT%TZ)" "$(json_escape "$so")"
        printf '%s,"precommit_probe":%s}\n' "$output" "$smoke_output"
    } > "$result_tmp"
    mv -f "$result_tmp" "$RESULT_FILE"
fi

if [ "$rc" -ne 0 ]; then
    combined="$output $(cat "$err_file")"
    if printf '%s' "$combined" | grep -q 'generation registry full' &&
       printf '%s' "$combined" | grep -q '"rejection_stage"[[:space:]]*:[[:space:]]*"registry"'; then
        printf '[dev-hotswap] mapped-generation budget reached; transactional reload is required\n' >&2
        exit 69
    fi
    if printf '%s' "$combined" | grep -Eqi \
        'method not found|"code"[[:space:]]*:[[:space:]]*-32601|could not connect|connection refused|rpc server unavailable|failed to connect'; then
        printf '[dev-hotswap] persistent dev-node RPC unavailable; reload fallback is permitted\n' >&2
        exit 69
    fi
    printf '[dev-hotswap] RPC transport failed rc=%s; generation was not accepted\n' "$rc" >&2
    exit 1
fi

if ! printf '%s' "$output" |
     grep -q '"ok"[[:space:]]*:[[:space:]]*true' ||
   ! printf '%s' "$output" | grep -q '"probe"[[:space:]]*:' ||
   printf '%s' "$output" | grep -q '"probe_error"[[:space:]]*:' ||
   printf '%s' "$output" | grep -q '"error"[[:space:]]*:'; then
    if printf '%s' "$output" | grep -q 'generation registry full' &&
       printf '%s' "$output" | grep -q '"rejection_stage"[[:space:]]*:[[:space:]]*"registry"'; then
        printf '[dev-hotswap] mapped-generation budget reached; transactional reload is required\n' >&2
        exit 69
    fi
    printf '[dev-hotswap] generation rejected; refusing to hide it behind a reload\n' >&2
    exit 1
fi
printf '[dev-hotswap] committed persistent generation artifact=%s\n' "$so"
