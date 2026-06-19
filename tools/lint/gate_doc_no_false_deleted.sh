#!/usr/bin/env bash
# Lint gate — doc no-false-deleted (doc-honesty ratchet).
#
# Doc rot in the dangerous direction: a doc declares a CODE ENGINE/PATH
# "deleted" / "removed" / "no longer exists" while that code is STILL
# PRESENT and STILL LIVE-CALLED. That is a lie a reader will act on
# (e.g. "the legacy block-connect engine has been deleted" — but
# lib/validation/src/connect_block.c still exists and boot_index.c calls
# connect_block()). This gate makes that specific class of lie a hard fail.
#
# It also pins two load-bearing FACTS the docs cite to the real tree:
#   - the binary size in MB  (vs `ls -lh build/bin/zclassic23`)
#   - the MCP tool count     (vs the tools/mcp/controllers/*_controller.c
#                             routing tables)
# so an ADR/FRAMEWORK number can never silently drift far from reality.
#
# RATCHET design: it encodes the CURRENT tree as the baseline (the real
# size band, the real tool-count band) so it PASSES once the docs are
# corrected, and only FAILS when someone (a) re-asserts a false "deleted"
# claim about live code, or (b) edits a doc to cite a size/count that no
# longer matches the binary.
#
# Exit 0 = pass. Non-zero + a clear message = fail.
set -uo pipefail

# Resolve repo root: arg 1 if given, else the dir this script lives in /../..,
# else cwd. Designed to be runnable from /tmp against an arbitrary tree.
if [ "${1:-}" != "" ] && [ -d "$1" ]; then
    ROOT="$1"
elif [ -d "$(dirname "$0")/../docs" ]; then
    ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
else
    ROOT="$(pwd)"
fi
cd "$ROOT" || { echo "FAIL: cannot cd into repo root '$ROOT'"; exit 1; }

fail=0
err() { echo "FAIL: $*"; fail=1; }

##############################################################################
# PART 1 — no false "deleted" claim about live code.
#
# Table of { forbidden_claim_regex | code_path | live_caller_glob | symbol }
# A row FIRES (fail) iff ALL of:
#   - some doc matches forbidden_claim_regex, AND
#   - code_path still exists, AND
#   - symbol is still referenced from a live-caller file.
# i.e. the doc says "gone" but the code is present + wired in.
##############################################################################
DOCS=$(ls docs/*.md CLAUDE.md 2>/dev/null; find docs -name '*.md' 2>/dev/null)
DOCS=$(printf '%s\n' $DOCS | sort -u)

# Each row: regex|||path|||caller1,caller2|||symbol
ROWS=(
  'legacy[^.]*block-connect engine[^.]*\b(deleted|removed|no longer exists)\b|||lib/validation/src/connect_block.c|||config/src/boot_index.c,app/controllers/src/sync_controller_blocks.c|||connect_block'
  'connect_block[^.]*\b(has been deleted|was deleted|is deleted|removed|no longer exists)\b|||lib/validation/src/connect_block.c|||config/src/boot_index.c|||connect_block'
)

for row in "${ROWS[@]}"; do
    # split on the multi-char '|||' delimiter via parameter expansion:
    regex=${row%%'|||'*}
    rest=${row#*'|||'}
    path=${rest%%'|||'*}
    rest=${rest#*'|||'}
    callers=${rest%%'|||'*}
    symbol=${rest##*'|||'}

    # Does any doc make the forbidden claim? Scan with newlines collapsed to
    # spaces so a claim that WRAPS across two lines (markdown reflow) is still
    # caught (e.g. "...the legacy\nblock-connect engine has been deleted").
    hit_doc=""
    for d in $DOCS; do
        [ -f "$d" ] || continue
        if tr '\n' ' ' < "$d" | grep -Eiq "$regex"; then
            # best-effort line hint: the single-line tail of the claim
            ln=$(grep -EniE 'block-connect engine|no longer exists|has been (deleted|removed)' "$d" 2>/dev/null | head -1 | cut -d: -f1 || true)
            hit_doc="$d${ln:+:$ln}"
            break
        fi
    done
    [ -n "$hit_doc" ] || continue   # no claim -> nothing to enforce

    # Claim exists. Is the code actually gone? If gone, the claim is TRUE -> ok.
    if [ ! -f "$path" ]; then
        continue
    fi
    # Code present. Is the symbol still live-called from a real caller?
    live=""
    IFS=',' read -ra carr <<<"$callers"
    for c in "${carr[@]}"; do
        [ -f "$c" ] || continue
        if grep -Enq "\b${symbol}\b *\(" "$c"; then
            live="$c"
            break
        fi
    done
    if [ -n "$live" ]; then
        err "doc claims '$symbol' engine is deleted/removed ($hit_doc) but $path EXISTS and is live-called from $live."
        echo "      -> Either the doc lies (fix the doc) or the code is truly dead (then delete $path)."
    fi
done

##############################################################################
# PART 2 — binary-size fact pinned to the real binary (band check).
# Docs cite e.g. "~15 MB" / "16 MB" / "26 MB". The real binary must be
# within [LO,HI] MB of every size an ADR/FRAMEWORK/CLAUDE doc cites.
##############################################################################
BIN=build/bin/zclassic23
SIZE_DOCS="docs/FRAMEWORK.md docs/adr/0001-personal-sovereignty-stack.md CLAUDE.md"
SIZE_BAND_MB=6   # tolerance: a cited MB number may not differ from real by > this

if [ -f "$BIN" ]; then
    real_bytes=$(wc -c < "$BIN")
    real_mb=$(( real_bytes / 1048576 ))
    for d in $SIZE_DOCS; do
        [ -f "$d" ] || continue
        # pull each "<num> MB" / "<num>MB" mention
        while read -r n; do
            [ -n "$n" ] || continue
            diff=$(( real_mb - n )); [ "$diff" -lt 0 ] && diff=$(( -diff ))
            if [ "$diff" -gt "$SIZE_BAND_MB" ]; then
                err "$d cites '${n} MB' but real $BIN is ${real_mb} MB (band ±${SIZE_BAND_MB} MB)."
            fi
        done < <(grep -oE '[0-9]+ ?MB' "$d" | grep -oE '[0-9]+' | sort -u)
    done
else
    echo "INFO: $BIN not built; skipping binary-size fact check (build first to enforce)."
fi

##############################################################################
# PART 3 — MCP tool-count fact pinned to the routing tables (band check).
# Real count = distinct "zcl_*" tool rows in tools/mcp/controllers/*.c.
# Docs cite "~100 tools", "93 MCP tools", etc. Each cited count must be
# within [-COUNT_LO, +COUNT_HI] of the real count.
##############################################################################
COUNT_DOCS="docs/adr/0001-personal-sovereignty-stack.md docs/FRAMEWORK.md CLAUDE.md"
COUNT_BAND=12   # "~100" prose vs a real 98 is fine; a hard 60 or 200 is not
CTRL_GLOB="tools/mcp/controllers"

if [ -d "$CTRL_GLOB" ]; then
    real_tools=$(grep -rhoE '^[[:space:]]*\{[[:space:]]*"zcl_[a-z_]+"' "$CTRL_GLOB"/*_controller.c 2>/dev/null \
                 | grep -oE '"zcl_[a-z_]+"' | sort -u | grep -c .)
    if [ "${real_tools:-0}" -lt 50 ]; then
        err "tool-count probe found only ${real_tools} zcl_ tools in $CTRL_GLOB — routing-table format may have changed; update this gate."
    else
        for d in $COUNT_DOCS; do
            [ -f "$d" ] || continue
            # match "<num> (typed )?(MCP )?tools" and "~<num> ... tools"
            while read -r n; do
                [ -n "$n" ] || continue
                diff=$(( real_tools - n )); [ "$diff" -lt 0 ] && diff=$(( -diff ))
                if [ "$diff" -gt "$COUNT_BAND" ]; then
                    err "$d cites '${n}' MCP tools but routing tables have ${real_tools} (band ±${COUNT_BAND})."
                fi
            done < <(grep -oiE '~?[0-9]+ (typed )?(mcp )?tools' "$d" | grep -oE '[0-9]+' | sort -u)
        done
    fi
else
    echo "INFO: $CTRL_GLOB not found; skipping tool-count fact check."
fi

if [ "$fail" -ne 0 ]; then
    echo "── gate_doc_no_false_deleted: FAIL ──"
    exit 1
fi
echo "── gate_doc_no_false_deleted: pass ──"
exit 0
