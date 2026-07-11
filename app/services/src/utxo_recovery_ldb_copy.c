/* Point-in-time copy of a live zclassicd chainstate LevelDB.
 *
 * A plain cp of a LevelDB under active write tears — MANIFEST, SSTs
 * and the .log are copied at different instants, and leveldb_open on
 * the copy "succeeds" with holes (2026-06-09 trackb: the imported
 * UTXO set silently lacked a coin created 77 blocks below the anchor;
 * its first spend wedged the reducer with prevout_unresolved at
 * h=3142118). The copy here is provably point-in-time: signature the
 * source dir before and after the copy and retry until nothing
 * changed mid-copy. zclassicd flushes its chainstate at block cadence
 * (~minutes), so a clean window is the common case.
 */

#include "utxo_recovery_internal.h"
#include "util/log_macros.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* FNV-1a signature over (name, size, mtime_ns) of every entry in a
 * LevelDB directory. Two equal signatures taken around a copy prove no
 * file changed while the copy ran — i.e. the copy is a point-in-time
 * image. Returns 0 only on opendir failure. */
static uint64_t chainstate_dir_signature(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return 0;
    uint64_t sig = 14695981039346656037ULL;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char fp[1400];
        int n = snprintf(fp, sizeof(fp), "%s/%s", path, e->d_name);
        if (n <= 0 || (size_t)n >= sizeof(fp))
            continue;
        struct stat st;
        if (stat(fp, &st) != 0)
            continue;
        const char *c = e->d_name;
        while (*c) { sig ^= (uint8_t)*c++; sig *= 1099511628211ULL; }
        uint64_t fields[3] = { (uint64_t)st.st_size,
                               (uint64_t)st.st_mtim.tv_sec,
                               (uint64_t)st.st_mtim.tv_nsec };
        const uint8_t *b = (const uint8_t *)fields;
        for (size_t i = 0; i < sizeof(fields); i++) {
            sig ^= b[i];
            sig *= 1099511628211ULL;
        }
    }
    closedir(d);
    return sig;
}

struct zcl_result utxo_recovery_copy_chainstate_stable(const char *cs_path,
                                                       const char *import_path)
{
    if (!cs_path || !import_path)
        return ZCL_ERR(-3, "copy_chainstate_stable: NULL path");

    char cmd[2300];
    int n = snprintf(cmd, sizeof(cmd), "rm -rf '%s' && cp -a '%s' '%s'",
                     import_path, cs_path, import_path);
    if (n <= 0 || (size_t)n >= sizeof(cmd))
        return ZCL_ERR(-3, "copy_chainstate_stable: path too long");

    const int max_copy_attempts = 6;
    for (int attempt = 1; attempt <= max_copy_attempts; attempt++) {
        uint64_t sig_before = chainstate_dir_signature(cs_path);
        /* IMPORTANT: system()'s return value is NOT trustworthy here.  The node
         * installs SIGCHLD with SA_NOCLDWAIT (lib/util/src/alerts.c), so the
         * cp child is auto-reaped and system()'s internal waitpid() fails with
         * ECHILD, returning -1 even when `cp` succeeded byte-for-byte.  Ignore
         * the code and prove the copy structurally instead: (1) the source did
         * not change during the copy (point-in-time), and (2) the destination
         * is a COMPLETE image of it.  chainstate_dir_signature folds
         * (name,size,mtime_ns) over every entry and `cp -a` preserves all three
         * on the same filesystem, so a complete copy has dest_sig == source
         * sig; a torn/failed cp (vanished SST, disk-full) does not. */
        int rc = system(cmd);
        (void)rc;
        uint64_t sig_after = chainstate_dir_signature(cs_path);
        uint64_t dest_sig  = chainstate_dir_signature(import_path);
        if (sig_before != 0 && sig_before == sig_after && dest_sig == sig_after)
            return ZCL_OK;   /* point-in-time source + complete destination */

        const char *why =
            (sig_before != sig_after) ? "source changed during copy"
            : (dest_sig != sig_after) ? "destination incomplete (cp raced/failed)"
                                      : "source unreadable";
        printf("chainstate copy not yet point-in-time (attempt %d/%d: %s) — "
               "retrying...\n", attempt, max_copy_attempts, why);
        fflush(stdout);
        if (attempt < max_copy_attempts)
            sleep(2);
    }
    return ZCL_ERR(-3,
        "copy_chainstate_stable: could not capture a complete point-in-time "
        "copy of %s across %d attempts — refusing a torn import (retry when "
        "the source is idle)", cs_path, max_copy_attempts);
}
