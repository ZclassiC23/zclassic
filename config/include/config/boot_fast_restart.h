/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tier-2 fast restart glue (boot layer).
 *
 * Keeps the verified-clean quick_check-skip wiring out of the already-large
 * boot.c: arming node_db_open's skip probe, and the post-READY background
 * quick_check that re-validates node.db when a boot took the skip.
 */

#ifndef ZCL_CONFIG_BOOT_FAST_RESTART_H
#define ZCL_CONFIG_BOOT_FAST_RESTART_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register the shutdown-marker quick_check-skip probe with node_db_open. Call
 * once, BEFORE node.db is opened (after detect_unclean has cached the marker). */
void boot_fast_restart_arm_quick_check_skip_probe(void);

/* If this boot skipped quick_check on node.db, spawn one background quick_check
 * (fresh read-only connection) so the skipped check is still eventually run.
 * A failure is raised loudly via EV_DB_ERROR + EV_OPERATOR_NEEDED. No-op when
 * quick_check actually ran this boot. `datadir` locates node.db. */
void boot_fast_restart_start_bg_quick_check(const char *datadir);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CONFIG_BOOT_FAST_RESTART_H */
