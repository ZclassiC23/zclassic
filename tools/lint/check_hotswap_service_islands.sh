#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Pure service-island confinement. Build-free and fail-loud: the C23 compiler
# remains the only compiler authority; this gate only narrows what an island
# may import or own.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$ROOT"
. tools/lint/gate_lib.sh

MANIFEST="${ZCL_HOTSWAP_SERVICE_MANIFEST:-config/hotswap_services.def}"
FIXTURE_MODE="${ZCL_HOTSWAP_SERVICE_FIXTURE:-0}"
echo "══ LINT: pure hot-swap service islands ══"
if [ ! -r "$MANIFEST" ]; then
    echo "check_hotswap_service_islands: FATAL — manifest '$MANIFEST' missing" >&2
    exit 2
fi

MACRO_AWK='
{ buf = buf $0 "\n" }
END {
  n=length(buf); tok="HOTSWAP_SERVICE("; L=length(tok); i=1
  while (i<=n) {
    if (substr(buf,i,L)!=tok || (i>1 && substr(buf,i-1,1)!="\n")) { i++; continue }
    j=i+L; depth=1; ins=0; esc=0; spec=""
    while (j<=n && depth>0) {
      c=substr(buf,j,1)
      if (ins) { if (esc) esc=0; else if (c=="\\") esc=1; else if (c=="\"") ins=0 }
      else { if (c=="\"") ins=1; else if (c=="(") depth++; else if (c==")") depth-- }
      if (depth>0) spec=spec c; j++
    }
    out=""; rest=spec
    for (k=0;k<10;k++) {
      v=""; if (match(rest,/"[^"]*"/)) { v=substr(rest,RSTART+1,RLENGTH-2); rest=substr(rest,RSTART+RLENGTH) }
      out=out (k?"\t":"") v
    }
    print out; i=j
  }
}'

mapfile -t ROWS < <(awk "$MACRO_AWK" "$MANIFEST")
gate_require_scanned "${#ROWS[@]}" 1 check_hotswap_service_islands \
    "no HOTSWAP_SERVICE rows parsed from $MANIFEST"

violations=""
scanned=0
declare -A seen_ids=() seen_sources=()
for row in "${ROWS[@]}"; do
    IFS=$'\t' read -r id source headers contract_headers imports abi schema wire kat probe <<<"$row"
    scanned=$((scanned + 1))
    if [ -z "$id" ] || [ -n "${seen_ids[$id]:-}" ]; then
        violations+="  $id (empty or duplicate service id)"$'\n'
    fi
    seen_ids[$id]=1
    if [ -n "${seen_sources[$source]:-}" ]; then
        violations+="  $source (translation unit belongs to two services)"$'\n'
    fi
    seen_sources[$source]=1
    if [[ ! "$source" =~ ^app/services/src/.+\.c$ ]] &&
       { [ "$FIXTURE_MODE" != 1 ] || [[ ! "$source" =~ ^lib/test/fixtures/.+\.c$ ]]; }; then
        violations+="  $source (service TU must live under app/services/src)"$'\n'
        continue
    fi
    if [ ! -f "$source" ]; then
        violations+="  $source (missing source)"$'\n'; continue
    fi
    for h in $headers; do
        [ "$h" = "-" ] && continue
        if [ ! -f "$h" ]; then violations+="  $source -> $h (missing private/public header)"$'\n'; fi
    done
    for h in $contract_headers; do
        [ "$h" = "-" ] && continue
        if [ ! -f "$h" ]; then violations+="  $source -> $h (missing service contract header)"$'\n'; fi
    done
    for stamp in "$abi" "$schema" "$wire" "$kat"; do
        if [ -z "$stamp" ]; then violations+="  $source (empty frozen contract fingerprint)"$'\n'; fi
    done
    if [ -z "$probe" ]; then violations+="  $source (empty resident-owned probe leaf)"$'\n'; fi

    # Ownership/state constructs that would create a second mutable world.
    hits="$(awk '
      /hotswap-service-static-ok:/ { next }
      /_Thread_local|__thread/ { print FILENAME ":" FNR ": TLS: " $0; next }
      /__attribute__[[:space:]]*\(\([^)]*(constructor|destructor)/ { print FILENAME ":" FNR ": lifecycle: " $0; next }
      /^[[:space:]]*static[[:space:]]/ {
        if ($0 ~ /const/ || $0 ~ /\(/) next
        if ($0 ~ /=/ || $0 ~ /\[/ || $0 ~ /\{[[:space:]]*$/) print FILENAME ":" FNR ": mutable: " $0
      }
      /^[[:space:]]*extern[[:space:]]/ { print FILENAME ":" FNR ": extern: " $0 }
    ' "$source")"
    if [ -n "$hits" ]; then violations+="$hits"$'\n'; fi

    # Calls that imply effects/ambient authority. Match call tokens, not prose.
    forbidden='(^|[^A-Za-z0-9_])(sqlite3_[A-Za-z0-9_]*|fopen|freopen|open|openat|close|read|write|pread|pwrite|stat|lstat|fstat|opendir|readdir|unlink|rename|mkdir|socket|connect|bind|listen|accept|send|recv|clock_gettime|gettimeofday|time|rand|random|getrandom|fork|vfork|exec[A-Za-z0-9_]*|system|popen|posix_spawn)[[:space:]]*\('
    bad_calls="$(grep -nE "$forbidden" "$source" || true)"
    if [ -n "$bad_calls" ]; then violations+="$source:$bad_calls"$'\n'; fi
    bad_includes="$(grep -nE '^#[[:space:]]*include[[:space:]]*[<"]([^>"]*/)?(wallet|storage|consensus|validation|net|coins|chain|mining|rpc)/' "$source" || true)"
    if [ -n "$bad_includes" ]; then violations+="$source:$bad_includes"$'\n'; fi

    # Every project-prefixed call must be an explicitly declared stable host
    # import or a function defined inside this TU. C23 rejects undeclared calls;
    # rejecting `extern` above closes the manual-declaration escape hatch.
    mapfile -t project_calls < <(grep -oE '\b(vcs_|zcl_)[A-Za-z0-9_]*[[:space:]]*\(' "$source" | sed -E 's/[[:space:]]*\($//' | sort -u)
    for call in "${project_calls[@]}"; do
        case " $imports " in
            *" $call "*) ;;
            *) violations+="  $source -> $call (host symbol absent from stable-import list)"$'\n' ;;
        esac
    done
done

gate_require_scanned "$scanned" 1 check_hotswap_service_islands \
    "service scan population is empty"
if [ -n "${violations//[[:space:]]/}" ]; then
    printf '%s' "$violations"
    echo "FAIL: a pure hot-swap service island owns state, effects, or an undeclared import."
    exit 1
fi
echo "  OK: $scanned pure service island(s), frozen contracts and stable imports only"
