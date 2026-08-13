#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# Prepare the pinned old-glibc boundary used by the portable C23 release.
# This is a sysroot, not a container and not an additional compiler.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TOOLCHAIN_ROOT="$REPO_ROOT/build/toolchains/debian-bullseye-glibc-2.31-amd64"
DOWNLOADS="$TOOLCHAIN_ROOT/downloads"
SYSROOT="$TOOLCHAIN_ROOT/root"
WRAPPER="$TOOLCHAIN_ROOT/cc"
STAMP="$TOOLCHAIN_ROOT/inputs.sha256"
PORTABLE_CC="${ZCL_PORTABLE_CC:-cc}"
SYSROOT_LAYOUT_REV="absolute-glibc-links-v2"

packages=(
    "libc6_2.31-13+deb11u14_amd64.deb|718e3ed8c92207caf5b541e7332affad4f62501cbab3f0cffbe9e6db0b89eae9|https://snapshot.debian.org/archive/debian-security/20260608T043349Z/pool/updates/main/g/glibc/libc6_2.31-13%2Bdeb11u14_amd64.deb"
    "libc6-dev_2.31-13+deb11u14_amd64.deb|7d4002192e09c3cc97f31dcc85ecd7741adbec9011abbdd14fb1790cab20bffa|https://snapshot.debian.org/archive/debian-security/20260608T043349Z/pool/updates/main/g/glibc/libc6-dev_2.31-13%2Bdeb11u14_amd64.deb"
    "linux-libc-dev_6.1.176-1_amd64.deb|8bb258735b9dffbb111da778ebdd024750878e435ffd9dfcadcb6762ede6b4cf|https://deb.debian.org/debian/pool/main/l/linux/linux-libc-dev_6.1.176-1_amd64.deb"
)

die() { echo "c23-portable-sysroot: $*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "required tool not found: $1"; }

inputs_digest() {
    { printf '%s\n' "$SYSROOT_LAYOUT_REV"; printf '%s\n' "${packages[@]}"; } |
        sha256sum | awk '{print $1}'
}

fetch_package() {
    local name="$1" sha="$2" url="$3" out="$DOWNLOADS/$name"
    if [ -f "$out" ] && printf '%s  %s\n' "$sha" "$out" | sha256sum -c - >/dev/null 2>&1; then
        return
    fi
    [ ! -e "$out" ] || unlink "$out"
    echo "c23-portable-sysroot: fetch $url" >&2
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 -o "$out.part" "$url"
    else
        need wget
        wget -q -O "$out.part" "$url"
    fi
    printf '%s  %s\n' "$sha" "$out.part" | sha256sum -c - >/dev/null ||
        die "checksum mismatch for $name"
    mv -f "$out.part" "$out"
}

write_wrapper() {
    local compiler tmp
    compiler="$(command -v "$PORTABLE_CC" 2>/dev/null || true)"
    [ -n "$compiler" ] || die "C23 compiler not found: $PORTABLE_CC"
    tmp="$(mktemp "$TOOLCHAIN_ROOT/.cc.XXXXXX")"
    {
        printf '%s\n' '#!/bin/sh' 'set -eu'
        # Put the CPU floor after caller flags so the portable release cannot
        # inherit Makefile's faster x86-64-v3 developer default or a caller's
        # -march=native.  x86-64 here means the original AMD64/SSE2 baseline;
        # optimized paths remain runtime-dispatched by the program.
        printf 'exec %q --sysroot=%q -B%q -L%q -L%q -isystem %q "$@" -march=x86-64 -mtune=generic\n' \
            "$compiler" "$SYSROOT" \
            "$SYSROOT/usr/lib/x86_64-linux-gnu/" \
            "$SYSROOT/usr/lib/x86_64-linux-gnu" \
            "$SYSROOT/lib/x86_64-linux-gnu" \
            "$SYSROOT/usr/include/x86_64-linux-gnu"
    } >"$tmp"
    chmod 0755 "$tmp"
    mv -f "$tmp" "$WRAPPER"
}

prepare() {
    local expected current entry name sha url stage loader link target
    need ar; need tar; need xz; need sha256sum; need mktemp; need objdump
    [ "$(uname -m)" = x86_64 ] || die "the pinned release sysroot currently supports x86_64 only"
    mkdir -p "$DOWNLOADS"
    expected="$(inputs_digest)"
    current="$(cat "$STAMP" 2>/dev/null || true)"
    if [ "$current" != "$expected" ] ||
            [ ! -f "$SYSROOT/usr/include/stdio.h" ] ||
            [ ! -f "$SYSROOT/usr/lib/x86_64-linux-gnu/crt1.o" ]; then
        for entry in "${packages[@]}"; do
            IFS='|' read -r name sha url <<<"$entry"
            fetch_package "$name" "$sha" "$url"
        done
        stage="$(mktemp -d "$TOOLCHAIN_ROOT/.root.XXXXXX")"
        trap 'rm -rf "$stage"' EXIT INT TERM
        mkdir -p "$stage/root"
        for entry in "${packages[@]}"; do
            IFS='|' read -r name sha url <<<"$entry"
            ar p "$DOWNLOADS/$name" data.tar.xz | tar -xJ -C "$stage/root"
        done
        # Debian's absolute loader symlink is correct after installation but
        # escapes a standalone sysroot during the link. Keep it relative here.
        loader="$stage/root/lib64/ld-linux-x86-64.so.2"
        [ -L "$loader" ] || die "Debian sysroot is missing its ELF loader"
        unlink "$loader"
        ln -s ../lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 "$loader"
        # Pre-glibc-2.34 development packages also expose linker names as
        # absolute symlinks into /lib. Those are correct after dpkg installs
        # into /, but would escape this extracted sysroot and select host
        # libraries (or fall back to stale static archives) during configure.
        for link in "$stage/root/usr/lib/x86_64-linux-gnu/"*.so; do
            [ -L "$link" ] || continue
            target="$(readlink "$link")"
            case "$target" in
                /lib/x86_64-linux-gnu/*)
                    unlink "$link"
                    ln -s "../../../${target#/}" "$link"
                    ;;
            esac
        done
        case "$SYSROOT" in "$TOOLCHAIN_ROOT/root") rm -rf "$SYSROOT" ;; *) die "unsafe sysroot path" ;; esac
        mv "$stage/root" "$SYSROOT"
        rmdir "$stage"
        trap - EXIT INT TERM
        printf '%s\n' "$expected" >"$STAMP"
    fi
    write_wrapper
    printf '%s\n' "$WRAPPER"
}

verify() {
    local cc smoke max macros
    cc="$(prepare)"
    smoke="$(mktemp "$TOOLCHAIN_ROOT/.smoke.XXXXXX")"
    trap 'rm -f "$smoke" "$smoke.c"' EXIT INT TERM
    printf '%s\n' '#include <stdio.h>' 'int main(void) { return puts("portable-c23") < 0; }' >"$smoke.c"
    "$cc" -std=c23 "$smoke.c" -o "$smoke"
    "$smoke" >/dev/null
    macros="$("$cc" -dM -E -x c /dev/null)"
    if printf '%s\n' "$macros" | grep -E \
            '^#define (__AVX|__AVX2__|__BMI|__FMA__)' >/dev/null; then
        die "portable compiler wrapper leaked a post-x86-64-baseline ISA"
    fi
    max="$(objdump -T "$smoke" | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' | sort -V | tail -1)"
    [ "$(printf '%s\n%s\n' "$max" GLIBC_2.31 | sort -V | tail -1)" = GLIBC_2.31 ] ||
        die "compiler support objects raised the ABI above GLIBC_2.31: $max"
    echo "c23-portable-sysroot: PASS compiler=$PORTABLE_CC cpu=x86-64 max_abi=$max sysroot=$SYSROOT" >&2
    unlink "$smoke"
    unlink "$smoke.c"
    trap - EXIT INT TERM
}

case "${1:-prepare}" in
    prepare|cc-path) prepare ;;
    root-path) prepare >/dev/null; printf '%s\n' "$SYSROOT" ;;
    verify) verify ;;
    *) die "usage: $0 [prepare|cc-path|root-path|verify]" ;;
esac
