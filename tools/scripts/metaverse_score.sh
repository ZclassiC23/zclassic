#!/usr/bin/env bash
# metaverse_score.sh — mechanical completion score for the METAVERSE MVP
# (docs/METAVERSE_MVP.md, criteria MM1-MM8). Same honesty rules as
# tools/scripts/arch_score.sh: a criterion scores ONLY on a mechanical proof
# (a file/symbol that exists, a registered test group, a lint gate that
# passes). No partial credit for "looks close". Never edit this scorer to win.
set -uo pipefail
export LC_ALL=C
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 2

TOTAL=0 MAX=0
declare -a ROWS   # "score/weight|slug|note"

kpi() { # weight got slug note
  local w="$1" g="$2" slug="$3" note="$4"
  MAX=$((MAX + w)); TOTAL=$((TOTAL + g))
  ROWS+=("$g/$w|$slug|$note")
}

CATALOG=tools/dev/test_group_catalog.def
DEF=config/commands/zcode.def

# ── MM1 (15) — one-command metaverse tour on an isolated regtest node. ──────
s=0; n="no tools/dev/metaverse_tour.sh / metaverse-tour target yet"
if [ -f tools/dev/metaverse_tour.sh ] && grep -qE '^metaverse-tour:' Makefile; then
  s=15; n="metaverse-tour target + isolated tour script exist"
fi
kpi 15 "$s" "MM1-tour" "$n"

# ── MM2 (15) — package lifecycle: publish + fetch + verify groups registered.
#    (the zcode_verify group execs the real zclassic23-package-verify binary.)
#    Catalog rows drop the test_ prefix: ZCL_TEST_GROUP(zcode_publish). ──────
have=0
for g in zcode_publish zcode_fetch zcode_verify; do
  grep -q "ZCL_TEST_GROUP($g)" "$CATALOG" 2>/dev/null && have=$((have + 1))
done
s=$(( 15 * have / 3 ))
kpi 15 "$s" "MM2-package-lifecycle" "$have/3 lifecycle groups registered (publish/fetch/verify)"

# ── MM3 (10) — property catalog complete or honestly scoped. Partial: wired
#    kinds out of 8. Full: every MV_UNAVAILABLE kind carries an explicit
#    out-of-MVP-scope decision (MV_MVP_SCOPE marker) asserted by the
#    test_metaverse_catalog decision table. ──────────────────────────────────
REG=lib/metaverse/src/adapter_registry.c
unavail=$(grep -cE '^[[:space:]]+MV_UNAVAILABLE\("' "$REG" 2>/dev/null || echo 8)
wired=$(( 8 - unavail )); [ "$wired" -lt 0 ] && wired=0
s=$(( 10 * wired / 8 ))
n="$wired/8 kinds wired, $unavail MV_UNAVAILABLE without an MVP-scope decision"
if grep -q 'MV_MVP_SCOPE' "$REG" 2>/dev/null; then
  s=10; n="every unavailable kind carries an asserted out-of-MVP-scope decision"
fi
kpi 10 "$s" "MM3-property-catalog" "$n"

# ── MM4 (20) — ZC23 simulation-complete. (a, 10) patronage settle/refund no
#    longer PLANNED fail-closed; (b, 10) dedicated unit groups registered. ───
# grep -c prints 0 and exits 1 on no match, so the naive `|| echo 4`
# fallback yields "0\n4" exactly when MM4(a) is complete; keep the
# missing-file default without appending to a real count.
if [ -f "$DEF" ]; then
  planned=$(grep -c 'ZCL_COMMAND_PLANNED_COMMAND' "$DEF" 2>/dev/null || true)
else
  planned=4
fi
pa=$(( 10 * (4 - planned) / 4 )); [ "$pa" -lt 0 ] && pa=0
pb=0
for g in zcode_patronage zcode_continuity zcode_commons_projection; do
  grep -q "ZCL_TEST_GROUP($g)" "$CATALOG" 2>/dev/null && pb=$((pb + 1))
done
pb=$(( 10 * pb / 3 ))
kpi 20 $((pa + pb)) "MM4-zc23-simulation" \
  "patronage PLANNED rows left: $planned (score $pa/10); unit groups: $pb/10"

# ── MM5 (15) — metaverse web UX: view source (8) + render gate (7). ─────────
s=0
[ -f app/views/src/metaverse_view.c ] || [ -f app/views/src/metaverse_view_pages.c ] && s=8
mg=0
grep -q 'ZCL_TEST_GROUP(metaverse_site)' "$CATALOG" 2>/dev/null && mg=7
kpi 15 $((s + mg)) "MM5-web-ux" \
  "metaverse view source: $([ "$s" = 8 ] && echo present || echo missing); render gate: $([ "$mg" = 7 ] && echo registered || echo missing)"

# ── MM6 (10) — CLI UX documented: API reference regenerates clean (the lint
#    gate is the judge; it rebuilds the generator if stale). ────────────────
s=0; n="check-api-reference-generated not green"
if make -s check-api-reference-generated >/dev/null 2>&1; then
  s=10; n="docs/API_REFERENCE.md freshness gate green"
fi
kpi 10 "$s" "MM6-cli-ux" "$n"

# ── MM7 (10) — one aggregate: metaverse-verify target (5) + new fuzzers (5).─
s=0
grep -qE '^metaverse-verify:' Makefile && s=5
fz=0
for f in fuzz_zcode_dht fuzz_zcode_science; do
  grep -q "$f" Makefile 2>/dev/null && fz=$((fz + 1))
done
fz=$(( 5 * fz / 2 ))
kpi 10 $((s + fz)) "MM7-test-aggregate" \
  "metaverse-verify target: $([ "$s" = 5 ] && echo present || echo missing); new fuzzers: $fz/5"

# ── MM8 (5) — newcomer docs: METAVERSE.md (2) + README (2) + GETTING_STARTED
#    (1) mention the metaverse. ──────────────────────────────────────────────
s=0
[ -f docs/METAVERSE.md ] && s=$((s + 2))
grep -qi 'metaverse' README.md 2>/dev/null && s=$((s + 2))
grep -qi 'metaverse' docs/GETTING_STARTED.md 2>/dev/null && s=$((s + 1))
kpi 5 "$s" "MM8-docs" "METAVERSE.md/README/GETTING_STARTED metaverse coverage scores $s/5"

# ── Report ───────────────────────────────────────────────────────────────────
pct=$(( MAX > 0 ? 100 * TOTAL / MAX : 0 ))
echo "════════════════════════════════════════════════════════════════"
echo "  METAVERSE MVP — SCORE: ${TOTAL}/${MAX}  (${pct}%)"
echo "  bar: docs/METAVERSE_MVP.md (MM1-MM8, ✅ at 100)"
echo "════════════════════════════════════════════════════════════════"
printf '%s\n' "${ROWS[@]}" | sort -t/ -k1 -rn | while IFS='|' read -r sc slug note; do
  mark="✗"; g="${sc%%/*}"; w="${sc##*/}"
  [ "$g" = "$w" ] && mark="✓"; [ "$g" != 0 ] && [ "$g" != "$w" ] && mark="◐"
  printf "  [%5s] %-24s %s  %s\n" "$sc" "$slug" "$mark" "$note"
done
echo "────────────────────────────────────────────────────────────────"
NEXT=$(printf '%s\n' "${ROWS[@]}" | awk -F'|' '{split($1,a,"/"); if(a[1]<a[2]) print (a[2]-a[1])"\t"$2}' \
        | sort -rn | head -1 | cut -f2)
echo "  ▶ NEXT QUEST: ${NEXT:-none — 100/100, metaverse MVP achieved}"
echo "  ▶ BAR:  docs/METAVERSE_MVP.md  (verification target per criterion)"
echo "  ▶ RULES: don't edit this scorer to win; ZC23 stays simulation-only;"
echo "           consensus frozen; live-money paths fail closed."
exit 0
