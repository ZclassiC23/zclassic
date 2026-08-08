#!/usr/bin/env bash
# Smoke the /metaverse site pages on an isolated regtest node over the
# HTTPS listener (the same metaverse_site_handle_request the onion serves).
set -euo pipefail
cd "$(dirname "$0")/../.."
ISO_KIND="soak"
# shellcheck source=tools/scripts/isolated_node_env.sh
. tools/scripts/isolated_node_env.sh
iso_init
# The HTTPS listener only starts when <datadir>/ssl/{fullchain,privkey}.pem
# exists (boot_https_explorer_start) — mint a throwaway self-signed pair.
mkdir -p "$ISO_DD/ssl"
openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$ISO_DD/ssl/privkey.pem" -out "$ISO_DD/ssl/fullchain.pem" \
    -days 1 -subj "/CN=localhost" >/dev/null 2>&1
iso_spawn_node ""
iso_wait_rpc_ready 90 || { tail -20 "$ISO_DD/node.log"; exit 1; }
sleep 2
fail=0
for route in /metaverse /metaverse/property /metaverse/space /metaverse/commons; do
    body="$(curl -kfsS --connect-timeout 5 --max-time 15 \
        "https://127.0.0.1:$ISO_HTTPSPORT$route" 2>/dev/null)" || {
        echo "SMOKE FAIL: $route did not return 200"; fail=1; continue; }
    echo "SMOKE 200 $route ($(printf '%s' "$body" | wc -c) bytes)"
done
body="$(curl -kfsS --max-time 15 "https://127.0.0.1:$ISO_HTTPSPORT/metaverse")"
printf '%s' "$body" | grep -q "nobody owns the world they build in" \
    && echo "SMOKE marker: mission text OK" || { echo "SMOKE FAIL: mission"; fail=1; }
printf '%s' "$body" | grep -q "SIMULATION" \
    && echo "SMOKE marker: SIMULATION label OK" || { echo "SMOKE FAIL: sim"; fail=1; }
body="$(curl -kfsS --max-time 15 "https://127.0.0.1:$ISO_HTTPSPORT/metaverse/property")"
printf '%s' "$body" | grep -q "zcode_package" \
    && echo "SMOKE marker: property kinds OK" || { echo "SMOKE FAIL: kinds"; fail=1; }
body="$(curl -kfsS --max-time 15 "https://127.0.0.1:$ISO_HTTPSPORT/metaverse/commons")"
printf '%s' "$body" | grep -q "ZC23 Living Commons" \
    && echo "SMOKE marker: commons OK" || { echo "SMOKE FAIL: commons"; fail=1; }
exit "$fail"
