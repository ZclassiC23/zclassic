# Hosting the zclassic23 Block Explorer over HTTPS on your own domain

zclassic23 **is its own web server** — the explorer, REST API, and onion service are
all served by the single binary's built-in HTTPS server (`lib/net/src/https_server.c`).
**There is no nginx / Caddy / reverse proxy, and you should never add one.** One C
binary is the entire serving surface.

This guide covers: (A) the architecture, (B) the exact runbook to bring up
`https://yourdomain.example`, and (C) how anyone can host the explorer on their
own domain.

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
   <httpsdomain>, 8443, 8080)`. (During IBD it defers and auto-starts near tip.)

The node binds the high ports (8443/8080) **unprivileged**. Public **443/80** reach it
via a tiny **linger port-forward service** (below) — the node itself never binds 443.
The forward is a ~40-line project-owned forwarder that gets ONE file capability via
a single `sudo setcap`, after which an AI agent / operator runs it with no sudo, ever.

> Note: the TLS servername is optional. Pass `-httpsdomain=yourdomain.example` to
> set it explicitly; otherwise it is left unset (`boot_frontend_services.c`) and the
> HTTP→HTTPS redirect falls back to the request's own `Host` header. With a single
> cert the server presents that cert regardless of SNI, so any domain works.

---

## B. Runbook — bring up `https://yourdomain.example`

Prereqs: DNS `yourdomain.example → your.server.ip` ✅, a Let's Encrypt cert for
`yourdomain.example` exists and auto-renews (`certbot.timer`) ✅, the node runs the
explorer profile ✅. Two privileged commands, both auto-detect your user / datadir /
domain (override with `ZCL_EXPLORER_USER` / `ZCL_EXPLORER_DATADIR` /
`ZCL_EXPLORER_DOMAIN` if needed):

```bash
# 1. Install the cert where the node reads it (<datadir>/ssl) and wire a
#    certbot renewal deploy-hook so it stays fresh. Auto-detects the operator
#    (from sudo), the datadir, and the cert domain (the sole cert under
#    /etc/letsencrypt/live/). The ONLY privileged action for the cert.
sudo bash tools/scripts/zcl-explorer-cert.sh
#    Multiple certs? Name the one to use:
#    ZCL_EXPLORER_DOMAIN=yourdomain.example sudo -E bash tools/scripts/zcl-explorer-cert.sh

# 2. Forward public 443→8443 with the linger forwarder. 443-ONLY by design:
#    port 8080 belongs to the `zcl-supply` stats service — we leave it ALONE and do
#    NOT retire it. The clearnet explorer is HTTPS-only (no plain-HTTP redirect),
#    which is the safer posture anyway. This builds a tiny project-owned forwarder,
#    installs a user-linger service, and (its ONLY privileged action) sets ONE file
#    capability on it. Run this ONE command — after it, the agent manages the
#    service with no sudo, ever:
sudo bash tools/scripts/zcl-portfwd-setup.sh
```

That single command:
- builds `~/.local/bin/zcl-portfwd` (the forwarder),
- installs `~/.config/systemd/user/zcl-portfwd.service`,
- runs `setcap 'cap_net_bind_service=+ep' ~/.local/bin/zcl-portfwd` (the **one** privileged line),
- enables + starts the service under linger (survives reboot).

From then on the agent (or you) manage it with **no sudo, ever**:

```bash
systemctl --user start   zcl-portfwd     # or stop / restart
systemctl --user status  zcl-portfwd
bash tools/scripts/zcl-portfwd-setup.sh --status      # forwarder/cap/unit/service health
bash tools/scripts/zcl-portfwd-setup.sh --uninstall   # tear it all down (no sudo)
```

**Why a capped forwarder, not setcap on the node or system socat?** The capability
lives on the forwarder file, so it **survives `make deploy`** (which rebuilds the
node binary and would otherwise drop the cap). It's scoped to one ~40-line file —
not the whole node, not system-wide socat. TLS still terminates inside the node;
the forwarder is a dumb byte pipe.

```bash
# 3. Restart the node so boot starts the HTTPS server with the new cert.
#    (Do this AFTER the restart-durability fix is deployed — a cold-import node must
#    not be restarted casually; see docs/TENACITY.md.)
systemctl --user restart zclassic23

# 4. Verify (substitute your domain).
sleep 20
curl -sS -o /dev/null -w "HTTPS %{http_code}\n" https://yourdomain.example/explorer
curl -sS -o /dev/null -w "HTTP→ %{http_code} %{redirect_url}\n" http://yourdomain.example/
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
5. **443-only by default — leave 8080 alone.** The forwarder ships forwarding only
   `443→8443` (HTTPS). If you run other services on 8080 (e.g. `zcl-supply`), they
   are untouched. The clearnet explorer is HTTPS-only; to also redirect plain HTTP,
   first give the node a redirect port that doesn't collide, then add an `80:…` pair.
6. **Forward 443→8443 and 80→8080** — run the one setup command, then the agent
   owns it:
   ```bash
   # ONE privileged command (its only sudo line is a single setcap):
   sudo bash tools/scripts/zcl-portfwd-setup.sh
   # afterwards, no sudo ever — manage with:
   systemctl --user enable --now zcl-portfwd
   systemctl --user status zcl-portfwd
   ```
   The capability sits on `~/.local/bin/zcl-portfwd`, so it survives node
   redeploys; the node binary stays unprivileged. (If you'd rather, the classic
   alternatives still work — `setcap` on the node itself, `authbind`, or an
   iptables/nftables REDIRECT — but the capped forwarder is the recommended,
   redeploy-proof, no-runtime-root posture.)
7. **Restart the node**, then `curl https://yourdomain/explorer`.

Pages available: `/explorer` (dashboard), `/explorer/block/<hash>`, `/explorer/tx/<txid>`,
`/explorer/address/<addr>`, `/explorer/stats`, `/explorer/factoids`, `/explorer/hodl`
(HodlWave). `/wallet` and top-level `/api/*` are operator-private (not served on the
public/onion surface by design).

**No nginx. No reverse proxy. The node is the server.**
