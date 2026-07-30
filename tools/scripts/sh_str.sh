# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# sh_str.sh — pipeline-free string predicates for this repo's shell tooling.
# Sourced, never executed. No `set` here: it must not change the caller's shell
# options.
#
# ── WHY THIS FILE EXISTS ────────────────────────────────────────────────────
# Nearly every script in tools/ runs under `set -o pipefail`, and the idiom
#
#     printf '%s' "$out" | grep -q 'needle'
#
# is BROKEN under it. grep -q exits at the FIRST match; printf then takes
# SIGPIPE; pipefail makes the pipeline report printf's 141 instead of grep's 0.
# So a SUCCESSFUL match becomes indistinguishable from a miss, and every
# decision written that way can silently invert.
#
# It is not theoretical and it is not rare. Hosted CI caught two instances on
# 2026-07-30 (run 30515927378, commit 7bcad23e6): the mandated stopwatch
# evidence judge and the promotion-receipt self-test each failed on a string
# they had printed themselves one line earlier, with
# `printf: write error: Broken pipe` in the log. Both pass on the dev host.
#
# Whether it bites depends on whether printf still had unwritten bytes when
# grep quit — i.e. on the 64 KB pipe buffer, the needle's position, and
# scheduling. That makes it a heisenbug that favours the quiet machine. With
# the needle on an early complete line and enough output after it, it is not
# flaky at all but deterministic: measured 30/30 false "missing" verdicts at
# 349 KB / 20,000 lines, versus 0/30 for str_contains. Reproduce with:
#
#   bash -c 'set -o pipefail
#     big="needle
#   $(for i in $(seq 1 20000); do echo filler $i; done)"
#     printf "%s" "$big" | grep -qF needle; echo "old rc=$?"'   # -> 141
#
# ── THE DIRECTION THAT MATTERS MOST ────────────────────────────────────────
# For a lint gate that greps for a violation, an EPIPE turns "found it" into
# "clean". A gate that reports a hollow PASS is worse than no gate, because
# something downstream is trusting it. That is why this is a correctness fix
# and not a tidy-up.
#
# ── RULE ───────────────────────────────────────────────────────────────────
# Any substring test whose EXIT STATUS is a decision must be pipeline-free.
# Use these helpers. Piping into `head -n1`/`sed` to extract a VALUE is fine —
# the value still arrives; only a status-carrying pipeline can invert.
#
# ── WHAT IS DELIBERATELY LEFT ALONE, AND WHY ───────────────────────────────
# `printf '%s' "$v" | grep -qE "$RE"` where $v is ONE SHORT VALUE — a single
# ledger line, a hex digest, a base64 signature, a hostname token — is not
# reachable by this bug. printf's whole write fits in the 64 KB pipe buffer and
# completes before grep can exit, so there is no unwritten remainder to take
# SIGPIPE. Eight such regex validators in promotion_receipt.sh were left exactly
# as they were, on purpose: `[[ $v =~ $RE ]]` would also be pipeline-free, but
# rewriting a signature/digest validator to a different regex engine to fix a
# bug it cannot have is how a real check quietly changes meaning. Convert those
# only with per-validator proof that accept/reject is unchanged.
#
# The dangerous shape is a MULTI-LINE haystack — a captured `$out` from a
# subcommand — with the needle on an early line. Those are the ones fixed.
#
# ── SCOPE STILL OPEN (measured 2026-07-30, not yet swept) ──────────────────
# Roughly 170 status-carrying `printf|grep -q` / `echo|grep -q` sites remain
# across ~55 files under tools/ and deploy/, every one of them in a script that
# sets pipefail. Several are lint gates, which is the bad direction: an EPIPE
# there reads a found violation as clean. Two files are fixed (this commit);
# the rest are a named, unratcheted debt. Sweeping them wants a per-site
# haystack-size judgement plus a gate that refuses new ones, which is its own
# task — do not bulk sed it.

# str_contains <haystack> <fixed-needle> — true if needle occurs in haystack.
# The needle is quoted inside the pattern, so it is always literal and can
# never be interpreted as a glob.
str_contains() {
    case "$1" in
        *"$2"*) return 0 ;;
        *)      return 1 ;;
    esac
}

# str_lacks <haystack> <fixed-needle> — the negation, spelled out so callers
# do not have to write `! str_contains` inside an `if ... && ...` chain where a
# stray `!` is easy to misread.
str_lacks() {
    case "$1" in
        *"$2"*) return 1 ;;
        *)      return 0 ;;
    esac
}
