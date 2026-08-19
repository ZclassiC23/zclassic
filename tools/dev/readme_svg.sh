#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# readme_svg.sh — render the README's terminal figures from REAL command
# output, driven by `make readme-svg`.
#
# WHY THIS EXISTS. A screenshot in a README is a pinned fact with no live
# source: it keeps showing yesterday's command surface long after the binary
# moved, and a reader acts on it. This script removes the hand step. It runs
# the commands against the built binary, captures exactly what a terminal
# would show (ZCL_HUMAN=1, fixed COLUMNS so the layout is reproducible), and
# converts the ANSI stream to SVG. No screen, no browser, no image library —
# the renderer is text in, text out, so it works on a headless build host and
# the output diffs like source.
#
# Modes:
#   (default)                 regenerate docs/assets/z23-term-*.svg
#   READMESVG_CHECK=1         regenerate to a scratch dir and require the
#                             committed figures to be byte-identical
#
# `make readme-svg-check` is therefore a real staleness gate, exactly like
# arena-svg-check: change the command registry or the human renderer without
# regenerating and it fails, naming the figure.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

BIN="${ZCL_BIN_DIR:-$REPO_ROOT/build/bin}/z23"
OUT_DIR="docs/assets"
CHECK="${READMESVG_CHECK:-0}"

[ -x "$BIN" ] || { echo "readme_svg: FAIL — no built binary at $BIN (run: make)" >&2; exit 1; }

if [ "$CHECK" = "1" ]; then
    OUT_DIR="$(mktemp -d)"
    trap 'rm -rf "$OUT_DIR"' EXIT
fi

# ── the figures ───────────────────────────────────────────────────────────
# name | columns | window title | argv...
# Every command here must be a READ-ONLY leaf that needs no datadir, no
# network and no running node, so the figures are a property of the built
# binary alone and regenerate identically on any machine.
FIGURES=(
    "command-surface|100|z23 discover help|discover help"
    "guide|100|z23 zcode guide|zcode guide"
    "contract|100|z23 discover describe zcode.package.source.reproduce|discover describe zcode.package.source.reproduce"
)

# ── the demo figures ──────────────────────────────────────────────────────
# These three are NOT a property of the binary alone, so they are handled
# separately and honestly.
#
#   commons-demo / commons-proof  are RECORDINGS. `make commons-demo` runs two
#     real nodes for minutes; a staleness gate cannot re-run it. So the demo
#     writes what it proved and what it measured to a text artifact, that
#     artifact is committed, and this script only draws it. The numbers on the
#     README are therefore one real run on stated hardware — never typed in.
#
#   commons-topology is emitted by the demo script itself, so it always
#     matches the script that is in the tree.
#
# Both are then checked against reality below: the recorded strip must still
# name the stages the demo runs, and every command path in the topology must
# still exist in this binary's own registry.
JOURNEY="tools/dev/commons_journey_acceptance.sh"
STRIP_FILE="docs/assets/z23-commons-demo.strip"
FACTS_FILE="docs/assets/z23-commons-demo.facts"

demo_check() {
    [ -r "$STRIP_FILE" ] || { echo "readme_svg: FAIL — no recorded strip at $STRIP_FILE (run: make commons-demo)" >&2; exit 1; }
    [ -r "$FACTS_FILE" ] || { echo "readme_svg: FAIL — no recorded facts at $FACTS_FILE (run: make commons-demo)" >&2; exit 1; }
    # 1. the recording still describes the journey the script runs, in order.
    local rest labels
    rest="$(cat "$STRIP_FILE")"
    labels="$(bash "$JOURNEY" --strip-labels)"
    while IFS= read -r label; do
        case "$rest" in
            *"$label"*) rest="${rest#*"$label"}" ;;
            *) echo "readme_svg: FAIL — the recorded strip no longer shows '$label' in order; rerun: make commons-demo" >&2
               exit 1 ;;
        esac
    done <<<"$labels"
    # 2. every command the topology names still exists in this binary.
    local path
    for path in $(bash "$JOURNEY" --topology | tr -cs 'a-z0-9.' '\n' |
                  grep -E '^zcode(\.[a-z0-9]+)+$' | sort -u); do
        "$BIN" discover describe "$path" >/dev/null 2>&1 ||
            { echo "readme_svg: FAIL — the topology names $path, which this binary does not have" >&2; exit 1; }
    done
    # 3. the recording was produced by this exact journey script. A script
    #    change without a re-recording means the committed strip/facts no
    #    longer describe the journey in the tree: stale by construction.
    local recorded_script_sha3
    recorded_script_sha3="$(sed -n 's/^journey_script_sha3[[:space:]]*= //p' "$FACTS_FILE")"
    [ -n "$recorded_script_sha3" ] ||
        { echo "readme_svg: FAIL — $FACTS_FILE carries no provenance; re-record: ZCL_COMMONS_DEMO_RECORD=1 make commons-demo" >&2; exit 1; }
    [ "$recorded_script_sha3" = "$(openssl dgst -sha3-256 "$JOURNEY" | awk '{print $NF}')" ] ||
        { echo "readme_svg: FAIL — stale recording: $JOURNEY changed after it; re-record: ZCL_COMMONS_DEMO_RECORD=1 make commons-demo" >&2; exit 1; }
    # 4. the recording is internally intact: evidence_root binds the strip to
    #    the facts body, so a hand edit to either file breaks the hash.
    local recorded_evidence recomputed_evidence
    recorded_evidence="$(sed -n 's/^evidence_root[[:space:]]*= //p' "$FACTS_FILE")"
    recomputed_evidence="$(cat "$STRIP_FILE" <(grep -v '^evidence_root[[:space:]]*=' "$FACTS_FILE") |
                           openssl dgst -sha3-256 | awk '{print $NF}')"
    [ "$recorded_evidence" = "$recomputed_evidence" ] ||
        { echo "readme_svg: FAIL — $STRIP_FILE and $FACTS_FILE no longer match each other; re-record: ZCL_COMMONS_DEMO_RECORD=1 make commons-demo" >&2; exit 1; }
    # 5. binary drift is reported, not fatal: the numbers are a recording of
    #    one real run, and the binary is rebuilt on every source change.
    local recorded_bin_sha3
    recorded_bin_sha3="$(sed -n 's/^z23_binary_sha3[[:space:]]*= //p' "$FACTS_FILE")"
    [ "$recorded_bin_sha3" = "$(openssl dgst -sha3-256 "$REPO_ROOT/build/bin/zclassic23" | awk '{print $NF}')" ] ||
        echo "readme_svg: note — the recording predates the current build/bin/zclassic23" >&2
}

# ── ANSI → SVG ────────────────────────────────────────────────────────────
# Monospace layout is computed, not trusted to the viewer's font: every run
# is placed at its exact column and given a textLength, so the figure stays
# aligned wherever it renders.
ansi_to_svg() {
    local title="$1" cols="$2"
    awk -v TITLE="$title" -v COLS="$cols" '
    function esc(s) {
        gsub(/&/, "\\&amp;", s)
        gsub(/</, "\\&lt;",  s)
        gsub(/>/, "\\&gt;",  s)
        return s
    }
    function color_of(  c) {
        c = FG
        if (c == "") c = (BOLD ? "#eef4fb" : (DIM ? "#5d7186" : "#c3d2e2"))
        return c
    }
    function flush_run(text,  n) {
        n = length(text)
        if (n == 0) return
        if (text ~ /^ +$/) { return }   # nothing to draw for pure whitespace
        printf "<tspan x=\"%.1f\" textLength=\"%.1f\" lengthAdjust=\"spacing\" fill=\"%s\"%s>%s</tspan>", \
               PADX + COL * CW, n * CW, color_of(), \
               (BOLD ? " font-weight=\"bold\"" : ""), esc(text)
    }
    function sgr(params,  i, n, parts, p) {
        n = split(params, parts, ";")
        if (n == 0) { BOLD = 0; DIM = 0; FG = ""; return }
        for (i = 1; i <= n; i++) {
            p = parts[i] + 0
            if (parts[i] == "")            { BOLD = 0; DIM = 0; FG = "" }
            else if (p == 0)               { BOLD = 0; DIM = 0; FG = "" }
            else if (p == 1)               { BOLD = 1 }
            else if (p == 2)               { DIM = 1 }
            else if (p == 22)              { BOLD = 0; DIM = 0 }
            else if (p == 39)              { FG = "" }
            else if (p == 30 || p == 90)   { FG = "#5d7186" }
            else if (p == 31 || p == 91)   { FG = "#ff6b5e" }
            else if (p == 32 || p == 92)   { FG = "#6bd18a" }
            else if (p == 33 || p == 93)   { FG = "#f2c14e" }
            else if (p == 34 || p == 94)   { FG = "#57a6ff" }
            else if (p == 35 || p == 95)   { FG = "#c88bff" }
            else if (p == 36 || p == 96)   { FG = "#5ecfd6" }
            else if (p == 37 || p == 97)   { FG = "#eef4fb" }
        }
    }
    BEGIN {
        CW = 8.4; LH = 21; FS = 14
        PADX = 24; TOPBAR = 44; PADTOP = 18; PADBOT = 22
        BOLD = 0; DIM = 0; FG = ""
        nlines = 0
    }
    {
        lines[nlines++] = $0
        plain = $0
        gsub(/\033\[[0-9;]*m/, "", plain)
        if (length(plain) > maxvis) maxvis = length(plain)
    }
    END {
        cells = (maxvis > COLS ? maxvis : COLS)  # COLS is the bound; maxvis is a safety net
        W = PADX * 2 + cells * CW
        H = TOPBAR + PADTOP + nlines * LH + PADBOT
        printf "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.0f\" height=\"%.0f\" viewBox=\"0 0 %.0f %.0f\" role=\"img\" aria-label=\"%s\">\n", \
               W, H, W, H, esc(TITLE)
        printf "<rect width=\"%.0f\" height=\"%.0f\" rx=\"12\" fill=\"#0b0f16\"/>\n", W, H
        printf "<rect x=\"0.5\" y=\"0.5\" width=\"%.0f\" height=\"%.0f\" rx=\"11.5\" fill=\"none\" stroke=\"#243044\"/>\n", W - 1, H - 1
        printf "<path d=\"M0 %d h%.0f\" stroke=\"#1a2432\"/>\n", TOPBAR, W
        printf "<circle cx=\"26\" cy=\"22\" r=\"5.5\" fill=\"#ff6b5e\"/>"
        printf "<circle cx=\"46\" cy=\"22\" r=\"5.5\" fill=\"#f2c14e\"/>"
        printf "<circle cx=\"66\" cy=\"22\" r=\"5.5\" fill=\"#6bd18a\"/>\n"
        printf "<text x=\"%.0f\" y=\"27\" font-family=\"ui-monospace, SFMono-Regular, Menlo, Consolas, monospace\" font-size=\"13\" fill=\"#66788c\" text-anchor=\"middle\">%s</text>\n", \
               W / 2, esc(TITLE)
        for (i = 0; i < nlines; i++) {
            line = lines[i]
            y = TOPBAR + PADTOP + i * LH + FS
            printf "<text y=\"%.0f\" font-family=\"ui-monospace, SFMono-Regular, Menlo, Consolas, monospace\" font-size=\"%d\" xml:space=\"preserve\">", y, FS
            COL = 0
            while (length(line) > 0) {
                idx = match(line, /\033\[[0-9;]*m/)
                if (idx == 0) { flush_run(line); COL += length(line); break }
                if (idx > 1) {
                    chunk = substr(line, 1, idx - 1)
                    flush_run(chunk)
                    COL += length(chunk)
                }
                seq = substr(line, idx, RLENGTH)
                sgr(substr(seq, 3, length(seq) - 3))
                line = substr(line, idx + RLENGTH)
            }
            printf "</text>\n"
        }
        printf "</svg>\n"
    }
    '
}

status=0
for fig in "${FIGURES[@]}"; do
    IFS='|' read -r name cols title argv <<<"$fig"
    dest="$OUT_DIR/z23-term-$name.svg"
    # shellcheck disable=SC2086
    if ! out="$(env -u NO_COLOR ZCL_HUMAN=1 COLUMNS="$cols" \
                TERM=xterm-256color "$BIN" $argv 2>&1)"; then
        echo "readme_svg: FAIL — '$BIN $argv' exited non-zero" >&2
        exit 1
    fi
    printf '%s\n' "$out" | ansi_to_svg "$title" "$cols" > "$dest"
    if [ "$CHECK" = "1" ]; then
        if cmp -s "$dest" "docs/assets/z23-term-$name.svg"; then
            echo "readme-svg-check: docs/assets/z23-term-$name.svg  up to date"
        else
            echo "readme-svg-check: STALE  docs/assets/z23-term-$name.svg" >&2
            echo "  the figure no longer matches '$BIN $argv' — run: make readme-svg" >&2
            status=1
        fi
    else
        echo "readme-svg: wrote $dest  ($(wc -c <"$dest") bytes)"
    fi
done

demo_check
for fig in \
    "commons-demo|100|make commons-demo|cat $STRIP_FILE" \
    "commons-proof|100|what the demo measured|cat $FACTS_FILE" \
    "commons-topology|100|how the bytes travel|bash $JOURNEY --topology"
do
    IFS='|' read -r name cols title source <<<"$fig"
    dest="$OUT_DIR/z23-term-$name.svg"
    # shellcheck disable=SC2086
    eval $source | ansi_to_svg "$title" "$cols" > "$dest"
    if [ "$CHECK" = "1" ]; then
        if cmp -s "$dest" "docs/assets/z23-term-$name.svg"; then
            echo "readme-svg-check: docs/assets/z23-term-$name.svg  up to date"
        else
            echo "readme-svg-check: STALE  docs/assets/z23-term-$name.svg" >&2
            echo "  the figure no longer matches its source — run: make readme-svg" >&2
            status=1
        fi
    else
        echo "readme-svg: wrote $dest  ($(wc -c <"$dest") bytes)"
    fi
done

exit "$status"
