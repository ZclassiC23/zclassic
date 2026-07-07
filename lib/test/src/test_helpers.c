/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Shared test helper functions. */

#include "test/test_helpers.h"
#include "services/chain_evidence_authority_service.h"
#include "validation/chain_linkage_check.h"
#include "jobs/tip_finalize_stage.h"
#include <signal.h>

/* Reset the process-global singletons that leak across groups in the
 * single-process monolith (test_zcl). The forked runner (test_parallel)
 * gets fresh globals per group, so it never sees this; the monolith shares
 * one address space, so a group that arms a global — e.g.
 * tip_finalize_stage_init() registers an active-chain authority bound to
 * its local main_state, which dangles once that group returns — pollutes
 * every later group that reads it. Call this at the TOP of any group whose
 * assertions consult a shared global. Idempotent and safe to call anytime;
 * it only clears to the clean baseline the forked runner starts from. */
void test_reset_shared_globals(void)
{
    /* active_chain_tip() consults this authority + its block_map; a leaked
     * pair points into a freed main_state -> dangling reads / SIGSEGV. */
    active_chain_register_authority(&(struct active_chain_authority){0});
    active_chain_register_block_map(NULL);
    /* the served-tip height backing that authority (a separate file-static;
     * resetting the authority struct alone leaves it stale -> NULL tip reads). */
    tip_finalize_stage_test_reset();
    /* chain-linkage HOLD / refuse-from cursor (survives until a witnessed
     * success otherwise). */
    chain_linkage_reset_for_testing();
    /* pending finalized-tip slot (health drain side-effect). */
    chain_evidence_pending_tip_test_reset();
    /* fatal-signal disposition: a prior group that installed the node crash
     * handlers leaves SIGABRT/SIGSEGV/SIGBUS/SIGFPE armed, which makes
     * postmortem_install() refuse (it requires SIG_DFL) and breaks the
     * fork-and-raise crash tests. Restore the baseline the forked runner has. */
    signal(SIGABRT, SIG_DFL);
    signal(SIGSEGV, SIG_DFL);
    signal(SIGBUS, SIG_DFL);
    signal(SIGFPE, SIG_DFL);
    /* SIGCHLD: a prior alerts_init() installs SA_NOCLDWAIT (the kernel
     * auto-reaps children), so waitpid() in fork-based tests returns ECHILD.
     * Restore default disposition with flags cleared (sigaction, not signal(),
     * because the SA_NOCLDWAIT *flag* must be cleared, not just the handler). */
    struct sigaction chld_dfl;
    memset(&chld_dfl, 0, sizeof(chld_dfl));
    chld_dfl.sa_handler = SIG_DFL;
    sigemptyset(&chld_dfl.sa_mask);
    sigaction(SIGCHLD, &chld_dfl, NULL);
}

int check_hex(const unsigned char *data, size_t len, const char *expected)
{
    char buf[256];
    for (size_t i = 0; i < len; i++)
        snprintf(buf + i * 2, 3, "%02x", data[i]);
    if (strcmp(buf, expected) != 0) {
        printf("FAIL\n  got:      %s\n  expected: %s\n", buf, expected);
        return 1;
    }
    printf("OK\n");
    return 0;
}

void test_hex_to_bytes_rev(const char *hex, uint8_t *out, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned int b;
        sscanf(hex + 2*i, "%02x", &b);
        out[len - 1 - i] = (uint8_t)b;
    }
}

void test_hex_to_bytes(const char *hex, uint8_t *out, int len)
{
    for (int i = 0; i < len; i++) {
        unsigned int b;
        sscanf(hex + 2*i, "%02x", &b);
        out[i] = (uint8_t)b;
    }
}

void test_rm_rf(const char *dir)
{
    if (!dir || !dir[0]) return;
    char cmd[4200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    (void)!system(cmd);
}

int test_rm_rf_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return unlink(path);

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0) {
            continue;
        }
        char child[768];
        int n = snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) continue;
        test_rm_rf_recursive(child);
    }
    closedir(d);
    return rmdir(path);
}

void test_make_tmpdir(char *buf, size_t n, const char *prefix,
                      const char *tag)
{
    test_fmt_tmpdir(buf, n, prefix, tag);
    test_cleanup_tmpdir(buf);
    mkdir("test-tmp", 0755);
    mkdir(buf, 0755);
}

void test_make_easy_consensus_params(struct consensus_params *p)
{
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < 32; i++) p->powLimit.data[i] = 0xff;
}

void test_projection_paths(const char *dir, const char *name,
                           char *elog, size_t elog_n,
                           char *proj, size_t proj_n)
{
    snprintf(elog, elog_n, "%s/event_log.dat", dir);
    snprintf(proj, proj_n, "%s/%s_projection.db", dir, name);
}
