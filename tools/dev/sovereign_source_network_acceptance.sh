#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
#
# Compose the established seven-daemon DHT/Noise acceptance with the complete
# source-publication hook. The hook assigns four of those independent daemons
# the publisher, Host A, Host B and fresh-consumer roles; the remaining three
# keep the sparse Kademlia topology nontrivial.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

export DHT_PACKAGEHOST=1
export DHT_AFTER_SPARSE_HOOK="$SCRIPT_DIR/sovereign_source_network_hook.sh"

exec bash "$SCRIPT_DIR/zcode_dht_acceptance.sh"
