#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Replay the derived non-live portion of the frozen production-C benchmark
# through the native resident watcher.  The runner never publishes a runtime:
# it uses verify mode, changes only a unique comment, waits for the exact
# verdict, stops that watcher, then restores the byte-identical source.

set -euo pipefail
shopt -s nullglob

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${ZCL_DEV_REPLAY_BIN:-$ROOT/build/bin/zclassic23-dev}"
HISTORY="${ZCL_DEV_HISTORY_OUTPUT:-$ROOT/build/dev-loop/history-benchmark.json}"
OUTPUT="${ZCL_DEV_REPLAY_OUTPUT:-$ROOT/build/dev-loop/history-replay.json}"
TEST_TIMING="$ROOT/.cache/test-timing/last-run.json"
LIMIT="${ZCL_DEV_REPLAY_LIMIT:-0}"
WAIT_MS="${ZCL_DEV_REPLAY_WAIT_MS:-10000}"
MODE="${1:-run}"

fail()
{
    printf 'dev-loop-history-replay: %s\n' "$*" >&2
    exit 2
}

aggregate()
{
    local samples="$1" output="$2" source_head="${3:-selftest}"
    local history_digest="${4:-selftest}"
    jq -s \
      --arg schema 'zcl.dev_loop_history_replay.v1' \
      --arg source_head "$source_head" \
      --arg history_digest "$history_digest" '
      def required:
        type == "object" and
        (.path|type) == "string" and
        (.class|type) == "string" and
        (.frequency|type) == "number" and .frequency > 0 and
        (.feedback_us|type) == "number" and .feedback_us >= 0 and
        (.result_bound|type) == "boolean" and
        (.feedback_green|type) == "boolean" and
        (.failure_capsule|type) == "string";
      def weighted($field):
        [.[] | . as $s | range(0; $s.frequency) | $s[$field]];
      def percentile($values; $percent):
        if ($values|length) == 0 then null
        else ($values|sort) as $ordered |
          $ordered[((((($ordered|length) * $percent) + 99) / 100)|floor)-1]
        end;
      if length == 0 or any(.[]; required|not) then
        error("invalid or empty replay sample set")
      else
        . as $samples |
        (weighted("feedback_us")) as $latencies |
        ([$samples[] | . as $s | range(0; $s.frequency) |
          select($s.feedback_green and $s.feedback_us <= 5000000)]|length)
          as $under_five |
        ($latencies|length) as $weighted_count |
        {schema:$schema, status:
           (if all($samples[]; .result_bound) then "complete" else "partial" end),
         source_head:$source_head,
         frozen_rows_sha256:$history_digest,
         representative_paths:($samples|length),
         weighted_edit_occurrences:$weighted_count,
         latency:{
           feedback_p50_us:percentile($latencies; 50),
           feedback_p95_us:percentile($latencies; 95),
           feedback_max_us:($latencies|max)},
         coverage:{
           trustworthy_under_5s_occurrences:$under_five,
           trustworthy_under_5s_percent:
             ((10000 * $under_five / $weighted_count)|round/100)},
         processes:{
           status:(if all($samples[]; .result_bound) then "complete"
                   else "partial_bound_receipts_only" end),
           unbound_samples:([$samples[]|select(.result_bound|not)]|length),
           compiler:([$samples[].compiler_processes]|add),
           linker:([$samples[].linker_processes]|add),
           test:([$samples[].test_processes]|add),
           probe:([$samples[].probe_processes]|add),
           make:([$samples[].make_processes]|add),
           shell:([$samples[].shell_processes]|add),
           lto:([$samples[].lto_invocations]|add),
           complete_graph_links:([$samples[].complete_graph_links]|add)},
         proof_groups:{
           selected:([$samples[]|(.tests_selected // 0)]|add),
           ran:([$samples[]|(.tests_run // 0)]|add),
           cached:([$samples[]|(.tests_cached // 0)]|add),
           deferred:([$samples[]|(.tests_deferred // 0)]|add),
           paths_with_bounded_deferred:
             ([$samples[]|select(.bounded_proof_deferred // false)]|length),
           paths_with_cache_hits:
             ([$samples[]|select((.tests_cached // 0) > 0)]|length)},
         gates:{
           feedback_p95_under_5s:(percentile($latencies; 95) < 5000000),
           feedback_95pct_under_5s:
             ($under_five * 100 >= $weighted_count * 95),
           zero_lto:(([$samples[].lto_invocations]|add) == 0),
           zero_make:(([$samples[].make_processes]|add) == 0),
           zero_shell:(([$samples[].shell_processes]|add) == 0)},
         blockers:($samples | group_by(.failure_capsule) |
           map(select(.[0].failure_capsule != "") |
             {failure:.[0].failure_capsule,paths:length,
              weighted_edit_occurrences:(map(.frequency)|add)})),
         bytes_scanned:{
           status:(if all($samples[]; .source_byte_accounting_complete)
                   then "complete" else "partial" end),
           source_guard_bytes_read:
             ([$samples[]|(.source_guard_bytes_read // 0)]|add),
           changed_source_bytes:
             ([$samples[]|(.changed_source_bytes // 0)]|add),
           source_bytes_total_max:
             ([$samples[]|(.source_bytes_total // 0)]|max)},
         samples:$samples}
      end' "$samples" >"$output"
}

self_test()
{
    local scratch samples receipt
    scratch="$(mktemp -d "${TMPDIR:-/tmp}/zcl-dev-replay-selftest.XXXXXX")"
    trap "rm -rf -- '$scratch'" EXIT INT TERM
    samples="$scratch/samples.jsonl"
    receipt="$scratch/receipt.json"
    printf '%s\n' \
      '{"path":"a.c","class":"requires_fast_restart","frequency":2,"feedback_us":1000000,"result_bound":true,"feedback_green":true,"failure_capsule":"","compiler_processes":2,"linker_processes":2,"test_processes":1,"probe_processes":1,"make_processes":0,"shell_processes":0,"lto_invocations":0,"complete_graph_links":2,"source_byte_accounting_complete":true,"source_guard_bytes_read":100,"source_bytes_total":1000,"changed_source_bytes":10}' \
      '{"path":"b.c","class":"requires_fast_restart","frequency":1,"feedback_us":4000000,"result_bound":true,"feedback_green":true,"failure_capsule":"","compiler_processes":2,"linker_processes":2,"test_processes":1,"probe_processes":1,"make_processes":0,"shell_processes":0,"lto_invocations":0,"complete_graph_links":2,"source_byte_accounting_complete":true,"source_guard_bytes_read":200,"source_bytes_total":1000,"changed_source_bytes":20}' \
      '{"path":"c.c","class":"requires_fast_restart","frequency":1,"feedback_us":6000000,"result_bound":false,"feedback_green":false,"failure_capsule":"WAIT_TIMEOUT","compiler_processes":1,"linker_processes":0,"test_processes":0,"probe_processes":0,"make_processes":0,"shell_processes":0,"lto_invocations":0,"complete_graph_links":0,"source_byte_accounting_complete":true,"source_guard_bytes_read":300,"source_bytes_total":1000,"changed_source_bytes":30}' \
      >"$samples"
    aggregate "$samples" "$receipt"
    jq -e '
      .schema == "zcl.dev_loop_history_replay.v1" and
      .status == "partial" and .representative_paths == 3 and
      .weighted_edit_occurrences == 4 and
      .latency.feedback_p50_us == 1000000 and
      .latency.feedback_p95_us == 6000000 and
      .coverage.trustworthy_under_5s_occurrences == 3 and
      .coverage.trustworthy_under_5s_percent == 75 and
      .processes.compiler == 5 and .processes.lto == 0 and
      .gates.feedback_p95_under_5s == false and
      .gates.zero_lto == true and
      .bytes_scanned.status == "complete" and
      .bytes_scanned.source_guard_bytes_read == 600 and
      .bytes_scanned.changed_source_bytes == 60 and
      .bytes_scanned.source_bytes_total_max == 1000' \
      "$receipt" >/dev/null || fail 'weighted aggregation contract regressed'
    printf '{"path":"bad"}\n' >"$samples"
    if aggregate "$samples" "$receipt" 2>/dev/null; then
        fail 'malformed sample was accepted'
    fi
    printf 'dev-loop-history-replay: self-test PASS\n'
}

if [ "$MODE" = "--self-test" ]; then
    self_test
    exit 0
fi
[ "$MODE" = run ] || fail 'usage: dev-loop-history-replay.sh [run|--self-test]'
[[ "$LIMIT" =~ ^[0-9]+$ ]] || fail 'ZCL_DEV_REPLAY_LIMIT must be nonnegative'
[[ "$WAIT_MS" =~ ^[1-9][0-9]*$ && "$WAIT_MS" -le 300000 ]] ||
    fail 'ZCL_DEV_REPLAY_WAIT_MS must be 1..300000'
command -v jq >/dev/null || fail 'jq is required'
command -v sha256sum >/dev/null || fail 'sha256sum is required'
[ -x "$BIN" ] || fail "missing dev binary: $BIN"
[ -r "$ROOT/build/dev-loop/restart.env" ] ||
    fail 'missing resident restart plan; run make dev-bin'
[ "$ROOT/build/dev-loop/restart.env" -nt "$ROOT/Makefile" ] ||
    fail 'resident restart plan predates Makefile; run make dev-bin'
if [ ! -r "$HISTORY" ]; then
    "$ROOT/tools/dev/dev-loop-history-bench.sh" run
fi
jq -e '.schema == "zcl.dev_loop_history_benchmark.v1" and
       (.representative_benchmark|type) == "array"' "$HISTORY" >/dev/null ||
    fail 'history benchmark receipt is invalid'

scratch="$(mktemp -d "${TMPDIR:-/tmp}/zcl-dev-history-replay.XXXXXX")"
samples="$scratch/samples.jsonl"
rows="$scratch/rows.jsonl"
bench_home="$scratch/home"
mkdir -p "$bench_home" "$scratch/backup"
: >"$samples"
watcher_id=0

native()
{
    HOME="$bench_home" \
    ZCL_DEV_SOURCE_ROOT="$ROOT" \
    ZCL_DEV_ARTIFACT_CACHE="${ZCL_DEV_ARTIFACT_CACHE:-${HOME:?}/.cache/zclassic23/dev-artifacts}" \
      "$BIN" "$@"
}

stop_watcher()
{
    if [ "$watcher_id" -gt 1 ]; then
        native dev loop stop \
          --input="$(jq -cn --argjson id "$watcher_id" '{watcher_id:$id}')" \
          >/dev/null 2>&1 || true
        watcher_id=0
    fi
}

quarantine_cancelled_test_scratch()
{
    local path base target
    local candidates=()
    for path in "$ROOT"/.zcl_test_*; do
        [ "${path##*/}" = .zcl_test_render ] || candidates+=("$path")
    done
    [ "${#candidates[@]}" -eq 0 ] && return 0
    mkdir -p "$scratch/cancelled-test-scratch"
    for path in "${candidates[@]}"; do
        [ -e "$path" ] || continue
        base="${path##*/}"
        case "$base" in
            .zcl_test_[A-Za-z0-9_.-]*) ;;
            *) fail "refusing unexpected cancelled-test path: $path";;
        esac
        target="$scratch/cancelled-test-scratch/$base"
        [ ! -e "$target" ] ||
            fail "duplicate cancelled-test scratch path: $base"
        mv -- "$path" "$target"
    done
}

restore_sources()
{
    local row path backup
    while IFS= read -r row; do
        path="$(jq -r '.path' <<<"$row")"
        backup="$scratch/backup/$path"
        if [ -f "$backup" ]; then
            cp -p -- "$backup" "$ROOT/$path"
        fi
    done <"$rows"
}

cleanup()
{
    stop_watcher
    quarantine_cancelled_test_scratch
    restore_sources
    rm -rf -- "$scratch"
}
trap cleanup EXIT INT TERM

jq -c '.representative_benchmark[] |
       select(.class != "currently_live_reloaded")' "$HISTORY" >"$rows"
if [ "$LIMIT" -gt 0 ]; then
    head -n "$LIMIT" "$rows" >"$scratch/limited"
    mv "$scratch/limited" "$rows"
fi
[ -s "$rows" ] || fail 'replay selected no non-live representative paths'
legacy_scratch=()
for legacy_path in "$ROOT"/.zcl_test_*; do
    [ "${legacy_path##*/}" = .zcl_test_render ] ||
        legacy_scratch+=("$legacy_path")
done
[ "${#legacy_scratch[@]}" -eq 0 ] ||
    fail 'pre-existing .zcl_test_* root scratch must be resolved before replay'

while IFS= read -r row; do
    path="$(jq -r '.path' <<<"$row")"
    case "$path" in
        /*|*..*|*[!A-Za-z0-9_./-]*) fail "unsafe benchmark path: $path";;
    esac
    [ -f "$ROOT/$path" ] && [ ! -L "$ROOT/$path" ] ||
        fail "benchmark path is not a regular source: $path"
    git -C "$ROOT" diff --quiet -- "$path" ||
        fail "benchmark path has an unstaged edit: $path"
    git -C "$ROOT" diff --cached --quiet -- "$path" ||
        fail "benchmark path has a staged edit: $path"
    mkdir -p "$scratch/backup/$(dirname "$path")"
    cp -p -- "$ROOT/$path" "$scratch/backup/$path"
done <"$rows"

sample_no=0
while IFS= read -r row; do
    sample_no=$((sample_no + 1))
    path="$(jq -r '.path' <<<"$row")"
    class="$(jq -r '.class' <<<"$row")"
    frequency="$(jq -r '.frequency' <<<"$row")"
    source="$ROOT/$path"
    ensure="$(native dev loop ensure \
      --input="$(jq -cn --arg root "$ROOT" '{root:$root,mode:"verify"}')")" ||
        fail 'native verify-only watcher did not start'
    [ "$(jq -r '.data.created // false' <<<"$ensure")" = true ] ||
        fail 'worktree already has a watcher; refusing to take or stop its lease'
    watcher_id="$(jq -r '.data.watcher_id // 0' <<<"$ensure")"
    epoch="$(jq -r '.data.epoch // 0' <<<"$ensure")"
    [[ "$watcher_id" =~ ^[0-9]+$ && "$epoch" =~ ^[0-9]+$ ]] &&
        [ "$watcher_id" -gt 1 ] ||
        fail 'watcher identity or starting epoch is invalid'
    staged="$(mktemp "$(dirname "$source")/.zcl-replay.XXXXXX")"
    cp -p -- "$source" "$staged"
    printf '\n/* ZCL_DEV_HISTORY_REPLAY:%02d:%s */\n' \
      "$sample_no" "$$" >>"$staged"
    chmod --reference="$source" "$staged"
    start_ns="$(date +%s%N)"
    mv -f -- "$staged" "$source"
    wait_input="$(jq -cn --argjson epoch "$epoch" --argjson timeout "$WAIT_MS" \
      '{after_epoch:$epoch,timeout_ms:$timeout}')"
    wait_rc=0
    verdict="$(native dev loop wait --input="$wait_input")" || wait_rc=$?
    end_ns="$(date +%s%N)"
    feedback_us=$(((end_ns - start_ns) / 1000))
    data="$(jq -c '.data // {}' <<<"$verdict" 2>/dev/null || printf '{}')"
    next_epoch="$(jq -r '.epoch // 0' <<<"$data")"
    result_bound=false
    if [ "$wait_rc" -eq 0 ] && [[ "$next_epoch" =~ ^[0-9]+$ ]] &&
       [ "$next_epoch" -gt "$epoch" ] &&
       jq -e --arg path "$path" '
         (.source_tu == $path) or ((.files // []) | index($path) != null)' \
         <<<"$data" >/dev/null; then
        result_bound=true
        epoch="$next_epoch"
    fi
    action="$(jq -r '.action // "timeout"' <<<"$data")"
    status="$(jq -r '.status // "blocked"' <<<"$data")"
    feedback_green=false
    if { [ "$action" = restart ] && [ "$status" = feedback_ready ] &&
         [ "$(jq -r '.immediate_proof_complete // false' <<<"$data")" = true ]; } ||
       { [ "$action" = hotswap ] && [ "$status" = passed ]; }; then
        feedback_green=true
    fi
    cycle_elapsed_us="$(jq -r '.elapsed_us // 0' <<<"$data")"
    compiler_processes="$(jq -r '
      ((.build_receipt.compiler_processes // 0) +
       (.proof_receipt.compiler_processes // 0))' <<<"$data")"
    linker_processes="$(jq -r '
      ((.build_receipt.linker_processes // 0) +
       (.proof_receipt.linker_processes // 0))' <<<"$data")"
    test_processes="$(jq -r '.proof_receipt.test_processes // 0' <<<"$data")"
    probe_processes="$(jq -r '.build_receipt.probe_processes // 0' <<<"$data")"
    complete_graph_links="$(jq -r '
      ((.build_receipt.complete_graph_linker_processes // 0) +
       (.proof_receipt.complete_graph_linker_processes // 0))' <<<"$data")"
    test_timing_bound=false
    test_group_timings='[]'
    if [ "$test_processes" -gt 0 ] && [ -r "$TEST_TIMING" ]; then
        expected_ran="$(jq -r '.proof_receipt.groups_ran // 0' <<<"$data")"
        expected_cached="$(jq -r '.proof_receipt.groups_cached // 0' <<<"$data")"
        if jq -e --argjson ran "$expected_ran" --argjson cached "$expected_cached" '
             .schema == "zcl.test_timing.v1" and
             .groups_ran == $ran and .groups_cached == $cached and
             (.groups | type) == "array"' "$TEST_TIMING" >/dev/null; then
            test_timing_bound=true
            test_group_timings="$(jq -c '[.groups[] |
              {name,ms,cached}]' "$TEST_TIMING")"
        fi
    fi
    jq -cn \
      --arg path "$path" --arg class "$class" --arg action "$action" \
      --arg status "$status" --argjson frequency "$frequency" \
      --argjson feedback_us "$feedback_us" \
      --argjson cycle_elapsed_us "$cycle_elapsed_us" \
      --argjson result_bound "$result_bound" \
      --argjson feedback_green "$feedback_green" \
      --argjson compiler "$compiler_processes" \
      --argjson linker "$linker_processes" \
      --argjson test "$test_processes" --argjson probe "$probe_processes" \
      --argjson complete_graph_links "$complete_graph_links" \
      --argjson test_timing_bound "$test_timing_bound" \
      --argjson test_group_timings "$test_group_timings" \
      --arg failure "$(jq -r '
        if (.data.failure_capsule // "") != "" then .data.failure_capsule
        elif (.error.code // "") != "" then
          (.error.code + ": " + (.error.message // "no diagnostic"))
        else "" end' <<<"$verdict")" \
      --arg process_output_tail "$(jq -r '.process_output // ""' <<<"$data")" \
      '{path:$path,class:$class,frequency:$frequency,action:$action,
        status:$status,result_bound:$result_bound,
        feedback_green:$feedback_green,feedback_us:$feedback_us,
        cycle_elapsed_us:$cycle_elapsed_us,
        detection_and_return_us:([$feedback_us-$cycle_elapsed_us,0]|max),
        source_guard_us:0,
        compile_us:((0)),link_us:((0)),test_us:((0)),
        compiler_processes:$compiler,linker_processes:$linker,
        test_processes:$test,probe_processes:$probe,
        make_processes:0,shell_processes:0,lto_invocations:0,
        complete_graph_links:$complete_graph_links,
        test_timing_bound:$test_timing_bound,
        test_group_timings:$test_group_timings,
        failure_capsule:$failure,process_output_tail:$process_output_tail}' \
      | jq --argjson cycle "$data" '
          .source_guard_us=($cycle.source_guard_us // 0) |
          .closure_us=($cycle.closure_us // 0) |
          .compile_us=(($cycle.build_receipt.compile_us // 0) +
                       ($cycle.proof_receipt.compile_us // 0)) |
          .link_us=(($cycle.build_receipt.link_us // 0) +
                    ($cycle.proof_receipt.link_us // 0)) |
          .test_us=($cycle.proof_receipt.test_us // 0) |
          .tests_selected=($cycle.proof_receipt.group_count // 0) |
          .tests_run=($cycle.proof_receipt.groups_ran // 0) |
          .tests_cached=($cycle.proof_receipt.groups_cached // 0) |
          .tests_deferred=($cycle.proof_receipt.deferred_group_count // 0) |
          .bounded_proof_deferred=
            ($cycle.proof_receipt.bounded_proof_deferred // false) |
          .artifact_cache_hit=($cycle.build_receipt.artifact_cache_hit // false)' \
      | jq --argjson cycle "$data" '
          .source_byte_accounting_complete=
            ($cycle.source_byte_accounting_complete // false) |
          .source_guard_bytes_read=($cycle.source_guard_bytes_read // 0) |
          .source_bytes_total=($cycle.source_bytes_total // 0) |
          .changed_source_bytes=($cycle.changed_source_bytes // 0)' \
      >>"$samples"
    printf 'replay %02d %-58s %8dus %s/%s\n' \
      "$sample_no" "$path" "$feedback_us" "$action" "$status" >&2

    stop_watcher
    quarantine_cancelled_test_scratch
    restore_stage="$(mktemp "$(dirname "$source")/.zcl-revert.XXXXXX")"
    cp -p -- "$scratch/backup/$path" "$restore_stage"
    mv -f -- "$restore_stage" "$source"
    cmp -s -- "$scratch/backup/$path" "$source" ||
        fail "source restoration mismatch for $path"
done <"$rows"

stop_watcher
source_head="$(git -C "$ROOT" rev-parse HEAD)"
history_digest="$(jq -r '.history.frozen_rows_sha256' "$HISTORY")"
mkdir -p "$(dirname "$OUTPUT")"
aggregate "$samples" "$OUTPUT.tmp" "$source_head" "$history_digest"
mv "$OUTPUT.tmp" "$OUTPUT"
restore_sources
trap - EXIT INT TERM
rm -rf -- "$scratch"

jq -r --arg output "$OUTPUT" '
  "dev-loop-history-replay: paths=\(.representative_paths) weighted=\(.weighted_edit_occurrences) p50=\(.latency.feedback_p50_us)us p95=\(.latency.feedback_p95_us)us under5=\(.coverage.trustworthy_under_5s_percent)% receipt=\($output)"' \
  "$OUTPUT"
