#!/bin/bash
# zcl-explorer-cert.sh — install the Let's Encrypt cert where the node's HTTPS
# server reads it, and keep it synced on renewal. Run ONCE as root:
#     sudo bash /home/rhett/github/zclassic23/tools/scripts/zcl-explorer-cert.sh
# That is the only privileged action for the cert; everything else is unprivileged.
set -euo pipefail

DOMAIN="${ZCL_EXPLORER_DOMAIN:-zclnet.net}"
OP="${ZCL_EXPLORER_USER:-rhett}"
DATADIR="${ZCL_EXPLORER_DATADIR:-/home/$OP/.zclassic-c23}"
LE="/etc/letsencrypt/live/$DOMAIN"
SSL="$DATADIR/ssl"

if [ "$(id -u)" -ne 0 ]; then
  echo "ERROR: run as root:  sudo bash $0" >&2; exit 1
fi
[ -r "$LE/privkey.pem" ] || { echo "ERROR: no cert at $LE (is the domain issued?)" >&2; exit 1; }

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
