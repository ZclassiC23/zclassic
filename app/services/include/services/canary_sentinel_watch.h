/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * canary_sentinel_watch — in-node watcher over the replay-canary verdict
 * sentinels written by tools/scripts/replay_canary.sh, so the node itself
 * pages on a canary FAIL instead of relying on an external timer's exit code.
 *
 * Contract with the harness (tools/scripts/replay_canary.sh):
 *   - Verdict dir: $ZCL_CANARY_VERDICT_DIR, defaulting to
 *     $HOME/.local/state/zclassic23-canary (same env the script uses, so
 *     tests stay hermetic with a private tmp dir).
 *   - Sentinels: replay_canary_<kind>.json (<kind> = anchor | genesis | ...),
 *     each atomically replaced (tmp + rename) by every run — file content IS
 *     the latest verdict for that kind. Fields read: verdict, from, ts,
 *     started_ts, reason, build_commit.
 *
 * Cross-build staleness (load-bearing): the verdict dir is SHARED across
 * lanes and restarts. A sentinel whose build_commit differs from the running
 * binary's (zcl_build_commit()) is NOT evidence about this binary — a prior
 * build's FAIL must never latch the pager on a freshly-deployed node. Such a
 * FAIL is recorded for display (stale_build=true in the dump) but does not
 * raise; only a SAME-build (or no-build_commit legacy) FAIL pages. The drop
 * is logged once per mtime (never a silent swallow).
 *
 * Pre-start staleness (load-bearing): a sentinel whose started_ts predates
 * this process is likewise not evidence about this process. It is recorded
 * for display (stale_run=true in the dump) but does not raise. Sentinels
 * missing started_ts retain legacy behavior rather than being silently
 * discarded.
 *
 * Absence/staleness policy (explicit, load-bearing): a sentinel can be
 * legitimately ABSENT — every canary run deletes its sentinel first, so a
 * killed run leaves nothing (never-exit-0-as-proof). Therefore absence NEVER
 * transitions state in either direction:
 *   - no dir / no files / unreadable dir → quiet no-op (fresh installs that
 *     never ran the canary stay silent — no log spam, no paging);
 *   - a kind that reported FAIL stays latched FAIL until a sentinel for that
 *     SAME kind reports PASS (a re-running canary deleting its FAIL sentinel
 *     must not un-page the node before a positive PASS lands).
 * Corrupt/torn JSON is skipped (logged once per file mtime) and never raises
 * on its own — only an explicit verdict=="FAIL" field pages.
 *
 * The watch is standalone file-scan only: NO database writes, NO threads
 * (cadence comes from the supervised canary_sentinel_poll Job), and it never
 * touches chain state or locks (lock-order law). */

#ifndef ZCL_SERVICES_CANARY_SENTINEL_WATCH_H
#define ZCL_SERVICES_CANARY_SENTINEL_WATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One supervised scan of the verdict dir. Updates the per-kind slots and the
 * FAIL latch. Quiet no-op when the dir is absent/unreadable or empty. */
void canary_sentinel_watch_tick_once(void);

/* True while any kind's latest verdict is FAIL (the replay_canary_failed
 * Condition's detect reads exactly this). */
bool canary_sentinel_watch_fail_active(void);

/* Compose "kind=<k> reason=<r> ts=<t>" for every FAIL-latched kind into out
 * (semicolon-separated). Returns the number of FAIL kinds. out may be
 * NULL/cap 0 for a count-only probe. */
int canary_sentinel_watch_fail_detail(char *out, size_t cap);

/* Resolve the verdict dir (env override, then $HOME default) into out.
 * Returns false when no dir can be resolved (no env, no HOME). */
bool canary_sentinel_watch_resolve_dir(char *out, size_t cap);

/* See CLAUDE.md "Adding state introspection". Reentrant-safe. */
struct json_value;
bool canary_watch_dump_state_json(struct json_value *out, const char *key);

#ifdef ZCL_TESTING
/* Clear all per-kind slots, counters, and the FAIL latch. */
void canary_sentinel_watch_test_reset(void);
void canary_sentinel_watch_test_set_process_start(int64_t start_unix);
#endif

#endif /* ZCL_SERVICES_CANARY_SENTINEL_WATCH_H */
