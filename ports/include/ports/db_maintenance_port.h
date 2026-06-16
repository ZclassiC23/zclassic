/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Rhett Creighton
 *
 * db_maintenance_port — storage interface for the three SQLite
 * housekeeping operations the db_maintenance scheduler runs.
 *
 * db_maintenance is a NON-CONSENSUS background service: it ticks three
 * independent maintenance ops on their own schedules and reports timing
 * via the EV_DB_MAINTENANCE_* events. The only thing it does that touches
 * sqlite is execute one fixed maintenance statement per op. Those three
 * executions are exactly what this port captures, one method per op:
 *
 *   wal_checkpoint(self, err, errsz)  "PRAGMA wal_checkpoint(TRUNCATE);"
 *                                     Flush committed WAL frames into the
 *                                     main file and truncate the WAL.
 *   analyze(self, err, errsz)         "ANALYZE;"
 *                                     Rebuild sqlite_stat1 planner stats.
 *   vacuum(self, err, errsz)          "VACUUM;"
 *                                     Rebuild + defragment the whole file.
 *
 * One read accessor accompanies the three ops so the service never names
 * sqlite for its WAL-size cap either:
 *
 *   wal_size_bytes(self, out)         sqlite3_db_filename(...,"main") then
 *                                     stat("<db>-wal") — size of the WAL
 *                                     file on disk, used by the size-cap
 *                                     forced-checkpoint path.
 *
 * No sqlite type appears in this header. The adapter under
 * adapters/outbound/persistence/ is the only thing that includes sqlite
 * for this subsystem. It wraps an already-open sqlite3* via `self` and
 * never closes it.
 *
 * Contract for every method:
 *   - Runs the op's SQL verbatim via sqlite3_exec (no statement caching).
 *   - Returns true iff the op reported success (SQLITE_OK).
 *   - On failure returns false and, if `err`/`errsz` are non-NULL, copies
 *     a NUL-terminated SQLite error string into `err` (truncated to
 *     `errsz`). The buffer is left a valid C string in all paths.
 *   - Returns false (with a "no/closed db" message) if `self` is NULL or
 *     the wrapped connection is NULL — the scheduler validates db-open
 *     state before it ever reaches the port, so this is a belt-and-braces
 *     guard.
 *
 * Threading: the scheduler holds its own mutex across the call, so a
 * synchronous run_now and the tick thread never race on the handle.
 */

#ifndef ZCL_PORTS_DB_MAINTENANCE_PORT_H
#define ZCL_PORTS_DB_MAINTENANCE_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct db_maintenance_port {
    void *self;

    /* "PRAGMA wal_checkpoint(TRUNCATE);" */
    bool (*wal_checkpoint)(void *self, char *err, size_t errsz);

    /* "ANALYZE;" */
    bool (*analyze)(void *self, char *err, size_t errsz);

    /* "VACUUM;" */
    bool (*vacuum)(void *self, char *err, size_t errsz);

    /* Size in bytes of the wrapped connection's write-ahead-log file
     * ("<db path>-wal"). Sets *out and returns true if the DB filename is
     * known and the WAL file stat()s successfully; returns false (out
     * untouched) for a NULL self / NULL out / NULL connection, an
     * in-memory DB with no on-disk path, or a WAL file that does not exist
     * yet (size-unavailable returns false / out untouched). */
    bool (*wal_size_bytes)(void *self, int64_t *out);
};

#endif /* ZCL_PORTS_DB_MAINTENANCE_PORT_H */
