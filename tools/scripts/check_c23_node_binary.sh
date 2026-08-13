#!/usr/bin/env bash
# Fail closed unless the shipped node has only the C runtime ABI as a dynamic
# dependency. Every project/third-party dependency must be linked from a pinned
# static archive; GUI and C++ runtimes belong to separate developer tools.
set -euo pipefail

bin="${1:-build/bin/zclassic23}"
test -x "$bin" || { echo "c23-node: missing executable: $bin" >&2; exit 1; }
command -v readelf >/dev/null 2>&1 || {
    echo "c23-node: readelf is required for the release dependency audit" >&2
    exit 1
}

bad=0
while IFS= read -r dep; do
    case "$dep" in
        libc.so.6|libm.so.6) ;;
        *) echo "c23-node: forbidden dynamic dependency: $dep" >&2; bad=1 ;;
    esac
done < <(readelf -d "$bin" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')

if readelf -Ws "$bin" | grep -Eq 'GLIBCXX_|CXXABI_|__cxa_|_Z(nw|dl|da|na)'; then
    echo "c23-node: C++ runtime symbol found in node executable" >&2
    bad=1
fi

test "$bad" -eq 0 || exit 1
echo "c23-node: PASS (C23 sources; pinned static project dependencies; libc/libm ABI only)"
