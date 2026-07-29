#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# promote_fuzz_artifacts.sh — crash/timeout/OOM triage for the background
# fuzz lane (lane S2d, wf/s2d-replay-canary-crashloop).
#
# libFuzzer drops every crash/timeout/oom/slow-unit it finds under
# $ARTIFACT_DIR as "<harness>-<kind>-<sha1>" (the "<harness>-" part is the
# -artifact_prefix= background_quality_lane.sh passes per target — see
# run_fuzz() in tools/scripts/background_quality_lane.sh). Left alone
# those artifacts just accumulate: they are real, already-triggered
# regression inputs that never make it into the checked-in corpus, so the
# SAME bug class can be re-discovered (and re-timed-out on) forever
# instead of being fuzzed-past once it is fixed.
#
# This script promotes each artifact into lib/test/fuzz_seeds/<harness>/
# (the corpus dir the fuzz lane already seeds every run from — see
# run_fuzz()'s "$seed_dir" arg), so:
#   - a fix for the underlying hang/crash gets a REGRESSION seed for free
#     (the next `make fuzz` / background fuzz run replays it every time),
#   - content-identical artifacts (same bytes, different libFuzzer hash
#     name across runs) are deduped against both the existing corpus and
#     each other in the same triage pass,
#   - an oversized artifact (>1 MiB — cap chosen so the corpus stays
#     fast-checkout-friendly; a genuine >1 MiB hang input is worth a
#     human look, not silent corpus bloat) is SKIPPED with a note and
#     left in place for manual triage, never silently dropped or promoted.
#
# Usage:
#   promote_fuzz_artifacts.sh [--artifact-dir=DIR] [--seed-root=DIR]
#                              [--size-cap-bytes=N] [--dry-run]
#
# Exit code is always 0 on a normal triage pass (best-effort, log-only —
# see the call site in background_quality_lane.sh's run_fuzz(), which
# must never fail the fuzz lane's own verdict over a triage hiccup); a
# non-zero exit is reserved for a usage error (bad flag) or a directory
# that cannot be created.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

STATE_ROOT="${ZCL_QUALITY_STATE_DIR:-${XDG_STATE_HOME:-${HOME:-/tmp}/.local/state}/zclassic23-quality}"
ARTIFACT_DIR="$STATE_ROOT/artifacts"
SEED_ROOT="$REPO_ROOT/lib/test/fuzz_seeds"
SIZE_CAP_BYTES=1048576   # 1 MiB
DRY_RUN=0

for arg in "$@"; do
    case "$arg" in
        --artifact-dir=*)   ARTIFACT_DIR="${arg#--artifact-dir=}" ;;
        --seed-root=*)      SEED_ROOT="${arg#--seed-root=}" ;;
        --size-cap-bytes=*) SIZE_CAP_BYTES="${arg#--size-cap-bytes=}" ;;
        --dry-run)          DRY_RUN=1 ;;
        -h|--help)
            echo "usage: promote_fuzz_artifacts.sh [--artifact-dir=DIR] [--seed-root=DIR] [--size-cap-bytes=N] [--dry-run]"
            exit 0 ;;
        *) echo "promote-fuzz-artifacts: unknown arg '$arg'" >&2; exit 2 ;;
    esac
done

# After the arg loop: --seed-root= may have moved SEED_ROOT.
LEDGER="$SEED_ROOT/ARTIFACT_VERDICTS.txt"

if [ ! -d "$ARTIFACT_DIR" ]; then
    echo "promote-fuzz-artifacts: no artifact dir ($ARTIFACT_DIR) — nothing to triage"
    exit 0
fi

mkdir -p "$SEED_ROOT" 2>/dev/null || { echo "promote-fuzz-artifacts: cannot create seed root $SEED_ROOT" >&2; exit 1; }

# ── Known libFuzzer artifact "kind" markers ─────────────────────────
# The prefix before the FIRST of these (with its own trailing '-') is the
# harness name. "slow-unit" and "minimized-from" themselves contain a '-', so
# they must be checked before a naive single-token split would misparse them.
# Kept in step with ARTIFACT_RE in tools/lint/check_fuzz_artifact_replay.sh:
# these are the six prefixes libFuzzer writes. "minimized-from" is what
# `-minimize_crash=1` produces — the natural next step after a crash- lands,
# and until 2026-07-29 neither this script nor the gate could name it.
KIND_MARKERS="crash timeout oom slow-unit leak minimized-from"

derive_harness_and_kind() {  # $1 = basename -> prints "harness kind" or "" on no match
    local base="$1" marker rest
    for marker in $KIND_MARKERS; do
        case "$base" in
            *"-${marker}-"*)
                rest="${base##*-${marker}-}"
                # Only accept if what follows looks like a hex digest (the
                # libFuzzer convention) — guards against a harness name that
                # itself happens to contain "-timeout-" etc.
                case "$rest" in
                    [0-9a-fA-F]*)
                        printf '%s %s\n' "${base%-${marker}-*}" "$marker"
                        return 0 ;;
                esac ;;
        esac
    done
    return 1
}

# ── Dedup index: sha256 of every file already in the corpus ────────
declare -A SEEN_HASH
while IFS= read -r -d '' f; do
    h="$(sha256sum "$f" | cut -d' ' -f1)"
    SEEN_HASH["$h"]="$f"
done < <(find "$SEED_ROOT" -type f -print0 2>/dev/null)

promoted=0
dup=0
oversize=0
unparsed=0
declare -A PROMOTED_BY_HARNESS

for f in "$ARTIFACT_DIR"/*; do
    [ -f "$f" ] || continue
    base="$(basename "$f")"
    size="$(stat -c%s "$f" 2>/dev/null || wc -c < "$f")"

    if [ "$size" -gt "$SIZE_CAP_BYTES" ]; then
        echo "promote-fuzz-artifacts: SKIP oversize ($size > $SIZE_CAP_BYTES bytes) $base — left in place for manual triage"
        oversize=$((oversize + 1))
        continue
    fi

    hk="$(derive_harness_and_kind "$base" || true)"
    if [ -z "$hk" ]; then
        echo "promote-fuzz-artifacts: SKIP unparsed name (no harness-<kind>-<hex> pattern) $base — left in place"
        unparsed=$((unparsed + 1))
        continue
    fi
    harness="${hk% *}"
    kind="${hk#* }"

    hash="$(sha256sum "$f" | cut -d' ' -f1)"
    if [ -n "${SEEN_HASH[$hash]:-}" ]; then
        echo "promote-fuzz-artifacts: SKIP duplicate (content == ${SEEN_HASH[$hash]}) $base"
        dup=$((dup + 1))
        [ "$DRY_RUN" -eq 1 ] || rm -f "$f"
        continue
    fi

    dest_dir="$SEED_ROOT/$harness"
    # digest suffix from the artifact's own name (already a hex sha1 from
    # libFuzzer); keep it for traceability back to the original artifact.
    digest="${base##*-}"
    dest="$dest_dir/${kind}-${digest}.bin"

    if [ "$DRY_RUN" -eq 1 ]; then
        echo "promote-fuzz-artifacts: [dry-run] would promote $base -> $dest"
    else
        mkdir -p "$dest_dir"
        mv -f "$f" "$dest"
        echo "promote-fuzz-artifacts: promoted $base -> ${dest#"$REPO_ROOT"/}"
    fi
    SEEN_HASH["$hash"]="$dest"
    promoted=$((promoted + 1))
    PROMOTED_BY_HARNESS["$harness"]=$(( ${PROMOTED_BY_HARNESS["$harness"]:-0} + 1 ))

    # File an UNAUDITED verdict line alongside the bytes.
    #
    # Promotion used to be the end of the story: the artifact landed in the
    # corpus on the strength of its filename prefix alone, nothing re-ran it,
    # and nothing downstream could fail over it. That produced both halves of
    # this corpus's problem — a real remote hang that sat unread for two weeks,
    # and eight artifacts that were never bugs at all (spurious -timeout=2 trips
    # on a contended box) diluting the eight that mattered.
    #
    # An `unaudited` line is deliberately NOT a verdict. check-fuzz-artifact-
    # ledger rejects it, so a promoted artifact arrives already failing the
    # build until a human replays it and writes down what they decided. That is
    # the opposite of the old behaviour, where arriving silently was the default.
    if [ "$DRY_RUN" -eq 0 ] && [ -f "$LEDGER" ]; then
        if ! grep -q "^$harness/${kind}-${digest}.bin[[:space:]]" "$LEDGER" 2>/dev/null; then
            printf '%s/%s-%s.bin  unaudited  %s  # auto-filed by promote_fuzz_artifacts.sh; nobody has replayed this yet\n' \
                "$harness" "$kind" "$digest" "$(date -u +%Y-%m-%d)" >> "$LEDGER"
            echo "promote-fuzz-artifacts: filed an 'unaudited' verdict for ${dest#"$REPO_ROOT"/}"
            echo "promote-fuzz-artifacts:   -> replay it and replace that line, or the build stays red"
        fi
    fi
done

echo "promote-fuzz-artifacts: SUMMARY promoted=$promoted duplicate=$dup oversize=$oversize unparsed=$unparsed"
if [ "$promoted" -gt 0 ]; then
    for harness in "${!PROMOTED_BY_HARNESS[@]}"; do
        echo "promote-fuzz-artifacts:   $harness: ${PROMOTED_BY_HARNESS[$harness]}"
    done
fi
exit 0
