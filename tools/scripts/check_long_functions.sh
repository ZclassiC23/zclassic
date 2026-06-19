#!/usr/bin/env bash
# Lint gate #12 — controllers/services functions over 500 lines.
#
# Long functions are hard to reason about, hard to test, and a sign
# that a single C function is doing too many things. This script keeps
# the regression door closed by flagging any new function that crosses
# the 500-line cap.
#
# Override: add `// long-function-ok:<tag>` to the function's signature
# line if a single state-machine truly belongs as one function.  The
# tag must explain WHY.
set -euo pipefail
LIMIT=500
fail=0
for f in app/controllers/src/*.c app/services/src/*.c tools/mcp/controllers/*.c; do
    awk -v file="$f" -v limit="$LIMIT" '
      /^[a-zA-Z_].*\(.*\)/ && !/;/ && !/^\s/ { sig=$0; start=NR; long_ok=0 }
      sig && /\/\/ *long-function-ok:[A-Za-z][A-Za-z0-9_-]*/ { long_ok=1 }
      /^\}[[:space:]]*(\/\/.*|\/\*.*)?$/ && start {
        len=NR-start;
        if (len > limit && !long_ok) {
          printf "FAIL: %s:%d  function spans %d lines (cap %d): %s\n", file, start, len, limit, substr(sig,1,80);
          exit 1
        }
        start=0; sig=""; long_ok=0
      }
    ' "$f" || fail=1
done
[ "$fail" = "0" ] && echo "check_long_functions: clean — no untagged functions over $LIMIT lines"
exit $fail
