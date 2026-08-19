#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# multihost_shim_ssh.sh — a local-kernel stand-in for ssh, for validating the
# multi-host acceptance's plumbing when no second host (or sshd) is available.
# It strips the ssh options and the host argument and execs the remote command
# locally, so `DHT_SSH=.../multihost_shim_ssh.sh` exercises every remote code
# path (spawn, kill, exec, file probes) end to end on one kernel.
#
# WHAT THIS IS NOT: a hardware-independence proof. Two shimmed "hosts" share a
# kernel, a filesystem namespace and a clock. It proves the routing and quoting
# plumbing only; the physical-independence claim still requires real hosts via
# `make commons-multihost-acceptance` with CJ_HOST_B/CJ_HOST_C.
set -euo pipefail

# Drop "-o <value>" option pairs; the first remaining argument is the host.
while [ $# -gt 0 ]; do
    case "$1" in
        -o) shift 2 ;;
        *) break ;;
    esac
done
[ $# -ge 1 ] || { echo "ssh-shim: no host argument" >&2; exit 255; }
shift                      # the host: discarded, everything runs locally
[ "${1:-}" = "--" ] && shift
[ $# -ge 1 ] || { echo "ssh-shim: no command given" >&2; exit 255; }
# ssh accepts both a single command string (shell-interpreted) and argv form.
if [ $# -eq 1 ]; then exec bash -c "$1"; else exec "$@"; fi
