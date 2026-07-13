#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Phase-0 release containment. Stable build/package/sign/publish does not exist
# as one exact-candidate transaction yet. This legacy path therefore provides
# strict read-only verification of an existing signed archive and refuses every
# artifact-building/tagging/publishing invocation before workspace mutation.
set -euo pipefail

die()
{
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

verify_archive()
{
    local archive="$1" sha3_file sig_file expected actual
    [[ -f "$archive" ]] || die "archive not found: $archive"
    sha3_file="${archive%.tar.gz}.sha3"
    sig_file="${sha3_file}.sig"
    [[ -f "$sha3_file" ]] || die "required SHA3 manifest missing: $sha3_file"
    [[ -f "$sig_file" ]] || die "required detached signature missing: $sig_file"
    command -v openssl >/dev/null 2>&1 || die "openssl is required"
    command -v gpg >/dev/null 2>&1 || die "gpg is required"
    expected="$(awk 'NR==1 && NF==2 { print $1 }' "$sha3_file")"
    [[ "$expected" =~ ^[0-9a-fA-F]{64}$ ]] ||
        die "malformed SHA3 manifest: $sha3_file"
    actual="$(openssl dgst -sha3-256 "$archive" | awk '{print $NF}')"
    [[ "${expected,,}" == "${actual,,}" ]] ||
        die "SHA3-256 mismatch: expected=$expected actual=$actual"
    gpg --verify "$sig_file" "$sha3_file" >/dev/null 2>&1 ||
        die "GPG signature verification failed"
    tar -tzf "$archive" >/dev/null 2>&1 || die "archive structure is invalid"
    printf '%s\n' \
        'legacy_local_artifact_verification=PASS' \
        'stable_release_verification=NOT_IMPLEMENTED' \
        'publishable=false'
}

if [[ "${1:-}" == "--verify" ]]; then
    [[ -n "${2:-}" && -z "${3:-}" ]] ||
        die "usage: $0 --verify <archive.tar.gz>"
    verify_archive "$2"
    exit 0
fi

printf '%s\n' \
    'REFUSING: stable release build/package/sign/publish is contained.' \
    'Required first: exact immutable quality evidence, complete canonical security posture,' \
    'clean parity/soak, two-builder byte identity, SBOM/provenance, signature quorum,' \
    'offline verification, upgrade, and rollback proof.' \
    'Unsigned output is local-development-only; use ordinary build targets for local binaries.' >&2
exit 2
