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
command -v objdump >/dev/null 2>&1 || {
    echo "c23-node: objdump is required for the release ABI audit" >&2
    exit 1
}

# Ubuntu 22.04's glibc 2.35 is older than this ceiling, while the project's
# established portable-release contract permits symbols through GLIBC_2.38.
# The ceiling is about symbols the executable REQUIRES, not the libc version
# installed on the build machine.  A developer may build on a newer distro,
# but publication must fail if that silently raises the deploy ABI.  Override
# only for an explicit cross-platform audit; the release recipe uses the
# fail-closed default.
max_glibc_allowed="${ZCL_C23_MAX_GLIBC:-GLIBC_2.38}"
case "$max_glibc_allowed" in
    GLIBC_[0-9]*.[0-9]*) ;;
    *) echo "c23-node: invalid ZCL_C23_MAX_GLIBC: $max_glibc_allowed" >&2; exit 1 ;;
esac

bad=0
while IFS= read -r dep; do
    case "$dep" in
        libc.so.6|libm.so.6) ;;
        *) echo "c23-node: forbidden dynamic dependency: $dep" >&2; bad=1 ;;
    esac
done < <(readelf -d "$bin" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')

# __cxa_finalize and __cxa_atexit are ordinary libc startup/teardown symbols
# emitted for C PIE executables too. Reject the C++ exception/guard/personality
# surface, operator new/delete manglings, and versioned C++ ABIs instead. Do
# not use grep -q here: under pipefail its early close SIGPIPEs readelf, making
# the answer depend on whether readelf filled the pipe buffer (large binaries
# used to false-pass while small binaries failed on __cxa_finalize).
if readelf -Ws "$bin" | grep -E \
        'GLIBCXX_|CXXABI_|__gxx_personality_v0|__cxa_(throw|rethrow|begin_catch|end_catch|allocate_exception|free_exception|pure_virtual|guard_)|_Z(nw|dl|da|na)' \
        >/dev/null; then
    echo "c23-node: C++ runtime symbol found in node executable" >&2
    bad=1
fi

max_glibc="$(objdump -T "$bin" 2>/dev/null |
    grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' | sort -V | tail -1 || true)"
if [[ -z "$max_glibc" ]]; then
    echo "c23-node: no required GLIBC symbol versions found (unparseable artifact)" >&2
    bad=1
elif [[ "$(printf '%s\n%s\n' "$max_glibc" "$max_glibc_allowed" |
        sort -V | tail -1)" != "$max_glibc_allowed" ]]; then
    echo "c23-node: required ABI $max_glibc exceeds supported ceiling $max_glibc_allowed" >&2
    echo "c23-node: build the release on the oldest supported toolchain/sysroot" >&2
    bad=1
fi

test "$bad" -eq 0 || exit 1
echo "c23-node: PASS (C23 sources; pinned static project dependencies; libc/libm ABI only; max $max_glibc)"
