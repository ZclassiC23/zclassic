/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Ban persistence round-trip (lib/net/src/net.c: ban_db_write()/
 * ban_db_read(), and ban_addr()/unban_addr()/clear_banned() auto-
 * persisting to <datadir>/banlist.dat whenever net_manager::datadir is
 * set — see connman_load_addrman()/connman_save_addrman() in connman.c
 * for how that field gets populated at boot).
 *
 * Before this, nm->banned[] was purely in-memory: any restart amnestied
 * every banned attacker. Coverage:
 *   1. ban -> reload in a FRESH net_manager -> still banned.
 *   2. expiry: an already-expired ban is neither persisted live nor
 *      resurrected on reload (lazy prune, both in is_banned() and at
 *      ban_db_write()/ban_db_read() time).
 *   3. unban_addr() persists too — a reload after unban does not
 *      resurrect the address.
 *   4. a missing banlist.dat is a clean cold-start miss (false, not an
 *      error) — matches addr_db_read()'s existing convention.
 *   5. a corrupt banlist.dat is quarantined and treated as "no
 *      persisted bans" rather than crashing boot (bans are advisory
 *      hardening, never fatal to boot).
 *
 * One TEST()/ASSERT() block per function — this codebase's TEST macro
 * uses a single fixed `_test_next:` goto label per function (see
 * test/test_helpers.h), so more than one TEST block in the same
 * function is a duplicate-label compile error.
 */

#include "test/test_helpers.h"
#include "net/net.h"

#include <stdio.h>
#include <string.h>

static struct net_addr nbp_addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    struct net_addr addr;
    net_addr_init(&addr);
    unsigned char ip4[4] = {a, b, c, d};
    net_addr_set_ipv4(&addr, ip4);
    return addr;
}

static int test_nbp_ban_reload_still_banned(void)
{
    int failures = 0;
    TEST("ban_db: ban -> reload in a fresh net_manager -> still banned") {
        char dir[256];
        test_make_tmpdir(dir, sizeof(dir), "net_ban_persistence", "main");

        struct net_manager nm;
        net_manager_init(&nm);
        nm.datadir = dir;

        struct net_addr addr = nbp_addr(203, 0, 113, 5);
        ban_addr(&nm, &addr, 3600, false);
        ASSERT(is_banned(&nm, &addr));

        struct net_manager nm2;
        net_manager_init(&nm2);
        bool loaded = ban_db_read(&nm2, dir);
        ASSERT(loaded);
        ASSERT(is_banned(&nm2, &addr));

        net_manager_free(&nm);
        net_manager_free(&nm2);
        PASS();
    } _test_next:;
    return failures;
}

static int test_nbp_expired_ban_not_resurrected(void)
{
    int failures = 0;
    TEST("ban_db: expired ban is not resurrected — live prune and reload skip") {
        char edir[256];
        test_make_tmpdir(edir, sizeof(edir), "net_ban_persistence", "expiry");
        struct net_manager nm;
        net_manager_init(&nm);
        nm.datadir = edir;

        struct net_addr live = nbp_addr(203, 0, 113, 6);
        struct net_addr expired = nbp_addr(203, 0, 113, 7);
        ban_addr(&nm, &live, 3600, false);
        /* since_epoch=true, ban_offset=1 -> ban_until=1 (1970-01-01),
         * already expired relative to any real wall-clock "now". */
        ban_addr(&nm, &expired, 1, true);

        /* is_banned() lazily prunes expired entries as it scans. */
        ASSERT(!is_banned(&nm, &expired));
        ASSERT(is_banned(&nm, &live));

        struct net_manager nm2;
        net_manager_init(&nm2);
        ASSERT(ban_db_read(&nm2, edir));
        ASSERT(!is_banned(&nm2, &expired));
        ASSERT(is_banned(&nm2, &live));

        net_manager_free(&nm);
        net_manager_free(&nm2);
        PASS();
    } _test_next:;
    return failures;
}

static int test_nbp_unban_persists(void)
{
    int failures = 0;
    TEST("ban_db: unban_addr persists — reload does not resurrect") {
        char udir[256];
        test_make_tmpdir(udir, sizeof(udir), "net_ban_persistence", "unban");
        struct net_manager nm;
        net_manager_init(&nm);
        nm.datadir = udir;

        struct net_addr addr = nbp_addr(203, 0, 113, 8);
        ban_addr(&nm, &addr, 3600, false);
        ASSERT(is_banned(&nm, &addr));
        ASSERT(unban_addr(&nm, &addr));
        ASSERT(!is_banned(&nm, &addr));

        struct net_manager nm2;
        net_manager_init(&nm2);
        /* Whether or not a (now-empty) banlist.dat exists on disk, the
         * address must not come back banned. */
        (void)ban_db_read(&nm2, udir);
        ASSERT(!is_banned(&nm2, &addr));

        net_manager_free(&nm);
        net_manager_free(&nm2);
        PASS();
    } _test_next:;
    return failures;
}

static int test_nbp_missing_file_clean_miss(void)
{
    int failures = 0;
    TEST("ban_db: missing file is a clean cold-start miss, not an error") {
        char empty_dir[256];
        test_make_tmpdir(empty_dir, sizeof(empty_dir), "net_ban_persistence", "empty");
        struct net_manager nm;
        net_manager_init(&nm);
        ASSERT(!ban_db_read(&nm, empty_dir));
        ASSERT(nm.num_banned == 0);
        net_manager_free(&nm);
        PASS();
    } _test_next:;
    return failures;
}

static int test_nbp_corrupt_file_quarantined(void)
{
    int failures = 0;
    TEST("ban_db: corrupt banlist.dat is quarantined, not fatal to boot") {
        char cdir[256];
        test_make_tmpdir(cdir, sizeof(cdir), "net_ban_persistence", "corrupt");
        char path[512];
        snprintf(path, sizeof(path), "%s/banlist.dat", cdir);
        FILE *f = fopen(path, "wb");
        ASSERT(f != NULL);
        const char *garbage = "not a valid banlist.dat body, just junk bytes";
        fwrite(garbage, 1, strlen(garbage), f);
        fclose(f);

        struct net_manager nm;
        net_manager_init(&nm);
        bool loaded = ban_db_read(&nm, cdir);
        ASSERT(!loaded);
        ASSERT(nm.num_banned == 0);
        net_manager_free(&nm);
        PASS();
    } _test_next:;
    return failures;
}

int test_net_ban_persistence(void);
int test_net_ban_persistence(void)
{
    int failures = 0;
    failures += test_nbp_ban_reload_still_banned();
    failures += test_nbp_expired_ban_not_resurrected();
    failures += test_nbp_unban_persists();
    failures += test_nbp_missing_file_clean_miss();
    failures += test_nbp_corrupt_file_quarantined();
    return failures;
}
