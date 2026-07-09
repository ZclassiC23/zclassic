#!/usr/bin/env bash
# Hermetic regression tests for stale/missing/mismatched vendor stamps.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tools/scripts/vendor_provenance_lib.sh
. "$SCRIPT_DIR/vendor_provenance_lib.sh"

die() {
    printf 'test_vendor_provenance: FAIL: %s\n' "$*" >&2
    exit 1
}

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
lib="$tmp/lib"
mkdir -p "$lib"

# Generic build tools must never receive compiler-only probes.  A historical
# bug called `perl -dumpmachine`, which starts Perl's interactive debugger and
# stalls unattended builds.  The fake tool fails if any argument other than
# --version reaches it, while the fake compiler requires -dumpmachine.
tool_log="$tmp/tool.log"
fake_tool="$tmp/fake-tool"
cat >"$fake_tool" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$VP_TEST_TOOL_LOG"
if [[ "$1" == "--version" ]]; then
    printf '\n'
    printf 'fake-tool 1.0\n'
    exit 0
fi
exit 97
EOF
chmod +x "$fake_tool"
tool_identity="$(VP_TEST_TOOL_LOG="$tool_log" \
    vp_tool_identity_sha "$fake_tool")" || die "generic tool identity failed"
[[ "$(cat "$tool_log")" == "--version" ]] ||
    die "generic tool received compiler-only identity probe"
[[ "$tool_identity" == "$(vp_sha256_text "command=$fake_tool
version=fake-tool 1.0")" ]] ||
    die "generic tool identity did not capture the first non-empty version line"

: >"$tool_log"
fake_compiler="$tmp/fake-compiler"
cat >"$fake_compiler" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$VP_TEST_TOOL_LOG"
case "$1" in
    --version) printf 'fake-cc 1.0\n' ;;
    -dumpmachine) printf 'fake-target-linux-gnu\n' ;;
    *) exit 97 ;;
esac
EOF
chmod +x "$fake_compiler"
VP_TEST_TOOL_LOG="$tool_log" vp_compiler_identity_sha "$fake_compiler" \
    >/dev/null || die "compiler identity failed"
[[ "$(cat "$tool_log")" == $'--version\n-dumpmachine' ]] ||
    die "compiler identity did not bind version and target"

descriptor_a="schema=$VP_SCHEMA
archive=libfixture.a
component=fixture
version=1.0
source_url=https://example.invalid/fixture-1.0.tar.gz
source_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
recipe_revision=fixture-r1
recipe_flags_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
toolchain_sha256=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
dependencies_sha256=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"

printf 'archive-v1' >"$lib/libfixture.a"
vp_write_stamp "$lib" libfixture.a "$descriptor_a" ||
    die "could not write fixture stamp"
vp_verify_stamp "$lib" libfixture.a "$descriptor_a" ||
    die "fresh archive/stamp did not verify"

printf 'tampered' >>"$lib/libfixture.a"
if vp_verify_stamp "$lib" libfixture.a "$descriptor_a"; then
    die "stale archive bytes passed their old stamp"
fi

printf 'archive-v1' >"$lib/libfixture.a"
vp_write_stamp "$lib" libfixture.a "$descriptor_a" ||
    die "could not restore fixture stamp"
descriptor_b="${descriptor_a/recipe_revision=fixture-r1/recipe_revision=fixture-r2}"
if vp_verify_stamp "$lib" libfixture.a "$descriptor_b"; then
    die "changed recipe revision passed an old stamp"
fi

rm -f "$(vp_stamp_path "$lib" libfixture.a)"
if vp_verify_stamp "$lib" libfixture.a "$descriptor_a"; then
    die "missing stamp passed"
fi

# Dependency invalidation: a parent descriptor binds the complete dependency
# stamp hash. Rebuilding the dependency changes that hash and stales the parent.
printf 'dep-v1' >"$lib/libdep.a"
vp_write_stamp "$lib" libdep.a "schema=$VP_SCHEMA
archive=libdep.a
component=dep
version=1" || die "could not stamp dependency"
dep_sha="$(vp_stamp_sha256 "$lib" libdep.a)"
parent_descriptor="schema=$VP_SCHEMA
archive=libparent.a
component=parent
version=1
dependencies_sha256=$dep_sha"
printf 'parent-v1' >"$lib/libparent.a"
vp_write_stamp "$lib" libparent.a "$parent_descriptor" ||
    die "could not stamp parent"
vp_verify_stamp "$lib" libparent.a "$parent_descriptor" ||
    die "fresh dependency-bound parent did not verify"
printf 'dep-v2' >"$lib/libdep.a"
vp_write_stamp "$lib" libdep.a "schema=$VP_SCHEMA
archive=libdep.a
component=dep
version=2" || die "could not restamp dependency"
new_dep_sha="$(vp_stamp_sha256 "$lib" libdep.a)"
new_parent_descriptor="${parent_descriptor/dependencies_sha256=$dep_sha/dependencies_sha256=$new_dep_sha}"
if vp_verify_stamp "$lib" libparent.a "$new_parent_descriptor"; then
    die "parent passed after its dependency provenance changed"
fi

# Committed archive integrity manifest: exact bytes pass; tampering fails.
printf 'committed-secp-fixture' >"$lib/libsecp256k1.a"
manifest="$tmp/libsecp256k1.manifest"
cat >"$manifest" <<EOF
schema=zclassic23.committed-vendor-manifest.v1
archive=libsecp256k1.a
archive_sha256=$(vp_sha256_file "$lib/libsecp256k1.a")
archive_size=$(wc -c <"$lib/libsecp256k1.a" | tr -d ' ')
source_status=legacy-import-source-unresolved
replacement_policy=source-proof-required
EOF
vp_verify_locked_manifest "$lib/libsecp256k1.a" "$manifest" \
    libsecp256k1.a || die "valid committed manifest did not verify"
printf 'x' >>"$lib/libsecp256k1.a"
if vp_verify_locked_manifest "$lib/libsecp256k1.a" "$manifest" \
        libsecp256k1.a; then
    die "tampered committed archive passed its manifest"
fi

printf 'test_vendor_provenance: OK\n'
