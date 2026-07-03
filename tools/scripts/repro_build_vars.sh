# tools/scripts/repro_build_vars.sh — shared reproducible-build flag profile.
#
# Sourced (NOT executed) by:
#   tools/release.sh                       — the release archive builder
#   tools/scripts/check_reproducible_build.sh — the build-twice byte-identity gate
#
# Centralizing the determinism flags here means the release artifact and the
# reproducibility gate CANNOT drift apart: the gate proves the EXACT flag set a
# release uses, not an approximation of it.
#
# Sourcing this file exports three variables:
#   SOURCE_DATE_EPOCH  — pinned to the HEAD commit time (or caller-overridden),
#                        so every embedded timestamp + staged-file mtime is
#                        fixed across machines and across two builds.
#   REL_CFLAGS         — make's RESOLVED CFLAGS (every -I include path kept
#                        intact) with -march=native rewritten to the portable
#                        -march=x86-64-v3 baseline.
#   REL_LDFLAGS        — make's RESOLVED LDFLAGS with -Wl,--build-id=none
#                        appended so two links of the same source drop the
#                        nondeterministic build-id.
#
# Why rewrite -march instead of trusting the Makefile default: the Makefile DEV
# default already targets x86-64-v3 when ZCL_NATIVE is unset, BUT a host that
# exports ZCL_NATIVE=1 flips dev builds to -march=native (machine-specific). A
# release must be byte-stable across machines, so the release config ALWAYS
# forces the portable v3 baseline regardless of the dev setting. This is the
# same contract release.sh has always enforced; it now lives here once.
#
# CONTRACT: the caller has already cd'd into REPO_ROOT and `make`/`git` are on
# PATH. The helper defines no logging — callers print their own "info" lines.

# Resolve one make variable to its fully-expanded value. A command-line make
# assignment overrides the Makefile `CFLAGS =` / `LDFLAGS =` definition
# wholesale, so we hand back the RESOLVED value (which already contains every
# -I include path) and rewrite only the reproducibility-hostile tokens.
_repro_make_var() {
    make -pn 2>/dev/null | grep -E "^$1 = " | head -1 | cut -d= -f2- | sed 's/^ //'
}

# SOURCE_DATE_EPOCH: pin from the HEAD commit time. Allow a caller to override
# (e.g. check_reproducible_build.sh may pin to a literal constant for a stricter
# two-build test). The two-build gate sets this ONCE before sourcing so both
# builds share the identical value.
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct 2>/dev/null || echo 0)}"
export SOURCE_DATE_EPOCH

# CFLAGS: resolved, then force the portable ISA baseline (drop -march=native).
REL_CFLAGS="$(_repro_make_var CFLAGS | sed 's/-march=native/-march=x86-64-v3/g')"
# LDFLAGS: resolved, then drop the nondeterministic linker build-id.
REL_LDFLAGS="$(_repro_make_var LDFLAGS) -Wl,--build-id=none"
export REL_CFLAGS REL_LDFLAGS
