#!/bin/bash
# One-time deployment setup for ZClassic23.
# Run once with: sudo bash deploy/setup.sh
#
# After this, 'make deploy' works without sudo ever again.
set -e

TARGET_USER="${SUDO_USER:-$(whoami)}"
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SERVICE_DIR="$(eval echo ~$TARGET_USER)/.config/systemd/user"

echo "Setting up ZClassic23 for user: $TARGET_USER"

# Enable linger (service survives logout) — may already be enabled
loginctl enable-linger "$TARGET_USER" 2>/dev/null || true

# Install port-forwarding service (root, runs iptables)
# Redirects 80→8080 and 443→8443 so zclassic23 needs no setcap
install -m 644 "$REPO_DIR/deploy/zcl-portfwd.service" /etc/systemd/system/zcl-portfwd.service
systemctl daemon-reload
systemctl enable --now zcl-portfwd

# Install user service file
mkdir -p "$SERVICE_DIR"
install -m 644 "$REPO_DIR/deploy/zclassic23.service" "$SERVICE_DIR/zclassic23.service"
su - "$TARGET_USER" -c "systemctl --user daemon-reload && systemctl --user enable zclassic23" 2>/dev/null || \
    echo "Note: run 'systemctl --user daemon-reload && systemctl --user enable zclassic23' as $TARGET_USER"

# Clean up old setcap sudoers rule if present
rm -f /etc/sudoers.d/zclassic23-setcap

echo "Done. Port forwarding active (80→8080, 443→8443)."
echo "'make deploy' will now work without sudo."
