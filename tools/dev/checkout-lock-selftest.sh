#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Executable regression for tools/dev/checkout-lock.sh: a foreground holder
# and a watcher never run concurrently, the watcher defers instead of
# racing (never blocks), a foreground holder always blocks and eventually
# runs, and a nested call inside an already-held lock does not self-deadlock.

set -euo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOL="$SELF_DIR/checkout-lock.sh"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/zcl-checkout-lock-selftest.XXXXXX")"
LOCK="$WORK/checkout.lock"
CHILD_PIDS=()

cleanup()
{
    local pid
    for pid in "${CHILD_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    for pid in "${CHILD_PIDS[@]:-}"; do
        wait "$pid" 2>/dev/null || true
    done
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 2' HUP INT TERM

fail()
{
    printf 'checkout-lock-selftest: FAIL: %s\n' "$*" >&2
    exit 1
}

[ -x "$TOOL" ] || fail 'checkout-lock.sh is not executable'

# 1. A watcher invocation with no contention runs normally and releases.
OUT="$("$TOOL" watcher "$LOCK" -- echo uncontended)"
[ "$OUT" = uncontended ] || fail 'uncontended watcher call did not run the command'

# 2. Foreground holds the lock; watcher must defer (exit 99), print the
#    documented one-line status, and never run the wrapped command.
HOLD_MARKER="$WORK/fg-holding"
HOLD_RELEASE="$WORK/fg-release"
RAN_MARKER="$WORK/watcher-ran"
"$TOOL" foreground "$LOCK" -- bash -c \
    ": > '$HOLD_MARKER'; while [ ! -e '$HOLD_RELEASE' ]; do sleep 0.01; done" &
FG_PID=$!
CHILD_PIDS+=("$FG_PID")
for _ in $(seq 1 500); do
    [ -e "$HOLD_MARKER" ] && break
    kill -0 "$FG_PID" 2>/dev/null || break
    sleep 0.01
done
[ -e "$HOLD_MARKER" ] || fail 'foreground holder did not start'

set +e
WATCHER_ERR="$("$TOOL" watcher "$LOCK" -- bash -c ": > '$RAN_MARKER'" 2>&1 >/dev/null)"
WATCHER_RC=$?
set -e
[ "$WATCHER_RC" -eq 99 ] || fail "watcher must defer with exit 99 under contention, got $WATCHER_RC"
[ ! -e "$RAN_MARKER" ] || fail 'watcher ran the wrapped command instead of deferring'
case "$WATCHER_ERR" in
    *"deferred: foreground build holds the lock"*) ;;
    *) fail "watcher did not print the documented deferred status: $WATCHER_ERR" ;;
esac

# 3. A foreground caller arriving after the watcher deferred must still
#    block (not refuse) and run as soon as the holder releases — the
#    watcher yields, a human/agent-invoked make never does.
FG2_START="$WORK/fg2-start"
FG2_DONE="$WORK/fg2-done"
"$TOOL" foreground "$LOCK" -- bash -c \
    ": > '$FG2_START'; : > '$FG2_DONE'" &
FG2_PID=$!
CHILD_PIDS+=("$FG2_PID")
sleep 0.2
[ ! -e "$FG2_START" ] || fail 'second foreground caller ran while the first still held the lock'
: > "$HOLD_RELEASE"
wait "$FG_PID"
wait "$FG2_PID"
[ -e "$FG2_DONE" ] || fail 'second foreground caller never ran after the holder released'

# 4. Nested acquisition inside an already-held section must not deadlock —
#    a lock-wrapped recipe invoking another lock-wrapped script in the same
#    process family runs directly instead of re-blocking on itself.
NESTED_OUT="$("$TOOL" foreground "$LOCK" -- bash -c \
    "'$TOOL' foreground '$LOCK' -- echo nested-ok")"
[ "$NESTED_OUT" = nested-ok ] || fail 'nested acquisition inside a held lock deadlocked or misbehaved'

# 5. A failing wrapped command still releases the lock (a later caller is
#    not left waiting on a dead holder).
"$TOOL" foreground "$LOCK" -- false 2>/dev/null || true
OUT2="$("$TOOL" foreground "$LOCK" -- echo released-after-failure)"
[ "$OUT2" = released-after-failure ] || fail 'lock was not released after the wrapped command failed'

printf 'checkout-lock-selftest: PASS uncontended=true watcher_defers=true foreground_blocks=true nested_no_deadlock=true releases_on_failure=true\n'
