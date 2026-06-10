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
        if (system(cmd) != 0)
            return ZCL_ERR(-3,
                "copy_chainstate_stable: failed to copy chainstate "
                "from %s to %s", cs_path, import_path);
        uint64_t sig_after = chainstate_dir_signature(cs_path);
        if (sig_before != 0 && sig_before == sig_after)
            return ZCL_OK;
        printf("chainstate changed during copy (attempt %d/%d) — "
               "retrying for a point-in-time image...\n",
               attempt, max_copy_attempts);
        fflush(stdout);
        sleep(2);
    }
    return ZCL_ERR(-3,
        "copy_chainstate_stable: chainstate at %s kept changing across "
        "%d copy attempts — refusing a torn import (stop zclassicd or "
        "retry when it is idle)", cs_path, max_copy_attempts);
}
