#!/usr/bin/env bash
# Provision or inspect the encrypted systemd boot credential for the isolated
# dev wallet. Secret bytes move Secret Service -> systemd-creds stdin only.
set -euo pipefail

ACTION="${1:-status}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CRED_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}/zclassic23/credentials"
CRED_FILE="$CRED_ROOT/dev-wallet-passphrase.cred"
UNIT="zcl23-dev.service"
DROPIN="91-wallet-credential.conf"

fail() {
    echo "dev-wallet-credential: $*" >&2
    exit 1
}

secret_present() {
    secret-tool lookup application zclassic23 wallet-scope dev \
        credential wallet-passphrase >/dev/null 2>&1
}

credential_file_ready() {
    [ -f "$CRED_FILE" ] && [ "$(stat -c '%a' "$CRED_FILE" 2>/dev/null)" = 600 ]
}

unit_binding_ready() {
    systemctl --user cat "$UNIT" 2>/dev/null |
        awk '/^LoadCredentialEncrypted=wallet-passphrase:/ { found=1 }
             END { exit found ? 0 : 1 }'
}

emit_status() {
    local n=0
    secret_present && n=$((n + 1))
    credential_file_ready && n=$((n + 1))
    unit_binding_ready && n=$((n + 1))
    local bar="---" state="blocked"
    [ "$n" -ge 1 ] && bar="#--"
    [ "$n" -ge 2 ] && bar="##-"
    [ "$n" -eq 3 ] && { bar="###"; state="ready"; }
    printf 'dev-wallet-credential: [%s] %d/3 state=%s secret=redacted config=encrypted\n' \
        "$bar" "$n" "$state"
}

setup() {
    command -v secret-tool >/dev/null 2>&1 || fail "secret-tool is required"
    command -v systemd-creds >/dev/null 2>&1 || fail "systemd-creds is required"
    command -v openssl >/dev/null 2>&1 || fail "openssl is required"
    case "$CRED_ROOT" in
        "$REPO"|"$REPO"/*|/|/tmp) fail "credential root is unsafe" ;;
    esac
    case "$CRED_ROOT" in
        /*) ;;
        *) fail "credential root must be absolute" ;;
    esac

    local secret
    secret="$(secret-tool lookup application zclassic23 wallet-scope dev \
        credential wallet-passphrase 2>/dev/null || true)"
    if [ -z "$secret" ]; then
        secret="$(openssl rand -base64 48 | tr -d '\n')"
        printf '%s' "$secret" | secret-tool store \
            --label='Z23 dev wallet unlock' \
            application zclassic23 wallet-scope dev \
            credential wallet-passphrase >/dev/null
    fi

    install -d -m 700 "$CRED_ROOT"
    [ "$(stat -c '%u:%a' "$CRED_ROOT" 2>/dev/null)" = "$(id -u):700" ] ||
        fail "credential root must be private and owned by the current user"
    local candidate="$CRED_ROOT/.dev-wallet-passphrase.cred.tmp"
    rm -f -- "$candidate"
    printf '%s' "$secret" | systemd-creds --user \
        --name=wallet-passphrase --newline=no encrypt - "$candidate" \
        >/dev/null 2>&1
    unset secret
    chmod 600 "$candidate"
    mv -f -- "$candidate" "$CRED_FILE"

    printf '%s\n' '[Service]' \
        "LoadCredentialEncrypted=wallet-passphrase:$CRED_FILE" |
        systemctl --user edit --drop-in="$DROPIN" --stdin "$UNIT" \
            >/dev/null 2>&1
    systemctl --user daemon-reload
    emit_status
}

case "$ACTION" in
    setup) setup ;;
    status) emit_status ;;
    *) fail "usage: $0 {setup|status}" ;;
esac
