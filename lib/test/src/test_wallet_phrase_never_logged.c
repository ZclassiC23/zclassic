/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * THE TWELVE WORDS MUST NEVER REACH A LOG FILE.
 *
 * A wallet created from here on is born with a twelve-word recovery
 * phrase, shown exactly once, at creation, on stdout. On the owner's
 * install the node runs as a systemd service and that service redirects
 * stdout to node.log. So the shipped configuration turned "show the user
 * their words once" into "write the wallet's entire spending authority
 * into a plaintext file" — a file that is rotated, copied into backups,
 * and read back on demand with `zclassic23 ops logs`. Anyone who ever
 * reads that file owns the money in that wallet, forever, and nothing in
 * the wallet can be changed to take it back.
 *
 * The rule is therefore: if stdout is not a terminal, the node does not
 * create the wallet at all. Not "creates it and skips the print" — that
 * would leave a wallet whose one and only backup the owner never saw, and
 * no command can ever print those words again. It refuses, before a phrase
 * is drawn or a key is minted, and says why.
 *
 * WHAT IS ASSERTED — the observable property, on disk and on the wire:
 *
 *   1. boot_wallet_phrase_stdout_is_a_terminal() tells the truth about a
 *      redirected fd and about a pty. Everything below rests on it.
 *   2. boot_wallet_create_new() with stdout redirected to a file returns
 *      FALSE, writes ZERO bytes to that file, and mints NOTHING — no keys
 *      in the keystore, no wallet_keys rows, no wallet_seed row. There is
 *      no half-made wallet to clean up.
 *   3. The refusal is loud: it names the problem on stderr.
 *   4. boot_wallet_show_recovery_phrase_once() — the one function that can
 *      print a phrase — prints nothing to a redirected stdout, even when
 *      called directly with a phrase in hand.
 *   5. ANTI-VACUOUS: the same capture harness, handed the same phrase
 *      through a plain printf, DOES find it. Without this the "no phrase
 *      in the capture" assertions would pass over a harness that could not
 *      see a leak if one happened.
 */

#define _GNU_SOURCE

#include "test/test_core.h"

#include "config/boot_wallet_phrase.h"
#include "models/database.h"
#include "models/wallet_key.h"
#include "wallet/wallet.h"
#include "wallet/wallet_sqlite.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WPL_CHECK(name, expr) do {                                     \
    printf("wallet_phrase_never_logged: %s... ", (name));              \
    if (expr) { printf("OK\n"); }                                      \
    else { printf("FAIL\n"); failures++; }                             \
} while (0)

/* A syntactically real BIP39 phrase. It is never installed anywhere; it
 * exists so the leak scanner has something specific to hunt for. */
#define WPL_PHRASE \
    "abandon abandon abandon abandon abandon abandon " \
    "abandon abandon abandon abandon abandon about"

/* ── stdout/stderr capture ─────────────────────────────────────────── */

struct wpl_capture {
    int  saved_out, saved_err;
    int  fd_out, fd_err;
    char path_out[1200], path_err[1200];
};

/* Redirect stdout and stderr into two files, exactly as the systemd unit
 * redirects the node's stdout into node.log. */
static bool wpl_capture_begin(struct wpl_capture *c, const char *dir,
                              const char *tag)
{
    memset(c, 0, sizeof(*c));
    c->saved_out = c->saved_err = c->fd_out = c->fd_err = -1;
    snprintf(c->path_out, sizeof(c->path_out), "%s/%s.out", dir, tag);
    snprintf(c->path_err, sizeof(c->path_err), "%s/%s.err", dir, tag);

    fflush(stdout);
    fflush(stderr);
    c->saved_out = dup(STDOUT_FILENO);
    c->saved_err = dup(STDERR_FILENO);
    c->fd_out = open(c->path_out, O_RDWR | O_CREAT | O_TRUNC, 0600);
    c->fd_err = open(c->path_err, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (c->saved_out < 0 || c->saved_err < 0 || c->fd_out < 0 ||
        c->fd_err < 0)
        return false;
    return dup2(c->fd_out, STDOUT_FILENO) >= 0 &&
           dup2(c->fd_err, STDERR_FILENO) >= 0;
}

static void wpl_capture_end(struct wpl_capture *c)
{
    fflush(stdout);
    fflush(stderr);
    if (c->saved_out >= 0) { dup2(c->saved_out, STDOUT_FILENO); close(c->saved_out); }
    if (c->saved_err >= 0) { dup2(c->saved_err, STDERR_FILENO); close(c->saved_err); }
    if (c->fd_out >= 0) close(c->fd_out);
    if (c->fd_err >= 0) close(c->fd_err);
    c->saved_out = c->saved_err = c->fd_out = c->fd_err = -1;
}

/* Whole file into `buf`, NUL-terminated. Returns bytes read, -1 on a file
 * that could not be read (never silently 0 — "I could not look" and "it
 * was empty" are the two answers this whole file exists to keep apart). */
static long wpl_slurp(const char *path, char *buf, size_t cap)
{
    if (cap)
        buf[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

/* ── the cases ─────────────────────────────────────────────────────── */

static int t_terminal_probe_tells_the_truth(const char *dir)
{
    int failures = 0;

    /* A redirected stdout is not a terminal. */
    struct wpl_capture cap;
    bool began = wpl_capture_begin(&cap, dir, "probe");
    bool redirected_says_no =
        began && !boot_wallet_phrase_stdout_is_a_terminal();
    wpl_capture_end(&cap);
    WPL_CHECK("capture harness installed", began);
    WPL_CHECK("a file on stdout is NOT reported as a terminal",
              redirected_says_no);

    /* A pty is. Opened and probed only — nothing is ever written to it, so
     * there is no way for this to block on a full pty buffer. */
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    bool pty_says_yes = false;
    bool pty_ready = false;
    if (master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0) {
        const char *slave_path = ptsname(master);
        int slave = slave_path ? open(slave_path, O_RDWR | O_NOCTTY) : -1;
        if (slave >= 0) {
            pty_ready = true;
            int saved = dup(STDOUT_FILENO);
            if (saved >= 0 && dup2(slave, STDOUT_FILENO) >= 0) {
                pty_says_yes = boot_wallet_phrase_stdout_is_a_terminal();
                dup2(saved, STDOUT_FILENO);
            }
            if (saved >= 0)
                close(saved);
            close(slave);
        }
    }
    if (master >= 0)
        close(master);
    /* Anti-vacuous: without a pty the "yes" half proves nothing, so say so
     * rather than passing on an untaken branch. */
    WPL_CHECK("a pty was available to probe against", pty_ready);
    WPL_CHECK("a terminal on stdout IS reported as a terminal",
              pty_says_yes);
    return failures;
}

static int t_capture_would_catch_a_leak(const char *dir)
{
    int failures = 0;
    struct wpl_capture cap;
    if (!wpl_capture_begin(&cap, dir, "vacuity")) {
        wpl_capture_end(&cap);
        WPL_CHECK("anti-vacuous: capture harness installed", false);
        return failures;
    }
    /* Exactly the shape of the defect: a phrase printed to a stdout that
     * is really a file. */
    printf("  %s\n", WPL_PHRASE);
    fflush(stdout);
    wpl_capture_end(&cap);

    char buf[8192];
    long n = wpl_slurp(cap.path_out, buf, sizeof(buf));
    WPL_CHECK("anti-vacuous: the capture file was readable", n > 0);
    WPL_CHECK("anti-vacuous: a phrase printed to a redirected stdout IS "
              "found by this test's scanner",
              n > 0 && strstr(buf, WPL_PHRASE) != NULL);
    return failures;
}

static int t_creation_refuses_and_leaves_nothing(const char *dir)
{
    int failures = 0;

    char db_path[1200];
    snprintf(db_path, sizeof(db_path), "%s/node.db", dir);

    struct node_db ndb;
    memset(&ndb, 0, sizeof(ndb));
    bool db_ok = node_db_open(&ndb, db_path) && ndb.open;
    WPL_CHECK("fixture node.db opened", db_ok);
    if (!db_ok)
        return failures;

    struct wallet_sqlite ws;
    struct zcl_result wr = wallet_sqlite_open_r(&ws, ndb.db);
    WPL_CHECK("fixture wallet tables opened", wr.ok);
    if (!wr.ok) {
        node_db_close(&ndb);
        return failures;
    }

    struct wallet *w = calloc(1, sizeof(*w));
    WPL_CHECK("fixture wallet allocated", w != NULL);
    if (!w) {
        wallet_sqlite_close(&ws);
        node_db_close(&ndb);
        return failures;
    }
    wallet_init(w);

    WPL_CHECK("fixture starts with an empty wallet",
              w->keystore.num_keys == 0 && db_wallet_key_count(&ndb) == 0);

    /* THE CALL, with stdout pointed at a file — a node.log by any other
     * name. */
    struct wpl_capture cap;
    bool began = wpl_capture_begin(&cap, dir, "create");
    bool created = true;
    if (began)
        created = boot_wallet_create_new(w, &ws, &ndb, true);
    wpl_capture_end(&cap);
    WPL_CHECK("capture harness installed for the creation call", began);

    WPL_CHECK("creating a wallet is REFUSED when stdout is not a terminal",
              began && !created);

    char out[16384], err[16384];
    long n_out = wpl_slurp(cap.path_out, out, sizeof(out));
    long n_err = wpl_slurp(cap.path_err, err, sizeof(err));

    if (n_out > 0)
        printf("    stdout capture (%ld bytes): %.400s\n", n_out, out);

    WPL_CHECK("the capture files were readable", n_out >= 0 && n_err >= 0);
    /* The strongest form: not "no phrase on stdout" but NOTHING on stdout.
     * A print that is never reached cannot leak. */
    WPL_CHECK("NOTHING at all was written to the redirected stdout",
              n_out == 0);
    WPL_CHECK("no recovery phrase reached the redirected stdout",
              n_out >= 0 && strstr(out, WPL_PHRASE) == NULL &&
              strstr(out, "WRITE THESE 12 WORDS DOWN") == NULL);

    /* Loud, and in plain English, on the channel a person still sees. */
    WPL_CHECK("the refusal says the wallet was not created",
              n_err > 0 && strstr(err, "WALLET NOT CREATED") != NULL);
    WPL_CHECK("the refusal says why — this output is not a terminal",
              n_err > 0 && strstr(err, "NOT going to a terminal") != NULL);
    WPL_CHECK("the refusal says what to do about it",
              n_err > 0 && strstr(err, "from a terminal") != NULL);
    WPL_CHECK("the refusal itself carries no phrase",
              n_err >= 0 && strstr(err, WPL_PHRASE) == NULL);

    /* NO HALF-MADE WALLET. This is the half that stops a user being left
     * with keys whose only backup they were never shown. */
    WPL_CHECK("no key was minted into the keystore",
              w->keystore.num_keys == 0);
    WPL_CHECK("no wallet_keys row was written", db_wallet_key_count(&ndb) == 0);
    {
        uint8_t seed[32];
        bool has_seed = wallet_sqlite_read_sapling_seed(&ws, seed);
        memset(seed, 0, sizeof(seed));
        WPL_CHECK("no wallet_seed row was written", !has_seed);
    }

    wallet_free(w);
    free(w);
    wallet_sqlite_close(&ws);
    node_db_close(&ndb);
    return failures;
}

static int t_direct_print_is_also_refused(const char *dir)
{
    int failures = 0;
    struct wpl_capture cap;
    bool began = wpl_capture_begin(&cap, dir, "direct");
    if (began)
        boot_wallet_show_recovery_phrase_once(WPL_PHRASE);
    wpl_capture_end(&cap);
    WPL_CHECK("capture harness installed for the direct print", began);

    char out[16384], err[16384];
    long n_out = wpl_slurp(cap.path_out, out, sizeof(out));
    long n_err = wpl_slurp(cap.path_err, err, sizeof(err));

    if (n_out > 0)
        printf("    stdout capture (%ld bytes): %.400s\n", n_out, out);

    /* Defence in depth: even called directly, with a phrase already in
     * hand, the one function that can print words prints none of them. */
    WPL_CHECK("show_recovery_phrase_once writes nothing to a redirected "
              "stdout", n_out == 0);
    WPL_CHECK("show_recovery_phrase_once leaks no phrase",
              n_out >= 0 && strstr(out, WPL_PHRASE) == NULL);
    WPL_CHECK("show_recovery_phrase_once explains itself on stderr",
              n_err > 0 && strstr(err, "WALLET NOT CREATED") != NULL);
    return failures;
}

int test_wallet_phrase_never_logged(void);
int test_wallet_phrase_never_logged(void)
{
    printf("\n=== the twelve words never reach a log file ===\n");
    int failures = 0;

    char dir[256];
    test_fmt_tmpdir(dir, sizeof(dir), "wallet_phrase_log", "main");
    mkdir("./test-tmp", 0700);
    test_rm_rf(dir);
    if (mkdir(dir, 0700) != 0) {
        printf("wallet_phrase_never_logged: could not make %s... FAIL\n", dir);
        return 1;
    }

    failures += t_terminal_probe_tells_the_truth(dir);
    failures += t_capture_would_catch_a_leak(dir);
    failures += t_creation_refuses_and_leaves_nothing(dir);
    failures += t_direct_print_is_also_refused(dir);

    test_rm_rf(dir);
    return failures;
}
