#!/usr/bin/env bash
# arena_kpis.sh — repeatable KPI harness for the zdogfight arena (M7).
#
#   tools/dev/arena_kpis.sh <pilot-red> <pilot-blue> [seed] [planes-per-team]
#
# Measures, on scratch files only:
#   - runner wall time and ticks/second for one full match
#   - runner peak RSS (via /usr/bin/time -v when available)
#   - replay file bytes (raw and the implied bytes/tick)
#   - pilot binary sizes
#   - determinism: two runs must be byte-identical (born-red guard)
#
# Never touches live nodes or canonical datadirs; everything lands in a
# mktemp scratch dir that is cleaned up on exit.
set -euo pipefail

RED="${1:?usage: arena_kpis.sh <pilot-red> <pilot-blue> [seed] [planes]}"
BLUE="${2:?usage: arena_kpis.sh <pilot-red> <pilot-blue> [seed] [planes]}"
SEED="${3:-7}"
PPT="${4:-3}"
RUNNER="${ARENA_RUNNER:-build/bin/arena_runner}"
TIME_BIN="$(command -v /usr/bin/time || true)"

WORK="$(mktemp -d /tmp/zcl-arena-kpis.XXXXXX)"
trap 'case "$WORK" in /tmp/zcl-arena-kpis.*) rm -rf "$WORK";; esac' EXIT

run_once() { # $1=outfile-prefix
    local t0 t1
    t0=$(date +%s%N)
    if [ -n "$TIME_BIN" ]; then
        /usr/bin/time -v "$RUNNER" --seed "$SEED" --planes-per-team "$PPT" \
            --pilot-red "$RED" --pilot-blue "$BLUE" \
            --replay-out "$1.bin" >"$1.out" 2>"$1.time"
    else
        "$RUNNER" --seed "$SEED" --planes-per-team "$PPT" \
            --pilot-red "$RED" --pilot-blue "$BLUE" \
            --replay-out "$1.bin" >"$1.out" 2>&1
    fi
    t1=$(date +%s%N)
    echo $(( (t1 - t0) / 1000000 ))  # wall ms
}

MS_A="$(run_once "$WORK/a")"
MS_B="$(run_once "$WORK/b")"

cmp "$WORK/a.bin" "$WORK/b.bin" || { echo "arena-kpis: FAIL replays diverge" >&2; exit 1; }

TICKS="$(sed -n 's/.*ticks=\([0-9]*\).*/\1/p' "$WORK/a.out")"
BYTES="$(stat -c %s "$WORK/a.bin")"
RSS_KB="n/a"
[ -n "$TIME_BIN" ] && RSS_KB="$(grep -o 'Maximum resident set size (kbytes): [0-9]*' "$WORK/a.time" | grep -o '[0-9]*$')"

echo "arena-kpis: seed=$SEED planes=$PPT ticks=$TICKS"
echo "arena-kpis: wall_ms_run1=$MS_A wall_ms_run2=$MS_B"
echo "arena-kpis: ticks_per_sec=$(( TICKS * 1000 / (MS_A > 0 ? MS_A : 1) ))"
echo "arena-kpis: replay_bytes=$BYTES bytes_per_tick=$(( BYTES / (TICKS > 0 ? TICKS : 1) ))"
echo "arena-kpis: runner_peak_rss_kb=$RSS_KB"
echo "arena-kpis: pilot_red_bytes=$(stat -c %s "$RED") pilot_blue_bytes=$(stat -c %s "$BLUE")"
echo "arena-kpis: determinism=byte-identical"
grep -E 'winner|_root=|_avg_response_us=' "$WORK/a.out"
