/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONFIG_BOOT_DATADIR_LOCK_H
#define ZCL_CONFIG_BOOT_DATADIR_LOCK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Single-process guard for the selected datadir.
 *
 * Returns false only when the datadir is owned by a running process or the
 * datadir path cannot be represented safely. PID-file creation failure remains
 * non-fatal to preserve the historical boot posture: warn loudly, then proceed
 * without a lock rather than refusing an otherwise usable local node. */
bool boot_datadir_lock_acquire(const char *datadir);
void boot_datadir_lock_release(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CONFIG_BOOT_DATADIR_LOCK_H */
