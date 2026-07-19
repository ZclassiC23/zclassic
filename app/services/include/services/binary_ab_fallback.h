/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Binary A/B fallback — the node's half of the crash-loop self-defense that
 * the systemd launcher (deploy/zclassic23-launch.sh) drives.
 *
 * The problem this closes
 * -----------------------
 * The unit runs Restart=always with a stepped backoff, so a deploy that is
 * dead-on-arrival (segfaults or FATALs before it ever reaches activation)
 * loops forever on the *bad* binary — visibly, but never recovering. That is
 * the sibling of the stale-binary blocker (services/binary_staleness_service.h,
 * config/src/boot.c): stale-binary catches "the on-disk file was replaced out
 * from under a running-good process"; THIS catches "the newly-deployed file
 * cannot boot at all".
 *
 * The split-of-responsibility (kept deliberately trivial)
 * -------------------------------------------------------
 *   launcher (shell, ExecStart): before every exec it increments a
 *     boot-failure streak counter file. If the streak has reached the
 *     fallback threshold it execs the LAST-KNOWN-GOOD slot instead of the
 *     current binary and sets ZCL_BINARY_FALLBACK_ACTIVE=1 in the child env.
 *
 *   node (this module): success is defined by the node ITSELF reaching
 *     activation-ready — not by "the process lived N seconds". On ready the
 *     node resets the streak to 0 and, unless it is running as the fallback
 *     slot, atomically promotes the current binary to the last-good slot.
 *     Because the launcher increments and the node resets, there is no
 *     double-count bookkeeping: a boot that never reaches ready simply leaves
 *     the incremented streak in place for the next launch to read.
 *
 * When the launcher falls back it passes ZCL_BINARY_FALLBACK_ACTIVE=1; the
 * node raises the `binary.fallback_active` typed blocker at boot so
 * `zclassic23 status` shows the degraded-but-alive state (OWNER remedy: an
 * operator must deploy a good binary and clear the streak).
 *
 * The pure seams below take explicit paths/flags (no env, no exec) so the
 * test group drives promotion/reset/blocker directly; the *_env wrappers read
 * the launcher-set environment and are the two call sites in the boot path.
 */
#ifndef ZCL_SERVICES_BINARY_AB_FALLBACK_H
#define ZCL_SERVICES_BINARY_AB_FALLBACK_H

#include <stdbool.h>

/* Typed blocker id raised while running the fallback (last-good) slot.
 * Named `*_BLOCKER_ID` so tools/scripts/check_blocker_remedy.sh resolves it
 * from this macro; the matching OWNER row lives in
 * app/conditions/include/conditions/blocker_remedy_bindings.def. */
#define BINARY_FALLBACK_BLOCKER_ID    "binary.fallback_active"
#define BINARY_FALLBACK_BLOCKER_OWNER "binary_ab"

/* Environment contract with deploy/zclassic23-launch.sh. */
#define BINARY_AB_ENV_SLOTS_DIR    "ZCL_BINARY_SLOTS_DIR"
#define BINARY_AB_ENV_CURRENT      "ZCL_BINARY_CURRENT"
#define BINARY_AB_ENV_FALLBACK     "ZCL_BINARY_FALLBACK_ACTIVE"

/* Basenames of the launcher-managed files inside the slots dir. */
#define BINARY_AB_STREAK_BASENAME   "boot-fail-streak"
#define BINARY_AB_LASTGOOD_BASENAME "last-good"

/* ── Pure seams (test drives these directly) ─────────────────────────── */

/* Overwrite `streak_file` with "0\n" (tmp+rename, crash-safe). Returns true
 * on success, false (logged) on any IO error. NULL/empty path → false. */
bool binary_ab_reset_streak(const char *streak_file);

/* E2 boot-loop-failsafe: increment `streak_file` by 1 (tmp+rename+fsync,
 * crash-safe), mirroring deploy/zclassic23-launch.sh's own "increment BEFORE
 * exec" step — but for a SELF-respawn exit (main.c's in-process execv after
 * chain_tip_watchdog / supervisor_backstop declares a liveness stall) that
 * never goes through the launcher shell script at all, so the launcher's own
 * increment never sees it. A missing/unreadable/malformed file reads as 0
 * (matches the launcher's own `cat ... || echo 0` fallback). Returns true on
 * success, false (logged) on any IO error. NULL/empty path → false. */
bool binary_ab_note_self_respawn_exit(const char *streak_file);

/* Atomically copy the bytes at `current_path` into
 * `<slots_dir>/last-good` (tmp write → fsync → chmod 0755 → rename → dir
 * fsync), so the last-good slot is an independent copy — never a symlink to
 * the file a bad deploy just overwrote. Returns true on success. */
bool binary_ab_promote(const char *slots_dir, const char *current_path);

/* Ready action: reset the streak in `slots_dir`; if !fallback_active and
 * `current_path` is non-empty, promote it to last-good. A NULL/empty
 * `slots_dir` means "not launcher-managed" and is a no-op success. Returns
 * true if every attempted step succeeded. */
bool binary_ab_on_ready(const char *slots_dir, const char *current_path,
                        bool fallback_active);

/* Raise the binary.fallback_active blocker when `fallback_active`. No-op
 * otherwise. Idempotent (blocker_set rate-limits dups). */
void binary_ab_raise_fallback_blocker(bool fallback_active);

/* ── Boot-path env wrappers ──────────────────────────────────────────── */

/* Reads ZCL_BINARY_SLOTS_DIR / ZCL_BINARY_CURRENT / ZCL_BINARY_FALLBACK_ACTIVE
 * and runs binary_ab_on_ready. Safe no-op when the slots-dir env is unset
 * (node launched directly, not via the launcher). */
void binary_ab_promote_on_ready_env(void);

/* Reads ZCL_BINARY_FALLBACK_ACTIVE and raises the blocker if set. */
void binary_ab_raise_fallback_blocker_env(void);

/* Reads ZCL_BINARY_SLOTS_DIR and increments its boot-fail-streak file (see
 * binary_ab_note_self_respawn_exit above). Safe no-op when the slots-dir env
 * is unset (node launched directly, not via the launcher) — there is no
 * streak file to increment. Called from config/src/boot_loop_guard.c when
 * this boot's exit-reason breadcrumb (util/shutdown_stagewatch.h) says the
 * PREVIOUS exit was a self-respawn. */
void binary_ab_note_self_respawn_exit_env(void);

#endif /* ZCL_SERVICES_BINARY_AB_FALLBACK_H */
