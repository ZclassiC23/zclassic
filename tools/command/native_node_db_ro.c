/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * The ONE read-only <datadir>/node.db open shared by native command leaves.
 *
 * Purpose: a leaf whose declared effect is READ must be able to open the
 * node database without any chance of writing to it. Before this file every
 * such leaf hand-rolled the same twenty lines, and six leaves got it wrong
 * in the other direction — they called node_db_open(), the BOOT ceremony,
 * which opens READWRITE|CREATE and then creates schema, migrates,
 * rename()s the file aside on a failed quick_check, and DELETEs the
 * snapshot_staging rows. Because `datadir` defaults to the CLI's resolved
 * datadir, running one of those leaves with no arguments did that to the
 * operator's LIVE node. See the contract comment on
 * zcl_native_node_db_open_readonly in command/native_command.h.
 *
 * Nothing here creates, migrates, schema-initializes, quarantines, renames
 * or deletes. The only sqlite3 calls are open_v2(READONLY), the query_only
 * pragma, busy_timeout, and close.
 *
 * AND SQLITE_OPEN_READONLY IS NOT ENOUGH ON ITS OWN. Both stores under a
 * datadir are WAL-mode (app/models/src/database.c sets journal_mode=WAL for
 * node.db, lib/storage/src/progress_store.c for consensus.db), and a
 * read-only connection to a WAL database still has to materialize the
 * wal-index before it can read consistently. So sqlite CREATES <db>-shm and
 * <db>-wal beside the database — and a read-only connection cannot unlink
 * them again on close, because unlinking them needs the write lock it does
 * not hold. Measured against the vendored sqlite 3.49.0: a directory holding
 * one 8192-byte node.db came out of open_v2(READONLY) + query_only +
 * schema_version + close holding node.db, node.db-shm (32768 bytes) and
 * node.db-wal (0 bytes).
 *
 * Two files appearing is not a rounding error on this project. The whole
 * recovery doctrine is copy-prove-then-trust-the-proof: copy a datadir, hash
 * it, ask a read leaf a question about the copy, and rely on the hash still
 * describing what is on disk. A read that adds two files has silently voided
 * that proof. And when the read runs as a different user than the node — an
 * operator or a CI job inspecting a copy as root — the sidecars land owned by
 * the wrong uid, where the node's own later open of that directory trips over
 * them.
 *
 * WHAT THIS FILE DOES INSTEAD is pick the open that is side-effect free for
 * the state the database is actually in, because no single open is:
 *
 *   not WAL mode        open_v2(READONLY). A rollback-journal database needs
 *                       no wal-index, so this creates nothing. (Also the
 *                       path a non-database takes, where the header touch
 *                       below is what refuses it.)
 *   WAL, wal-index
 *   already present     open_v2(READONLY). Both sidecars exist, so there is
 *                       nothing left to create, and reading through the live
 *                       wal-index is the ONLY way to see a running node's
 *                       uncheckpointed commits. This is the live-node case.
 *   WAL, no wal-index   file:...?immutable=1. Tells sqlite the database will
 *                       not change under it, which is what lets it skip the
 *                       wal-index entirely: zero files created. Safe here
 *                       precisely BECAUSE there is no wal-index — a WAL
 *                       writer holds one open for the whole life of its
 *                       connection, so its absence means no writer is
 *                       attached and no committed frame is waiting in a log
 *                       for immutable=1 to miss.
 *
 * immutable=1 is deliberately NOT the blanket answer, and the measurement is
 * why: pointed at a database a writer WAS attached to, an immutable=1 read
 * returned 1 row where the truth was 2. It does not consult the log. Reaching
 * for it unconditionally would have traded two stray files for silently stale
 * answers, which on this project is the worse bug — a diagnostic that reads a
 * running node and quietly reports its pre-WAL past.
 *
 * The one state with no side-effect-free correct read is a non-empty <db>-wal
 * with no <db>-shm: the log holds commits, and consulting it means creating
 * the wal-index. That is reported as ZCL_NODE_DB_RO_UNRECOVERED_LOG rather
 * than answered — a named refusal, never a quiet stale read and never a
 * created file. */

#define _GNU_SOURCE
#include "command/native_command.h"

#include "base/hex.h"
#include "base/log_macros.h"
#include "models/database.h"
#include "storage/consensus_db.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A locked WAL must give up rather than park a cursor on a connection the
 * live node is writing through (see the wallet write-wedge post-mortem). */
#define ZCL_NODE_DB_RO_BUSY_MS 2000

/* The longest datadir-derived path a caller hands in is 1200 bytes (see the
 * `path` buffers below), and a file: URI percent-encodes at worst 3 bytes per
 * byte, plus "file:" and "?immutable=1". */
#define ZCL_NODE_DB_RO_URI_MAX (1200 * 3 + 32)

/* Is the database at `path` in WAL mode? Header byte 18 is the "file format
 * write version": 1 for a rollback journal, 2 for WAL. Twenty bytes of read,
 * so this creates nothing, and it survives a clean close — the byte stays 2
 * on a WAL database whose sidecars have been checkpointed away, which is
 * exactly the copied-datadir case that has to be recognized.
 *
 * Anything that is short, or does not carry the magic, is reported as NOT
 * WAL. That routes a non-database through the plain open, where the header
 * touch in zcl_ro_attach is what refuses it — the same answer as before this
 * distinction existed. */
static bool zcl_ro_is_wal_database(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    unsigned char hdr[20] = { 0 };
    size_t got = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (got < sizeof(hdr))
        return false;
    /* 16 bytes, NUL included — the literal is exactly the header magic. */
    if (memcmp(hdr, "SQLite format 3", 16) != 0)
        return false;
    return hdr[18] == 2;
}

/* What already sits beside the database. `wal_bytes` is -1 when there is no
 * log at all, which is a different fact from a log of length 0. */
struct zcl_ro_sidecars {
    bool wal;
    bool shm;
    long long wal_bytes;
};

static void zcl_ro_probe_sidecars(const char *path,
                                  struct zcl_ro_sidecars *out)
{
    out->wal = false;
    out->shm = false;
    out->wal_bytes = -1;

    char side[1300];
    struct stat st;
    int n = snprintf(side, sizeof(side), "%s-wal", path);
    if (n > 0 && (size_t)n < sizeof(side) && stat(side, &st) == 0) {
        out->wal = true;
        out->wal_bytes = (long long)st.st_size;
    }
    n = snprintf(side, sizeof(side), "%s-shm", path);
    if (n > 0 && (size_t)n < sizeof(side) && stat(side, &st) == 0)
        out->shm = true;
}

/* A file: URI over an arbitrary datadir path. A datadir may legitimately hold
 * `?` or `#` — which sqlite would read as the start of the query and the
 * fragment — or `%`, which it would read as an escape, so everything outside
 * the unreserved set is percent-encoded. False means it would not fit, which
 * the caller reports as a path-too-long rather than opening some other file. */
static bool zcl_ro_immutable_uri(const char *path, char *out, size_t out_size)
{
    static const char prefix[] = "file:";
    static const char suffix[] = "?immutable=1";
    const size_t plen = sizeof(prefix) - 1;
    const size_t slen = sizeof(suffix) - 1;

    if (out_size <= plen + slen)
        return false;
    memcpy(out, prefix, plen);
    size_t n = plen;
    for (const char *p = path; *p; p++) {
        unsigned char c = (unsigned char)*p;
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                          c == '_' || c == '~' || c == '/';
        size_t need = unreserved ? 1u : 3u;
        if (n + need + slen + 1 > out_size)
            return false;
        if (unreserved) {
            out[n++] = (char)c;
        } else {
            /* The tree's one hex encoder; lowercase, and percent-encoding is
             * case-insensitive, so sqlite reads %2f and %2F alike. */
            char esc[3];
            zcl_hex_encode(&c, 1, esc);
            out[n++] = '%';
            out[n++] = esc[0];
            out[n++] = esc[1];
        }
    }
    memcpy(out + n, suffix, slen + 1);
    return true;
}

/* Open one DSN and arm the connection. `dsn` is either the bare path or the
 * immutable file: URI; `path` is always the bare path, for the messages. */
static enum zcl_node_db_ro_status zcl_ro_attach(const char *dsn, int flags,
                                                const char *path,
                                                sqlite3 **db_out)
{
    sqlite3 *db = NULL;
    /* READONLY only: no CREATE (a typo'd datadir must fail, never mint a
     * database) and no READWRITE (this handle answers a read leaf). */
    int rc = sqlite3_open_v2(dsn, &db, flags, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("cmd", "database present but not readable: %s: %s", path,
                  db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        if (db)
            sqlite3_close(db);
        return ZCL_NODE_DB_RO_UNREADABLE;
    }
    /* Second, connection-wide refusal of any write that the open flags
     * somehow let through (an ATTACHed db, a temp-table INSERT). */
    (void)sqlite3_exec(db, "PRAGMA query_only=ON", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, ZCL_NODE_DB_RO_BUSY_MS);

    /* Third, TOUCH THE FILE. sqlite3_open_v2 is lazy: it does not read the
     * header, so a 47-byte text file, a truncated database, or any other
     * non-database opens with SQLITE_OK and only fails later, at the first
     * statement — by which point the caller has already been told OK and is
     * inside its own query path, where "it did not open" and "it opened and
     * held nothing" are the same shape. That is the exact confusion this
     * whole helper exists to remove, so pay one page read here.
     *
     * This matters beyond tidiness: zcl_native_node_db_require_readonly's
     * callers include two pre-flights that spend a fee (core.identity.anchor,
     * the zdir register path). They treat ABSENT as "proceed, this is a
     * legitimate first anchor" and anything else as fatal. A corrupt node.db
     * returning OK made the revocation lookup fail, read as "not revoked",
     * and spent the fee on a check that never ran. */
    if (sqlite3_exec(db, "PRAGMA schema_version", NULL, NULL, NULL)
        != SQLITE_OK) {
        LOG_ERROR("cmd", "present but not a readable database: %s: %s", path,
                  sqlite3_errmsg(db));
        sqlite3_close(db);
        return ZCL_NODE_DB_RO_UNREADABLE;
    }

    *db_out = db;
    return ZCL_NODE_DB_RO_OK;
}

/* The read-only open itself, over a path the caller has already resolved.
 *
 * Everything that makes this safe lives in ONE function so a second store
 * cannot drift from the first: there are two databases under a datadir that
 * read leaves are pointed at — node.db and the consensus.db kernel store —
 * and the whole reason both had a boot-ceremony hole is that each open was
 * written out longhand at its own call site. Adding a store means adding a
 * path resolver below, never a second copy of this.
 *
 * The choice of open, and why each branch creates nothing, is the block
 * comment at the top of this file. */
static enum zcl_node_db_ro_status zcl_ro_open_existing(const char *path,
                                                       sqlite3 **db_out)
{
    /* Absent vs unreadable are answered BEFORE the open, because
     * sqlite3_open_v2 collapses both into SQLITE_CANTOPEN and a caller that
     * may proceed on "absent" must never proceed on "unreadable". F_OK only
     * stats; it creates nothing. */
    if (access(path, F_OK) != 0)
        return ZCL_NODE_DB_RO_ABSENT;

    struct zcl_ro_sidecars side;
    zcl_ro_probe_sidecars(path, &side);
    /* Only a WAL database with no wal-index of its own needs the immutable
     * open, and only there is the immutable open honest. */
    bool immutable = zcl_ro_is_wal_database(path) && !(side.wal && side.shm);

    if (immutable && side.wal && side.wal_bytes > 0)
        LOG_RETURN(ZCL_NODE_DB_RO_UNRECOVERED_LOG, "cmd",
                   "%s has a %lld-byte write-ahead log and no wal-index "
                   "(%s-shm) beside it: the log's commits can only be read by "
                   "creating that wal-index, and a read leaf creates nothing. "
                   "Copy the -shm alongside the database, or let the owning "
                   "node recover the log once",
                   path, side.wal_bytes, path);

    sqlite3 *db = NULL;
    enum zcl_node_db_ro_status st;
    if (immutable) {
        char uri[ZCL_NODE_DB_RO_URI_MAX];
        if (!zcl_ro_immutable_uri(path, uri, sizeof(uri)))
            LOG_RETURN(ZCL_NODE_DB_RO_PATH_TOO_LONG, "cmd",
                       "database path does not fit a file: URI: %s", path);
        st = zcl_ro_attach(uri, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, path,
                           &db);
    } else {
        st = zcl_ro_attach(path, SQLITE_OPEN_READONLY, path, &db);
    }
    if (st != ZCL_NODE_DB_RO_OK)
        return st;

    /* Fourth, and only on the immutable path: make sure nothing attached to
     * the database WHILE it was being read. immutable=1 does not consult a
     * write-ahead log — that is exactly what lets it create no wal-index — so
     * a writer arriving between the probe above and here would leave this
     * handle reading the database's pre-log past. A wal-index existing now
     * says that happened. Retry ONCE through the plain open, which at that
     * point creates nothing because both sidecars are already there.
     *
     * The window is a node attaching to a datadir a read leaf was pointed at
     * in the same instant, so this almost never fires; it is here because a
     * silently stale answer is the failure this file cannot ship. */
    if (immutable) {
        struct zcl_ro_sidecars now;
        zcl_ro_probe_sidecars(path, &now);
        if (now.wal && now.shm) {
            sqlite3_close(db);
            db = NULL;
            st = zcl_ro_attach(path, SQLITE_OPEN_READONLY, path, &db);
            if (st != ZCL_NODE_DB_RO_OK)
                return st;
        } else if (now.wal && now.wal_bytes > 0) {
            sqlite3_close(db);
            LOG_RETURN(ZCL_NODE_DB_RO_UNRECOVERED_LOG, "cmd",
                       "%s grew a %lld-byte write-ahead log while it was being "
                       "read and has no wal-index beside it, so this read may "
                       "be missing its commits: refusing rather than answering "
                       "from the database's pre-log state",
                       path, now.wal_bytes);
        }
    }

    *db_out = db;
    return ZCL_NODE_DB_RO_OK;
}

enum zcl_node_db_ro_status zcl_native_node_db_open_readonly(
    const char *datadir, sqlite3 **db_out, struct node_db *ndb_out,
    char *path_out, size_t path_size)
{
    if (path_out && path_size)
        path_out[0] = '\0';
    if (db_out)
        *db_out = NULL;
    if (ndb_out)
        memset(ndb_out, 0, sizeof(*ndb_out));

    if (!db_out || !ndb_out)
        LOG_RETURN(ZCL_NODE_DB_RO_NO_DATADIR, "cmd",
                   "read-only node.db open called without out-parameters");
    if (!datadir || !datadir[0])
        return ZCL_NODE_DB_RO_NO_DATADIR;

    char path[1200];
    int n = snprintf(path, sizeof(path), "%s/node.db", datadir);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return ZCL_NODE_DB_RO_PATH_TOO_LONG;
    if (path_out && path_size) {
        if (path_size <= (size_t)n)
            return ZCL_NODE_DB_RO_PATH_TOO_LONG;
        memcpy(path_out, path, (size_t)n + 1);
    }

    sqlite3 *db = NULL;
    enum zcl_node_db_ro_status st = zcl_ro_open_existing(path, &db);
    if (st != ZCL_NODE_DB_RO_OK)
        return st;

    *db_out = db;
    ndb_out->db = db;
    ndb_out->open = true;
    snprintf(ndb_out->path, sizeof(ndb_out->path), "%s", path);
    return ZCL_NODE_DB_RO_OK;
}

/* The kernel store — <datadir>/consensus.db, or the legacy progress.kv name
 * when that is what the datadir still carries.
 *
 * The write-side open is progress_store_open(): READWRITE|CREATE, a rename
 * migration from progress.kv, a schema ensure, and — on a failed integrity
 * check — progress_store_quarantine_corrupt(), which rename()s the
 * append-only fact log that is the authority for every stage cursor aside to
 * consensus.db.corrupt-<ts> and installs a fresh empty one. That is the
 * right behaviour for a BOOTING node, which can re-derive the store from the
 * snapshot and the anchor. It is data destruction when a read leaf does it to
 * a copied datadir the operator asked a question about, so a read leaf gets
 * this instead. It also takes no part in the process singleton: an
 * offline-copy read must not become the process's one open kernel store. */
enum zcl_node_db_ro_status zcl_native_kernel_store_open_readonly(
    const char *datadir, sqlite3 **db_out, char *path_out, size_t path_size)
{
    if (path_out && path_size)
        path_out[0] = '\0';
    if (db_out)
        *db_out = NULL;

    if (!db_out)
        LOG_RETURN(ZCL_NODE_DB_RO_NO_DATADIR, "cmd",
                   "read-only kernel store open called without a handle");
    if (!datadir || !datadir[0])
        return ZCL_NODE_DB_RO_NO_DATADIR;

    char path[1200];
    if (!consensus_db_kernel_store_path(datadir, path, sizeof(path)))
        return ZCL_NODE_DB_RO_PATH_TOO_LONG;
    size_t n = strlen(path);
    /* Reported even when the file turns out to be absent, so the caller can
     * name exactly which path it looked at. */
    if (path_out && path_size) {
        if (path_size <= n)
            return ZCL_NODE_DB_RO_PATH_TOO_LONG;
        memcpy(path_out, path, n + 1);
    }

    return zcl_ro_open_existing(path, db_out);
}

bool zcl_native_node_db_require_readonly(
    const char *datadir, struct zcl_command_reply *reply, const char *what,
    sqlite3 **db_out, struct node_db *ndb_out)
{
    if (!reply)
        LOG_FAIL("cmd", "read-only node.db require called without a reply");
    const char *noun = (what && what[0]) ? what : "the node database";

    char path[1200];
    enum zcl_node_db_ro_status st = zcl_native_node_db_open_readonly(
        datadir, db_out, ndb_out, path, sizeof(path));
    if (st == ZCL_NODE_DB_RO_OK)
        return true;

    char message[512];
    switch (st) {
    case ZCL_NODE_DB_RO_NO_DATADIR:
        snprintf(message, sizeof(message),
                 "no datadir resolved for %s — pass --datadir", noun);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false, message, "");
        return false;
    case ZCL_NODE_DB_RO_PATH_TOO_LONG:
        snprintf(message, sizeof(message),
                 "datadir path too long for %s", noun);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID,
                               "DATADIR_PATH_TOO_LONG", "normalize", false,
                               false, message, datadir ? datadir : "");
        return false;
    case ZCL_NODE_DB_RO_ABSENT:
        snprintf(message, sizeof(message),
                 "no node.db at that datadir, so %s cannot be read — check "
                 "--datadir, or boot the node once to create it", noun);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED,
                               "NODE_DB_UNAVAILABLE", "execute", true, false,
                               message, path);
        return false;
    case ZCL_NODE_DB_RO_UNRECOVERED_LOG:
        /* Readable in principle, and deliberately not read: consulting the
         * write-ahead log means creating the wal-index next to it, and a read
         * leaf does not modify the directory it was pointed at. Says what to
         * do about it, because "would not open" would send the operator
         * hunting permissions on a database whose permissions are fine. */
        snprintf(message, sizeof(message),
                 "node.db has a write-ahead log with no node.db-shm beside "
                 "it, so %s was NOT read: reading the log would mean creating "
                 "that file in your datadir. Copy node.db-shm alongside "
                 "node.db, or let the owning node recover the log once",
                 noun);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED,
                               "NODE_DB_UNRECOVERED_LOG", "execute", true,
                               false, message, path);
        return false;
    case ZCL_NODE_DB_RO_UNREADABLE:
    default:
        /* Distinct from ABSENT on purpose: the file IS there and we could
         * not read it, which is never the same answer as "empty". */
        snprintf(message, sizeof(message),
                 "node.db exists but would not open read-only, so %s could "
                 "not be read — check permissions and that the path is a "
                 "SQLite database", noun);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_BLOCKED,
                               ZCL_COMMAND_EXIT_BLOCKED,
                               "NODE_DB_UNREADABLE", "execute", true, false,
                               message, path);
        return false;
    }
}

void zcl_native_node_db_close_readonly(sqlite3 **db, struct node_db *ndb)
{
    sqlite3 *handle = db ? *db : (ndb ? ndb->db : NULL);
    if (handle)
        sqlite3_close(handle);
    if (db)
        *db = NULL;
    /* The shim never owned prepared statements, so there is nothing for
     * node_db_close to finalize — just drop the borrowed pointer. */
    if (ndb)
        memset(ndb, 0, sizeof(*ndb));
}
