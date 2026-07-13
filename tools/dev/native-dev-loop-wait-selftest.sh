#!/usr/bin/env bash
# Hermetic contract test for the native dev-loop wait surface.  It uses a
# private HOME and a synthetic durable verdict; it starts no watcher, contacts
# no node, and has no runtime-publication path.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${ZCL_DEV_LOOP_TEST_BIN:-$ROOT/build/bin/zclassic23-dev}"

if [[ ! -x "$BIN" ]]; then
    echo "native-dev-loop-wait-selftest: missing dev binary: $BIN" >&2
    exit 2
fi

SANDBOX="$(mktemp -d "${TMPDIR:-/tmp}/zcl-native-loop-wait.XXXXXX")"
trap 'rm -rf "$SANDBOX"' EXIT HUP INT TERM
STATE="$SANDBOX/.local/state/zclassic23-dev"
mkdir -p "$STATE"
cat >"$STATE/native-cycle.json" <<'JSON'
{"schema":"zcl.dev_cycle.v1","producer":"selftest","status":"passed","action":"check","reason":"contract_fixture","phase":"verified","runtime_published":false,"elapsed_ms":1,"files":["fixture.c"]}
JSON

OUT="$(HOME="$SANDBOX" "$BIN" dev loop wait \
    --input='{"after_epoch":0,"timeout_ms":100,"view":"summary"}')"

[[ "$OUT" == *'"data_schema":"zcl.dev_cycle.v1"'* ]] || {
    echo "native-dev-loop-wait-selftest: declared data schema drifted" >&2
    exit 1
}
[[ "$OUT" == *'"data":{"schema":"zcl.dev_cycle.v1"'* ]] || {
    echo "native-dev-loop-wait-selftest: verdict is not the direct data object" >&2
    exit 1
}
[[ "$OUT" == *'"reason":"contract_fixture"'* ]] || {
    echo "native-dev-loop-wait-selftest: durable verdict was not returned" >&2
    exit 1
}
[[ "$OUT" == *'"epoch":'* ]] || {
    echo "native-dev-loop-wait-selftest: chaining epoch is missing" >&2
    exit 1
}
[[ "$OUT" != *'"latest_verdict"'* && "$OUT" != *'zcl.dev_loop_status.v1'* ]] || {
    echo "native-dev-loop-wait-selftest: wait leaked the loop-status wrapper" >&2
    exit 1
}
[[ "$OUT" == *'"runtime_published":false'* ]] || {
    echo "native-dev-loop-wait-selftest: fixture lost publication containment" >&2
    exit 1
}

EPOCH="$(sed -n 's/.*"epoch":\([0-9][0-9]*\).*/\1/p' <<<"$OUT")"
[[ -n "$EPOCH" ]] || {
    echo "native-dev-loop-wait-selftest: chaining epoch is not an integer" >&2
    exit 1
}
set +e
TIMEOUT_OUT="$(HOME="$SANDBOX" "$BIN" dev loop wait \
    --input="{\"after_epoch\":$EPOCH,\"timeout_ms\":25}" 2>&1)"
TIMEOUT_RC=$?
set -e
[[ "$TIMEOUT_RC" -eq 3 && "$TIMEOUT_OUT" == *'"code":"WAIT_TIMEOUT"'* ]] || {
    echo "native-dev-loop-wait-selftest: returned epoch did not chain to a bounded wait" >&2
    exit 1
}
[[ "$TIMEOUT_OUT" == *"\"after_epoch\":$EPOCH"* ]] || {
    echo "native-dev-loop-wait-selftest: timeout recovery lost the current epoch" >&2
    exit 1
}

echo "native-dev-loop-wait-selftest: PASS"
