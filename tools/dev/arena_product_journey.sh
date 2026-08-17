#!/usr/bin/env bash
# Copyright 2026 Rhett Creighton - Apache License 2.0
# purpose: Run the accepted zdogace change through the installed Commons.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# The ordinary installed-product owner creates the guarded scratch root,
# builds the portable binaries, changes cwd outside the checkout, and starts
# the existing authenticated DHT topology.  This wrapper contributes no node,
# transport, package-store, worker, signer, or cleanup authority.
export C23_BETA_LABEL=arena-product-journey
export C23_BETA_HOOK="$SCRIPT_DIR/arena_product_journey_hook.sh"
export C23_BETA_INSTALL_ARENA_RUNNER=1
exec bash "$SCRIPT_DIR/c23_commons_beta_acceptance.sh"
