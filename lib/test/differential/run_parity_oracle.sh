#!/usr/bin/env bash
# Sapling Groth16 differential parity oracle — build + replay.
#
#   run_parity_oracle.sh check    (default) rebuild verifier + assert every
#                                  verdict matches the frozen golden. rc!=0 on
#                                  ANY mismatch (= a consensus-affecting change).
#   run_parity_oracle.sh record    re-freeze the golden from the CURRENT verifier
#                                  (only after an INTENTIONAL, replay-approved
#                                  consensus change — never to paper over a diff).
#   run_parity_oracle.sh list      print the vector manifest.
#   run_parity_oracle.sh bench [n] time naive vs fixed-base public-input paths
#                                  over n iterations (default 30).
#
# Builds the CURRENT in-tree consensus verifier (lib/sapling/src/bls12_381.c)
# straight from source, so any edit to that file is exercised on the next run.
set -euo pipefail
MODE="${1:-check}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BIN="${TMPDIR:-/tmp}/g16_parity_oracle.$$"

case "$MODE" in
    bench) MAIN="$HERE/groth16_comb_bench.c" ;;
    *)     MAIN="$HERE/groth16_parity_oracle.c" ;;
esac

cc -std=c23 -O2 -march=x86-64-v3 -DZCL_TESTING \
   -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
   -I"$ROOT/lib/sapling/include" -I"$ROOT/lib/crypto/include" \
   -I"$ROOT/lib/util/include" \
   "$MAIN" \
   "$ROOT/lib/sapling/src/bls12_381.c" \
   "$ROOT/lib/sapling/src/fr_avx512.c" \
   "$ROOT/lib/crypto/src/blake2b.c" \
   "$ROOT/lib/util/src/safe_alloc.c" \
   "$ROOT/lib/util/src/log_level.c" \
   -o "$BIN"
trap 'rm -f "$BIN"' EXIT

# Both binaries silence the verifier's own reject-path logging themselves, so
# stderr here carries only mismatch / divergence diagnostics — never swallow it.
if [ "$MODE" = bench ]; then
    "$BIN" "${2:-30}"
else
    "$BIN" "$MODE" "$HERE"
fi
