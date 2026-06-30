/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONFIG_BOOT_SHUTDOWN_MARKER_H
#define ZCL_CONFIG_BOOT_SHUTDOWN_MARKER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Clean-shutdown marker lifecycle.
 *
 * detect_unclean() runs during boot: it observes the previous run's marker and
 * node.db WAL, emits EV_CRASH_RECOVERY_START when the WAL exists without the
 * clean marker, then unlinks the marker so only a full future shutdown can
 * recreate it.
 *
 * write_clean() runs last during shutdown. A true return means the marker was
 * written; callers may ignore false to preserve the existing best-effort
 * shutdown posture. */
bool boot_shutdown_marker_detect_unclean(const char *datadir);
bool boot_shutdown_marker_write_clean(const char *datadir);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CONFIG_BOOT_SHUTDOWN_MARKER_H */
