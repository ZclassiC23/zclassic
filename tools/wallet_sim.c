/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * wallet_sim — full wallet simulator with real data.
 *
 * Connects to zclassicd, syncs wallet, renders every page as
 * complete HTML files in sim/ directory. Open sim/index.html
 * in any browser to see exactly what the user sees.
 *
 * Usage:
 *   make sim
 *   # then open sim/index.html in browser
 *
 * Every page is a real render with real balance, real transactions,
 * real peer data — identical to what the GTK WebKit GUI shows. */

#include "controllers/wallet_view_controller.h"
#include "controllers/wallet_view_internal.h"
#include "chain/chainparams.h"
#include "keys/key.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>

volatile sig_atomic_t g_shutdown_requested = 0;

static uint8_t buf[262144]; /* 256KB per page */

static int render_to_file(const char *method, const char *path,
                           const char *body, const char *filename,
                           const char *label) {
    memset(buf, 0, sizeof(buf));
    size_t n = wallet_view_handle_request(
        method, path,
        body ? (const uint8_t *)body : NULL,
        body ? strlen(body) : 0,
        buf, sizeof(buf));

    if (n == 0) {
        printf("  %-35s (no response)\n", label);
        return 0;
    }

    /* Find body start (skip HTTP headers) */
    const char *html = strstr((char *)buf, "\r\n\r\n");
    size_t html_len;
    if (html) {
        html += 4;
        html_len = n - (size_t)(html - (char *)buf);
    } else {
        html = (char *)buf;
        html_len = n;
    }

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "sim/%s", filename);
    FILE *f = fopen(filepath, "w");
    if (!f) {
        printf("  %-35s (can't write %s)\n", label, filepath);
        return 0;
    }
    fwrite(html, 1, html_len, f);
    fclose(f);

    printf("  %-35s → sim/%s (%zu bytes)\n", label, filename, html_len);
    return 1;
}

static void write_index(int count) {
    FILE *f = fopen("sim/index.html", "w");
    if (!f) return;

    fprintf(f,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>\n"
        "<meta name='viewport' content='width=device-width'>\n"
        "<title>ZClassic23 Wallet Simulator</title>\n"
        "<style>\n"
        "body{font-family:Inter,system-ui,sans-serif;background:#0c0c0c;"
        "color:#e2e2e2;max-width:800px;margin:40px auto;padding:0 20px}\n"
        "h1{color:#34d399;font-size:28px}\n"
        "h2{color:#a78bfa;font-size:18px;margin-top:32px;border-bottom:"
        "1px solid #222;padding-bottom:8px}\n"
        ".page{display:block;padding:12px 16px;margin:4px 0;"
        "background:#111;border-radius:8px;color:#60a5fa;"
        "text-decoration:none;font-size:15px;transition:background .15s}\n"
        ".page:hover{background:#1a1a2a}\n"
        ".desc{color:#888;font-size:13px;margin-left:8px}\n"
        ".meta{color:#555;font-size:12px;margin-top:20px}\n"
        "</style></head><body>\n"
        "<h1>ZClassic23 Wallet Simulator</h1>\n"
        "<p style='color:#888'>%d pages rendered with live data from "
        "zclassicd. Click any page to see exactly what the user sees.</p>\n",
        count);

    fprintf(f, "<h2>Main Pages</h2>\n");
    fprintf(f, "<a class='page' href='dashboard.html'>Dashboard"
               "<span class='desc'>Balance, privacy meter, recent txs</span></a>\n");
    fprintf(f, "<a class='page' href='send.html'>Send"
               "<span class='desc'>Send form with spendable balance</span></a>\n");
    fprintf(f, "<a class='page' href='receive.html'>Receive"
               "<span class='desc'>QR code, z-address, t-address tabs</span></a>\n");
    fprintf(f, "<a class='page' href='history.html'>History"
               "<span class='desc'>Transaction list with filters</span></a>\n");
    fprintf(f, "<a class='page' href='node.html'>Node / Command Center"
               "<span class='desc'>Peers, block height, Tor status</span></a>\n");
    fprintf(f, "<a class='page' href='coins.html'>Coin Audit"
               "<span class='desc'>Every UTXO and shielded note</span></a>\n");
    fprintf(f, "<a class='page' href='shield.html'>Shield"
               "<span class='desc'>Shield amount form</span></a>\n");

    fprintf(f, "<h2>Shield Flow</h2>\n");
    fprintf(f, "<a class='page' href='shield-confirm.html'>Shield Confirm (0.5 ZCL)"
               "<span class='desc'>Confirmation page with fee breakdown</span></a>\n");

    fprintf(f, "<h2>Send Flow</h2>\n");
    fprintf(f, "<a class='page' href='send-review-bad.html'>Send Review (bad address)"
               "<span class='desc'>Validation error handling</span></a>\n");
    fprintf(f, "<a class='page' href='send-review-empty.html'>Send Review (empty)"
               "<span class='desc'>Empty form submission</span></a>\n");

    fprintf(f, "<h2>History Variants</h2>\n");
    fprintf(f, "<a class='page' href='history-p2.html'>History Page 2"
               "<span class='desc'>Pagination</span></a>\n");
    fprintf(f, "<a class='page' href='history-sent.html'>History (sent only)"
               "<span class='desc'>Filter: sent transactions</span></a>\n");
    fprintf(f, "<a class='page' href='history-recv.html'>History (received only)"
               "<span class='desc'>Filter: received transactions</span></a>\n");

    fprintf(f, "<h2>Error States</h2>\n");
    fprintf(f, "<a class='page' href='shield-post-zero.html'>Shield POST (zero amount)"
               "<span class='desc'>Invalid amount error</span></a>\n");
    fprintf(f, "<a class='page' href='send-confirm-empty.html'>Send Confirm (empty)"
               "<span class='desc'>Empty form error</span></a>\n");

    fprintf(f, "<h2>API</h2>\n");
    fprintf(f, "<a class='page' href='pulse.json'>Pulse API (JSON)"
               "<span class='desc'>Real-time balance/height/peers</span></a>\n");

    fprintf(f, "<div class='meta'>Generated by: make sim</div>\n");
    fprintf(f, "</body></html>\n");
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *datadir = NULL;
    if (argc > 1) {
        datadir = argv[1];
    } else {
        static char dd[512];
        const char *home = getenv("HOME");
        if (home) {
            snprintf(dd, sizeof(dd), "%s/.zclassic-c23", home);
            datadir = dd;
        }
    }

    chain_params_select(CHAIN_MAIN);
    ecc_start();
    ecc_verify_init();

    printf("wallet_sim: rendering all pages with live data\n");
    printf("datadir: %s\n", datadir ? datadir : "(none)");

    wallet_view_enable_sync();
    wallet_view_init(datadir);

    printf("Syncing wallet from zclassicd...\n");
    wv_sync_wallet_from_zclassicd();
    printf("Done.\n\n");

    mkdir("sim", 0755);

    int count = 0;
    printf("Rendering pages:\n");

    /* Main pages */
    count += render_to_file("GET", "/wallet", NULL,
        "dashboard.html", "Dashboard");
    count += render_to_file("GET", "/wallet/send", NULL,
        "send.html", "Send");
    count += render_to_file("GET", "/wallet/receive", NULL,
        "receive.html", "Receive");
    count += render_to_file("GET", "/wallet/history", NULL,
        "history.html", "History");
    count += render_to_file("GET", "/wallet/node", NULL,
        "node.html", "Node / Command Center");
    count += render_to_file("GET", "/wallet/coins", NULL,
        "coins.html", "Coin Audit");
    count += render_to_file("GET", "/wallet/shield", NULL,
        "shield.html", "Shield");

    /* Shield flow */
    count += render_to_file("GET", "/wallet/shield?amount=0.5", NULL,
        "shield-confirm.html", "Shield Confirm (0.5 ZCL)");

    /* Send flow */
    count += render_to_file("POST", "/wallet/send/review",
        "address=bad&amount=1.0",
        "send-review-bad.html", "Send Review (bad address)");
    count += render_to_file("POST", "/wallet/send/review", "",
        "send-review-empty.html", "Send Review (empty)");

    /* History variants */
    count += render_to_file("GET", "/wallet/history?page=1", NULL,
        "history-p2.html", "History Page 2");
    count += render_to_file("GET", "/wallet/history?filter=sent", NULL,
        "history-sent.html", "History (sent)");
    count += render_to_file("GET", "/wallet/history?filter=recv", NULL,
        "history-recv.html", "History (received)");

    /* Error states */
    count += render_to_file("POST", "/wallet/shield/confirm",
        "amount=0",
        "shield-post-zero.html", "Shield POST (zero)");
    count += render_to_file("POST", "/wallet/send/confirm", "",
        "send-confirm-empty.html", "Send Confirm (empty)");

    /* API */
    count += render_to_file("GET", "/api/wallet/pulse", NULL,
        "pulse.json", "Pulse API");

    /* Index page */
    write_index(count);

    printf("\n%d pages rendered → sim/\n", count);
    printf("Open sim/index.html in a browser to review.\n");

    ecc_verify_destroy();
    ecc_stop();
    return 0;
}
