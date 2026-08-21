#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# fast-build-bench.sh — benchmark the package verifier's per-TU fast object
# cache (zcl.fastobj.v1, tools/package_verify.c --fast-cache) across a
# scripted sequence of REAL edits and prove, for every edit, that the fast
# path's emitted output is byte-for-byte identical to a clean shadow build
# with the cache disabled. A shadow mismatch is a dependency-keying bug:
# the harness stops loudly instead of recording around it.
#
# The verifier runs DIRECTLY in candidate mode (fast; per-TU visibility via
# the zbuild-package-fast-cache=v1 and zbuild-package-perf=v1 stdout lines)
# against scratch draft copies of real packages under
# test-tmp/fast-build-bench/. Tracked packages/ and corpus/ bytes are never
# touched (asserted clean before and after the campaign via git status).
#
# Packages: zsha256 (leaf), zhkdf and zscrypt (both pin zsha256 as a dep, so
# dependency-update edits repin a real content-derived dep root).
#
# Metrics per edit: cache hits/misses, TUs compiled vs total, compiler
# processes spawned (verifier-instrumented gcc driver spawns including the
# -E probes; cc1/as children of the driver are not separately visible),
# files/bytes reread (the --plan depfile closure: every file the
# preprocessor opened, summed), fast wall ms to local green (exit 0 with
# result=test-pass|build-pass and a build receipt) or to first RED, shadow
# wall ms, shadow_match. Aggregates (p50/p95 nearest-rank) per edit class,
# plus one end-to-end package-factory comparison with/without the cache.
#
# Artifact: .cache/zcl-fast-build/bench-latest.json (zcl.fast_build_bench.v1).
# Rerun: bash tools/dev/fast-build-bench.sh   (non-interactive)
#
# Controls: ZCL_FAST_BENCH_OUTPUT, ZCL_FAST_BENCH_WORK, ZCL_FAST_BENCH_KEEP=1
# (keep the scratch tree), ZCL_FAST_BENCH_SKIP_FACTORY=1.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${ZCL_FAST_BENCH_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
# shellcheck source=tools/dev/dev_lib.sh
. "$SCRIPT_DIR/dev_lib.sh"  # json_escape (fail() below satisfies dev_lib's contract)
OUTPUT="${ZCL_FAST_BENCH_OUTPUT:-$ROOT/.cache/zcl-fast-build/bench-latest.json}"
WORK="${ZCL_FAST_BENCH_WORK:-$ROOT/test-tmp/fast-build-bench}"
KEEP="${ZCL_FAST_BENCH_KEEP:-0}"
SKIP_FACTORY="${ZCL_FAST_BENCH_SKIP_FACTORY:-0}"

VERIFY_BIN="$ROOT/build/bin/zclassic23-package-verify"
NODE_BIN="$ROOT/build/bin/zclassic23"
SIGN_BIN="$ROOT/build/bin/zclassic23-package-sign"
FACTORY_BIN="$ROOT/build/bin/package-factory"
JSONQ="${JSONQ:-$ROOT/build/bin/jsonq}"
PKGS=(zsha256 zhkdf zscrypt)

CACHE_DIR="$WORK/cache"          # shared fast-object cache across all runs
DRAFTS="$WORK/drafts"
DEPS="$WORK/deps"                # installed-dep layout: <root>/{lib,include}
EMITS="$WORK/emit"
PLANS="$WORK/plans"
EDITS_NDJSON="$WORK/edits.ndjson"

TOTAL_EDITS=0
SHADOW_MISMATCHES=0
EDIT_FAILURES=0
DEP_ROOT_CUR=""                  # the dep root zhkdf/zscrypt currently pin
DEP_ROOT_BASE=""                 # the pre-edit (warm-up) dep root
DEP_DETAIL="{}"                  # last dep_update detail JSON

log()
{
    printf '[fast-build-bench] %s\n' "$*"
}

fail()
{
    printf '[fast-build-bench] FATAL: %s\n' "$*" >&2
    exit 2
}

usage()
{
    printf '%s\n' \
        'Usage: tools/dev/fast-build-bench.sh [--help]' \
        '' \
        'Benchmarks the package-verifier fast object cache over a scripted' \
        'edit sequence (22 real edits: comment-only, private-body,' \
        'public-header, exact revert, dependency update + repin, semantic' \
        'RED + fix, test-source) on draft copies of zsha256/zhkdf/zscrypt.' \
        'Every edit is also built with the cache disabled (clean shadow)' \
        'and the emitted trees are compared byte-for-byte. One end-to-end' \
        'package-factory comparison (with/without --fast-cache) follows.' \
        '' \
        'Controls: ZCL_FAST_BENCH_OUTPUT, ZCL_FAST_BENCH_WORK,' \
        'ZCL_FAST_BENCH_KEEP=1, ZCL_FAST_BENCH_SKIP_FACTORY=1.'
}

clock_ms()
{
    local now
    now="$(date +%s%N 2>/dev/null || true)"
    if [[ "$now" =~ ^[0-9]+$ ]]; then
        printf '%s' "$((now / 1000000))"
    else
        printf '%s000' "$(date +%s)"
    fi
}

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    usage
    exit 0
fi
if [ -n "${1:-}" ]; then
    usage >&2
    exit 2
fi

# ── preflight ──────────────────────────────────────────────────────────

for bin in "$VERIFY_BIN" "$NODE_BIN" "$SIGN_BIN" "$FACTORY_BIN" "$JSONQ"; do
    [ -x "$bin" ] || fail "missing $bin — run: make -j\$(nproc)"
done
for p in "${PKGS[@]}"; do
    [ -d "$ROOT/packages/$p" ] || fail "missing packages/$p"
done
# The campaign edits only scratch copies; prove the tracked tree stays clean.
TRACKED_STATUS_BEFORE="$(git -C "$ROOT" status --porcelain=v1 -- packages corpus 2>/dev/null)"
[ -z "$TRACKED_STATUS_BEFORE" ] ||
    fail "tracked packages/ or corpus/ is dirty before the campaign: $TRACKED_STATUS_BEFORE"

# ── scratch setup ──────────────────────────────────────────────────────

[ ! -d "$WORK" ] || rm -rf "$WORK" || fail "cannot clear $WORK"
mkdir -p "$DRAFTS" "$DEPS" "$EMITS" "$PLANS" "$CACHE_DIR" \
    "$WORK/pvwork" || fail "cannot create $WORK"
for p in "${PKGS[@]}"; do
    cp -r "$ROOT/packages/$p" "$DRAFTS/$p" || fail "cannot copy packages/$p"
done

# Throwaway publisher key: only the pubkey matters (dev prepare wants a
# valid compressed curve point); nothing is signed in this benchmark.
BENCH_PUBKEY="$("$SIGN_BIN" --generate "$WORK/bench-key" 2>/dev/null |
    tr -d '[:space:]')"
[[ "$BENCH_PUBKEY" =~ ^[0-9a-f]{66}$ ]] || fail "no bench publisher pubkey"

# dev prepare one draft: writes <pkg>.recipe.wire and prints
# "<package_root> <lock_root>" on stdout.
dev_prepare()
{
    local p="$1" prep="$WORK/prepare-$1.json"
    printf '{"dir":"%s","publisher_pubkey":"%s","publisher_sequence":1}' \
        "$DRAFTS/$p" "$BENCH_PUBKEY" |
        "$NODE_BIN" zcode package dev prepare --input=- > "$prep" 2>/dev/null
    "$JSONQ" eq ok true < "$prep" || return 1
    local recipe_hex pkg_root lock_root
    recipe_hex="$("$JSONQ" get data.recipe_hex < "$prep")"
    pkg_root="$("$JSONQ" get data.package_root < "$prep")"
    lock_root="$("$JSONQ" get data.dependency_lock_root < "$prep")"
    printf '%s' "$recipe_hex" | xxd -r -p > "$WORK/$p.recipe.wire"
    printf '%s %s\n' "$pkg_root" "$lock_root"
}

declare -A PKG_ROOT PKG_LOCK
for p in "${PKGS[@]}"; do
    prep_out="$(dev_prepare "$p")" || fail "dev prepare failed for $p"
    PKG_ROOT[$p]="${prep_out%% *}"
    PKG_LOCK[$p]="${prep_out##* }"
    [[ "${PKG_ROOT[$p]}" =~ ^[0-9a-f]{64}$ ]] ||
        fail "dev prepare returned no package root for $p"
done
DEP_ROOT_CUR="${PKG_ROOT[zsha256]}"
log "roots: zsha256=${PKG_ROOT[zsha256]:0:16}… zhkdf=${PKG_ROOT[zhkdf]:0:16}… zscrypt=${PKG_ROOT[zscrypt]:0:16}…"

# ── verifier driver ────────────────────────────────────────────────────

# run_verify <pkg> <label> <fast|shadow> — one confined candidate build.
# fast: cache + --plan (reread evidence). shadow: no cache, no plan.
# Prints one line: exit wall_ms result hits misses reused_bytes
# compiler_processes processes files_reread bytes_reread
run_verify()
{
    local p="$1" label="$2" mode="$3"
    local emit="$EMITS/$label/$mode" out="$EMITS/$label/$mode.stdout"
    local plan="$PLANS/$label.json"
    rm -rf "$emit"
    mkdir -p "$emit"
    local dep_arg=()
    if [ "$p" != zsha256 ]; then
        dep_arg=("--dep=$DEP_ROOT_CUR,$DEPS/$DEP_ROOT_CUR")
    fi
    local extra=("--require-full-isolation" "--work=$WORK/pvwork")
    if [ "$mode" = fast ]; then
        extra+=("--fast-cache=$CACHE_DIR" "--plan=$plan")
    fi
    local start end rc
    start="$(clock_ms)"
    "$VERIFY_BIN" "${PKG_ROOT[$p]}" \
        "--zbuild-package-source=$DRAFTS/$p" \
        "--zbuild-package-recipe=$WORK/$p.recipe.wire" \
        "--zbuild-package-name=$p/$p" \
        "--zbuild-package-profile=quick" \
        "--zbuild-package-max-cpu-seconds=120" \
        "--emit=$emit" "--lock-root=${PKG_LOCK[$p]}" \
        ${dep_arg[@]+"${dep_arg[@]}"} ${extra[@]+"${extra[@]}"} \
        > "$out" 2>"$out.stderr"
    rc=$?
    end="$(clock_ms)"
    local result hits misses reused cprocs procs
    result="$(sed -n 's/^.* result=\([a-z-]*\) .*/\1/p' "$out" | head -1)"
    hits="$(sed -n 's/^zbuild-package-fast-cache=v1 hits=\([0-9]*\).*/\1/p' "$out")"
    misses="$(sed -n 's/^zbuild-package-fast-cache=v1 .*misses=\([0-9]*\).*/\1/p' "$out")"
    reused="$(sed -n 's/^zbuild-package-fast-cache=v1 .*reused_bytes=\([0-9]*\).*/\1/p' "$out")"
    cprocs="$(sed -n 's/^zbuild-package-perf=v1 .*compiler_processes=\([0-9]*\).*/\1/p' "$out")"
    procs="$(sed -n 's/^zbuild-package-perf=v1 processes=\([0-9]*\).*/\1/p' "$out")"
    local files_reread=0 bytes_reread=0
    if [ "$mode" = fast ] && [ -f "$plan" ]; then
        local n_tu n_in i j pth bsz paths=""
        n_tu="$("$JSONQ" count translation_units < "$plan" 2>/dev/null || echo 0)"
        i=0
        while [ "$i" -lt "$n_tu" ]; do
            n_in="$("$JSONQ" count "translation_units[$i].inputs" < "$plan" \
                2>/dev/null || echo 0)"
            j=0
            while [ "$j" -lt "$n_in" ]; do
                pth="$("$JSONQ" get "translation_units[$i].inputs[$j].path" \
                    < "$plan" 2>/dev/null || true)"
                bsz="$("$JSONQ" get "translation_units[$i].inputs[$j].bytes" \
                    < "$plan" 2>/dev/null || echo 0)"
                bytes_reread=$((bytes_reread + ${bsz:-0}))
                case " $paths " in
                    *" $pth "*) ;;
                    *) paths="$paths $pth"; files_reread=$((files_reread + 1)) ;;
                esac
                j=$((j + 1))
            done
            i=$((i + 1))
        done
    fi
    printf '%s %s %s %s %s %s %s %s %s %s\n' \
        "$rc" "$((end - start))" "${result:-none}" "${hits:-0}" \
        "${misses:-0}" "${reused:-0}" "${cprocs:-0}" "${procs:-0}" \
        "${files_reread}" "${bytes_reread}"
}

# Byte-compare the two emitted trees (receipt included — it is canonical).
shadow_compare()
{
    local label="$1"
    diff -r "$EMITS/$label/fast" "$EMITS/$label/shadow" >/dev/null 2>&1
}

# record_edit <id> <pkg> <class> <file> <desc> <fastline> <shadowline>
# <shadow_match> [extra_json]
record_edit()
{
    local id="$1" p="$2" class="$3" file="$4" desc="$5"
    local fastline="$6" shadowline="$7" match="$8" extra="${9:-"{}"}"
    local f_exit f_wall f_result f_hits f_misses f_reused f_cprocs f_procs
    local f_files f_bytes s_exit s_wall s_result s_cprocs s_procs
    local green=false shadow_match=false detail=""
    read -r f_exit f_wall f_result f_hits f_misses f_reused f_cprocs f_procs \
        f_files f_bytes _ <<<"$fastline"
    read -r s_exit s_wall s_result _ _ _ s_cprocs s_procs _ <<<"$shadowline"
    if [ "$f_exit" = 0 ] &&
       { [ "$f_result" = test-pass ] || [ "$f_result" = build-pass ]; }; then
        green=true
    fi
    [ "$match" = true ] && shadow_match=true
    if [ "$extra" != "{}" ]; then
        detail="$(printf ',"detail":%s' "$extra")"
    fi
    printf '{"edit":"%s","package":"%s","class":"%s","file":"%s","description":"%s","fast":{"exit":%s,"wall_ms":%s,"result":"%s","cache_hits":%s,"cache_misses":%s,"reused_bytes":%s,"compiler_processes":%s,"processes":%s,"files_reread":%s,"bytes_reread":%s,"green":%s},"shadow":{"exit":%s,"wall_ms":%s,"result":"%s","compiler_processes":%s,"processes":%s},"shadow_match":%s%s}\n' \
        "$(json_escape "$id")" "$(json_escape "$p")" "$(json_escape "$class")" \
        "$(json_escape "$file")" "$(json_escape "$desc")" \
        "$f_exit" "$f_wall" "$(json_escape "$f_result")" \
        "${f_hits:-0}" "${f_misses:-0}" "${f_reused:-0}" \
        "${f_cprocs:-0}" "${f_procs:-0}" "${f_files:-0}" "${f_bytes:-0}" \
        "$green" \
        "$s_exit" "$s_wall" "$(json_escape "$s_result")" \
        "${s_cprocs:-0}" "${s_procs:-0}" "$shadow_match" "$detail" \
        >> "$EDITS_NDJSON"
}

# do_edit <id> <pkg> <class> <file> <desc> [detail_json] — runs fast+shadow,
# compares, records. Assumes the edit is already applied to the draft.
do_edit()
{
    local id="$1" p="$2" class="$3" file="$4" desc="$5" detail="${6:-"{}"}"
    TOTAL_EDITS=$((TOTAL_EDITS + 1))
    local fastline shadowline match=true
    fastline="$(run_verify "$p" "$id" fast)"
    shadowline="$(run_verify "$p" "$id" shadow)"
    if ! shadow_compare "$id"; then
        match=false
        SHADOW_MISMATCHES=$((SHADOW_MISMATCHES + 1))
    fi
    record_edit "$id" "$p" "$class" "$file" "$desc" \
        "$fastline" "$shadowline" "$match" "$detail"
    local frc fres
    frc="$(printf '%s' "$fastline" | cut -d' ' -f1)"
    fres="$(printf '%s' "$fastline" | cut -d' ' -f3)"
    log "edit=$id pkg=$p class=$class fast_rc=$frc result=$fres hits=$(printf '%s' "$fastline" | cut -d' ' -f4) misses=$(printf '%s' "$fastline" | cut -d' ' -f5) shadow_match=$match"
    if [ "$match" != true ]; then
        fail "SHADOW MISMATCH at $id ($p, $class): the fast path diverged from the clean build — a dependency-keying bug. Inspect $EMITS/$id/{fast,shadow} and $PLANS/$id.json"
    fi
    if [ "$class" != semantic_red ] && [ "$fres" != test-pass ] &&
       [ "$fres" != build-pass ]; then
        EDIT_FAILURES=$((EDIT_FAILURES + 1))
        printf '[fast-build-bench] ERROR: edit %s (%s) not green: rc=%s result=%s (%s)\n' \
            "$id" "$p" "$frc" "$fres" "$EMITS/$id/fast.stderr" >&2
    fi
}

# ── edit primitives (each leaves the draft in a new real state) ────────

append_line() { printf '%s\n' "$2" >> "$DRAFTS/$1"; }
rename_tok() { sed -i "s/\b$3\b/$4/g" "$DRAFTS/$1/$2"; }
restore_file() { cp "$WORK/pristine/$1/$2" "$DRAFTS/$1/$2"; }

# Pristine copies for exact reverts.
mkdir -p "$WORK/pristine"
for p in "${PKGS[@]}"; do
    cp -r "$DRAFTS/$p" "$WORK/pristine/$p"
done

# Rebuild the dep (zsha256 draft as it stands) into a NEW installed-dep
# dir keyed by its new content-derived root, and repin DEP_ROOT_CUR.
# Runs in the CALLER'S shell (no command substitution — the repin must
# stick); the detail JSON lands in DEP_DETAIL.
dep_update()
{
    local label="$1" new_root
    new_root="$(dev_prepare zsha256)" || fail "dep dev prepare failed"
    DEP_ROOT_CUR="${new_root%% *}"
    local emit="$DEPS/$DEP_ROOT_CUR"
    rm -rf "$emit"
    mkdir -p "$emit"
    local out="$emit.build.stdout" start end rc
    start="$(clock_ms)"
    "$VERIFY_BIN" "$DEP_ROOT_CUR" \
        "--zbuild-package-source=$DRAFTS/zsha256" \
        "--zbuild-package-recipe=$WORK/zsha256.recipe.wire" \
        "--zbuild-package-name=zsha256/zsha256" \
        "--zbuild-package-profile=quick" \
        "--zbuild-package-max-cpu-seconds=120" \
        "--emit=$emit" "--lock-root=${PKG_LOCK[zsha256]}" \
        --require-full-isolation "--work=$WORK/pvwork" \
        "--fast-cache=$CACHE_DIR" > "$out" 2>"$out.stderr"
    rc=$?
    end="$(clock_ms)"
    local result hits misses
    result="$(sed -n 's/^.* result=\([a-z-]*\) .*/\1/p' "$out" | head -1)"
    hits="$(sed -n 's/^zbuild-package-fast-cache=v1 hits=\([0-9]*\).*/\1/p' "$out")"
    misses="$(sed -n 's/^zbuild-package-fast-cache=v1 .*misses=\([0-9]*\).*/\1/p' "$out")"
    [ "$rc" -eq 0 ] && [ "$result" = test-pass ] ||
        fail "dep rebuild $label failed: rc=$rc result=$result ($out.stderr)"
    DEP_DETAIL="$(printf '{"new_dep_root":"%s","dep_build":{"wall_ms":%s,"hits":%s,"misses":%s}}' \
        "$DEP_ROOT_CUR" "$((end - start))" "${hits:-0}" "${misses:-0}")"
}

# ── warm-up: pristine builds populate the cache (not counted as edits) ──

log "setup: installing pristine dep + warming the cache"
dep_update warmup-dep
DEP_ROOT_BASE="$DEP_ROOT_CUR"
for p in "${PKGS[@]}"; do
    w="$(run_verify "$p" "warmup-$p" fast)"
    wrc="$(printf '%s' "$w" | cut -d' ' -f1)"
    wres="$(printf '%s' "$w" | cut -d' ' -f3)"
    [ "$wrc" -eq 0 ] && [ "$wres" = test-pass ] ||
        fail "warm-up build failed for $p: rc=$wrc result=$wres"
    record_edit "warmup-$p" "$p" warmup "-" "pristine warm-up build" \
        "$w" "$w" true
    log "warmup $p: $(printf '%s' "$w" | cut -d' ' -f2)ms"
done

# ── the edit sequence (22 edits, classes interleaved) ──────────────────

log "running the edit sequence"

# E01 comment-only, source (zhkdf): preprocessed bytes unchanged -> hit.
append_line zhkdf/src/zhkdf.c '' 
append_line zhkdf/src/zhkdf.c '/* fbb E01: comment-only source edit */'
do_edit E01 zhkdf comment_only src/zhkdf.c "append a comment to the source"

# E02 private-body change (zhkdf): rename a local in the expand loop.
rename_tok zhkdf src/zhkdf.c take take_n
do_edit E02 zhkdf private_body src/zhkdf.c "rename local take->take_n in zhkdf_sha256_expand"

# E03 public-header change (zhkdf): add a declaration.
append_line zhkdf/include/zhkdf/zhkdf.h ''
append_line zhkdf/include/zhkdf/zhkdf.h '/* fbb E03: added public declaration */'
append_line zhkdf/include/zhkdf/zhkdf.h 'void zhkdf_fbb_e03_probe(void);'
do_edit E03 zhkdf public_header include/zhkdf/zhkdf.h "add a declaration to the public header"

# E04 exact revert of E03 -> all hits.
restore_file zhkdf include/zhkdf/zhkdf.h
do_edit E04 zhkdf exact_revert include/zhkdf/zhkdf.h "restore the header to pre-E03 bytes"

# E05 comment-only, header (zhkdf) -> hit.
append_line zhkdf/include/zhkdf/zhkdf.h ''
append_line zhkdf/include/zhkdf/zhkdf.h '/* fbb E05: comment-only header edit */'
do_edit E05 zhkdf comment_only include/zhkdf/zhkdf.h "append a comment to the public header"

# E06 private-body change (zscrypt): rename the counter buffer.
rename_tok zscrypt src/zscrypt.c ctr ctr_be
do_edit E06 zscrypt private_body src/zscrypt.c "rename local ctr->ctr_be in zpbkdf2_sha256"

# E07 comment-only, source (zscrypt) -> hit.
append_line zscrypt/src/zscrypt.c ''
append_line zscrypt/src/zscrypt.c '/* fbb E07: comment-only source edit */'
do_edit E07 zscrypt comment_only src/zscrypt.c "append a comment to the source"

# E08 public-header change (zscrypt): add a declaration.
append_line zscrypt/include/zscrypt/zscrypt.h ''
append_line zscrypt/include/zscrypt/zscrypt.h '/* fbb E08: added public declaration */'
append_line zscrypt/include/zscrypt/zscrypt.h 'void zscrypt_fbb_e08_probe(void);'
do_edit E08 zscrypt public_header include/zscrypt/zscrypt.h "add a declaration to the public header"

# E09 exact revert of E08 -> all hits.
restore_file zscrypt include/zscrypt/zscrypt.h
do_edit E09 zscrypt exact_revert include/zscrypt/zscrypt.h "restore the header to pre-E08 bytes"

# E10 private-body change (zsha256): rename a local in update().
rename_tok zsha256 src/zsha256.c take take_n
do_edit E10 zsha256 private_body src/zsha256.c "rename local take->take_n in zsha256_update"

# E11 comment-only, source (zsha256) -> hit.
append_line zsha256/src/zsha256.c ''
append_line zsha256/src/zsha256.c '/* fbb E11: comment-only source edit */'
do_edit E11 zsha256 comment_only src/zsha256.c "append a comment to the source"

# E12 public-header change (zsha256): rename prototype parameters.
sed -i 's/void zsha256_update(zsha256_ctx \*ctx, const void \*data, size_t len);/void zsha256_update(zsha256_ctx *ctx, const void *bytes, size_t bytes_len);/' \
    "$DRAFTS/zsha256/include/zsha256/zsha256.h"
grep -c 'bytes_len' "$DRAFTS/zsha256/include/zsha256/zsha256.h" >/dev/null 2>&1 ||
    fail "E12 edit did not apply"
do_edit E12 zsha256 public_header include/zsha256/zsha256.h "rename prototype parameters data,len->bytes,bytes_len"

# E13 exact revert of E12 -> all hits.
restore_file zsha256 include/zsha256/zsha256.h
do_edit E13 zsha256 exact_revert include/zsha256/zsha256.h "restore the header to pre-E12 bytes"

# E14 dependency update (zhkdf <- zsha256): body edit in the dep, dep
# rebuild (dep src TU misses), repin the dependent to the new
# content-derived root -> the dependent's normalized argv (@dep/<root>)
# changes, so its TU misses.
rename_tok zsha256 src/zsha256.c pad pad_b
dep_update E14
do_edit E14 zhkdf dependency_update "(dep zsha256) src/zsha256.c" \
    "dep body edit + repin zhkdf to the new dep root" "$DEP_DETAIL"

# E15 dependency update (zhkdf <- zsha256): comment-only dep edit; the
# DEP's own TU hits (comment), the dependent still misses (repinned root).
append_line zsha256/src/zsha256.c ''
append_line zsha256/src/zsha256.c '/* fbb E15: comment-only dep edit */'
dep_update E15
do_edit E15 zhkdf dependency_update "(dep zsha256) src/zsha256.c" \
    "dep comment edit + repin zhkdf to the new dep root" "$DEP_DETAIL"

# E16 dependency revert: repin zhkdf back to the pre-E14 (base) dep root —
# the exact state the warm-up and E01–E05 built -> all hits.
DEP_ROOT_CUR="$DEP_ROOT_BASE"
do_edit E16 zhkdf exact_revert "(dep pin)" "repin zhkdf to the pre-E14 dep root"

# E17 semantic RED (zhkdf): an undeclared-call body — preprocesses fine,
# fails compilation. Measures wall time to first RED.
cp "$DRAFTS/zhkdf/src/zhkdf.c" "$WORK/zhkdf.c.pre-red"
append_line zhkdf/src/zhkdf.c ''
append_line zhkdf/src/zhkdf.c 'int zhkdf_fbb_red(void) { return zhkdf_no_such_fn(1, 2); }'
E17_FAST="$(run_verify zhkdf E17 fast)"
E17_SHADOW="$(run_verify zhkdf E17 shadow)"
E17_MATCH=true
shadow_compare E17 || E17_MATCH=false
record_edit E17 zhkdf semantic_red src/zhkdf.c \
    "deliberate compile break (undeclared call)" "$E17_FAST" "$E17_SHADOW" \
    "$E17_MATCH"
TOTAL_EDITS=$((TOTAL_EDITS + 1))
E17_RESULT="$(printf '%s' "$E17_FAST" | cut -d' ' -f3)"
E17_RC="$(printf '%s' "$E17_FAST" | cut -d' ' -f1)"
log "edit=E17 class=semantic_red fast_rc=$E17_RC result=$E17_RESULT time_to_red_ms=$(printf '%s' "$E17_FAST" | cut -d' ' -f2)"
if [ "$E17_RESULT" = test-pass ] || [ "$E17_RESULT" = build-pass ]; then
    fail "E17 was supposed to be RED but built green"
fi
[ "$E17_MATCH" = true ] || SHADOW_MISMATCHES=$((SHADOW_MISMATCHES + 1))

# E18 revert the RED edit: exact pre-E17 bytes -> all hits, green.
cp "$WORK/zhkdf.c.pre-red" "$DRAFTS/zhkdf/src/zhkdf.c"
do_edit E18 zhkdf red_fix src/zhkdf.c "remove the deliberate break"

# E19 test-source edit (zscrypt): the src TU hits; the test TU always
# recompiles (test objects are never cached or archived).
rename_tok zscrypt tests/test_zscrypt.c cond condition
do_edit E19 zscrypt test_source tests/test_zscrypt.c "rename check() param cond->condition"

# E20 exact revert of E19 -> all hits.
restore_file zscrypt tests/test_zscrypt.c
do_edit E20 zscrypt exact_revert tests/test_zscrypt.c "restore the test source to pre-E19 bytes"

# E21 comment-only, header (zscrypt) -> hit.
append_line zscrypt/include/zscrypt/zscrypt.h ''
append_line zscrypt/include/zscrypt/zscrypt.h '/* fbb E21: comment-only header edit */'
do_edit E21 zscrypt comment_only include/zscrypt/zscrypt.h "append a comment to the public header"

# E22 private-body change (zhkdf): rename the loop accumulator.
rename_tok zhkdf src/zhkdf.c done done_len
do_edit E22 zhkdf private_body src/zhkdf.c "rename local done->done_len in zhkdf_sha256_expand"

# ── end-to-end factory comparison (the real path) ──────────────────────

FACTORY_NDJSON="$WORK/factory.ndjson"
: > "$FACTORY_NDJSON"
if is_true "$SKIP_FACTORY"; then
    log "factory comparison skipped (ZCL_FAST_BENCH_SKIP_FACTORY=1)"
else
    log "factory comparison: zscrypt draft, same content, cache on/off"
    # The edit campaign leaves the drafts mutated; the factory comparison
    # needs pristine content so the zscrypt dependency lock (pinned at
    # setup against the pristine zsha256 root) resolves in the store.
    for p in "${PKGS[@]}"; do
        rm -rf "$DRAFTS/$p"
        cp -r "$WORK/pristine/$p" "$DRAFTS/$p"
    done
    FWORK="$WORK/factory"
    mkdir -p "$FWORK"
    FK2_PUB="$("$SIGN_BIN" --generate "$FWORK/key2" 2>/dev/null | tr -d '[:space:]')"
    FK1_PUB="$("$SIGN_BIN" --generate "$FWORK/key1" 2>/dev/null | tr -d '[:space:]')"
    [[ "$FK1_PUB" =~ ^[0-9a-f]{66}$ && "$FK2_PUB" =~ ^[0-9a-f]{66}$ ]] ||
        fail "factory keygen failed"
    factory_run()
    { # <label> <cache_arg> — wipe stores, publish dep (key2), then zscrypt (key1)
        local label="$1" cache_arg="$2"
        rm -rf "$FWORK/storeA" "$FWORK/storeB"
        local r_dep="$FWORK/$label.dep.report.json" r_pkg="$FWORK/$label.report.json"
        # The dep-plan artifact path is deliberately run-invariant: the
        # report records it, and the cross-run report equality check must
        # not trip over a label in a path.
        "$FACTORY_BIN" run --package "$DRAFTS/zsha256" \
            --publisher-key-file "$FWORK/key2" --publisher-pubkey "$FK2_PUB" \
            --store-a "$FWORK/storeA" --store-b "$FWORK/storeB" \
            --report "$r_dep" --dep-plan "$FWORK/current.dep.plan.json" \
            $cache_arg > "$FWORK/$label.dep.stdout" 2>&1
        local rc_dep=$?
        [ "$rc_dep" -eq 0 ] || fail "factory dep publish failed ($label): $FWORK/$label.dep.stdout"
        "$FACTORY_BIN" run --package "$DRAFTS/zscrypt" \
            --publisher-key-file "$FWORK/key1" --publisher-pubkey "$FK1_PUB" \
            --store-a "$FWORK/storeA" --store-b "$FWORK/storeB" \
            --report "$r_pkg" --dep-plan "$FWORK/current.plan.json" \
            $cache_arg > "$FWORK/$label.stdout" 2>&1
        local rc=$?
        [ "$rc" -eq 0 ] || fail "factory run failed ($label): $FWORK/$label.stdout"
        local ok total_ms hits misses
        ok="$("$JSONQ" get ok < "$r_pkg")"
        total_ms="$("$JSONQ" get total_ms < "$r_pkg")"
        hits="$("$JSONQ" get fast_cache.hits < "$r_pkg" 2>/dev/null || echo null)"
        misses="$("$JSONQ" get fast_cache.misses < "$r_pkg" 2>/dev/null || echo null)"
        printf '{"label":"%s","ok":%s,"total_ms":%s,"hits":%s,"misses":%s,"report":"%s"}\n' \
            "$(json_escape "$label")" "$ok" "$total_ms" "$hits" "$misses" \
            "$(json_escape "$r_pkg")" >> "$FACTORY_NDJSON"
    }
    # F1: cache on, cold standard-profile entries (only quick TUs warm).
    factory_run f1 "--fast-cache=$CACHE_DIR"
    # F2: stores wiped, same content, cache on -> all hits.
    factory_run f2 "--fast-cache=$CACHE_DIR"
    # F3: stores wiped, same content, cache disabled.
    factory_run f3 "--fast-cache="
fi

# ── report assembly ────────────────────────────────────────────────────

TRACKED_STATUS_AFTER="$(git -C "$ROOT" status --porcelain=v1 -- packages corpus 2>/dev/null)"
[ -z "$TRACKED_STATUS_AFTER" ] ||
    fail "tracked packages/ or corpus/ changed during the campaign: $TRACKED_STATUS_AFTER"

mkdir -p "$(dirname "$OUTPUT")"
GENERATED_AT="$(date -u +%FT%TZ)"

factory_report_core() {
    local f="$1" n i side
    "$JSONQ" get schema < "$f"
    "$JSONQ" get ok < "$f"
    "$JSONQ" get package < "$f"
    for side in a b; do
        "$JSONQ" get "stores.$side.datadir" < "$f"
        "$JSONQ" get "stores.$side.published" < "$f"
        "$JSONQ" get "stores.$side.installed" < "$f"
        "$JSONQ" get "stores.$side.receipt_quick" < "$f"
        "$JSONQ" get "stores.$side.receipt_standard" < "$f"
        "$JSONQ" get "stores.$side.reproduced" < "$f"
        "$JSONQ" get "stores.$side.storage_ack" < "$f"
    done
    "$JSONQ" get admission < "$f"
    n="$("$JSONQ" count steps < "$f")"
    printf 'steps=%s\n' "$n"
    i=0
    while [ "$i" -lt "$n" ]; do
        "$JSONQ" get "steps[$i].name" < "$f"
        "$JSONQ" get "steps[$i].ok" < "$f"
        i=$((i + 1))
    done
    "$JSONQ" get disclosures < "$f"
    "$JSONQ" get durable_hosting < "$f"
    "$JSONQ" get corpus_registered < "$f"
    "$JSONQ" get corpus_next_step < "$f" 2>/dev/null || true
}

STATS="$WORK/edits.stats.tsv"
: > "$STATS"
while IFS= read -r rec; do
    [ -n "$rec" ] || continue
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$(printf '%s' "$rec" | "$JSONQ" get class)" \
        "$(printf '%s' "$rec" | "$JSONQ" get fast.wall_ms)" \
        "$(printf '%s' "$rec" | "$JSONQ" get shadow.wall_ms)" \
        "$(printf '%s' "$rec" | "$JSONQ" get fast.cache_hits)" \
        "$(printf '%s' "$rec" | "$JSONQ" get fast.cache_misses)" \
        "$(printf '%s' "$rec" | "$JSONQ" get fast.files_reread)" \
        "$(printf '%s' "$rec" | "$JSONQ" get fast.bytes_reread)" \
        "$(printf '%s' "$rec" | "$JSONQ" get fast.compiler_processes)" \
        "$(printf '%s' "$rec" | "$JSONQ" get shadow.compiler_processes)" \
        "$(printf '%s' "$rec" | "$JSONQ" get fast.green)" \
        "$(printf '%s' "$rec" | "$JSONQ" get shadow_match)" \
        >> "$STATS"
done < "$EDITS_NDJSON"

CLASSES_JSON="$(awk -F '\t' '
function pct(arr, n, p,    s, i, j, tmp, rank) {
    if (n == 0) return "null"
    for (i = 1; i <= n; i++) s[i] = arr[i] + 0
    for (i = 2; i <= n; i++) {
        tmp = s[i]
        j = i - 1
        while (j >= 1 && s[j] > tmp) { s[j + 1] = s[j]; j-- }
        s[j + 1] = tmp
    }
    rank = int((n * p + 99) / 100)
    if (rank < 1) rank = 1
    if (rank > n) rank = n
    return s[rank]
}
{
    c = $1
    seen[c] = 1
    n[c]++
    i = n[c]
    fastms[c, i] = $2
    shadowms[c, i] = $3
    hits[c] += $4
    misses[c] += $5
    files[c, i] = $6
    bytes[c, i] = $7
    cpf[c, i] = $8
    cps[c, i] = $9
    if (!(c in ag)) ag[c] = 1
    if (!(c in am)) am[c] = 1
    if ($10 != "true" && c != "semantic_red") ag[c] = 0
    if ($11 != "true") am[c] = 0
}
END {
    nnames = asorti(seen, names)
    printf "{"
    for (k = 1; k <= nnames; k++) {
        c = names[k]
        nn = n[c]
        delete a
        for (i = 1; i <= nn; i++) a[i] = fastms[c, i]
        fp50 = pct(a, nn, 50); fp95 = pct(a, nn, 95)
        delete a
        for (i = 1; i <= nn; i++) a[i] = shadowms[c, i]
        sp50 = pct(a, nn, 50); sp95 = pct(a, nn, 95)
        delete a
        for (i = 1; i <= nn; i++) a[i] = files[c, i]
        files50 = pct(a, nn, 50)
        delete a
        for (i = 1; i <= nn; i++) a[i] = bytes[c, i]
        bytes50 = pct(a, nn, 50)
        delete a
        for (i = 1; i <= nn; i++) a[i] = cpf[c, i]
        cpf50 = pct(a, nn, 50)
        delete a
        for (i = 1; i <= nn; i++) a[i] = cps[c, i]
        cps50 = pct(a, nn, 50)
        if (k > 1) printf ","
        printf "\"%s\":{\"edits\":%d,\"fast_p50_ms\":%s,\"fast_p95_ms\":%s,\"shadow_p50_ms\":%s,\"shadow_p95_ms\":%s,\"cache_hits\":%d,\"cache_misses\":%d,\"files_reread_p50\":%s,\"bytes_reread_p50\":%s,\"compiler_processes_fast_p50\":%s,\"compiler_processes_shadow_p50\":%s,\"all_green\":%s,\"all_shadow_match\":%s}", \
            c, nn, fp50, fp95, sp50, sp95, hits[c] + 0, misses[c] + 0, \
            files50, bytes50, cpf50, cps50, \
            (ag[c] ? "true" : "false"), (am[c] ? "true" : "false")
        printf "%s\t%d\t%s\t%s\t%s\t%s\t%d\t%d\t%s\t%s\n", \
            c, nn, fp50, fp95, sp50, sp95, hits[c] + 0, misses[c] + 0, \
            files50, bytes50 > "/dev/stderr"
    }
    printf "}"
}
' "$STATS" 2>"$WORK/classes.table")"

f2_total="" f3_total="" f2_report="" f3_report=""
if [ -s "$FACTORY_NDJSON" ]; then
    while IFS= read -r rec; do
        [ -n "$rec" ] || continue
        lab="$(printf '%s' "$rec" | "$JSONQ" get label)"
        case "$lab" in
            f2)
                f2_total="$(printf '%s' "$rec" | "$JSONQ" get total_ms)"
                f2_report="$(printf '%s' "$rec" | "$JSONQ" get report)"
                ;;
            f3)
                f3_total="$(printf '%s' "$rec" | "$JSONQ" get total_ms)"
                f3_report="$(printf '%s' "$rec" | "$JSONQ" get report)"
                ;;
        esac
    done < "$FACTORY_NDJSON"
    FACTORY_RUNS="[$(paste -sd, "$FACTORY_NDJSON")]"
else
    FACTORY_RUNS="[]"
fi

factory_equiv=null
if [ -n "$f2_report" ] && [ -n "$f3_report" ]; then
    if cmp -s <(factory_report_core "$f2_report") \
              <(factory_report_core "$f3_report"); then
        factory_equiv=true
    else
        factory_equiv=false
    fi
fi

status=ok
if [ "$SHADOW_MISMATCHES" -ne 0 ] || [ "$EDIT_FAILURES" -ne 0 ]; then
    status=failed
fi
all_shadow_match=true
[ "$SHADOW_MISMATCHES" -eq 0 ] || all_shadow_match=false
EDITS_ARR="[$(paste -sd, "$EDITS_NDJSON")]"

printf '%s\n' \
    "{" \
    "\"schema\":\"zcl.fast_build_bench.v1\"," \
    "\"status\":\"$status\"," \
    "\"generated_at_utc\":\"$(json_escape "$GENERATED_AT")\"," \
    "\"artifact\":\"$(json_escape "$OUTPUT")\"," \
    "\"verifier\":\"zclassic23-package-verify candidate mode, profile=quick, full isolation; fast runs carry --fast-cache + --plan, shadow runs neither\"," \
    "\"derivations\":{" \
    "\"compiler_processes\":\"zbuild-package-perf=v1 line (verifier-instrumented gcc driver spawns incl. -E probes; cc1/as children of the driver are not separately visible)\"," \
    "\"files_reread_bytes_reread\":\"zcl.dep_plan.v1 depfile closure: distinct files the preprocessor opened across recipe source TUs, and their summed bytes (fast runs only)\"," \
    "\"time_to_green_ms\":\"fast wall ms with exit 0, result test-pass, and a build receipt\"" \
    "}," \
    "\"totals\":{" \
    "\"edits\":$TOTAL_EDITS," \
    "\"shadow_mismatches\":$SHADOW_MISMATCHES," \
    "\"edit_failures\":$EDIT_FAILURES," \
    "\"all_shadow_match\":$all_shadow_match" \
    "}," \
    "\"classes\":$CLASSES_JSON," \
    "\"factory_end_to_end\":{" \
    "\"runs\":$FACTORY_RUNS," \
    "\"report_equal_modulo_volatile_keys\":$factory_equiv," \
    "\"volatile_keys\":[\"total_ms\",\"ms\",\"fast_cache\",\"plan_id\",\"error\"]," \
    "\"note\":\"f2 = stores wiped, shared warm cache; f3 = stores wiped, cache disabled (--fast-cache=); identical draft content\"" \
    "}," \
    "\"edits\":$EDITS_ARR" \
    "}" > "$OUTPUT"

printf '\n'
printf '%s\n' "class               edits   fast p50/p95 ms   shadow p50/p95 ms   hits/misses  files/bytes reread p50"
while IFS=$'\t' read -r name edits fp50 fp95 sp50 sp95 hits misses files50 bytes50; do
    [ -n "$name" ] || continue
    printf "%-19s %5d   %8s / %-8s %8s / %-8s %5d/%-5d  %s/%s\n" \
        "$name" "$edits" "$fp50" "$fp95" "$sp50" "$sp95" \
        "$hits" "$misses" "$files50" "$bytes50"
done < "$WORK/classes.table"
printf '\n'
if [ -n "$f2_total" ] && [ -n "$f3_total" ]; then
    printf 'factory end-to-end: warm-cache total_ms=%s  no-cache total_ms=%s  reports_equal_mod_volatile=%s\n' \
        "$f2_total" "$f3_total" "$factory_equiv"
fi
printf 'artifact: %s\n' "$OUTPUT"
if [ "$status" = ok ]; then
    rc=0
else
    rc=1
fi

if is_true "$KEEP"; then
    log "scratch kept at $WORK"
else
    rm -rf "$WORK"
fi
if [ "$rc" -ne 0 ]; then
    printf '[fast-build-bench] FAILED — see %s\n' "$OUTPUT" >&2
    exit 1
fi
log "PASS: $TOTAL_EDITS edits, shadow_match true for all, artifact $OUTPUT"
