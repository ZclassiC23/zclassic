/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * index_fold_guard — shared safety rails for the body-derived secondary
 * index backfills (address_index, txindex projection). These are the two
 * always-on "omniscience" catalogs: ON by default so a plain boot knows
 * everything about its own chain, but a historical fold walks millions of
 * blocks and must never (a) fill the disk blindly, nor (b) spin forever below a
 * snapshot-seed floor where block bodies are structurally absent.
 *
 * Both rails surface a NAMED typed blocker (util/blocker.h) so a stall is an
 * operator-visible entry in `zclassic23 dumpstate blocker`, never a silent
 * refuse. Neither ever blocks tip-follow: the caller holds only a bounded batch
 * of the progress-store trylock and yields immediately (see
 * address_index_service.c / txindex_projection_service.c). */

#ifndef ZCL_SERVICES_INDEX_FOLD_GUARD_H
#define ZCL_SERVICES_INDEX_FOLD_GUARD_H

#include <stdbool.h>
#include <stdint.h>

typedef struct sqlite3 sqlite3;

/* Conservative free-space floor a first-run/continuing index backfill requires
 * before it writes new rows. A full historical fold can add several GiB; a
 * 10 GiB headroom keeps the node well clear of the disk_monitor refuse
 * threshold (1 GiB) so consensus writes never lose their last bytes to an
 * observability index. Overridable in tests via
 * index_fold_set_min_free_for_test(). */
#define INDEX_FOLD_MIN_FREE_BYTES ((int64_t)(10LL * 1024 * 1024 * 1024))

/* Free-disk precheck. Returns true if it is safe to write index rows now.
 * Returns false — and raises a named "<index_id>.disk_low" BLOCKER_RESOURCE
 * blocker — when free space on `datadir` is below the backfill floor OR the
 * disk_monitor is already CRITICAL. Clears that blocker when space is healthy
 * again. `index_id` is the blocker id-prefix (e.g. "address_index"); `subsys`
 * is the owner subsystem. Both must be interned/static strings. A datadir whose
 * free space cannot be measured (statvfs error) fails OPEN (returns true) — the
 * disk_monitor condition owns the hard refuse; this is a conservative gate. */
bool index_fold_disk_ok(const char *index_id, const char *subsys,
                            const char *datadir);

/* Called when a fold hit an ABSENT body at `absent_height`. If that height is
 * at/below the durable snapshot-seed floor (REDUCER_TRUSTED_BASE_HEIGHT_KEY in
 * progress_meta), bodies below it are structurally absent and the projection
 * can never fold across the floor: raise a named
 * "<index_id>.below_snapshot_seed" BLOCKER_DEPENDENCY blocker (waiting on the
 * historical body/shielded backfill) instead of spinning. Above the floor — or
 * on a from-genesis datadir with no seed — it is a transient/genuine gap the
 * service's own coverage_blocked flag surfaces, so the seed blocker is cleared.
 * A DB read error leaves any existing seed blocker untouched (fail-soft). */
void index_fold_note_absent_body(const char *index_id, const char *subsys,
                                     sqlite3 *db, int64_t absent_height);

/* Clear the "<index_id>.below_snapshot_seed" blocker (the fold advanced or
 * caught up to H*). No-op if not set. */
void index_fold_clear_seed_blocker(const char *index_id);

/* Test-only: override the free-space floor (bytes). Pass a negative value to
 * restore the compiled INDEX_FOLD_MIN_FREE_BYTES default. */
void index_fold_set_min_free_for_test(int64_t bytes);

#endif /* ZCL_SERVICES_INDEX_FOLD_GUARD_H */
