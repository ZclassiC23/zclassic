/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Wallet wireframe renderer — generates static HTML for every wallet page.
 * Run: make wallet-wireframes && build/bin/wallet-wireframes
 * Output: wireframes/ directory with one HTML file per screen.
 *
 * This is the UX review tool: open each file in a browser to see
 * exactly what users see at every step of every workflow. */

#include "controllers/wallet_view_controller.h"
#include "util/template.h"
#include "views/wallet_templates_gen.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdatomic.h>

/* Stubs for symbols normally provided by main.c */
_Atomic int g_shutdown_requested = 0;

static uint8_t buf[131072];

static void render(const char *method, const char *path,
                   const char *body, const char *filename) {
    memset(buf, 0, sizeof(buf));
    size_t n = wallet_view_handle_request(
        method, path,
        body ? (const uint8_t *)body : NULL,
        body ? strlen(body) : 0,
        buf, sizeof(buf));

    if (n == 0) {
        fprintf(stderr, "  SKIP %s (0 bytes)\n", path);
        return;
    }

    /* Strip HTTP headers — find \r\n\r\n */
    const char *html = strstr((char *)buf, "\r\n\r\n");
    if (html) html += 4; else html = (char *)buf;

    char filepath[256];
    snprintf(filepath, sizeof(filepath), "wireframes/%s", filename);
    FILE *f = fopen(filepath, "w");
    if (!f) { fprintf(stderr, "Cannot write: %s\n", filepath); return; }
    fwrite(html, 1, strlen(html), f);
    fclose(f);
    fprintf(stderr, "  %s -> %s (%zu bytes)\n", path, filepath, n);
}

int main(void) {
    mkdir("wireframes", 0755);
    wallet_view_init(NULL);

    fprintf(stderr, "\n=== Wallet Wireframes ===\n\n");
    fprintf(stderr, "Rendering every wallet screen...\n\n");

    /* ── NAV PAGES (what users see from the tab bar) ── */
    fprintf(stderr, "── Main Pages ──\n");
    render("GET", "/wallet",         NULL, "01-home.html");
    render("GET", "/wallet/send",    NULL, "02-send.html");
    render("GET", "/wallet/receive", NULL, "03-receive.html");
    render("GET", "/wallet/history", NULL, "04-history.html");
    render("GET", "/wallet/node",    NULL, "05-node.html");

    /* ── SEND FLOW ── */
    fprintf(stderr, "\n── Send Flow ──\n");
    render("POST", "/wallet/send/review",
        "address=t1ExampleAddr123456789012345&amount=0.5",
        "06-send-review.html");
    render("POST", "/wallet/send/confirm",
        "address=t1ExampleAddr123456789012345&amount=0.5",
        "07-send-confirm.html");

    /* ── SHIELD FLOW ── */
    fprintf(stderr, "\n── Shield Flow ──\n");
    render("GET", "/wallet/shield",       NULL, "08-shield-form.html");
    render("GET", "/wallet/shield?all=1", NULL, "09-shield-all.html");
    render("POST", "/wallet/shield/confirm",
        "amount=0.5", "10-shield-confirm.html");

    /* ── SUB PAGES ── */
    fprintf(stderr, "\n── Sub Pages ──\n");
    render("GET", "/wallet/coins", NULL, "11-coins.html");
    render("GET", "/wallet/tx/0000000000000000000000000000000000000000000000000000000000000000",
        NULL, "12-tx-detail.html");
    render("GET", "/wallet/history?filter=sent", NULL, "13-history-sent.html");
    render("GET", "/wallet/history?filter=recv", NULL, "14-history-recv.html");

    fprintf(stderr, "\n=== Done: %d wireframes in wireframes/ ===\n", 14);
    fprintf(stderr, "Open in browser: firefox wireframes/01-home.html\n\n");

    /* Print the complete page map */
    printf("\n");
    printf("┌─────────────────────────────────────────────────────────┐\n");
    printf("│            ZClassic23 Wallet — Page Map                 │\n");
    printf("├─────────────────────────────────────────────────────────┤\n");
    printf("│                                                         │\n");
    printf("│  [Home] [Send] [Receive] [History] [Node]  <- nav tabs  │\n");
    printf("│                                                         │\n");
    printf("│  HOME (/wallet)                                         │\n");
    printf("│    Balance + Privacy Meter                              │\n");
    printf("│    [Send] [Receive] buttons                             │\n");
    printf("│    Privacy nudge -> /wallet/shield                      │\n");
    printf("│    Recent txs -> /wallet/history                        │\n");
    printf("│    Node strip -> /wallet/node                           │\n");
    printf("│                                                         │\n");
    printf("│  SEND (/wallet/send)                                    │\n");
    printf("│    Address + Amount form                                │\n");
    printf("│    -> Review (/wallet/send/review)                      │\n");
    printf("│       -> Confirm (/wallet/send/confirm)                 │\n");
    printf("│          -> Success or Error                            │\n");
    printf("│                                                         │\n");
    printf("│  RECEIVE (/wallet/receive)                              │\n");
    printf("│    [Private (recommended)] [Public] tabs                │\n");
    printf("│    QR code + address + copy button                      │\n");
    printf("│                                                         │\n");
    printf("│  HISTORY (/wallet/history)                              │\n");
    printf("│    [All] [Sent] [Received] filter tabs                  │\n");
    printf("│    Search + paginated tx cards                          │\n");
    printf("│    -> Tx Detail (/wallet/tx/:txid)                      │\n");
    printf("│       Breadcrumb: History > Transaction                 │\n");
    printf("│                                                         │\n");
    printf("│  NODE (/wallet/node)                                    │\n");
    printf("│    Command Center (peers, Tor, difficulty)              │\n");
    printf("│    -> Coin Audit (/wallet/coins)                        │\n");
    printf("│       Breadcrumb: Node > Coin Audit                    │\n");
    printf("│                                                         │\n");
    printf("│  SHIELD (/wallet/shield)                                │\n");
    printf("│    Breadcrumb: Home > Secure Funds                     │\n");
    printf("│    Amount form or ?all=1 auto-fill                      │\n");
    printf("│    -> Confirm (/wallet/shield/confirm)                  │\n");
    printf("│       -> Success (back to dashboard)                    │\n");
    printf("│                                                         │\n");
    printf("└─────────────────────────────────────────────────────────┘\n\n");

    /* Print user journeys */
    printf("USER JOURNEYS:\n\n");
    printf("  1. First Launch\n");
    printf("     Home (0 ZCL) -> Receive -> copy z-address -> funds arrive\n\n");
    printf("  2. Shield Public Funds\n");
    printf("     Home -> 'Secure All' -> Shield confirm -> Home (100%% private)\n\n");
    printf("  3. Send Private\n");
    printf("     Home -> Send -> enter zs1... addr -> Review -> Confirm -> Done\n\n");
    printf("  4. Send Public (with warning)\n");
    printf("     Home -> Send -> enter t1... addr -> amber warning -> Review -> Confirm\n\n");
    printf("  5. Check History\n");
    printf("     History tab -> filter/search -> tap card -> Tx Detail\n\n");
    printf("  6. Audit Coins\n");
    printf("     Node tab -> Coin Audit -> UTXO table + notes + tokens\n\n");
    printf("  7. Monitor Node\n");
    printf("     Node tab -> peers, Tor, difficulty, chain supply\n\n");

    return 0;
}
