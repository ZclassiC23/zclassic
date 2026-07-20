/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ROM download resume journal — see net/rom_journal.h.
 *
 * STEP-0 STATUS: contracts-commit stubs. The on-disk header layout + the
 * discard path are real; open/is_done/mark/count_done are honest stubs that
 * refuse (returning NULL/false/0 with logged context) until lane 2B
 * (wf/artifact-protocol) lands the real pwrite→fdatasync→setbit→fdatasync
 * durability engine. No caller invokes these yet. */

#include "net/rom_journal.h"

#include "util/log_macros.h"

#include <errno.h>
#include <unistd.h>

struct rom_journal {
    int      fd;
    uint32_t num_chunks;
    uint32_t done;
};

struct rom_journal *rom_journal_open(const char *journal_path,
                                     const uint8_t chunk_root[32],
                                     const uint8_t whole_sha3[32],
                                     uint32_t chunk_size, uint32_t num_chunks)
{
    (void)chunk_root;
    (void)whole_sha3;
    (void)chunk_size;
    (void)num_chunks;
    if (!journal_path)
        LOG_NULL("rom_journal", "NULL journal_path");
    /* Not yet implemented (lane 2B). Fail closed so no caller mistakes a stub
     * for a durable journal. */
    LOG_NULL("rom_journal", "rom_journal_open not implemented yet (step-0 stub)");
}

bool rom_journal_is_done(const struct rom_journal *j, uint32_t idx)
{
    (void)j;
    (void)idx;
    return false; /* raw-return-ok: no journal state exists in the step-0 stub */
}

bool rom_journal_mark(struct rom_journal *j, uint32_t idx)
{
    if (!j)
        LOG_FAIL("rom_journal", "NULL journal handle");
    (void)idx;
    LOG_FAIL("rom_journal", "rom_journal_mark not implemented yet (step-0 stub)");
}

uint32_t rom_journal_count_done(const struct rom_journal *j)
{
    return j ? j->done : 0u; /* raw-return-ok: pure accessor, 0 when absent */
}

void rom_journal_close(struct rom_journal *j)
{
    if (!j)
        return;
    if (j->fd >= 0)
        (void)close(j->fd);
    /* The stub never heap-allocates a handle (open refuses), so there is
     * nothing to free here; lane 2B pairs this with its allocator. */
}

bool rom_journal_discard(const char *journal_path)
{
    if (!journal_path)
        LOG_FAIL("rom_journal", "NULL journal_path");
    if (unlink(journal_path) != 0 && errno != ENOENT)
        LOG_FAIL("rom_journal", "unlink(%s) failed: errno=%d", journal_path, errno);
    return true;
}
