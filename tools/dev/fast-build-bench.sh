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

for bin in "$VERIFY_BIN" "$NODE_BIN" "$SIGN_BIN" "$FACTORY_BIN"; do
    [ -x "$bin" ] || fail "missing $bin — run: make -j\$(nproc)"
done
command -v python3 >/dev/null 2>&1 ||
    fail "python3 is required (plan-stat extraction, report assembly)"
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
    python3 - "$prep" "$WORK/$p.recipe.wire" <<'PYEOF'
import json, sys
doc = json.load(open(sys.argv[1]))
if not doc.get("ok"):
    sys.exit(1)
data = doc["data"]
with open(sys.argv[2], "wb") as fh:
    fh.write(bytes.fromhex(data["recipe_hex"]))
print(data["package_root"], data["dependency_lock_root"])
PYEOF
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
        local rr
        rr="$(python3 - "$plan" <<'PYEOF'
import json, sys
try:
    doc = json.load(open(sys.argv[1]))
except Exception:
    print(0, 0)
    sys.exit(0)
files = set()
total = 0
for tu in doc.get("translation_units", []):
    for inp in tu.get("inputs", []):
        files.add(inp.get("path", ""))
        total += int(inp.get("bytes", 0))
print(len(files), total)
PYEOF
)"
        files_reread="${rr%% *}"
        bytes_reread="${rr##* }"
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
    python3 - "$EDITS_NDJSON" "$id" "$p" "$class" "$file" "$desc" \
        "$fastline" "$shadowline" "$match" "$extra" <<'PYEOF'
import json, sys
(path, eid, pkg, klass, file_, desc, fastline, shadowline, match,
 extra_raw) = sys.argv[1:11]
f = fastline.split()
s = shadowline.split()
rec = {
    "edit": eid, "package": pkg, "class": klass, "file": file_,
    "description": desc,
    "fast": {
        "exit": int(f[0]), "wall_ms": int(f[1]), "result": f[2],
        "cache_hits": int(f[3]), "cache_misses": int(f[4]),
        "reused_bytes": int(f[5]),
        "compiler_processes": int(f[6]), "processes": int(f[7]),
        "files_reread": int(f[8]), "bytes_reread": int(f[9]),
        "green": f[2] in ("test-pass", "build-pass") and int(f[0]) == 0,
    },
    "shadow": {
        "exit": int(s[0]), "wall_ms": int(s[1]), "result": s[2],
        "compiler_processes": int(s[6]), "processes": int(s[7]),
    },
    "shadow_match": match == "true",
}
if extra_raw != "{}":
    rec["detail"] = json.loads(extra_raw)
with open(path, "a", encoding="utf-8") as fh:
    fh.write(json.dumps(rec) + "\n")
PYEOF
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
        python3 - "$r_pkg" "$label" >> "$FACTORY_NDJSON" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
fc = d.get("fast_cache") or {}
print(json.dumps({
    "label": sys.argv[2], "ok": d["ok"], "total_ms": d["total_ms"],
    "hits": fc.get("hits"), "misses": fc.get("misses"),
    "report": sys.argv[1],
}))
PYEOF
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
python3 - "$EDITS_NDJSON" "$FACTORY_NDJSON" "$OUTPUT" "$GENERATED_AT" \
    "$TOTAL_EDITS" "$SHADOW_MISMATCHES" "$EDIT_FAILURES" <<'PYEOF'
import json, sys

(edits_path, factory_path, out_path, generated_at,
 total_edits, shadow_mismatches, edit_failures) = sys.argv[1:8]

edits = [json.loads(line) for line in open(edits_path) if line.strip()]
factory = [json.loads(line) for line in open(factory_path) if line.strip()]

def pct(samples, p):
    if not samples:
        return None
    s = sorted(samples)
    rank = (len(s) * p + 99) // 100
    return s[min(max(rank, 1), len(s)) - 1]

classes = {}
for rec in edits:
    c = classes.setdefault(rec["class"], {"edits": 0, "fast_ms": [],
        "shadow_ms": [], "hits": 0, "misses": 0, "files_reread": [],
        "bytes_reread": [], "compiler_processes_fast": [],
        "compiler_processes_shadow": [], "all_green": True,
        "all_shadow_match": True})
    c["edits"] += 1
    c["fast_ms"].append(rec["fast"]["wall_ms"])
    c["shadow_ms"].append(rec["shadow"]["wall_ms"])
    c["hits"] += rec["fast"]["cache_hits"]
    c["misses"] += rec["fast"]["cache_misses"]
    c["files_reread"].append(rec["fast"]["files_reread"])
    c["bytes_reread"].append(rec["fast"]["bytes_reread"])
    c["compiler_processes_fast"].append(rec["fast"]["compiler_processes"])
    c["compiler_processes_shadow"].append(rec["shadow"]["compiler_processes"])
    c["all_green"] = c["all_green"] and (
        rec["fast"]["green"] or rec["class"] == "semantic_red")
    c["all_shadow_match"] = c["all_shadow_match"] and rec["shadow_match"]

class_rows = {}
for name, c in sorted(classes.items()):
    class_rows[name] = {
        "edits": c["edits"],
        "fast_p50_ms": pct(c["fast_ms"], 50),
        "fast_p95_ms": pct(c["fast_ms"], 95),
        "shadow_p50_ms": pct(c["shadow_ms"], 50),
        "shadow_p95_ms": pct(c["shadow_ms"], 95),
        "cache_hits": c["hits"], "cache_misses": c["misses"],
        "files_reread_p50": pct(c["files_reread"], 50),
        "bytes_reread_p50": pct(c["bytes_reread"], 50),
        "compiler_processes_fast_p50": pct(c["compiler_processes_fast"], 50),
        "compiler_processes_shadow_p50": pct(c["compiler_processes_shadow"], 50),
        "all_green": c["all_green"],
        "all_shadow_match": c["all_shadow_match"],
    }

f2 = next((f for f in factory if f["label"] == "f2"), None)
f3 = next((f for f in factory if f["label"] == "f3"), None)
factory_equiv = None
if f2 and f3:
    VOLATILE = {"total_ms", "ms", "fast_cache", "plan_id", "error"}
    def strip(v):
        if isinstance(v, dict):
            return {k: strip(x) for k, x in v.items() if k not in VOLATILE}
        if isinstance(v, list):
            return [strip(x) for x in v]
        return v
    factory_equiv = strip(json.load(open(f2["report"]))) == \
                    strip(json.load(open(f3["report"])))

status = "ok"
if int(shadow_mismatches) or int(edit_failures):
    status = "failed"

doc = {
    "schema": "zcl.fast_build_bench.v1",
    "status": status,
    "generated_at_utc": generated_at,
    "artifact": out_path,
    "verifier": "zclassic23-package-verify candidate mode, profile=quick, "
                "full isolation; fast runs carry --fast-cache + --plan, "
                "shadow runs neither",
    "derivations": {
        "compiler_processes": "zbuild-package-perf=v1 line (verifier-"
            "instrumented gcc driver spawns incl. -E probes; cc1/as children "
            "of the driver are not separately visible)",
        "files_reread_bytes_reread": "zcl.dep_plan.v1 depfile closure: "
            "distinct files the preprocessor opened across recipe source "
            "TUs, and their summed bytes (fast runs only)",
        "time_to_green_ms": "fast wall ms with exit 0, result test-pass, "
            "and a build receipt",
    },
    "totals": {
        "edits": int(total_edits),
        "shadow_mismatches": int(shadow_mismatches),
        "edit_failures": int(edit_failures),
        "all_shadow_match": int(shadow_mismatches) == 0,
    },
    "classes": class_rows,
    "factory_end_to_end": {
        "runs": factory,
        "report_equal_modulo_volatile_keys": factory_equiv,
        "volatile_keys": ["total_ms", "ms", "fast_cache", "plan_id", "error"],
        "note": "f2 = stores wiped, shared warm cache; f3 = stores wiped, "
                "cache disabled (--fast-cache=); identical draft content",
    },
    "edits": edits,
}
with open(out_path, "w", encoding="utf-8") as fh:
    json.dump(doc, fh, indent=1)
    fh.write("\n")

# ── text table ──
print()
print("class               edits   fast p50/p95 ms   shadow p50/p95 ms   hits/misses  files/bytes reread p50")
for name, row in class_rows.items():
    print("%-19s %5d   %8s / %-8s %8s / %-8s %5d/%-5d  %s/%s" % (
        name, row["edits"], row["fast_p50_ms"], row["fast_p95_ms"],
        row["shadow_p50_ms"], row["shadow_p95_ms"],
        row["cache_hits"], row["cache_misses"],
        row["files_reread_p50"], row["bytes_reread_p50"]))
print()
if f2 and f3:
    print("factory end-to-end: warm-cache total_ms=%s  no-cache total_ms=%s  "
          "reports_equal_mod_volatile=%s" %
          (f2["total_ms"], f3["total_ms"], factory_equiv))
print("artifact: %s" % out_path)
sys.exit(0 if status == "ok" else 1)
PYEOF
rc=$?

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
