/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_consensus_bundle_marker.c — the durable "a sovereign consensus bundle is
 * installed in this datadir" marker. Written by boot_install_consensus_bundle
 * after a successful atomic activation; read by boot_snapshot_failure_memory to
 * refuse re-loading a borrowed starter-pack seed over the installed state.
 *
 * Contract in config/boot_consensus_bundle_marker.h. */

#include "config/boot_consensus_bundle_marker.h"

#include "util/log_macros.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool marker_path(char *out, size_t cap, const char *datadir)
{
    if (!out || cap == 0 || !datadir || !datadir[0])
        return false;
    int n = snprintf(out, cap, "%s/%s", datadir,
                     BOOT_CONSENSUS_BUNDLE_MARKER_NAME);
    return n > 0 && (size_t)n < cap;
}

bool boot_consensus_bundle_marker_write(const char *datadir, int32_t height,
                                        const uint8_t artifact_digest[32])
{
    if (!datadir || !datadir[0] || !artifact_digest)
        LOG_FAIL("install_consensus_bundle",
                 "marker write: missing datadir or artifact digest");

    char final_path[1200];
    char tmp_path[1216];
    if (!marker_path(final_path, sizeof(final_path), datadir))
        LOG_FAIL("install_consensus_bundle",
                 "marker write: datadir path too long (%s)", datadir);
    int tn = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);
    if (tn <= 0 || (size_t)tn >= sizeof(tmp_path))
        LOG_FAIL("install_consensus_bundle",
                 "marker write: temp path too long (%s)", final_path);

    char digest_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(digest_hex + i * 2, 3, "%02x", artifact_digest[i]);

    FILE *f = fopen(tmp_path, "we");
    if (!f)
        LOG_FAIL("install_consensus_bundle",
                 "marker write: fopen(%s) failed: %s", tmp_path,
                 strerror(errno));
    int rc = fprintf(f,
                     "zclassic23-consensus-bundle-installed v1\n"
                     "height=%d\n"
                     "artifact_digest=%s\n"
                     "installed_unix=%lld\n",
                     height, digest_hex, (long long)time(NULL));
    bool wrote = rc > 0 && fflush(f) == 0;
    if (wrote) {
        int fd = fileno(f);
        if (fd >= 0)
            (void)fsync(fd); /* best-effort durability before rename */
    }
    if (fclose(f) != 0)
        wrote = false;
    if (!wrote) {
        (void)remove(tmp_path);
        LOG_FAIL("install_consensus_bundle",
                 "marker write: writing %s failed: %s", tmp_path,
                 strerror(errno));
    }
    if (rename(tmp_path, final_path) != 0) {
        (void)remove(tmp_path);
        LOG_FAIL("install_consensus_bundle",
                 "marker write: rename(%s -> %s) failed: %s", tmp_path,
                 final_path, strerror(errno));
    }
    LOG_INFO("install_consensus_bundle",
             "consensus-bundle-installed marker written: %s (height=%d "
             "artifact_digest=%s)", final_path, height, digest_hex);
    return true;
}

bool boot_consensus_bundle_marker_exists(const char *datadir)
{
    char path[1200];
    if (!marker_path(path, sizeof(path), datadir))
        return false;
    return access(path, F_OK) == 0;
}
