# Hosting the zclassic23 Block Explorer over HTTPS on your own domain

zclassic23 **is its own web server** — the explorer, REST API, and onion service are
all served by the single binary's built-in HTTPS server (`lib/net/src/https_server.c`).
**There is no nginx / Caddy / reverse proxy, and you should never add one.** One C
binary is the entire serving surface.

This guide covers: (A) the architecture, (B) the exact runbook to bring up
`https://zclnet.net`, and (C) how anyone can host the explorer on their own domain.

---

## A. How it serves

| Surface | Where | Status |
|---|---|---|
| Onion (`.onion`) | embedded Tor → `onion_service.c` → explorer controllers | always on with `-tor` |
| Clearnet HTTPS | node's own server on **port 8443** | on **iff** a TLS cert is present |
| Clearnet HTTP | port **8080** (8443 − 363) | redirect → HTTPS |

At boot, `config/src/boot_frontend_services.c:boot_https_explorer_start()`:
1. Looks for `~/.zclassic-c23/ssl/fullchain.pem` + `~/.zclassic-c23/ssl/privkey.pem`
   (datadir-relative; replace `~/.zclassic-c23` with your `-datadir`).
2. If absent → logs `HTTPS: no cert … not on clearnet` and stays onion-only.
3. If present and the node is near tip → `https_server_start_on_port(cert, key,
   "zclnet.net", 8443, 8080)`. (During IBD it defers and auto-starts near tip.)

The node binds the high ports (8443/8080) **unprivileged**. Public **443/80** reach it
via a small root-only **port forward** (below) — the node itself never binds 443.

> Note: the TLS servername is currently hardcoded to `"zclnet.net"`
> (`boot_frontend_services.c:178`). With a single cert the server presents that cert
> regardless of SNI, so other domains still work — but making this a `-httpsdomain`
> flag is a tracked follow-up for clean multi-domain support.

---

## B. Runbook — bring up `https://zclnet.net`

Prereqs already satisfied on this host: DNS `zclnet.net → 74.50.74.102` ✅, a Let's
Encrypt cert for `zclnet.net` exists and auto-renews (`certbot.timer`) ✅, the node
runs the explorer profile ✅. Remaining steps need **root** (run these in a sudo shell):

```bash
# 1. Install the cert where the node reads it (datadir/ssl). Copy, don't move —
#    certbot owns the originals. Make them readable by the node's user (rhett).
sudo install -d -o rhett -g rhett /home/rhett/.zclassic-c23/ssl
sudo install -o rhett -g rhett -m 644 /etc/letsencrypt/live/zclnet.net/fullchain.pem /home/rhett/.zclassic-c23/ssl/fullchain.pem
sudo install -o rhett -g rhett -m 600 /etc/letsencrypt/live/zclnet.net/privkey.pem   /home/rhett/.zclassic-c23/ssl/privkey.pem

# 2. Keep it fresh across renewals: a certbot deploy-hook re-installs + nudges the node.
sudo tee /etc/letsencrypt/renewal-hooks/deploy/zclassic23-explorer.sh >/dev/null <<'HOOK'
#!/bin/bash
D=/home/rhett/.zclassic-c23/ssl
install -o rhett -g rhett -m 644 /etc/letsencrypt/live/zclnet.net/fullchain.pem "$D/fullchain.pem"
install -o rhett -g rhett -m 600 /etc/letsencrypt/live/zclnet.net/privkey.pem   "$D/privkey.pem"
# hot-reload without a full restart if supported, else the node picks it up next boot
sudo -u rhett XDG_RUNTIME_DIR=/run/user/$(id -u rhett) systemctl --user restart zclassic23 || true
HOOK
sudo chmod +x /etc/letsencrypt/renewal-hooks/deploy/zclassic23-explorer.sh

# 3. Forward public 443→8443 and 80→8080 (the node stays unprivileged on the high ports).
#    Persisted as a user-linger oneshot so it survives reboot; root via a scoped sudoers rule.
sudo tee /etc/sudoers.d/zcl-portfwd >/dev/null <<'SUDO'
rhett ALL=(root) NOPASSWD: /usr/sbin/iptables -t nat -A PREROUTING -p tcp --dport 443 -j REDIRECT --to-ports 8443
rhett ALL=(root) NOPASSWD: /usr/sbin/iptables -t nat -A PREROUTING -p tcp --dport 80 -j REDIRECT --to-ports 8080
rhett ALL=(root) NOPASSWD: /usr/sbin/iptables -t nat -C PREROUTING -p tcp --dport 443 -j REDIRECT --to-ports 8443
rhett ALL=(root) NOPASSWD: /usr/sbin/iptables -t nat -C PREROUTING -p tcp --dport 80 -j REDIRECT --to-ports 8080
SUDO
sudo chmod 440 /etc/sudoers.d/zcl-portfwd
```

Then, as the `rhett` user (no password needed thanks to the scoped sudoers rule),
install the linger forward unit:

```bash
cat > ~/.config/systemd/user/zcl-portfwd.service <<'UNIT'
[Unit]
Description=zclassic23 clearnet port forward 443->8443, 80->8080 (node owns high ports)
After=network-online.target
[Service]
Type=oneshot
RemainAfterExit=yes
# Add rules only if absent (-C check), idempotent across restarts.
ExecStart=/bin/sh -c 'sudo -n /usr/sbin/iptables -t nat -C PREROUTING -p tcp --dport 443 -j REDIRECT --to-ports 8443 2>/dev/null || sudo -n /usr/sbin/iptables -t nat -A PREROUTING -p tcp --dport 443 -j REDIRECT --to-ports 8443'
ExecStart=/bin/sh -c 'sudo -n /usr/sbin/iptables -t nat -C PREROUTING -p tcp --dport 80 -j REDIRECT --to-ports 8080 2>/dev/null || sudo -n /usr/sbin/iptables -t nat -A PREROUTING -p tcp --dport 80 -j REDIRECT --to-ports 8080'
[Install]
WantedBy=default.target
UNIT
systemctl --user daemon-reload
systemctl --user enable --now zcl-portfwd.service
```

```bash
# 4. Restart the node so boot starts the HTTPS server with the new cert.
#    (Do this AFTER the restart-durability fix is deployed — a cold-import node must
#    not be restarted casually; see docs/TENACITY.md.)
systemctl --user restart zclassic23

# 5. Verify.
sleep 20
curl -sS -o /dev/null -w "HTTPS %{http_code}\n" https://zclnet.net/explorer
curl -sS -o /dev/null -w "HTTP→ %{http_code} %{redirect_url}\n" http://zclnet.net/
```

Expected: `HTTPS 200` on `/explorer`, and `http://` redirecting to `https://`.

---

## C. Host the explorer on YOUR domain

Same shape — substitute your domain and datadir.

1. **DNS:** point an `A` record `yourdomain → your.server.ip`.
2. **Run the node** with the explorer profile (default) and `-tor` if you also want the
   onion. Note its `-datadir` (default `~/.zclassic-c23`) and HTTPS port (default 8443).
3. **Get a cert** (Let's Encrypt). Easiest is certbot standalone — but the node may be
   the only thing you want on 443, so issue while 443 is free, or use the DNS-01
   challenge:
   ```bash
   sudo certbot certonly --standalone -d yourdomain        # needs 80 free briefly
   # or, no open port needed:
   sudo certbot certonly --preferred-challenges dns -d yourdomain
   ```
4. **Install the cert** into `<datadir>/ssl/fullchain.pem` + `privkey.pem`, owned by the
   node's user (mode 644 / 600). Wire the certbot `renewal-hooks/deploy` script from §B.2
   with your domain + datadir so renewals stay in sync.
5. **Forward 443→8443 and 80→8080** with the linger unit + scoped sudoers rule from §B.3.
   (Alternatives if you prefer: `setcap 'cap_net_bind_service=+ep' <binary>` and run the
   node directly on 443/80, or `authbind`. The forward keeps the binary unprivileged,
   which is the recommended posture.)
6. **Restart the node**, then `curl https://yourdomain/explorer`.

Pages available: `/explorer` (dashboard), `/explorer/block/<hash>`, `/explorer/tx/<txid>`,
`/explorer/address/<addr>`, `/explorer/stats`, `/explorer/factoids`, `/explorer/hodl`
(HodlWave). `/wallet` and top-level `/api/*` are operator-private (not served on the
public/onion surface by design).

**No nginx. No reverse proxy. The node is the server.**
