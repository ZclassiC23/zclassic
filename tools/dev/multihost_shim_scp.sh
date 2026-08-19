#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# multihost_shim_scp.sh — the cp stand-in for scp, paired with
# multihost_shim_ssh.sh. Strips the scp options and the remote-host prefix
# from the destination and copies locally. Same caveat: this validates
# plumbing, never hardware independence.
set -euo pipefail

while [ $# -gt 0 ]; do
    case "$1" in
        -o) shift 2 ;;
        *) break ;;
    esac
done
[ $# -eq 2 ] || { echo "scp-shim: expected src and host:dst, got $# args" >&2; exit 255; }
src="$1"; dst="$2"
exec cp "$src" "${dst#*:}"
