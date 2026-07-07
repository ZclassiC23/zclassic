#!/usr/bin/env bash
# public_explorer_smoke.sh - external smoke checks for the public explorer.
#
# Purpose: catch regressions where the public HODL page/API tells users to
# refresh, wait, or retry instead of serving the best available projection.

set -euo pipefail

base="${ZCL_PUBLIC_BASE:-https://zclnet.net}"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

fail() {
    echo "public-explorer-smoke: FAIL: $*" >&2
    exit 1
}

fetch() {
    local url="$1"
    local out="$2"
    curl -kfsS --connect-timeout 8 --max-time 20 "$url" -o "$out" ||
        fail "fetch failed: $url"
}

api="$tmpdir/hodl.json"
html="$tmpdir/hodl.html"

fetch "$base/api/v1/hodl" "$api"
fetch "$base/explorer/hodl" "$html"

grep -q '"schema":"zcl.hodl_wave.v1"' "$api" ||
    fail "HODL API schema marker missing"
grep -q '"status":"ok"' "$api" ||
    fail "HODL API status is not ok"
grep -q '"blocker":"none"' "$api" ||
    fail "HODL API reports a blocker"
grep -q '"fresh":true' "$api" ||
    fail "HODL API is not fresh"

if grep -Eiq 'refresh in a minute|not processed|please retry|try again|waiting|temporarily unavailable' "$api"; then
    fail "HODL API contains a wait/retry marker"
fi
if grep -Eiq 'refresh in a minute|not processed|please retry|try again|waiting|temporarily unavailable' "$html"; then
    fail "HODL page contains a wait/retry marker"
fi

height="$(sed -n 's/.*"served_height":\([0-9][0-9]*\).*/\1/p' "$api")"
percent="$(sed -n 's/.*"older_than_1y":{"value":[0-9][0-9.]*,"percent":\([0-9][0-9.]*\)},"skipped_rows".*/\1/p' "$api")"

echo "public-explorer-smoke: PASS base=$base served_height=${height:-unknown} older_than_1y_percent=${percent:-unknown}"
