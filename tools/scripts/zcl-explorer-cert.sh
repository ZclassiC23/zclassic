#!/bin/bash
# zcl-explorer-cert.sh — install the Let's Encrypt cert where the node's HTTPS
# server reads it, and keep it synced on renewal. Run ONCE as root:
#     sudo bash tools/scripts/zcl-explorer-cert.sh
# That is the only privileged action for the cert; everything else is unprivileged.
#
# Everything auto-detects for the invoking operator; override via env if needed:
#   ZCL_EXPLORER_USER    operator user        (default: ${SUDO_USER:-$(id -un)})
#   ZCL_EXPLORER_DATADIR node datadir         (default: <op-home>/.zclassic-c23)
#   ZCL_EXPLORER_DOMAIN  cert domain          (default: the sole /etc/letsencrypt/live/* dir)
set -euo pipefail

# ── Resolve the operator (the user who owns the node + reads the cert) ──
# When invoked via `sudo`, $SUDO_USER is the human; otherwise it's us.
OP="${ZCL_EXPLORER_USER:-${SUDO_USER:-$(id -un)}}"
OP_HOME="$(getent passwd "$OP" | cut -d: -f6)"
[ -n "$OP_HOME" ] || { echo "ERROR: cannot resolve home for user '$OP'" >&2; exit 1; }

DATADIR="${ZCL_EXPLORER_DATADIR:-$OP_HOME/.zclassic-c23}"
SSL="$DATADIR/ssl"

# ── Resolve the cert domain ──
# Prefer ZCL_EXPLORER_DOMAIN; else auto-detect when exactly ONE cert exists.
DOMAIN="${ZCL_EXPLORER_DOMAIN:-}"
if [ -z "$DOMAIN" ]; then
  shopt -s nullglob
  live=(/etc/letsencrypt/live/*/)
  shopt -u nullglob
  # /etc/letsencrypt/live/README is a file, not a cert dir — the */ glob skips it.
  if [ "${#live[@]}" -eq 1 ]; then
    DOMAIN="$(basename "${live[0]}")"
  elif [ "${#live[@]}" -eq 0 ]; then
    echo "ERROR: no Let's Encrypt cert found under /etc/letsencrypt/live/." >&2
    echo "       Issue one (e.g. sudo certbot certonly -d yourdomain) first." >&2
    exit 1
  else
    echo "ERROR: multiple certs under /etc/letsencrypt/live/ — pick one:" >&2
    for d in "${live[@]}"; do echo "         $(basename "$d")" >&2; done
    echo "       Re-run with: ZCL_EXPLORER_DOMAIN=yourdomain sudo -E bash $0" >&2
    exit 1
  fi
fi
LE="/etc/letsencrypt/live/$DOMAIN"

if [ "$(id -u)" -ne 0 ]; then
  echo "ERROR: run as root:  sudo bash $0" >&2; exit 1
fi
[ -r "$LE/privkey.pem" ] || { echo "ERROR: no cert at $LE (is the domain issued?)" >&2; exit 1; }

echo "operator=$OP  datadir=$DATADIR  domain=$DOMAIN"

# 1. Install the cert, owned by the node user (node reads it unprivileged).
install -d -o "$OP" -g "$OP" "$SSL"
install -o "$OP" -g "$OP" -m 644 "$LE/fullchain.pem" "$SSL/fullchain.pem"
install -o "$OP" -g "$OP" -m 600 "$LE/privkey.pem"   "$SSL/privkey.pem"

# 2. Re-sync automatically on every certbot renewal (else HTTPS breaks in ~90 days).
install -d /etc/letsencrypt/renewal-hooks/deploy
cat > /etc/letsencrypt/renewal-hooks/deploy/zclassic23-explorer.sh <<HOOK
#!/bin/bash
install -o $OP -g $OP -m 644 "$LE/fullchain.pem" "$SSL/fullchain.pem"
install -o $OP -g $OP -m 600 "$LE/privkey.pem"   "$SSL/privkey.pem"
HOOK
chmod +x /etc/letsencrypt/renewal-hooks/deploy/zclassic23-explorer.sh

echo "OK: cert installed to $SSL (owner $OP), auto-resync on renewal wired."
echo "Next: run the forwarder setup, then the node restart brings HTTPS up."
